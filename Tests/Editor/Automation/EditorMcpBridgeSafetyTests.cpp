#include "../TestHarness.h"
#include "EditorMcpBridge.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include "EditorContext.h"
#include "ProjectManager.h"

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::BridgeState;
	using TerraEngine::Editor::Automation::EditorMcpBridge;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		class ScopedPreferenceDirectory
		{
		public:
			explicit ScopedPreferenceDirectory(const std::string& name)
				: root(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / name)
				, preferencesPath(root / ".terra_editor_prefs.json")
			{
				// The editor uses a user-level preference file. This fixture redirects
				// that file so the test proves persistence without touching a real
				// developer profile.
				if (const char* existingOverride = std::getenv("TERRAENGINE_EDITOR_PREFS_PATH"))
				{
					previousOverride = existingOverride;
					hadPreviousOverride = true;
				}
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
				SetPreferenceOverride(preferencesPath.string());
			}

			~ScopedPreferenceDirectory()
			{
				std::error_code ignored;
				if (hadPreviousOverride)
				{
					SetPreferenceOverride(previousOverride);
				}
				else
				{
					ClearPreferenceOverride();
				}
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path PreferencesPath() const
			{
				return preferencesPath;
			}

		private:
			static void SetPreferenceOverride(const std::string& path)
			{
#ifdef _WIN32
				_putenv_s("TERRAENGINE_EDITOR_PREFS_PATH", path.c_str());
#else
				setenv("TERRAENGINE_EDITOR_PREFS_PATH", path.c_str(), 1);
#endif
			}

			static void ClearPreferenceOverride()
			{
#ifdef _WIN32
				_putenv_s("TERRAENGINE_EDITOR_PREFS_PATH", "");
#else
				unsetenv("TERRAENGINE_EDITOR_PREFS_PATH");
#endif
			}

			std::filesystem::path root;
			std::filesystem::path preferencesPath;
			std::string previousOverride;
			bool hadPreviousOverride = false;
		};

		nlohmann::json ReadPreferenceJson(const std::filesystem::path& path)
		{
			std::ifstream stream(path);
			nlohmann::json json;
			stream >> json;
			return json;
		}
	}

	void RegisterEditorMcpBridgeSafetyTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Safety.EnableDisableStateTransitions", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			context.ExpectEqual(std::string(TerraEngine::Editor::Automation::ToString(bridge.GetState())), std::string("disabled"), "Bridge should start disabled.");
			bridge.SetEnabled(true);
			context.Expect(bridge.IsEnabled(), "Bridge should enable.");
			context.Expect(bridge.GetListenPort() != 0, "Direct editor enable should start the local listener.");
			bridge.SetEnabled(false);
			context.Expect(!bridge.IsEnabled(), "Bridge should disable.");
			context.ExpectEqual(static_cast<std::uint16_t>(0), bridge.GetListenPort(), "Direct editor disable should stop the listener immediately.");
			bridge.Shutdown();
			context.ExpectEqual(std::string(TerraEngine::Editor::Automation::ToString(bridge.GetState())), std::string("stopped"), "Bridge should stop on shutdown.");
		});

		registry.Add("Editor.MCP.Safety.DisabledBridgeRejectsEnableTool", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetWriteMode(WriteMode::WritesEnabled);
			const nlohmann::json capabilities = bridge.BuildCapabilityListJson()["capabilities"];
			const auto advertisedEnablementTool = std::find_if(capabilities.begin(), capabilities.end(), [](const nlohmann::json& capability) {
				return capability.value("name", std::string()) == "bridge.set_enabled";
			});
			context.Expect(advertisedEnablementTool == capabilities.end(), "MCP discovery should not advertise editor-owned bridge enablement.");
			const nlohmann::json result = bridge.CallTool("bridge.set_enabled", {{"enabled", true}});
			context.Expect(!result.value("ok", true), "Disabled bridge should reject bridge.set_enabled through the MCP tool path.");
			context.ExpectEqual(std::string("permission"), result["error"].value("category", std::string()), "Disabled bridge enable attempt should be a permission failure.");
			context.Expect(!bridge.IsEnabled(), "Rejected enable attempt should leave the bridge disabled.");
		});

		registry.Add("Editor.MCP.Safety.EnabledBridgeRejectsDisableTool", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.SetWriteMode(WriteMode::WritesEnabled);
			context.Expect(bridge.GetListenPort() != 0, "Enabled bridge should own a live local listener.");
			const nlohmann::json result = bridge.CallTool("bridge.set_enabled", {{"enabled", false}});
			context.Expect(!result.value("ok", true), "Enabled bridge should reject bridge.set_enabled through the MCP tool path.");
			context.Expect(bridge.IsEnabled(), "Rejected disable attempt should leave the editor-owned bridge setting enabled.");
			context.Expect(bridge.GetListenPort() != 0, "Rejected disable attempt should leave the local listener active.");
			bridge.Shutdown();
		});

		registry.Add("Editor.MCP.Safety.EnabledBridgeRejectsWriteModeTool", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.SetWriteMode(WriteMode::WritesEnabled);
			const nlohmann::json capabilities = bridge.BuildCapabilityListJson()["capabilities"];
			const auto advertisedWriteModeTool = std::find_if(capabilities.begin(), capabilities.end(), [](const nlohmann::json& capability) {
				return capability.value("name", std::string()) == "bridge.set_write_mode";
			});
			context.Expect(advertisedWriteModeTool == capabilities.end(), "MCP discovery should not advertise editor-owned write-mode changes.");
			const nlohmann::json result = bridge.CallTool("bridge.set_write_mode", {{"writeMode", "read_only"}});
			context.Expect(!result.value("ok", true), "Enabled bridge should reject write-mode changes through the MCP tool path.");
			context.ExpectEqual(std::string("unsupported_capability"), result["error"].value("category", std::string()), "Write-mode MCP attempts should be unsupported.");
			context.ExpectEqual(std::string("writes_enabled"), std::string(TerraEngine::Editor::Automation::ToString(bridge.GetWriteMode())), "Rejected write-mode attempt should leave the editor-owned mode unchanged.");
			bridge.Shutdown();
		});

		registry.Add("Editor.MCP.Safety.RequestQueueIsFifo", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.EnqueueRequest("first", {});
			bridge.EnqueueRequest("second", {});
			const auto first = bridge.PopNextQueuedRequest();
			const auto second = bridge.PopNextQueuedRequest();
			context.Expect(first.has_value() && second.has_value(), "Two queued requests should be returned.");
			context.ExpectEqual(first->capability, std::string("first"), "First request should be popped first.");
			context.ExpectEqual(second->capability, std::string("second"), "Second request should be popped second.");
		});

		registry.Add("Editor.MCP.Safety.ActivityHistoryIsBoundedNewestFirst", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			for (int index = 0; index < 300; ++index)
			{
				bridge.RecordActivity(TerraEngine::Editor::Automation::ActivityEventType::Request, std::to_string(index), "capability", "message", "completed");
			}
			const auto activity = bridge.GetRecentActivity(300);
			context.ExpectEqual(activity.size(), static_cast<std::size_t>(256), "Activity history should be bounded.");
			context.Expect(activity.front().sequence > activity.back().sequence, "Newest activity should be returned first.");
		});

		registry.Add("Editor.MCP.Safety.DestructiveToolCreatesApproval", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.SetWriteMode(WriteMode::WritesEnabled);
			const nlohmann::json result = bridge.CallTool("object.delete", {{"object", "missing"}});
			context.ExpectEqual(result.value("status", std::string()), std::string("waiting_approval"), "Destructive tools should create approval prompts before execution.");
			context.ExpectEqual(bridge.GetPendingApprovals().size(), static_cast<std::size_t>(1), "Approval should be pending.");
		});

		registry.Add("Editor.MCP.Safety.ApprovalCanBeDeniedOrCanceled", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.SetWriteMode(WriteMode::WritesEnabled);
			const nlohmann::json first = bridge.CallTool("asset.prepare_delete", {{"path", "Assets/Missing.txt"}});
			context.ExpectEqual(first.value("status", std::string()), std::string("waiting_approval"), "Destructive asset preparation should wait for approval.");
			context.Expect(bridge.ResolveApproval(first["data"].value("approvalId", ""), false), "Deny should resolve an approval.");
			context.Expect(bridge.GetPendingApprovals().empty(), "Denied approval should leave no pending prompt.");
			const nlohmann::json second = bridge.CallTool("asset.prepare_delete", {{"path", "Assets/Missing.txt"}});
			context.Expect(bridge.CancelApproval(second["data"].value("approvalId", "")), "Cancel should resolve an approval separately from deny.");
			context.Expect(bridge.GetPendingApprovals().empty(), "Canceled approval should leave no pending prompt.");
		});

		registry.Add("Editor.MCP.Safety.ApprovalCanBeApprovedOrExpired", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.SetWriteMode(WriteMode::WritesEnabled);
			const nlohmann::json approved = bridge.CallTool("asset.prepare_delete", {{"path", "Assets/Missing.txt"}});
			context.Expect(bridge.ResolveApproval(approved["data"].value("approvalId", ""), true), "Approve should resolve a pending approval.");
			context.Expect(bridge.GetPendingApprovals().empty(), "Approved prompt should no longer be pending.");
			bridge.CreateApprovalPrompt("expire-1", "object.delete", "Expire immediately", nlohmann::json::object(), std::chrono::system_clock::now() - std::chrono::seconds(1));
			context.ExpectEqual(static_cast<std::size_t>(1), bridge.GetPendingApprovals().size(), "Expired fixture should start as pending until update processes it.");
			bridge.Update(0.0f);
			context.Expect(bridge.GetPendingApprovals().empty(), "Expired approval should be resolved during bridge update.");
		});

		registry.Add("Editor.MCP.Safety.InvalidRequestsReturnContractErrors", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			const nlohmann::json unknown = bridge.CallTool("missing.tool", nlohmann::json::object());
			context.Expect(!unknown.value("ok", true), "Unknown tools should fail.");
			context.ExpectEqual(std::string("unsupported_capability"), unknown["error"].value("category", std::string()), "Unknown tools should use unsupported_capability.");
			const nlohmann::json wrongShape = bridge.CallTool("diagnostics.get_summary", nlohmann::json::array());
			context.Expect(!wrongShape.value("ok", true), "Non-object tool arguments should fail validation.");
			context.ExpectEqual(std::string("validation"), wrongShape["error"].value("category", std::string()), "Bad argument shape should use validation.");
		});

		registry.Add("Editor.MCP.Safety.DiagnosticsIgnoreReadOnlyWriteMode", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.SetWriteMode(WriteMode::ReadOnly);
			const nlohmann::json result = bridge.CallTool("diagnostics.get_summary", {{"includeDetails", true}});
			context.Expect(result.value("ok", false), "Diagnostics should remain available in read-only mode.");
			context.Expect(bridge.BuildStatusJson().value("lastDiagnosticRequest", std::string()).find("includeDetails") != std::string::npos, "Bridge should remember the last diagnostics request.");
		});

		registry.Add("Editor.MCP.Safety.QueuedRequestsCanBeCanceledBeforeUpdate", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.EnqueueRequest("object.create", {{"name", "Canceled"}});
			const auto queued = bridge.GetQueuedRequests();
			context.ExpectEqual(static_cast<std::size_t>(1), queued.size(), "Fixture should queue one request.");
			context.Expect(bridge.CancelQueuedRequest(queued.front().requestId), "Queued request should be cancelable.");
			bridge.Update(0.0f);
			context.Expect(bridge.GetQueuedRequests().empty(), "Canceled request should drain on update.");
		});

		registry.Add("Editor.MCP.Safety.EnablementWriteModeAndPortPersist", [](TestContext& context) {
			ScopedPreferenceDirectory preferences("McpBridgePreferences");
			EditorContext editorContext;
			editorContext.projectManager = std::make_shared<ProjectManager>();

			EditorMcpBridge bridge;
			bridge.Initialize(&editorContext);
			bridge.SetWriteMode(WriteMode::WritesEnabled);
			bridge.SetEnabled(true);

			nlohmann::json json = ReadPreferenceJson(preferences.PreferencesPath());
			context.Expect(json.value("editorMcpEnabled", false), "Enabled MCP bridge should persist as a global editor preference.");
			context.ExpectEqual(std::string("writes_enabled"), json.value("editorMcpWriteModeDefault", std::string()), "Write mode should persist for the next editor launch.");
			context.Expect(!json.value("editorMcpRecentSession", std::string()).empty(), "Recent MCP endpoint should be written when the bridge is listening.");
			context.ExpectEqual(
				TerraEngine::Editor::Preferences::kDefaultEditorMcpListenPort,
				static_cast<std::uint16_t>(json.value("editorMcpListenPort", 0u)),
				"The default fixed MCP port should persist while listening.");

			bridge.SetEnabled(false);
			constexpr std::uint16_t savedPort = 43127;
			context.Expect(bridge.SetListenPort(savedPort), "A valid MCP port should be configurable while the bridge is disabled.");
			json = ReadPreferenceJson(preferences.PreferencesPath());
			context.Expect(!json.value("editorMcpEnabled", true), "Disabled MCP bridge should persist when the user turns it off.");
			context.ExpectEqual(savedPort, static_cast<std::uint16_t>(json.value("editorMcpListenPort", 0u)), "Configured MCP port should persist for the next editor launch.");

			ProjectManager reloadedPreferences;
			context.ExpectEqual(savedPort, reloadedPreferences.GetEditorPreferences().editorMcpListenPort, "A new editor preference owner should reload the configured MCP port.");
			bridge.Shutdown();
		});
	}
}
