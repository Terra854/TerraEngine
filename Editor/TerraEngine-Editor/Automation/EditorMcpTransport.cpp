// EditorMcpTransport.cpp - Minimal local JSON-RPC/MCP method router.
//
// This is intentionally transport-shaped rather than server-shaped. It handles
// loopback checks and MCP message names, then delegates all state and editor
// behavior to EditorMcpBridge.

#include "EditorMcpTransport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <exception>
#include <sstream>
#include <utility>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		template<typename Callback>
		class ScopeExitGuard final
		{
		public:
			explicit ScopeExitGuard(Callback callbackValue)
				: callback(std::move(callbackValue))
			{
			}

			ScopeExitGuard(const ScopeExitGuard&) = delete;
			ScopeExitGuard& operator=(const ScopeExitGuard&) = delete;

			~ScopeExitGuard()
			{
				// Accepted sockets have one listener-owned close path. Running the
				// callback from the destructor covers continue, break, and exception.
				callback();
			}

		private:
			Callback callback;
		};

#if defined(_WIN32)
		using SocketHandle = SOCKET;
		constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

		void CloseSocket(SocketHandle socket)
		{
			if (socket != kInvalidSocket)
			{
				closesocket(socket);
			}
		}

		std::string LastSocketError()
		{
			return "WinSock error " + std::to_string(WSAGetLastError());
		}
#else
		using SocketHandle = int;
		constexpr SocketHandle kInvalidSocket = -1;

		void CloseSocket(SocketHandle socket)
		{
			if (socket != kInvalidSocket)
			{
				close(socket);
			}
		}

		std::string LastSocketError()
		{
			return std::strerror(errno);
		}
