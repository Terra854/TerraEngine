#include "../TestHarness.h"
#include "EditorMcpTransport.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Renderer/RenderSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

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
	#include <sys/time.h>
	#include <unistd.h>
#endif

namespace TerraEngine::EditorTests
{
	static nlohmann::json CanonicalPosition(double x, double y, double z)
	{
		return {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", {x, y, z}}};
	}
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

		bool StartTestSocketRuntime()
		{
#if defined(_WIN32)
			WSADATA data{};
			return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
			return true;
#endif
		}

		void StopTestSocketRuntime()
		{
#if defined(_WIN32)
			WSACleanup();
#endif
		}

		class ScopedTestSocketRuntime
		{
		public:
			ScopedTestSocketRuntime()
				: started(StartTestSocketRuntime())
			{
			}

			~ScopedTestSocketRuntime()
			{
				if (started)
				{
					StopTestSocketRuntime();
				}
			}

			bool IsStarted() const
			{
				return started;
			}

		private:
			bool started = false;
		};

		class ScopedTestSocket
		{
		public:
			explicit ScopedTestSocket(TestSocket socketValue)
				: socketHandle(socketValue)
			{
			}

			~ScopedTestSocket()
			{
				CloseTestSocket(socketHandle);
			}

			ScopedTestSocket(const ScopedTestSocket&) = delete;
			ScopedTestSocket& operator=(const ScopedTestSocket&) = delete;

			TestSocket Get() const
			{
				return socketHandle;
			}

			bool IsValid() const
			{
				return socketHandle != kInvalidTestSocket;
			}

		private:
			TestSocket socketHandle = kInvalidTestSocket;
		};

		void SetTestSocketReceiveTimeout(TestSocket socket, std::chrono::milliseconds timeout)
		{
#if defined(_WIN32)
			const DWORD timeoutMilliseconds = static_cast<DWORD>(timeout.count());
			setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
				reinterpret_cast<const char*>(&timeoutMilliseconds), sizeof(timeoutMilliseconds));
#else
			timeval nativeTimeout{};
			nativeTimeout.tv_sec = static_cast<long>(timeout.count() / 1000);
			nativeTimeout.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
			setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &nativeTimeout, sizeof(nativeTimeout));
#endif
		}

		bool LastTestSocketErrorWasTimeout()
		{
#if defined(_WIN32)
			const int error = WSAGetLastError();
			return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
		}

		bool LastTestSocketErrorWasClosure()
		{
#if defined(_WIN32)
			const int error = WSAGetLastError();
			return error == WSAECONNRESET
				|| error == WSAECONNABORTED
				|| error == WSAENOTCONN
				|| error == WSAESHUTDOWN;
#else
			return errno == ECONNRESET || errno == ECONNABORTED || errno == ENOTCONN;
#endif
		}

		TestSocket OpenLoopbackConnection(std::uint16_t port)
		{
			TestSocket socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (socketHandle == kInvalidTestSocket)
			{
				return kInvalidTestSocket;
			}
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
			{
				CloseTestSocket(socketHandle);
				return kInvalidTestSocket;
			}
			return socketHandle;
		}

		std::string SendLoopbackHttpPost(
			std::uint16_t port,
			const std::string& body,
			std::chrono::milliseconds receiveTimeout = std::chrono::seconds(30))
		{
			ScopedTestSocketRuntime socketRuntime;
			if (!socketRuntime.IsStarted())
			{
				return {};
			}
			ScopedTestSocket socketHandle(OpenLoopbackConnection(port));
			if (!socketHandle.IsValid())
			{
				return {};
			}
			SetTestSocketReceiveTimeout(socketHandle.Get(), receiveTimeout);

			std::ostringstream request;
			request << "POST /mcp HTTP/1.1\r\n";
			request << "Host: 127.0.0.1\r\n";
			request << "Content-Type: application/json\r\n";
			request << "Content-Length: " << body.size() << "\r\n";
			request << "Connection: close\r\n\r\n";
			request << body;
			const std::string payload = request.str();
			send(socketHandle.Get(), payload.data(), static_cast<int>(payload.size()), 0);

			std::string response;
			std::array<char, 4096> buffer{};
			for (;;)
			{
				const int received = recv(socketHandle.Get(), buffer.data(), static_cast<int>(buffer.size()), 0);
				if (received <= 0)
				{
					break;
				}
				response.append(buffer.data(), static_cast<std::size_t>(received));
			}
			return response;
		}

		std::string SendLoopbackRawHttpRequest(
			std::uint16_t port,
			const std::string& request,
			std::chrono::milliseconds receiveTimeout = std::chrono::seconds(5))
		{
			ScopedTestSocketRuntime socketRuntime;
			if (!socketRuntime.IsStarted())
			{
				return {};
			}
			ScopedTestSocket socketHandle(OpenLoopbackConnection(port));
			if (!socketHandle.IsValid())
			{
				return {};
			}
			SetTestSocketReceiveTimeout(socketHandle.Get(), receiveTimeout);
			const int sent = send(socketHandle.Get(), request.data(), static_cast<int>(request.size()), 0);
			if (sent != static_cast<int>(request.size()))
			{
				return {};
			}

			std::string response;
			std::array<char, 4096> buffer{};
			for (;;)
			{
				const int received = recv(socketHandle.Get(), buffer.data(), static_cast<int>(buffer.size()), 0);
				if (received <= 0)
				{
					break;
				}
				response.append(buffer.data(), static_cast<std::size_t>(received));
			}
			return response;
		}

		nlohmann::json ParseLoopbackResourcePayload(std::string_view response)
		{
			// The HTTP response contains a JSON-RPC envelope whose MCP content item
			// contains a second JSON document as text. Parse both layers so tests
			// validate typed fields rather than depending on escaped string layout.
			const std::size_t bodyOffset = response.find("\r\n\r\n");
			if(bodyOffset == std::string_view::npos) {
				return nullptr;
			}
			const nlohmann::json envelope = nlohmann::json::parse(
				std::string(response.substr(bodyOffset + 4U)), nullptr, false);
			if(envelope.is_discarded() || !envelope.contains("result")
				|| !envelope["result"].contains("contents")
				|| !envelope["result"]["contents"].is_array()
				|| envelope["result"]["contents"].size() != 1U) {
				return nullptr;
			}
			const std::string text = envelope["result"]["contents"][0U].value("text", std::string{});
			const nlohmann::json payload = nlohmann::json::parse(text, nullptr, false);
			return payload.is_discarded() ? nlohmann::json(nullptr) : payload;
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

		registry.Add("Editor.MCP.Transport.CancellationNotificationCancelsPendingHttpToolCall", [](TestContext& context) {
			ObjectManager& objectManager = ObjectManager::instance();
			objectManager.clear();

			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetWriteMode(TerraEngine::Editor::Automation::WriteMode::WritesEnabled);
			bridge.SetEnabled(true);
			const std::uint16_t port = bridge.GetListenPort();
			context.Expect(port != 0, "Cancellation fixture should own a loopback listener.");

			std::string toolResponse;
			std::jthread toolClient([&]() {
				toolResponse = SendLoopbackHttpPost(port,
					R"({"jsonrpc":"2.0","id":"cancel-http-1","method":"tools/call","params":{"name":"object.create","arguments":{"name":"MCP Canceled HTTP Object"}}})",
					std::chrono::seconds(5));
			});

			const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (bridge.BuildStatusJson().value("pendingToolCallCount", 0u) == 0u
				&& std::chrono::steady_clock::now() < admissionDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const bool toolCallAdmitted = bridge.BuildStatusJson().value("pendingToolCallCount", 0u) == 1u;

			std::string cancellationResponse;
			if (toolCallAdmitted)
			{
				// A second real HTTP connection carries the JSON-RPC notification.
				// The listener must remain available while the first worker waits
				// for editor-thread dispatch.
				cancellationResponse = SendLoopbackHttpPost(port,
					R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"cancel-http-1","reason":"test cancellation"}})",
					std::chrono::seconds(2));
			}
			const bool notificationAcknowledged = cancellationResponse.find("202 Accepted") != std::string::npos;
			const bool cancellationDrainedCall =
				bridge.BuildStatusJson().value("pendingToolCallCount", 1u) == 0u;

			// Always release the first client before assertions can throw. On a
			// regression, disable provides bounded cleanup without making the
			// cancellation-specific observations above pass.
			if (!toolCallAdmitted || !notificationAcknowledged || !cancellationDrainedCall)
			{
				bridge.SetEnabled(false);
			}
			toolClient.join();

			bool structuredCancellation = false;
			try
			{
				// HTTP carries a JSON-RPC result whose text content is itself the
				// typed TerraEngine envelope. Parse both layers instead of searching
				// the escaped wire text, which would make the assertion depend on
				// JSON serializer escaping rather than the cancellation contract.
				const std::size_t bodyStart = toolResponse.find("\r\n\r\n");
				if (bodyStart != std::string::npos)
				{
					const nlohmann::json rpc = nlohmann::json::parse(toolResponse.substr(bodyStart + 4));
					for (const nlohmann::json& item : rpc["result"].value("content", nlohmann::json::array()))
					{
						if (item.value("type", std::string{}) != "text" || !item.contains("text"))
						{
							continue;
						}
						const nlohmann::json envelope = nlohmann::json::parse(item["text"].get<std::string>());
						structuredCancellation = !envelope.value("ok", true)
							&& envelope.value("error", nlohmann::json::object()).value("category", std::string{}) == "canceled";
					}
				}
			}
			catch (const nlohmann::json::exception&)
			{
				// The assertion below reports the malformed response while normal
				// bridge shutdown still runs, keeping test cleanup deterministic.
			}

			bool canceledObjectCreated = false;
			if (bridge.IsEnabled())
			{
				bridge.Update(0.0f);
			}
			for (const Object& object : objectManager.getRootObjects())
			{
				if (objectManager.getObjectName(object) == "MCP Canceled HTTP Object")
				{
					canceledObjectCreated = true;
					break;
				}
			}
			bridge.Shutdown();
			objectManager.clear();

			context.Expect(toolCallAdmitted, "The first HTTP tools/call should reach the editor dispatch queue.");
			context.Expect(notificationAcknowledged, "A notification on a concurrent HTTP connection should receive a 202 acknowledgement.");
			context.Expect(cancellationDrainedCall, "The JSON-RPC request id should remove the real pending tool call.");
			context.Expect(structuredCancellation,
				"The original HTTP tools/call should receive a structured cancellation. Response: " + toolResponse);
			context.Expect(!canceledObjectCreated, "A canceled HTTP mutation must never execute on a later editor update.");
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
						{"camera", {{"position", CanonicalPosition(0.0, 1.0, 5.0)}, {"lookAt", CanonicalPosition(0.0, 0.0, 0.0)}}},
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
			TerraEngine::Scene::WorldPosition gameViewPosition;
			gameViewPosition.offsetMetres = {0.0, 0.0, 5.0};
			RenderSystem::Instance().SetGameViewCamera(
				glm::mat4(1.0f),
				glm::perspectiveZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f),
				gameViewPosition);
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

		registry.Add("Editor.MCP.Transport.HttpListenerRejectsInvalidRequestBounds", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			const std::uint16_t port = bridge.GetListenPort();
			context.Expect(port != 0, "Bounded HTTP fixture should own a loopback listener.");

			const std::string missingLength = SendLoopbackRawHttpRequest(port,
				"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n{\"jsonrpc\":\"2.0\"}");
			const std::string malformedLength = SendLoopbackRawHttpRequest(port,
				"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: not-a-number\r\n\r\n{}");
			const std::string oversizedBody = SendLoopbackRawHttpRequest(port,
				"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 1048577\r\n\r\n");
			const std::string oversizedHeader = SendLoopbackRawHttpRequest(port,
				"POST /mcp HTTP/1.1\r\nX-Fill: " + std::string(16U * 1024U, 'x'));
			const auto idleStart = std::chrono::steady_clock::now();
			const std::string idleRequest = SendLoopbackRawHttpRequest(port,
				"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n");
			const auto idleElapsed = std::chrono::steady_clock::now() - idleStart;
			std::string slowDripResponse;
			auto slowDripElapsed = std::chrono::steady_clock::duration::zero();
			{
				ScopedTestSocketRuntime socketRuntime;
				ScopedTestSocket slowClient(socketRuntime.IsStarted()
					? OpenLoopbackConnection(port)
					: kInvalidTestSocket);
				if (slowClient.IsValid())
				{
					SetTestSocketReceiveTimeout(slowClient.Get(), std::chrono::seconds(2));
					const std::string headers = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100\r\n\r\n";
					send(slowClient.Get(), headers.data(), static_cast<int>(headers.size()), 0);
					const auto slowStart = std::chrono::steady_clock::now();
					// Keep the socket active until just before the three-second total
					// deadline, then read. The one-second idle deadline cannot win this
					// ordering, so the 408 proves the total deadline is enforced.
					while (std::chrono::steady_clock::now() - slowStart < std::chrono::milliseconds(2800))
					{
						const char byte = '{';
						if (send(slowClient.Get(), &byte, 1, 0) != 1)
						{
							break;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
					}
					slowDripElapsed = std::chrono::steady_clock::now() - slowStart;
					std::array<char, 256> responseBuffer{};
					for (;;)
					{
						const int received = recv(slowClient.Get(), responseBuffer.data(), static_cast<int>(responseBuffer.size()), 0);
						if (received <= 0)
						{
							break;
						}
						slowDripResponse.append(responseBuffer.data(), static_cast<std::size_t>(received));
					}
				}
			}

			context.Expect(missingLength.find("411") != std::string::npos,
				"A complete HTTP request without Content-Length should receive 411.");
			context.Expect(malformedLength.find("400") != std::string::npos,
				"A non-numeric Content-Length should receive 400.");
			context.Expect(oversizedBody.find("413") != std::string::npos,
				"An oversized declared body should receive 413 before allocation.");
			context.Expect(oversizedHeader.find("431") != std::string::npos,
				"An unterminated oversized header should receive 431.");
			context.Expect(idleRequest.find("408") != std::string::npos,
				"An incomplete idle request should receive a bounded 408 response.");
			context.Expect(idleElapsed < std::chrono::seconds(3),
				"The idle deadline should release a slow client worker well before the total request deadline.");
			context.Expect(slowDripResponse.find("408") != std::string::npos,
				"A client that keeps dripping bytes should still hit the total request deadline.");
			context.Expect(slowDripElapsed < std::chrono::seconds(4),
				"The total request deadline should close a slow-drip client before the test's four-second upper bound.");
			bridge.Shutdown();
		});

		registry.Add("Editor.MCP.Transport.CancellationReserveSurvivesEightBlockedToolCalls", [](TestContext& context) {
			ObjectManager& objectManager = ObjectManager::instance();
			objectManager.clear();
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetWriteMode(TerraEngine::Editor::Automation::WriteMode::WritesEnabled);
			bridge.SetEnabled(true);
			const std::uint16_t port = bridge.GetListenPort();
			std::vector<std::string> toolResponses(8);
			std::array<std::atomic_bool, 8> toolResponsesReady{};
			std::vector<std::jthread> toolClients;
			toolClients.reserve(toolResponses.size());
			for (std::size_t index = 0; index < toolResponses.size(); ++index)
			{
				toolClients.emplace_back([&, index]() {
					toolResponses[index] = SendLoopbackHttpPost(port,
						"{\"jsonrpc\":\"2.0\",\"id\":\"saturation-" + std::to_string(index)
						+ "\",\"method\":\"tools/call\",\"params\":{\"name\":\"object.create\",\"arguments\":{\"name\":\"MCP Saturation "
						+ std::to_string(index) + "\"}}}", std::chrono::seconds(5));
					// Publish after the string write. The main test thread waits on
					// this flag instead of reading a client-owned response string.
					toolResponsesReady[index].store(true, std::memory_order_release);
				});
			}
			const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (bridge.BuildStatusJson().value("pendingToolCallCount", 0u) < toolResponses.size()
				&& std::chrono::steady_clock::now() < admissionDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const bool allToolsBlocked = bridge.BuildStatusJson().value("pendingToolCallCount", 0u) == toolResponses.size();
			const std::string cancellationResponse = SendLoopbackHttpPost(port,
				R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"saturation-0"}})",
				std::chrono::seconds(2));
			const auto cancellationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (bridge.BuildStatusJson().value("pendingToolCallCount", 0u) == toolResponses.size()
				&& std::chrono::steady_clock::now() < cancellationDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const bool canceledOneQueuedCall = bridge.BuildStatusJson().value("pendingToolCallCount", 0u) == toolResponses.size() - 1;
			const auto canceledResponseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!toolResponsesReady.front().load(std::memory_order_acquire)
				&& std::chrono::steady_clock::now() < canceledResponseDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const bool canceledResponseCompletedBeforeShutdown =
				toolResponsesReady.front().load(std::memory_order_acquire);
			// The cancellation updates the pending-call count before the affected
			// ordinary worker flushes its typed HTTP response. Waiting for that one
			// response makes shutdown prove transport behavior rather than racing
			// socket teardown against the worker under aggregate test load.
			bridge.SetEnabled(false);
			for (std::jthread& client : toolClients)
			{
				client.join();
			}
			bridge.Update(0.0f);

			bool anySaturationObjectCreated = false;
			for (const Object& object : objectManager.getRootObjects())
			{
				if (objectManager.getObjectName(object).starts_with("MCP Saturation"))
				{
					anySaturationObjectCreated = true;
					break;
				}
			}
			objectManager.clear();

			context.Expect(allToolsBlocked, "Eight ordinary HTTP tools/call workers should occupy every ordinary dispatch lane.");
			context.Expect(cancellationResponse.find("202 Accepted") != std::string::npos,
				"The reserved control worker should acknowledge a cancellation while all ordinary lanes are blocked.");
			context.Expect(canceledOneQueuedCall, "The reserved cancellation should remove the matching pending tool call.");
			context.Expect(canceledResponseCompletedBeforeShutdown,
				"The canceled ordinary worker should flush its typed response before listener shutdown.");
			context.Expect(toolResponses.front().find("canceled") != std::string::npos,
				"The canceled ordinary call should receive its typed cancellation response.");
			context.Expect(!anySaturationObjectCreated, "No saturated or canceled mutation may run after listener shutdown.");
		});

		registry.Add("Editor.MCP.Transport.DestructorCancelsQueuedToolCallWithoutFramePump", [](TestContext& context) {
			ObjectManager& objectManager = ObjectManager::instance();
			objectManager.clear();
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetWriteMode(TerraEngine::Editor::Automation::WriteMode::WritesEnabled);
			bridge.SetEnabled(true);
			std::string response;
			std::jthread client;
			bool queuedBeforeDestruction = false;
			auto destructionElapsed = std::chrono::steady_clock::duration::zero();
			auto destructionStart = std::chrono::steady_clock::time_point{};
			{
				EditorMcpTransport scopedTransport(bridge);
				const bool started = scopedTransport.StartListening(0);
				const std::uint16_t port = scopedTransport.GetListenPort();
				context.Expect(started && port != 0, "Standalone transport fixture should acquire an ephemeral listener.");
				if (started && port != 0)
				{
					client = std::jthread([&]() {
						response = SendLoopbackHttpPost(port,
							R"({"jsonrpc":"2.0","id":"destructor-cancel","method":"tools/call","params":{"name":"object.create","arguments":{"name":"MCP Destructor Canceled"}}})",
							std::chrono::seconds(5));
					});
					const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
					while (bridge.BuildStatusJson().value("pendingToolCallCount", 0u) == 0u
						&& std::chrono::steady_clock::now() < admissionDeadline)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					queuedBeforeDestruction = bridge.BuildStatusJson().value("pendingToolCallCount", 0u) == 1u;
				}
				destructionStart = std::chrono::steady_clock::now();
				// Leaving scope invokes StopListening without disabling the bridge or
				// pumping a frame, exercising the standalone RAII path directly.
			}
			destructionElapsed = std::chrono::steady_clock::now() - destructionStart;
			if (client.joinable())
			{
				client.join();
			}
			bridge.Update(0.0f);

			bool canceledObjectCreated = false;
			for (const Object& object : objectManager.getRootObjects())
			{
				if (objectManager.getObjectName(object) == "MCP Destructor Canceled")
				{
					canceledObjectCreated = true;
					break;
				}
			}
			bridge.Shutdown();
			objectManager.clear();

			context.Expect(queuedBeforeDestruction, "The standalone listener should queue its HTTP tool call before destruction.");
			context.Expect(destructionElapsed < std::chrono::seconds(2), "Standalone transport destruction should not wait for the 30-second dispatch timeout.");
			context.Expect(response.empty() || response.find("canceled") != std::string::npos,
				"A stopped standalone transport should return a cancellation or close its client socket.");
			context.Expect(!canceledObjectCreated, "A transport-stopped queued mutation must never execute on a later frame.");
		});

		registry.Add("Editor.MCP.Transport.DestructorStopsListenerAndReleasesPort", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			std::uint16_t releasedPort = 0;
			bool firstListenerStarted = false;
			{
				EditorMcpTransport scopedTransport(bridge);
				firstListenerStarted = scopedTransport.StartListening(0);
				releasedPort = scopedTransport.GetListenPort();
				// No explicit StopListening: leaving scope is the behavior under
				// test, including exception-safe socket-runtime cleanup.
			}

			EditorMcpTransport reboundTransport(bridge);
			const bool rebound = firstListenerStarted
				&& releasedPort != 0
				&& reboundTransport.StartListening(releasedPort);
			reboundTransport.StopListening();

			context.Expect(firstListenerStarted && releasedPort != 0,
				"RAII fixture should acquire an ephemeral listener before destruction.");
			context.Expect(rebound,
				"Transport destruction should join the listener and release its exact port.");
		});

		registry.Add("Editor.MCP.Transport.HttpApprovalSurvivesConnectionClose", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.SetWriteMode(TerraEngine::Editor::Automation::WriteMode::WritesEnabled);
			const std::uint16_t port = bridge.GetListenPort();
			context.Expect(port != 0, "Enabled bridge should expose a loopback listener port.");

			// The listener thread waits for main-thread tool dispatch. Pump the bridge
			// exactly as the editor frame loop does while the test client waits for
			// the one-request HTTP socket to close.
			std::atomic<bool> stopPump = false;
			std::thread pump([&]() {
				while (!stopPump.load())
				{
					bridge.Update(0.0f);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			});
			const std::string response = SendLoopbackHttpPost(port,
				R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"asset.prepare_delete","arguments":{"path":"Assets/Missing.txt"}}})");
			stopPump = true;
			pump.join();

			context.Expect(response.find("waiting_approval") != std::string::npos, "High-impact HTTP tool call should return an approval prompt.");
			const std::string pendingResponse = SendLoopbackHttpPost(port,
				R"({"jsonrpc":"2.0","id":3,"method":"resources/read","params":{"uri":"terraengine://approvals/pending"}})");
			context.Expect(pendingResponse.find("approval-") != std::string::npos, "A second stateless HTTP request should rediscover the pending approval.");
			const auto approvals = bridge.GetPendingApprovals();
			context.ExpectEqual(approvals.size(), static_cast<std::size_t>(1), "Request socket closure should leave one resolvable approval.");
			if (!approvals.empty())
			{
				context.Expect(bridge.ResolveApproval(approvals.front().approvalId, true), "Preferences-side approval should resolve after HTTP disconnect.");
			}
			const std::string recentResponse = SendLoopbackHttpPost(port,
				R"({"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":"terraengine://approvals/recent"}})");
			const nlohmann::json recentPayload = ParseLoopbackResourcePayload(recentResponse);
			const nlohmann::json recentApprovals = recentPayload.is_object()
				? recentPayload.value("approvals", nlohmann::json::array())
				: nlohmann::json::array();
			const auto retained = std::find_if(recentApprovals.begin(), recentApprovals.end(),
				[&approvals](const nlohmann::json& entry) {
					return !approvals.empty()
						&& entry.value("approvalId", std::string{}) == approvals.front().approvalId;
				});
			const bool recoveredExactResult = retained != recentApprovals.end()
				&& retained->value("state", std::string{}) == "approved"
				&& retained->value("approved", false)
				&& retained->value("dispatchCompleted", false)
				&& retained->contains("dispatchResult")
				&& (*retained)["dispatchResult"].is_object()
				&& (*retained)["dispatchResult"].value("requestId", std::string{}) == approvals.front().requestId;
			context.Expect(recoveredExactResult,
				"A later HTTP connection should recover the exact completed protected-handler result.");

			const auto activity = bridge.GetRecentActivity(64);
			const bool recordedApproval = std::any_of(activity.begin(), activity.end(), [](const auto& event) {
				return event.message == "Approved request.";
			});
			const bool executedHandler = std::any_of(activity.begin(), activity.end(), [](const auto& event) {
				return event.message == "Prepared asset delete approval.";
			});
			const bool recordedExpiredBinding = std::any_of(activity.begin(), activity.end(), [](const auto& event) {
				return event.message.find("Approval binding expired") != std::string::npos;
			});
			context.Expect(recordedApproval, "Real loopback approval should execute through the approved path.");
			context.Expect(executedHandler, "Resolved approval should execute the protected handler, not only consume the prompt.");
			context.Expect(!recordedExpiredBinding, "Real loopback socket closure must not expire the listener-bound approval.");
			bridge.Shutdown();
		});

		registry.Add("Editor.MCP.Transport.DisableCancelsBlockedToolCallWithoutFramePump", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			const std::uint16_t port = bridge.GetListenPort();
			std::string response;
			std::jthread client([&]() {
				response = SendLoopbackHttpPost(port,
					R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"diagnostics.get_summary","arguments":{}}})",
					std::chrono::seconds(5));
			});

			const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (bridge.BuildStatusJson().value("pendingToolCallCount", 0u) == 0u
				&& std::chrono::steady_clock::now() < admissionDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const std::size_t admittedCallCount =
				bridge.BuildStatusJson().value("pendingToolCallCount", 0u);

			const auto disableStart = std::chrono::steady_clock::now();
			bridge.SetEnabled(false);
			const auto disableElapsed = std::chrono::steady_clock::now() - disableStart;
			client.join();

			context.ExpectEqual(admittedCallCount, static_cast<std::size_t>(1),
				"Listener should expose one blocked main-thread tool call before disable.");
			context.Expect(disableElapsed < std::chrono::seconds(2), "Disable should drain a blocked tool call before joining the listener.");
			context.Expect(bridge.GetState() == TerraEngine::Editor::Automation::BridgeState::Disabled, "Disable should finish in the Disabled state.");
			context.ExpectEqual(bridge.BuildStatusJson().value("pendingToolCallCount", 1u), 0u, "Disable should leave no pending tool calls.");
			context.Expect(response.empty() || response.find("canceled") != std::string::npos,
				"Interrupted client should either receive the structured cancellation or observe the closed socket.");
		});

		registry.Add("Editor.MCP.Transport.ShutdownBetweenPendingCallsCancelsRemainder", [](TestContext& context) {
			ObjectManager& objectManager = ObjectManager::instance();
			objectManager.clear();
			(void)objectManager.createObject("MCP Shutdown Blocker");

			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetWriteMode(TerraEngine::Editor::Automation::WriteMode::WritesEnabled);

			std::mutex selectionMutex;
			std::condition_variable selectionSignal;
			bool selectionEntered = false;
			bool releaseSelection = false;
			bridge.SetSelectionCallbacks(
				[]() -> std::optional<entt::entity> {
					return std::nullopt;
				},
				[&](std::optional<entt::entity>) {
					std::unique_lock lock(selectionMutex);
					selectionEntered = true;
					selectionSignal.notify_all();
					selectionSignal.wait(lock, [&]() {
						return releaseSelection;
					});
				});
			bridge.SetEnabled(true);

			nlohmann::json firstResult;
			nlohmann::json secondResult;
			std::thread firstClient([&]() {
				firstResult = bridge.SubmitToolCall("object.select", {{"object", "MCP Shutdown Blocker"}});
			});
			const auto firstAdmissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (bridge.BuildStatusJson().value("pendingToolCallCount", 0u) < 1u
				&& std::chrono::steady_clock::now() < firstAdmissionDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const bool firstAdmitted = bridge.BuildStatusJson().value("pendingToolCallCount", 0u) >= 1u;

			std::thread secondClient([&]() {
				secondResult = bridge.SubmitToolCall("object.create", {{"name", "MCP Must Not Run"}});
			});
			const auto secondAdmissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (bridge.BuildStatusJson().value("pendingToolCallCount", 0u) < 2u
				&& std::chrono::steady_clock::now() < secondAdmissionDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const bool secondAdmitted = bridge.BuildStatusJson().value("pendingToolCallCount", 0u) >= 2u;

			std::thread update([&]() {
				bridge.Update(0.0f);
			});
			bool handlerEntered = false;
			{
				std::unique_lock lock(selectionMutex);
				handlerEntered = selectionSignal.wait_for(lock, std::chrono::seconds(2), [&]() {
					return selectionEntered;
				});
			}

			std::thread shutdown([&]() {
				bridge.Shutdown();
			});
			const auto admissionCloseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (bridge.BuildStatusJson().value("acceptingToolCalls", true)
				&& std::chrono::steady_clock::now() < admissionCloseDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			const bool shutdownClosedAdmission = !bridge.BuildStatusJson().value("acceptingToolCalls", true);

			{
				std::lock_guard lock(selectionMutex);
				releaseSelection = true;
			}
			selectionSignal.notify_all();

			update.join();
			shutdown.join();
			firstClient.join();
			secondClient.join();

			bool secondObjectCreated = false;
			for (const Object& object : objectManager.getRootObjects())
			{
				if (objectManager.getObjectName(object) == "MCP Must Not Run")
				{
					secondObjectCreated = true;
					break;
				}
			}
			objectManager.clear();

			context.Expect(firstAdmitted && secondAdmitted, "Both client calls should be queued before update begins.");
			context.Expect(handlerEntered, "The first pending call should enter its blocking selection handler.");
			context.Expect(shutdownClosedAdmission, "Shutdown should close admission while the first handler owns the lifecycle gate.");
			context.Expect(firstResult.value("ok", false), "The handler already processing at shutdown should complete normally.");
			context.ExpectEqual(std::string("canceled"), secondResult["error"].value("category", std::string()),
				"The still-queued second call should be canceled by shutdown.");
			context.Expect(!secondObjectCreated, "Shutdown must prevent the second queued mutation from executing.");
			context.Expect(bridge.GetState() == TerraEngine::Editor::Automation::BridgeState::Stopped,
				"Shutdown between pending calls should leave the bridge stopped.");
		});

		registry.Add("Editor.MCP.Transport.ShutdownInterruptsPartialHttpRequest", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			ScopedTestSocketRuntime socketRuntime;
			context.Expect(socketRuntime.IsStarted(), "Test socket runtime should initialize.");
			ScopedTestSocket client(OpenLoopbackConnection(bridge.GetListenPort()));
			context.Expect(client.IsValid(), "Partial-request client should connect to the listener.");
			if (client.IsValid())
			{
				const std::string partialRequest =
					"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100\r\n\r\n{";
				const int sent = send(client.Get(), partialRequest.data(), static_cast<int>(partialRequest.size()), 0);
				context.ExpectEqual(static_cast<int>(partialRequest.size()), sent,
					"Partial-request fixture should send the complete incomplete-body prefix.");
			}

			const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (bridge.GetState() != TerraEngine::Editor::Automation::BridgeState::Connected
				&& std::chrono::steady_clock::now() < connectDeadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			context.Expect(bridge.GetState() == TerraEngine::Editor::Automation::BridgeState::Connected,
				"Listener should accept the partial request before shutdown.");

			const auto shutdownStart = std::chrono::steady_clock::now();
			bridge.Shutdown();
			const auto shutdownElapsed = std::chrono::steady_clock::now() - shutdownStart;
			bool clientObservedShutdown = false;
			if (client.IsValid())
			{
				// Bound the read so this assertion can never hang the suite. The
				// accepted-client worker owns the server socket and must close it
				// after shutdown interrupts recv, producing EOF or a reset here.
				SetTestSocketReceiveTimeout(client.Get(), std::chrono::seconds(1));
				std::array<char, 32> shutdownBuffer{};
				for (;;)
				{
					const int received = recv(client.Get(), shutdownBuffer.data(), static_cast<int>(shutdownBuffer.size()), 0);
					if (received == 0)
					{
						clientObservedShutdown = true;
						break;
					}
					if (received < 0)
					{
						// A timeout proves only that no bytes arrived. Require a
						// definite reset/closed-socket error for shutdown evidence.
						clientObservedShutdown = !LastTestSocketErrorWasTimeout()
							&& LastTestSocketErrorWasClosure();
						break;
					}
					// A server may flush a short error response before close. Drain
					// buffered bytes until EOF/reset or the bounded timeout.
				}
			}

			context.Expect(shutdownElapsed < std::chrono::seconds(2), "Shutdown should interrupt a client blocked in recv.");
			context.Expect(clientObservedShutdown, "Shutdown should make the accepted client observe EOF or a socket reset.");
			context.Expect(bridge.GetState() == TerraEngine::Editor::Automation::BridgeState::Stopped, "Shutdown should finish in the Stopped state.");
		});
	}
}
