// EditorMcpTransport.h - Local-only MCP message ingress/egress.
//
// This class deliberately does not open a general web server or mutate editor
// state. It validates loopback peers, parses JSON messages, and routes MCP
// methods to EditorMcpBridge so editor updates remain serialized elsewhere.

#pragma once

#include "EditorMcpBridge.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Editor::Automation
{
	class EditorMcpTransport
	{
	public:
		explicit EditorMcpTransport(EditorMcpBridge& bridge);
		~EditorMcpTransport() noexcept;

		static bool IsLocalAddress(std::string_view address);
		bool AcceptPeer(std::string_view address, std::string_view clientName);
		void DropPeer(std::string_view reason);
		bool StartListening(std::uint16_t preferredPort = 0);
		void StopListening() noexcept;
		bool IsListening() const;
		std::uint16_t GetListenPort() const;
		// Socket ownership is observable independently of MCP session admission.
		std::size_t GetActiveClientCount() const;
		std::string GetEndpoint() const;
		std::string GetLastError() const;
		nlohmann::json HandleMessage(const nlohmann::json& message);

	private:
		struct ClientWorker
		{
			std::thread thread;
			std::atomic<bool> completed = false;
			// A newly accepted request temporarily occupies the control reserve until
			// its complete JSON-RPC method is known. Only cancellation notifications
			// may keep that reserve once parsing succeeds.
			std::atomic<bool> usesControlReserve = true;
		};

		void ListenLoop();
		void HandleAcceptedClient(std::intptr_t clientHandle, std::string peerAddress, std::uint16_t acceptedPort, ClientWorker& worker);
		void ReleaseAcceptedClient(std::intptr_t clientHandle);
		void ReapCompletedClientWorkers() noexcept;
		void JoinAllClientWorkers() noexcept;
		nlohmann::json BuildJsonRpcResult(const nlohmann::json& id, nlohmann::json result) const;
		nlohmann::json BuildJsonRpcError(const nlohmann::json& id, int code, std::string_view message) const;

		EditorMcpBridge& bridge;
		std::thread listenerThread;
		std::atomic<bool> stopRequested = false;
		std::atomic<bool> listening = false;
		// Start/stop own the listener thread and WinSock reference count. Serialize
		// those lifecycle operations so two UI/shutdown callers can never join the
		// same std::thread or clean up the socket runtime twice.
		mutable std::mutex lifecycleMutex;
		mutable std::mutex socketMutex;
		mutable std::mutex clientWorkersMutex;
		std::uint16_t listenPort = 0;
		std::string lastError;
		std::intptr_t listenSocket = -1;
		// Accepted descriptors remain worker-owned until that worker exits. Stop
		// may shutdown them, but never closes them while a worker can still use one.
		std::vector<std::intptr_t> activeClientSockets;
		std::vector<std::unique_ptr<ClientWorker>> clientWorkers;
		std::size_t connectedPeerCount = 0;
	};
}