#endif

		bool EnsureSocketRuntime(std::string& error)
		{
#if defined(_WIN32)
			WSADATA data{};
			const int result = WSAStartup(MAKEWORD(2, 2), &data);
			if (result != 0)
			{
				error = "WSAStartup failed with " + std::to_string(result);
				return false;
			}
#else
			(void)error;
#endif
			return true;
		}

		void ShutdownSocketRuntime()
		{
#if defined(_WIN32)
			WSACleanup();
#endif
		}

		std::string BuildHttpResponse(const nlohmann::json& body)
		{
			const std::string payload = body.dump();
			std::ostringstream stream;
			stream << "HTTP/1.1 200 OK\r\n";
			stream << "Content-Type: application/json\r\n";
			stream << "Content-Length: " << payload.size() << "\r\n";
			stream << "Connection: close\r\n\r\n";
			stream << payload;
			return stream.str();
		}

		std::string BuildHttpError(int status, std::string_view message)
		{
			nlohmann::json body = {{"ok", false}, {"error", message}};
			const std::string payload = body.dump();
			std::ostringstream stream;
			stream << "HTTP/1.1 " << status << " Error\r\n";
			stream << "Content-Type: application/json\r\n";
			stream << "Content-Length: " << payload.size() << "\r\n";
			stream << "Connection: close\r\n\r\n";
			stream << payload;
			return stream.str();
		}

		std::string BuildHttpAccepted()
		{
			// JSON-RPC notifications have no response object. HTTP still needs a
			// bounded acknowledgement so the notification connection can close.
			return "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		}

		enum class ContentLengthStatus
		{
			Missing,
			Malformed,
			Present
		};

		struct ContentLengthParseResult
		{
			ContentLengthStatus status = ContentLengthStatus::Missing;
			std::size_t value = 0;
		};

		ContentLengthParseResult ParseContentLength(std::string_view headers)
		{
			// Parse individual header lines instead of searching a substring. This
			// rejects duplicate or malformed lengths, which otherwise let different
			// HTTP parsers disagree about where this request ends.
			ContentLengthParseResult result;
			std::size_t lineStart = headers.find("\r\n");
			if (lineStart == std::string_view::npos)
			{
				return {ContentLengthStatus::Malformed, 0};
			}
			lineStart += 2;
			while (lineStart < headers.size())
			{
				const std::size_t lineEnd = headers.find("\r\n", lineStart);
				const std::string_view line = headers.substr(lineStart,
					lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - lineStart);
				if (line.empty())
				{
					break;
				}
				const std::size_t separator = line.find(':');
				if (separator == std::string_view::npos)
				{
					return {ContentLengthStatus::Malformed, 0};
				}
				std::string name(line.substr(0, separator));
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
					return static_cast<char>(std::tolower(character));
				});
				if (name == "content-length")
				{
					if (result.status == ContentLengthStatus::Present)
					{
						return {ContentLengthStatus::Malformed, 0};
					}
					std::string_view value = line.substr(separator + 1);
					while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
					{
						value.remove_prefix(1);
					}
					while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
					{
						value.remove_suffix(1);
					}
					std::size_t parsedLength = 0;
					const auto [parsedEnd, error] = std::from_chars(value.data(), value.data() + value.size(), parsedLength);
					if (value.empty() || error != std::errc{} || parsedEnd != value.data() + value.size())
					{
						return {ContentLengthStatus::Malformed, 0};
					}
					result = {ContentLengthStatus::Present, parsedLength};
				}
				if (lineEnd == std::string_view::npos)
				{
					break;
				}
				lineStart = lineEnd + 2;
			}
			return result;
		}

		bool SendAll(SocketHandle socket, const std::string& response)
		{
			std::size_t sentBytes = 0;
			while (sentBytes < response.size())
			{
				const int sent = send(socket, response.data() + sentBytes, static_cast<int>(response.size() - sentBytes), 0);
				if (sent <= 0)
				{
					return false;
				}
				sentBytes += static_cast<std::size_t>(sent);
			}
			return true;
		}

		enum class SocketReadiness
		{
			Ready,
			TimedOut,
			Failed
		};

		SocketReadiness WaitForReadableSocket(SocketHandle socket)
		{
			fd_set readableSockets;
			FD_ZERO(&readableSockets);
			FD_SET(socket, &readableSockets);
			// Polling is deliberately bounded. Windows does not guarantee that a
			// synchronous recv already running on another thread returns promptly
			// after shutdown(), so this timeout gives stopRequested a reliable
			// protected-boundary check without imposing a client request timeout.
			timeval timeout{};
			timeout.tv_usec = 100'000;
#if defined(_WIN32)
			const int readyCount = select(0, &readableSockets, nullptr, nullptr, &timeout);
#else
			const int readyCount = select(socket + 1, &readableSockets, nullptr, nullptr, &timeout);
#endif
			if (readyCount > 0)
			{
				return SocketReadiness::Ready;
			}
			return readyCount == 0 ? SocketReadiness::TimedOut : SocketReadiness::Failed;
		}

		void WakeListener(std::uint16_t port)
		{
			if (port == 0)
			{
				return;
			}
			SocketHandle wakeSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (wakeSocket == kInvalidSocket)
			{
				return;
			}
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = htons(port);
			connect(wakeSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
			CloseSocket(wakeSocket);
		}

		void JoinThreadNoThrow(std::thread& thread) noexcept
		{
			if (!thread.joinable())
			{
				return;
			}
			try
			{
				// Transport ownership keeps destruction on the editor/lifecycle
				// thread. The defensive self check prevents std::system_error from
				// escaping a no-throw cleanup path if that contract is violated.
				if (thread.get_id() == std::this_thread::get_id())
				{
					thread.detach();
				}
				else
				{
					thread.join();
				}
			}
			catch (...)
			{
				// A joinable std::thread would terminate during member destruction.
				// Detach is only a last-resort containment path for an OS error.
				try
				{
					if (thread.joinable())
					{
						thread.detach();
					}
				}
				catch (...)
				{
				}
			}
		}

		// Eight ordinary tool calls may wait for the editor thread. One additional
		// bounded worker is reserved for a cancellation notification so a user can
		// still retract a queued call while all ordinary lanes are occupied.
		constexpr std::size_t kMaxConcurrentOrdinaryClientWorkers = 8;
		constexpr std::size_t kMaxConcurrentClientWorkers = kMaxConcurrentOrdinaryClientWorkers + 1;
		constexpr std::size_t kMaxHttpHeaderBytes = 16U * 1024U;
		constexpr std::size_t kMaxHttpBodyBytes = 1024U * 1024U;
		constexpr auto kHttpRequestDeadline = std::chrono::seconds(3);
		constexpr auto kHttpIdleReadDeadline = std::chrono::seconds(1);
	}

	EditorMcpTransport::EditorMcpTransport(EditorMcpBridge& bridge)
		: bridge(bridge)
	{
	}

	EditorMcpTransport::~EditorMcpTransport() noexcept
	{
		// The transport owns sockets, a listener, and accepted-client workers.
		// Always reclaim them even when a caller leaves scope through an exception.
		StopListening();
	}

	bool EditorMcpTransport::IsLocalAddress(std::string_view address)
	{
		return address == "127.0.0.1"
			|| address == "localhost"
			|| address == "::1"
			|| address == "[::1]"
			|| address.rfind("127.", 0) == 0;
	}

	bool EditorMcpTransport::AcceptPeer(std::string_view address, std::string_view clientName)
	{
		if (!IsLocalAddress(address))
		{
			bridge.RecordRejectedPeer(std::string(address), "non-local peer");
			return false;
		}
		if (!bridge.TryOnLocalClientConnected(std::string(clientName), std::string(address)))
		{
			bridge.RecordRejectedPeer(std::string(address), "bridge disabled");
			return false;
		}
		{
			std::lock_guard lock(socketMutex);
			++connectedPeerCount;
		}
		return true;
	}

	void EditorMcpTransport::DropPeer(std::string_view reason)
	{
		bool wasConnected = false;
		{
			std::lock_guard lock(socketMutex);
			wasConnected = connectedPeerCount > 0;
			if (wasConnected)
			{
				--connectedPeerCount;
			}
		}
		if (wasConnected)
		{
			bridge.OnLocalClientDisconnected(std::string(reason));
		}
	}

	bool EditorMcpTransport::StartListening(std::uint16_t preferredPort)
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		if (listening)
		{
			return true;
		}

		std::string startupError;
		if (!EnsureSocketRuntime(startupError))
		{
			std::lock_guard lock(socketMutex);
			lastError = std::move(startupError);
			return false;
		}
		{
			std::lock_guard lock(socketMutex);
			lastError.clear();
		}

		SocketHandle socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socketHandle == kInvalidSocket)
		{
			{
				std::lock_guard lock(socketMutex);
				lastError = LastSocketError();
			}
			ShutdownSocketRuntime();
			return false;
		}

