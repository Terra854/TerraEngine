// EditorMcpTransport.cpp - Minimal local JSON-RPC/MCP method router.
//
// This is intentionally transport-shaped rather than server-shaped. It handles
// loopback checks and MCP message names, then delegates all state and editor
// behavior to EditorMcpBridge.

#include "EditorMcpTransport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <arpa/inet.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <unistd.h>
#endif

namespace TerraEngine::Editor::Automation
{
	namespace
	{
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

		std::size_t ParseContentLength(const std::string& headers)
		{
			std::string lower = headers;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			const std::string key = "content-length:";
			const std::size_t keyPos = lower.find(key);
			if (keyPos == std::string::npos)
			{
				return 0;
			}
			const std::size_t valueStart = keyPos + key.size();
			const std::size_t valueEnd = lower.find("\r\n", valueStart);
			const std::string value = lower.substr(valueStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
			std::size_t length = 0;
			const char* begin = value.data();
			const char* end = value.data() + value.size();
			while (begin != end && (*begin == ' ' || *begin == '\t'))
			{
				++begin;
			}
			std::from_chars(begin, end, length);
			return length;
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
	}

	EditorMcpTransport::EditorMcpTransport(EditorMcpBridge& bridge)
		: bridge(bridge)
	{
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
		if (!bridge.IsEnabled())
		{
			bridge.RecordRejectedPeer(std::string(address), "bridge disabled");
			return false;
		}
		if (!IsLocalAddress(address))
		{
			bridge.RecordRejectedPeer(std::string(address), "non-local peer");
			return false;
		}
		connected = true;
		bridge.OnLocalClientConnected(std::string(clientName), std::string(address));
		return true;
	}

	void EditorMcpTransport::DropPeer(std::string_view reason)
	{
		if (connected)
		{
			connected = false;
			bridge.OnLocalClientDisconnected(std::string(reason));
		}
	}

	bool EditorMcpTransport::StartListening(std::uint16_t preferredPort)
	{
		if (listening)
		{
			return true;
		}

		lastError.clear();
		if (!EnsureSocketRuntime(lastError))
		{
			return false;
		}

		SocketHandle socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socketHandle == kInvalidSocket)
		{
			lastError = LastSocketError();
			ShutdownSocketRuntime();
			return false;
		}

		int reuse = 1;
		setsockopt(socketHandle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(preferredPort);
		if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			lastError = LastSocketError();
			CloseSocket(socketHandle);
			ShutdownSocketRuntime();
			return false;
		}

		sockaddr_in boundAddress{};
		socklen_t boundLength = sizeof(boundAddress);
		if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&boundAddress), &boundLength) == 0)
		{
			listenPort = ntohs(boundAddress.sin_port);
		}

		if (listen(socketHandle, 8) != 0)
		{
			lastError = LastSocketError();
			CloseSocket(socketHandle);
			ShutdownSocketRuntime();
			return false;
		}

		listenSocket = static_cast<std::intptr_t>(socketHandle);
		stopRequested = false;
		listening = true;
		// The listener owns only local socket IO. It never applies editor
		// mutations; tools/call is handed to the bridge queue and processed from
		// the editor update loop.
		listenerThread = std::thread(&EditorMcpTransport::ListenLoop, this);
		return true;
	}

	void EditorMcpTransport::StopListening()
	{
		if (!listening && !listenerThread.joinable())
		{
			return;
		}
		stopRequested = true;
		WakeListener(listenPort);
		const SocketHandle socketHandle = static_cast<SocketHandle>(listenSocket);
		if (socketHandle != kInvalidSocket)
		{
#if defined(_WIN32)
			shutdown(socketHandle, SD_BOTH);
#else
			shutdown(socketHandle, SHUT_RDWR);
#endif
		}
		CloseSocket(socketHandle);
		listenSocket = -1;
		if (listenerThread.joinable())
		{
			listenerThread.join();
		}
		listening = false;
		listenPort = 0;
		ShutdownSocketRuntime();
	}

	bool EditorMcpTransport::IsListening() const
	{
		return listening;
	}

	std::uint16_t EditorMcpTransport::GetListenPort() const
	{
		return listenPort;
	}

	std::string EditorMcpTransport::GetEndpoint() const
	{
		return listenPort == 0 ? std::string() : "http://127.0.0.1:" + std::to_string(listenPort) + "/mcp";
	}

	std::string EditorMcpTransport::GetLastError() const
	{
		return lastError;
	}

	void EditorMcpTransport::ListenLoop()
	{
		while (!stopRequested)
		{
			sockaddr_in clientAddress{};
			socklen_t clientLength = sizeof(clientAddress);
			const SocketHandle client = accept(static_cast<SocketHandle>(listenSocket), reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
			if (client == kInvalidSocket)
			{
				if (!stopRequested)
				{
					lastError = LastSocketError();
				}
				continue;
			}

			const std::string peerAddress = inet_ntoa(clientAddress.sin_addr);
			if (!AcceptPeer(peerAddress, "http-json-rpc"))
			{
				SendAll(client, BuildHttpError(403, "Only enabled local clients may use the editor MCP bridge."));
				CloseSocket(client);
				continue;
			}

			std::string request;
			std::array<char, 4096> buffer{};
			std::size_t headerEnd = std::string::npos;
			std::size_t contentLength = 0;
			while (!stopRequested)
			{
				const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
				if (received <= 0)
				{
					break;
				}
				request.append(buffer.data(), static_cast<std::size_t>(received));
				headerEnd = request.find("\r\n\r\n");
				if (headerEnd != std::string::npos)
				{
					contentLength = ParseContentLength(request.substr(0, headerEnd));
					if (request.size() >= headerEnd + 4 + contentLength)
					{
						break;
					}
				}
			}

			if (headerEnd == std::string::npos)
			{
				SendAll(client, BuildHttpError(400, "Malformed HTTP request."));
				DropPeer("malformed HTTP request");
				CloseSocket(client);
				continue;
			}

			const std::string body = request.substr(headerEnd + 4, contentLength);
			try
			{
				const nlohmann::json message = nlohmann::json::parse(body);
				SendAll(client, BuildHttpResponse(HandleMessage(message)));
			}
			catch (const nlohmann::json::parse_error&)
			{
				SendAll(client, BuildHttpError(400, "Malformed JSON request."));
			}
			DropPeer("request complete");
			CloseSocket(client);
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
			nlohmann::json toolResult = bridge.SubmitToolCall(params["name"].get<std::string>(), arguments);
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
			const std::string requestId = params.value("requestId", "");
			return BuildJsonRpcResult(id, {{"canceled", !requestId.empty() && bridge.CancelQueuedRequest(requestId)}, {"requestId", requestId}});
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
