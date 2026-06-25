#include "../TestHarness.h"
#include "EditorMcpTransport.h"

#include <array>
#include <sstream>
#include <string>

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

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorMcpBridge;
	using TerraEngine::Editor::Automation::EditorMcpTransport;

	namespace
	{
#if defined(_WIN32)
		using TestSocket = SOCKET;
		constexpr TestSocket kInvalidTestSocket = INVALID_SOCKET;

		void CloseTestSocket(TestSocket socket)
		{
			if (socket != kInvalidTestSocket)
			{
				closesocket(socket);
			}
		}
#else
		using TestSocket = int;
		constexpr TestSocket kInvalidTestSocket = -1;

		void CloseTestSocket(TestSocket socket)
		{
			if (socket != kInvalidTestSocket)
			{
				close(socket);
			}
		}
#endif

		std::string SendLoopbackHttpPost(std::uint16_t port, const std::string& body)
		{
#if defined(_WIN32)
			WSADATA data{};
			WSAStartup(MAKEWORD(2, 2), &data);
#endif
			TestSocket socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (socketHandle == kInvalidTestSocket)
			{
				return {};
			}
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
			{
				CloseTestSocket(socketHandle);
				return {};
			}

			std::ostringstream request;
			request << "POST /mcp HTTP/1.1\r\n";
			request << "Host: 127.0.0.1\r\n";
			request << "Content-Type: application/json\r\n";
			request << "Content-Length: " << body.size() << "\r\n";
			request << "Connection: close\r\n\r\n";
			request << body;
			const std::string payload = request.str();
			send(socketHandle, payload.data(), static_cast<int>(payload.size()), 0);

			std::string response;
			std::array<char, 4096> buffer{};
			for (;;)
			{
				const int received = recv(socketHandle, buffer.data(), static_cast<int>(buffer.size()), 0);
				if (received <= 0)
				{
					break;
				}
				response.append(buffer.data(), static_cast<std::size_t>(received));
			}
			CloseTestSocket(socketHandle);
#if defined(_WIN32)
			WSACleanup();
#endif
			return response;
		}
	}

	void RegisterEditorMcpTransportTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Transport.LocalAddressChecks", [](TestContext& context) {
			context.Expect(EditorMcpTransport::IsLocalAddress("127.0.0.1"), "IPv4 loopback should be accepted.");
			context.Expect(EditorMcpTransport::IsLocalAddress("localhost"), "localhost should be accepted.");
			context.Expect(EditorMcpTransport::IsLocalAddress("::1"), "IPv6 loopback should be accepted.");
			context.Expect(!EditorMcpTransport::IsLocalAddress("10.0.0.25"), "Non-local address should be rejected.");
		});

		registry.Add("Editor.MCP.Transport.AcceptRejectsByBridgeStateAndPeer", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			EditorMcpTransport transport(bridge);
			context.Expect(!transport.AcceptPeer("127.0.0.1", "test"), "Disabled bridge should reject clients.");
			bridge.SetEnabled(true);
			context.Expect(!transport.AcceptPeer("192.168.1.2", "test"), "Non-local clients should be rejected.");
			context.Expect(transport.AcceptPeer("127.0.0.1", "test"), "Local clients should be accepted when enabled.");
			transport.DropPeer("test complete");
		});

		registry.Add("Editor.MCP.Transport.RoutesInitializeToolsAndResources", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			EditorMcpTransport transport(bridge);
			context.Expect(transport.AcceptPeer("127.0.0.1", "test"), "Local client should connect.");
			const nlohmann::json initialize = transport.HandleMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
			context.Expect(initialize["result"].contains("serverInfo"), "Initialize should return server info.");
			const nlohmann::json tools = transport.HandleMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
			context.Expect(!tools["result"]["tools"].empty(), "tools/list should return tool metadata.");
			const nlohmann::json resources = transport.HandleMessage({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "resources/list"}});
			context.Expect(!resources["result"]["resources"].empty(), "resources/list should return resource metadata.");
			const nlohmann::json status = transport.HandleMessage({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "resources/read"}, {"params", {{"uri", "terraengine://bridge/status"}}}});
			context.Expect(status["result"].contains("contents"), "resources/read should return JSON text content.");
		});

		registry.Add("Editor.MCP.Transport.CancellationNotificationCancelsQueuedRequest", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.EnqueueRequest("object.create", {{"name", "Queued"}});
			const auto queued = bridge.GetQueuedRequests();
			context.ExpectEqual(static_cast<std::size_t>(1), queued.size(), "Fixture should queue one request before cancellation.");
			EditorMcpTransport transport(bridge);
			const nlohmann::json canceled = transport.HandleMessage({{"jsonrpc", "2.0"}, {"id", 7}, {"method", "notifications/cancelled"}, {"params", {{"requestId", queued.front().requestId}}}});
			context.Expect(canceled["result"].value("canceled", false), "Cancellation notification should cancel the queued request id.");
			bridge.Update(0.0f);
			context.Expect(bridge.GetQueuedRequests().empty(), "Canceled queued request should drain without executing.");
		});

		registry.Add("Editor.MCP.Transport.HttpListenerParsesContentLengthWithWhitespace", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			const std::uint16_t port = bridge.GetListenPort();
			context.Expect(port != 0, "Enabled bridge should expose a loopback listener port.");
			const std::string response = SendLoopbackHttpPost(port, R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
			context.Expect(response.find("200 OK") != std::string::npos, "HTTP listener should return OK for initialize.");
			context.Expect(response.find("serverInfo") != std::string::npos, "HTTP listener should route initialize JSON-RPC.");
			context.Expect(response.find("Malformed JSON request") == std::string::npos, "HTTP listener should not drop the body when Content-Length has normal whitespace.");
			bridge.Shutdown();
		});
	}
}
