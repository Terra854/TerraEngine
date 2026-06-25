// EditorMcpTransport.h - Local-only MCP message ingress/egress.
//
// This class deliberately does not open a general web server or mutate editor
// state. It validates loopback peers, parses JSON messages, and routes MCP
// methods to EditorMcpBridge so editor updates remain serialized elsewhere.

#pragma once

#include "EditorMcpBridge.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <string>
#include <string_view>

namespace TerraEngine::Editor::Automation
{
	class EditorMcpTransport
	{
	public:
		explicit EditorMcpTransport(EditorMcpBridge& bridge);

		static bool IsLocalAddress(std::string_view address);
		bool AcceptPeer(std::string_view address, std::string_view clientName);
		void DropPeer(std::string_view reason);
		bool StartListening(std::uint16_t preferredPort = 0);
		void StopListening();
		bool IsListening() const;
		std::uint16_t GetListenPort() const;
		std::string GetEndpoint() const;
		std::string GetLastError() const;
		nlohmann::json HandleMessage(const nlohmann::json& message);

	private:
		void ListenLoop();
		nlohmann::json BuildJsonRpcResult(const nlohmann::json& id, nlohmann::json result) const;
		nlohmann::json BuildJsonRpcError(const nlohmann::json& id, int code, std::string_view message) const;

		EditorMcpBridge& bridge;
		std::thread listenerThread;
		std::atomic<bool> stopRequested = false;
		std::atomic<bool> listening = false;
		std::uint16_t listenPort = 0;
		std::string lastError;
		std::intptr_t listenSocket = -1;
		bool connected = false;
	};
}