#if defined(_WIN32)
		// A local automation listener must own its port exclusively. Windows
		// SO_REUSEADDR can allow another process to bind the same endpoint, which
		// would make client routing ambiguous and weaken the loopback boundary.
		int exclusive = 1;
		setsockopt(socketHandle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
		int reuse = 1;
		setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(preferredPort);
		if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			{
				std::lock_guard lock(socketMutex);
				lastError = "Could not bind MCP bridge to 127.0.0.1:" + std::to_string(preferredPort) + ": " + LastSocketError();
			}
			CloseSocket(socketHandle);
			ShutdownSocketRuntime();
			return false;
		}

		std::uint16_t boundPort = 0;
		sockaddr_in boundAddress{};
		socklen_t boundLength = sizeof(boundAddress);
		if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&boundAddress), &boundLength) == 0)
		{
			boundPort = ntohs(boundAddress.sin_port);
		}

		if (listen(socketHandle, 8) != 0)
		{
			{
				std::lock_guard lock(socketMutex);
				lastError = LastSocketError();
			}
			CloseSocket(socketHandle);
			ShutdownSocketRuntime();
			return false;
		}

		{
			std::lock_guard lock(socketMutex);
			listenSocket = static_cast<std::intptr_t>(socketHandle);
			activeClientSockets.clear();
			listenPort = boundPort;
			connectedPeerCount = 0;
		}
		stopRequested = false;
		listening = true;
		// The listener owns only local socket IO. It never applies editor
		// mutations; tools/call is handed to the bridge queue and processed from
		// the editor update loop.
		try
		{
			listenerThread = std::thread(&EditorMcpTransport::ListenLoop, this);
		}
		catch (const std::exception& exception)
		{
			// Publishing the descriptor before thread creation makes Start/Stop
			// state observable consistently. Roll every publication back if the OS
			// cannot create the listener thread.
			{
				std::lock_guard lock(socketMutex);
				listenSocket = -1;
				listenPort = 0;
				lastError = std::string("Could not start MCP listener thread: ") + exception.what();
			}
			stopRequested = true;
			listening = false;
			CloseSocket(socketHandle);
			ShutdownSocketRuntime();
			return false;
		}
		return true;
	}

	void EditorMcpTransport::StopListening() noexcept
	{
		std::lock_guard lifecycleLock(lifecycleMutex);
		bool hasClientWorkers = false;
		{
			std::lock_guard lock(clientWorkersMutex);
			hasClientWorkers = !clientWorkers.empty();
		}
		if (!listening && !listenerThread.joinable() && !hasClientWorkers)
		{
			return;
		}
		stopRequested = true;
		SocketHandle socketHandle = kInvalidSocket;
		std::uint16_t port = 0;
		{
			std::lock_guard lock(socketMutex);
			socketHandle = static_cast<SocketHandle>(listenSocket);
			port = listenPort;
			// Remove public admission immediately, but keep the local descriptor
			// open until the listener has relinquished its copied handle. Closing it
			// now could let another subsystem reuse the value before accept().
			listenSocket = -1;
			// Keep the publication mutex through every shutdown so a worker cannot
			// close and release a descriptor for reuse before the interrupt reaches
			// the intended accepted client.
			for (const std::intptr_t activeClient : activeClientSockets)
			{
#if defined(_WIN32)
				shutdown(static_cast<SocketHandle>(activeClient), SD_BOTH);
#else
				shutdown(static_cast<SocketHandle>(activeClient), SHUT_RDWR);
#endif
			}
		}
		// A loopback connection wakes an accept already in progress and is also
		// queued safely if the listener copied the descriptor just before stop.
		WakeListener(port);
		JoinThreadNoThrow(listenerThread);

		// The listener can publish a client between the first snapshot and its
		// final stop check. It is now joined, so a second pass covers that window
		// before waiting for accepted-client workers.
		{
			std::lock_guard lock(socketMutex);
			for (const std::intptr_t activeClient : activeClientSockets)
			{
#if defined(_WIN32)
				shutdown(static_cast<SocketHandle>(activeClient), SD_BOTH);
#else
				shutdown(static_cast<SocketHandle>(activeClient), SHUT_RDWR);
#endif
			}
		}
		// Only the lifecycle owner closes the listening descriptor, and only after
		// the listener can no longer call accept with its local copy.
		CloseSocket(socketHandle);
		JoinAllClientWorkers();
		listening = false;
		std::vector<std::intptr_t> remainingActiveClients;
		{
			std::lock_guard lock(socketMutex);
			listenPort = 0;
			// Normal worker cleanup removes and closes every publication. Swapping
			// any remainder provides no-throw defensive cleanup after all joins.
			remainingActiveClients.swap(activeClientSockets);
			connectedPeerCount = 0;
		}
		for (const std::intptr_t remainingClient : remainingActiveClients)
		{
			CloseSocket(static_cast<SocketHandle>(remainingClient));
		}
		ShutdownSocketRuntime();
	}

	bool EditorMcpTransport::IsListening() const
	{
		return listening;
	}

	std::uint16_t EditorMcpTransport::GetListenPort() const
	{
		std::lock_guard lock(socketMutex);
		return listenPort;
	}

	std::string EditorMcpTransport::GetEndpoint() const
	{
		const std::uint16_t port = GetListenPort();
		return port == 0 ? std::string() : "http://127.0.0.1:" + std::to_string(port) + "/mcp";
	}

	std::string EditorMcpTransport::GetLastError() const
	{
		std::lock_guard lock(socketMutex);
		return lastError;
	}

	void EditorMcpTransport::ReleaseAcceptedClient(std::intptr_t clientHandle)
	{
		const SocketHandle client = static_cast<SocketHandle>(clientHandle);
		bool ownsClose = false;
		{
			std::lock_guard lock(socketMutex);
			const auto found = std::find(activeClientSockets.begin(), activeClientSockets.end(), clientHandle);
			if (found != activeClientSockets.end())
			{
				activeClientSockets.erase(found);
				ownsClose = true;
			}
		}
		if (ownsClose)
		{
			CloseSocket(client);
		}
	}

	void EditorMcpTransport::ReapCompletedClientWorkers() noexcept
	{
		for (;;)
		{
			std::unique_ptr<ClientWorker> completedWorker;
			{
				std::lock_guard lock(clientWorkersMutex);
				const auto found = std::find_if(clientWorkers.begin(), clientWorkers.end(),
					[](const std::unique_ptr<ClientWorker>& worker) {
						return worker->completed.load(std::memory_order_acquire);
					});
				if (found == clientWorkers.end())
				{
					return;
				}
				completedWorker = std::move(*found);
				clientWorkers.erase(found);
			}
			JoinThreadNoThrow(completedWorker->thread);
		}
	}

	void EditorMcpTransport::JoinAllClientWorkers() noexcept
	{
		std::vector<std::unique_ptr<ClientWorker>> workers;
		{
			std::lock_guard lock(clientWorkersMutex);
			workers.swap(clientWorkers);
		}
		for (const std::unique_ptr<ClientWorker>& worker : workers)
		{
			JoinThreadNoThrow(worker->thread);
		}
	}

	void EditorMcpTransport::ListenLoop()
	{
		while (!stopRequested)
		{
			ReapCompletedClientWorkers();
			SocketHandle listener = kInvalidSocket;
			{
				std::lock_guard lock(socketMutex);
				listener = static_cast<SocketHandle>(listenSocket);
			}
			if (listener == kInvalidSocket)
			{
				break;
			}
			const SocketReadiness listenerReadiness = WaitForReadableSocket(listener);
			if (listenerReadiness == SocketReadiness::TimedOut)
			{
				continue;
			}
			if (listenerReadiness == SocketReadiness::Failed)
			{
				if (!stopRequested)
				{
					std::lock_guard lock(socketMutex);
					lastError = LastSocketError();
				}
				break;
			}
			sockaddr_in clientAddress{};
			socklen_t clientLength = sizeof(clientAddress);
			const SocketHandle client = accept(listener, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
			if (client == kInvalidSocket)
			{
				if (!stopRequested)
				{
					std::lock_guard lock(socketMutex);
					lastError = LastSocketError();
				}
				continue;
			}

			bool published = false;
			try
			{
				// Publish before checking stopRequested. StopListening can now
				// interrupt a client that was accepted concurrently; if stop was
				// already requested, this thread observes it and owns the close.
				std::lock_guard lock(socketMutex);
				activeClientSockets.push_back(static_cast<std::intptr_t>(client));
				published = true;
			}
			catch (...)
			{
				CloseSocket(client);
				break;
			}
			if (stopRequested)
			{
				if (published)
				{
					ReleaseAcceptedClient(static_cast<std::intptr_t>(client));
				}
				break;
			}

			try
			{
				bool atCapacity = false;
				{
					std::lock_guard lock(clientWorkersMutex);
					atCapacity = clientWorkers.size() >= kMaxConcurrentClientWorkers;
				}
				if (atCapacity)
				{
					SendAll(client, BuildHttpError(503, "The editor MCP bridge has reached its local client limit."));
					ReleaseAcceptedClient(static_cast<std::intptr_t>(client));
					continue;
				}

				const char* peerText = inet_ntoa(clientAddress.sin_addr);
				std::string peerAddress = peerText != nullptr ? peerText : "127.0.0.1";
				auto worker = std::make_unique<ClientWorker>();
				ClientWorker* workerPointer = worker.get();
				{
					std::lock_guard lock(clientWorkersMutex);
					clientWorkers.push_back(std::move(worker));
				}

				try
				{
					workerPointer->thread = std::thread(
						[this, client, workerPointer, peerAddress = std::move(peerAddress)]() mutable {
							ScopeExitGuard cleanup([this, client, workerPointer]() {
								ReleaseAcceptedClient(static_cast<std::intptr_t>(client));
								workerPointer->completed.store(true, std::memory_order_release);
							});
							HandleAcceptedClient(static_cast<std::intptr_t>(client), std::move(peerAddress), *workerPointer);
						});
				}
				catch (...)
				{
					std::unique_ptr<ClientWorker> failedWorker;
					{
						std::lock_guard lock(clientWorkersMutex);
						const auto found = std::find_if(clientWorkers.begin(), clientWorkers.end(),
							[workerPointer](const std::unique_ptr<ClientWorker>& candidate) {
								return candidate.get() == workerPointer;
							});
						if (found != clientWorkers.end())
						{
							failedWorker = std::move(*found);
							clientWorkers.erase(found);
						}
					}
					ReleaseAcceptedClient(static_cast<std::intptr_t>(client));
					break;
				}
			}
			catch (const std::exception&)
			{
				ReleaseAcceptedClient(static_cast<std::intptr_t>(client));
				break;
			}
		}
		ReapCompletedClientWorkers();
	}

	void EditorMcpTransport::HandleAcceptedClient(std::intptr_t clientHandle, std::string peerAddress, ClientWorker& worker)
	{
		const SocketHandle client = static_cast<SocketHandle>(clientHandle);
		bool peerAccepted = false;
		std::string disconnectReason = "request complete";
		ScopeExitGuard peerCleanup([this, &peerAccepted, &disconnectReason]() {
			if (peerAccepted)
			{
				DropPeer(disconnectReason);
			}
		});
		try
		{
			if (!AcceptPeer(peerAddress, "http-json-rpc"))
			{
				SendAll(client, BuildHttpError(403, "Only enabled local clients may use the editor MCP bridge."));
				return;
			}
			peerAccepted = true;

			std::string request;
			std::array<char, 4096> buffer{};
			std::size_t headerEnd = std::string::npos;
			std::size_t contentLength = 0;
			bool parsedHeaders = false;
			const auto requestDeadline = std::chrono::steady_clock::now() + kHttpRequestDeadline;
			auto idleDeadline = std::chrono::steady_clock::now() + kHttpIdleReadDeadline;
			while (!stopRequested)
			{
				const auto now = std::chrono::steady_clock::now();
				if (now >= requestDeadline || now >= idleDeadline)
				{
					disconnectReason = "HTTP request deadline exceeded";
					SendAll(client, BuildHttpError(408, "HTTP request timed out before completion."));
					return;
				}
				const SocketReadiness readiness = WaitForReadableSocket(client);
				if (readiness == SocketReadiness::TimedOut)
				{
					continue;
				}
				if (readiness == SocketReadiness::Failed)
				{
					break;
				}
				const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
				if (received <= 0)
				{
					break;
				}
				request.append(buffer.data(), static_cast<std::size_t>(received));
				idleDeadline = std::chrono::steady_clock::now() + kHttpIdleReadDeadline;
				headerEnd = request.find("\r\n\r\n");
				if (headerEnd == std::string::npos)
				{
					if (request.size() > kMaxHttpHeaderBytes)
					{
						disconnectReason = "HTTP headers exceeded limit";
						SendAll(client, BuildHttpError(431, "HTTP request headers exceed the local bridge limit."));
						return;
					}
					continue;
				}
				if (headerEnd > kMaxHttpHeaderBytes)
				{
					disconnectReason = "HTTP headers exceeded limit";
					SendAll(client, BuildHttpError(431, "HTTP request headers exceed the local bridge limit."));
					return;
				}
				if (!parsedHeaders)
				{
					const ContentLengthParseResult parsedLength = ParseContentLength(std::string_view(request).substr(0, headerEnd));
					if (parsedLength.status == ContentLengthStatus::Missing)
					{
						disconnectReason = "HTTP Content-Length missing";
						SendAll(client, BuildHttpError(411, "HTTP Content-Length is required."));
						return;
					}
					if (parsedLength.status == ContentLengthStatus::Malformed)
					{
						disconnectReason = "HTTP Content-Length malformed";
						SendAll(client, BuildHttpError(400, "HTTP Content-Length is malformed."));
						return;
					}
					contentLength = parsedLength.value;
					if (contentLength > kMaxHttpBodyBytes)
					{
						disconnectReason = "HTTP body exceeded limit";
						SendAll(client, BuildHttpError(413, "HTTP request body exceeds the local bridge limit."));
						return;
					}
					parsedHeaders = true;
				}
				if (request.size() >= headerEnd + 4 + contentLength)
				{
					break;
				}
			}

			if (stopRequested)
			{
				disconnectReason = "listener stopped";
				return;
			}
			if (headerEnd == std::string::npos)
			{
				disconnectReason = "malformed HTTP request";
				SendAll(client, BuildHttpError(400, "Malformed HTTP request."));
				return;
			}

			const std::string body = request.substr(headerEnd + 4, contentLength);
			const nlohmann::json message = nlohmann::json::parse(body);
			const bool cancellationNotification = message.is_object()
				&& !message.contains("id")
				&& message.contains("method")
				&& message["method"].is_string()
				&& message["method"].get<std::string>() == "notifications/cancelled";
			if (!cancellationNotification)
			{
				// The ninth worker is only a temporary parsing reserve. Ordinary
				// requests cannot consume it while eight editor-dispatch calls wait.
				worker.usesControlReserve = false;
				std::size_t ordinaryWorkerCount = 0;
				{
					std::lock_guard lock(clientWorkersMutex);
					ordinaryWorkerCount = static_cast<std::size_t>(std::count_if(
						clientWorkers.begin(), clientWorkers.end(), [](const std::unique_ptr<ClientWorker>& candidate) {
							return !candidate->usesControlReserve.load(std::memory_order_acquire);
						}));
				}
				if (ordinaryWorkerCount > kMaxConcurrentOrdinaryClientWorkers)
				{
					disconnectReason = "ordinary client limit reached";
					SendAll(client, BuildHttpError(503, "The editor MCP bridge has reached its local tool-call limit."));
					return;
				}
			}
			const nlohmann::json response = HandleMessage(message);
			SendAll(client, cancellationNotification ? BuildHttpAccepted() : BuildHttpResponse(response));
		}
		catch (const nlohmann::json::parse_error&)
		{
			disconnectReason = "malformed JSON request";
			SendAll(client, BuildHttpError(400, "Malformed JSON request."));
		}
		catch (const std::exception&)
		{
			// Keep malformed arguments or provider failures contained to this
			// request instead of terminating a client worker or the editor.
			disconnectReason = "request failed";
			SendAll(client, BuildHttpError(500, "MCP request failed."));
		}
		catch (...)
		{
			disconnectReason = "request failed";
			SendAll(client, BuildHttpError(500, "MCP request failed."));
		}
	}

	nlohmann::json EditorMcpTransport::HandleMessage(const nlohmann::json& message)
	{
		const nlohmann::json id = message.contains("id") ? message["id"] : nullptr;
		if (!message.is_object() || !message.contains("method") || !message["method"].is_string())
		{
			return BuildJsonRpcError(id, -32600, "Invalid MCP JSON message.");
		}

		const std::string method = message["method"].get<std::string>();
		const nlohmann::json params = message.value("params", nlohmann::json::object());
		if (method == "initialize")
		{
			return BuildJsonRpcResult(id, {{"protocolVersion", "2025-03-26"}, {"serverInfo", {{"name", "TerraEngine Editor"}, {"version", "0.1"}}}, {"capabilities", {{"tools", true}, {"resources", true}}}});
		}
		if (method == "tools/list")
		{
			return BuildJsonRpcResult(id, {{"tools", bridge.BuildCapabilityListJson()["capabilities"]}});
		}
		if (method == "resources/list")
		{
			return BuildJsonRpcResult(id, {{"resources", bridge.ListResources()}});
		}
		if (method == "resources/read")
		{
			if (!params.contains("uri") || !params["uri"].is_string())
			{
				return BuildJsonRpcError(id, -32602, "resources/read requires params.uri.");
			}
			return BuildJsonRpcResult(id, {{"contents", nlohmann::json::array({{{"uri", params["uri"]}, {"mimeType", "application/json"}, {"text", bridge.ReadResource(params["uri"].get<std::string>()).dump()}}})}});
		}
		if (method == "tools/call")
		{
			if (!params.contains("name") || !params["name"].is_string())
			{
				return BuildJsonRpcError(id, -32602, "tools/call requires params.name.");
			}
			const nlohmann::json arguments = params.value("arguments", nlohmann::json::object());
			nlohmann::json toolResult = bridge.SubmitToolCall(params["name"].get<std::string>(), arguments, id, &stopRequested);
			nlohmann::json content = nlohmann::json::array();
			nlohmann::json textResult = toolResult;
			textResult.erase("mcpContent");
			content.push_back({{"type", "text"}, {"text", textResult.dump()}});
			if (toolResult.contains("mcpContent") && toolResult["mcpContent"].is_array())
			{
				for (const nlohmann::json& item : toolResult["mcpContent"])
				{
					content.push_back(item);
				}
			}
			return BuildJsonRpcResult(id, {{"content", std::move(content)}});
		}
		if (method == "notifications/cancelled")
		{
			const nlohmann::json requestId = params.is_object() && params.contains("requestId")
				? params["requestId"]
				: nlohmann::json(nullptr);
			bool canceled = bridge.CancelPendingToolCall(requestId);
			// Retain compatibility for the older assistant queue, whose bridge
			// request IDs are strings rather than transport JSON-RPC identities.
			if (!canceled && requestId.is_string())
			{
				canceled = bridge.CancelQueuedRequest(requestId.get<std::string>());
			}
			return BuildJsonRpcResult(id, {{"canceled", canceled}, {"requestId", requestId}});
		}
		return BuildJsonRpcError(id, -32601, "Unsupported MCP method.");
	}

	nlohmann::json EditorMcpTransport::BuildJsonRpcResult(const nlohmann::json& id, nlohmann::json result) const
	{
		return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
	}

	nlohmann::json EditorMcpTransport::BuildJsonRpcError(const nlohmann::json& id, int code, std::string_view message) const
	{
		return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
	}
}
