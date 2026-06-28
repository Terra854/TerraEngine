#include "../TestHarness.h"
#include "EditorMcpBridge.h"

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorMcpBridge;
	using TerraEngine::Editor::Automation::WriteMode;

	void RegisterEditorMcpBridgeContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Contract.ListsToolsAndResources", [](TestContext& context) {
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			const nlohmann::json capabilities = bridge.BuildCapabilityListJson();
			const nlohmann::json resources = bridge.ListResources();
			context.Expect(capabilities["count"].get<std::size_t>() >= 19u, "Bridge should expose the planned tool surface.");
			context.Expect(!resources.empty(), "Bridge should expose resources.");
			context.ExpectEqual(resources[0].value("uri", std::string()), std::string("terraengine://bridge/status"), "Status should be listed first.");
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
	}
}
