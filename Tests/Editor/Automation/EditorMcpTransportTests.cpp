#include "../TestHarness.h"
#include "EditorMcpTransport.h"
#include "Engine/Renderer/RenderSystem.h"

#include <array>
#include <atomic>
#include <chrono>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <string_view>

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

		constexpr std::array<std::string_view, 30> kLegacyToolNames = {
			"activity.list",
			"scene.open", "scene.save", "scene.list_objects",
			"object.create", "object.delete", "object.select", "object.set_metadata", "object.set_parent",
			"component.add", "component.remove", "component.get", "component.set_property",
			"asset.list", "asset.import", "asset.virtualGeometry.settings.get", "asset.virtualGeometry.settings.set",
			"asset.virtualGeometry.settings.reset", "asset.virtualGeometry.rebuild", "asset.virtualGeometry.inspect", "asset.prepare_delete",
			"export.validate", "export.start", "export.cancel",
			"game_view.capture", "camera.capture_object", "camera.capture_custom",
			"renderer.virtualGeometry.stats", "renderer.recovery.safe_save_project", "diagnostics.get_summary"
		};

		constexpr std::array<std::string_view, 8> kLegacyResourceUris = {
			"terraengine://bridge/status",
			"terraengine://bridge/capabilities",
			"terraengine://project/state",
			"terraengine://scene/summary",
			"terraengine://diagnostics/summary",
			"terraengine://renderer/recovery",
			"terraengine://capture/availability",
			"terraengine://activity/recent"
		};
	}

	void RegisterEditorMcpTransportTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Transport.LocalAddressChecks", [](TestContext& context) {
			context.Expect(EditorMcpTransport::IsLocalAddress("127.0.0.1"), "IPv4 loopback should be accepted.");
			context.Expect(EditorMcpTransport::IsLocalAddress("localhost"), "localhost should be accepted.");
			context.Expect(EditorMcpTransport::IsLocalAddress("::1"), "IPv6 loopback should be accepted.");
			context.Expect(!EditorMcpTransport::IsLocalAddress("10.0.0.25"), "Non-local address should be rejected.");
		});

		registry.Add("Editor.MCP.Transport.ConfiguredPortBindsExactly", [](TestContext& context) {
			// Ask the OS for an available test port, release it, then prove the
			// bridge binds that exact configured value rather than another port.
			EditorMcpBridge probeBridge;
			probeBridge.Initialize(nullptr);
			EditorMcpTransport probeTransport(probeBridge);
			context.Expect(probeTransport.StartListening(0), "Test port probe should start an ephemeral listener.");
			const std::uint16_t selectedPort = probeTransport.GetListenPort();
			probeTransport.StopListening();

			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			context.Expect(bridge.SetListenPort(selectedPort), "Bridge should accept the available configured port.");
			bridge.SetEnabled(true);
			context.Expect(bridge.IsEnabled(), "Bridge should enable on the configured port.");
			context.ExpectEqual(selectedPort, bridge.GetConfiguredListenPort(), "Bridge should retain the configured port.");
			context.ExpectEqual(selectedPort, bridge.GetListenPort(), "Live listener should bind the exact configured port.");
			bridge.Shutdown();
		});

		registry.Add("Editor.MCP.Transport.OccupiedConfiguredPortDoesNotRandomize", [](TestContext& context) {
			EditorMcpBridge blockerBridge;
			blockerBridge.Initialize(nullptr);
			EditorMcpTransport blocker(blockerBridge);
			context.Expect(blocker.StartListening(0), "Port blocker should acquire an available loopback port.");
			const std::uint16_t occupiedPort = blocker.GetListenPort();

			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			context.Expect(bridge.SetListenPort(occupiedPort), "Bridge should save the requested occupied port before listening.");
			bridge.SetEnabled(true);
			context.Expect(!bridge.IsEnabled(), "Bridge should remain disabled when its configured port is occupied.");
			context.ExpectEqual(occupiedPort, bridge.GetConfiguredListenPort(), "Bind failure should not replace the configured port.");
			context.ExpectEqual(static_cast<std::uint16_t>(0), bridge.GetListenPort(), "Bind failure should not fall back to an ephemeral listener.");
			context.Expect(bridge.GetTransportLastError().find(std::to_string(occupiedPort)) != std::string::npos, "Bind error should identify the occupied configured port.");

			bridge.Shutdown();
			blocker.StopListening();
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

		registry.Add("Editor.MCP.Transport.RoutesEveryLegacyResource", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			EditorMcpTransport transport(bridge);
			context.Expect(transport.AcceptPeer("127.0.0.1", "legacy-resource-test"), "Local client should connect before reading legacy resources.");

			for (std::size_t index = 0; index < kLegacyResourceUris.size(); ++index)
			{
				const std::string uri(kLegacyResourceUris[index]);
				const nlohmann::json response = transport.HandleMessage({
					{"jsonrpc", "2.0"},
					{"id", 100 + index},
					{"method", "resources/read"},
					{"params", {{"uri", uri}}}
				});
				context.Expect(response.contains("result"), std::string("Legacy resource should keep its JSON-RPC route: ") + uri);
				const nlohmann::json contents = response["result"].value("contents", nlohmann::json::array());
				context.ExpectEqual(static_cast<std::size_t>(1), contents.size(), std::string("Legacy resource should keep one JSON text content item: ") + uri);
				context.ExpectEqual(uri, contents[0].value("uri", std::string()), "Resource content should echo the requested legacy URI.");
				context.ExpectEqual(std::string("application/json"), contents[0].value("mimeType", std::string()), "Legacy resources should remain JSON text.");
				const nlohmann::json payload = nlohmann::json::parse(contents[0].value("text", std::string("null")), nullptr, false);
				context.Expect(!payload.is_discarded(), std::string("Legacy resource text should remain valid JSON: ") + uri);
			}
			bridge.Shutdown();
		});

		registry.Add("Editor.MCP.Transport.RoutesEveryLegacyToolWithTextEnvelope", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			EditorMcpTransport transport(bridge);
			context.Expect(transport.AcceptPeer("127.0.0.1", "legacy-tool-test"), "Local client should connect before calling legacy tools.");

			std::jthread pump([&](std::stop_token stopToken) {
				while (!stopToken.stop_requested())
				{
					bridge.Update(0.0f);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});

			for (std::size_t index = 0; index < kLegacyToolNames.size(); ++index)
			{
				const std::string name(kLegacyToolNames[index]);
				nlohmann::json arguments = nlohmann::json::object();
				// Invalid capture dimensions keep this compatibility test independent
				// of a live renderer while still exercising the registered handler.
				if (name == "game_view.capture")
				{
					arguments["width"] = 0;
				}
				const nlohmann::json response = transport.HandleMessage({
					{"jsonrpc", "2.0"},
					{"id", 200 + index},
					{"method", "tools/call"},
					{"params", {{"name", name}, {"arguments", arguments}}}
				});
				context.Expect(response.contains("result"), std::string("Legacy tool should keep its JSON-RPC route: ") + name);
				const nlohmann::json content = response["result"].value("content", nlohmann::json::array());
				context.Expect(!content.empty(), std::string("Legacy tool should return MCP content: ") + name);
				context.ExpectEqual(std::string("text"), content[0].value("type", std::string()), "Legacy tool responses should retain the JSON text envelope first.");
				const nlohmann::json envelope = nlohmann::json::parse(content[0].value("text", std::string("null")), nullptr, false);
				context.Expect(!envelope.is_discarded(), std::string("Legacy tool text should remain valid JSON: ") + name);
				const nlohmann::json error = envelope.value("error", nlohmann::json());
				const std::string errorCategory = error.is_object() ? error.value("category", "") : std::string();
				context.Expect(errorCategory != "unsupported_capability", std::string("Legacy tool should still resolve to a handler: ") + name);
			}

			pump.request_stop();
			pump.join();
			bridge.Shutdown();
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

		registry.Add("Editor.MCP.Transport.ToolsCallCanReturnTextAndImageContent", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			EditorMcpTransport transport(bridge);
			context.Expect(transport.AcceptPeer("127.0.0.1", "test"), "Local client should connect before calling tools.");

			std::atomic<bool> stopPump = false;
			std::thread pump([&]() {
				while (!stopPump.load())
				{
					bridge.Update(0.0f);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});

			const nlohmann::json response = transport.HandleMessage({
				{"jsonrpc", "2.0"},
				{"id", 8},
				{"method", "tools/call"},
				{"params", {
					{"name", "camera.capture_custom"},
					{"arguments", {
						{"position", nlohmann::json::array({0.0f, 1.0f, 5.0f})},
						{"target", nlohmann::json::array({0.0f, 0.0f, 0.0f})},
						{"width", 32u},
						{"height", 32u}
					}}
				}}
			});

			stopPump = true;
			pump.join();
			bridge.Shutdown();

			const nlohmann::json content = response["result"]["content"];
			context.Expect(content.size() >= 2u, "tools/call should return text plus image content for captures.");
			context.ExpectEqual(std::string("text"), content[0].value("type", std::string()), "First capture content item should remain the JSON text envelope.");
			context.ExpectEqual(std::string("image"), content[1].value("type", std::string()), "Second capture content item should be an MCP image.");
			context.ExpectEqual(std::string("image/png"), content[1].value("mimeType", std::string()), "MCP image content should advertise PNG.");
			context.Expect(!content[1].value("data", std::string()).empty(), "MCP image content should carry base64 data.");
		});

		registry.Add("Editor.MCP.Transport.GameViewCaptureReturnsImageContent", [](TestContext& context) {
			RenderSystem::Instance().Clear();
			RenderSystem::Instance().SetGameViewTargetSize(glm::uvec2(32u, 32u));
			RenderSystem::Instance().SetGameViewCamera(
				glm::mat4(1.0f),
				glm::perspectiveZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f),
				glm::vec3(0.0f, 0.0f, 5.0f));
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			EditorMcpTransport transport(bridge);
			context.Expect(transport.AcceptPeer("127.0.0.1", "test"), "Local client should connect before Game View capture.");

			std::atomic<bool> stopPump = false;
			std::thread pump([&]() {
				while (!stopPump.load())
				{
					bridge.Update(0.0f);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});

			const nlohmann::json response = transport.HandleMessage({
				{"jsonrpc", "2.0"},
				{"id", 9},
				{"method", "tools/call"},
				{"params", {
					{"name", "game_view.capture"},
					{"arguments", {{"width", 32u}, {"height", 32u}}}
				}}
			});

			stopPump = true;
			pump.join();
			bridge.Shutdown();
			RenderSystem::Instance().Clear();

			const nlohmann::json content = response["result"]["content"];
			context.Expect(content.size() >= 2u, "Game View capture should return text plus image content.");
			context.ExpectEqual(std::string("image"), content[1].value("type", std::string()), "Game View capture should include an MCP image.");
			context.ExpectEqual(std::string("image/png"), content[1].value("mimeType", std::string()), "Game View capture image should be PNG.");
			context.Expect(!content[1].value("data", std::string()).empty(), "Game View capture image should include base64 data.");
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
