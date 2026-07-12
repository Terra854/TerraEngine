#include "../TestHarness.h"
#include "EditorMcpBridge.h"

#include <array>
#include <string_view>
#include <unordered_set>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorMcpBridge;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
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

	void RegisterEditorMcpBridgeContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Contract.ListsToolsAndResources", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			const nlohmann::json capabilities = bridge.BuildCapabilityListJson();
			const nlohmann::json resources = bridge.ListResources();
			context.Expect(capabilities["count"].get<std::size_t>() >= 22u, "Bridge should expose the planned tool surface.");
			context.Expect(!resources.empty(), "Bridge should expose resources.");
			context.ExpectEqual(resources[0].value("uri", std::string()), std::string("terraengine://bridge/status"), "Status should be listed first.");
			bool sawCaptureAvailability = false;
			for (const auto& resource : resources)
			{
				if (resource.value("uri", std::string()) == "terraengine://capture/availability")
				{
					sawCaptureAvailability = true;
				}
			}
			context.Expect(sawCaptureAvailability, "Bridge should list capture availability.");
		});

		registry.Add("Editor.MCP.Contract.StatusAndActivityResources", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			bridge.SetWriteMode(WriteMode::WritesEnabled);
			const nlohmann::json status = bridge.ReadResource("terraengine://bridge/status");
			context.Expect(status.value("enabled", false), "Status resource should reflect enablement.");
			context.ExpectEqual(status.value("writeMode", std::string()), std::string("writes_enabled"), "Status resource should reflect write mode.");
			const nlohmann::json activity = bridge.ReadResource("terraengine://activity/recent");
			context.Expect(activity.contains("activity"), "Activity resource should return activity array.");
		});

		registry.Add("Editor.MCP.Contract.ProjectSceneDiagnosticsResourcesAreSafeEmpty", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			context.Expect(bridge.ReadResource("terraengine://project/state").contains("hasProject"), "Project state should have an empty-state response.");
			context.Expect(bridge.ReadResource("terraengine://scene/summary").contains("roots"), "Scene summary should have an empty-state response.");
			context.Expect(bridge.ReadResource("terraengine://diagnostics/summary").contains("surfaces"), "Diagnostics summary should have an empty-state response.");
			context.Expect(bridge.ReadResource("terraengine://capture/availability").contains("supportedSources"), "Capture availability should have an empty-state response.");
		});

		registry.Add("Editor.MCP.Contract.AssetExportToolsAdvertiseSafetyMetadata", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			const nlohmann::json capabilities = bridge.BuildCapabilityListJson()["capabilities"];
			bool sawAssetImport = false;
			bool sawExportStart = false;
			bool sawAssetDeleteApproval = false;
			bool sawWriteModeTool = false;
			for (const auto& capability : capabilities)
			{
				if (capability.value("name", "") == "bridge.set_write_mode")
				{
					sawWriteModeTool = true;
				}
				if (capability.value("name", "") == "asset.import")
				{
					sawAssetImport = capability.value("mutatesState", false) && capability.value("requiresProject", false);
				}
				if (capability.value("name", "") == "export.start")
				{
					sawExportStart = capability.value("requiresApproval", false) && capability.value("mutationMode", "") == "export_mutation";
				}
				if (capability.value("name", "") == "asset.prepare_delete")
				{
					sawAssetDeleteApproval = capability.value("requiresApproval", false);
				}
			}
			context.Expect(sawAssetImport, "asset.import should advertise project-scoped mutation behavior.");
			context.Expect(sawExportStart, "export.start should advertise approval-gated export mutation behavior.");
			context.Expect(sawAssetDeleteApproval, "asset.prepare_delete should advertise destructive approval behavior.");
			context.Expect(!sawWriteModeTool, "MCP discovery should not advertise editor-owned write-mode changes.");
		});

		registry.Add("Editor.MCP.Contract.PreservesEveryLegacyToolName", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			const nlohmann::json capabilities = bridge.BuildCapabilityListJson()["capabilities"];
			std::unordered_set<std::string> discovered;
			for (const auto& capability : capabilities)
			{
				const std::string name = capability.value("name", "");
				context.Expect(!name.empty(), "Every discovered MCP capability should retain a stable name.");
				context.Expect(discovered.insert(name).second, "MCP discovery should not publish duplicate capability names.");
			}

			for (const std::string_view legacyName : kLegacyToolNames)
			{
				context.Expect(discovered.contains(std::string(legacyName)), std::string("Legacy MCP discovery should preserve ") + std::string(legacyName) + ".");
			}
			bridge.Shutdown();
		});

		registry.Add("Editor.MCP.Contract.PreservesEveryLegacyResource", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			const nlohmann::json resources = bridge.ListResources();
			std::unordered_set<std::string> discovered;
			for (const auto& resource : resources)
			{
				const std::string uri = resource.value("uri", "");
				context.Expect(!uri.empty(), "Every MCP resource should retain a stable URI.");
				context.Expect(discovered.insert(uri).second, "MCP discovery should not publish duplicate resource URIs.");
			}

			for (const std::string_view legacyUri : kLegacyResourceUris)
			{
				context.Expect(discovered.contains(std::string(legacyUri)), std::string("Legacy MCP discovery should preserve ") + std::string(legacyUri) + ".");
				const nlohmann::json value = bridge.ReadResource(legacyUri);
				context.Expect(!(value.contains("ok") && !value.value("ok", true)), std::string("Legacy resource should remain readable: ") + std::string(legacyUri));
			}
			bridge.Shutdown();
		});
	}
}
