#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/EditorAutomationService.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		std::optional<AutomationCapabilitySnapshot> FindSceneViewCapability(
			const EditorAutomationService& service,
			std::string_view name,
			const AutomationStateSnapshot& state)
		{
			return service.GetRegistry().Find(name, 1, state);
		}

		bool SchemaHasProperty(const nlohmann::json& schema, std::string_view property)
		{
			return schema.contains("properties")
				&& schema["properties"].is_object()
				&& schema["properties"].contains(std::string(property));
		}
	}

	void RegisterEditorMcpSceneViewTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.SceneView.DiscoveryCoversSemanticAuthoringControls", [](TestContext& context) {
			EditorAutomationService service;
			AutomationStateSnapshot state;
			state.projectAvailable = true;
			state.sceneAvailable = true;
			state.rendererAvailable = true;
			state.availableServices.insert("SceneView");
			const std::array<std::string_view, 9> operations{
				"scene_view.state",
				"scene_view.settings.get",
				"scene_view.settings.set",
				"scene_view.camera.set",
				"scene_view.frame_object",
				"scene_view.tool.set",
				"scene_view.orientation.set",
				"scene_view.snapping.set",
				"scene_view.visualization.set"
			};

			for (const std::string_view operation : operations)
			{
				const auto capability = FindSceneViewCapability(service, operation, state);
				context.Expect(capability.has_value(), std::string(operation) + " should be registered by the Scene View provider.");
				if (capability.has_value())
				{
					context.ExpectEqual(std::string("scene_view"), capability->descriptor.category, "Scene View operations should share a stable category.");
					context.Expect(capability->availability.available, std::string(operation) + " should be available when the Scene View service, scene, and renderer are ready.");
				}
			}
		});

		registry.Add("Editor.MCP.SceneView.PerformanceSettingsUseTypedAtomicContracts", [](TestContext& context) {
			EditorAutomationService service;
			AutomationStateSnapshot state;
			const auto get = FindSceneViewCapability(service, "scene_view.settings.get", state);
			const auto set = FindSceneViewCapability(service, "scene_view.settings.set", state);

			context.Expect(get.has_value() && set.has_value(), "Scene View performance settings should be additively discoverable.");
			if (get.has_value())
			{
				context.Expect(get->descriptor.permission == AutomationPermission::Read, "Reading user-local Scene View settings should remain read-only.");
				context.Expect(!get->descriptor.execution.protectedCommit, "A settings snapshot read must not schedule a protected mutation.");
			}
			if (set.has_value())
			{
				context.Expect(set->descriptor.permission == AutomationPermission::GlobalWrite, "User-local Scene View settings require explicit global-write permission.");
				context.Expect(set->descriptor.execution.protectedCommit, "A settings replacement should publish only at the editor preference boundary.");
				context.ExpectEqual(std::string("editor_preferences_atomic_replace"), set->descriptor.execution.visibilityBoundary, "The contract should advertise atomic preference visibility.");
				context.Expect(SchemaHasProperty(set->descriptor.inputSchema, "settings"), "Settings replacement should accept one complete typed candidate document.");
				const nlohmann::json& settingsSchema = set->descriptor.inputSchema["properties"]["settings"];
				context.Expect(SchemaHasProperty(settingsSchema, "visibleMarkerBudget"), "The candidate schema should expose the general marker budget.");
				context.Expect(SchemaHasProperty(settingsSchema, "physicsMarkerMode"), "The candidate schema should expose physics marker behavior.");
			}
		});

		registry.Add("Editor.MCP.SceneView.CanonicalStateAndControlsRemainDiscoverable", [](TestContext& context) {
			EditorAutomationService service;
			AutomationStateSnapshot state;
			state.projectAvailable = true;
			state.sceneAvailable = true;
			state.rendererAvailable = true;
			state.availableServices.insert("SceneView");
			const auto stateCapability = FindSceneViewCapability(service, "scene_view.state", state);
			const auto cameraCapability = FindSceneViewCapability(service, "scene_view.camera.set", state);

			context.Expect(stateCapability.has_value() && stateCapability->availability.available, "Canonical Scene View state should remain discoverable.");
			context.Expect(cameraCapability.has_value() && SchemaHasProperty(cameraCapability->descriptor.inputSchema, "position"), "Camera controls should expose a named canonical position field.");
			if (cameraCapability.has_value())
			{
				const nlohmann::json& position = cameraCapability->descriptor.inputSchema["properties"]["position"];
				context.Expect(position.value("type", std::string{}) == "object", "Scene View must reject legacy three-number camera positions at schema discovery.");
				context.Expect(SchemaHasProperty(position, "cell") && SchemaHasProperty(position, "offsetMetres"), "A Scene View camera address must declare exact cell and local-offset fields.");
			}
		});

		registry.Add("Editor.MCP.SceneView.ControlSchemasAreSemantic", [](TestContext& context) {
			EditorAutomationService service;
			AutomationStateSnapshot state;
			state.projectAvailable = true;
			state.sceneAvailable = true;
			state.rendererAvailable = true;
			state.availableServices.insert("SceneView");
			const auto camera = FindSceneViewCapability(service, "scene_view.camera.set", state);
			const auto frame = FindSceneViewCapability(service, "scene_view.frame_object", state);
			const auto tool = FindSceneViewCapability(service, "scene_view.tool.set", state);
			const auto orientation = FindSceneViewCapability(service, "scene_view.orientation.set", state);
			const auto snapping = FindSceneViewCapability(service, "scene_view.snapping.set", state);
			const auto visualization = FindSceneViewCapability(service, "scene_view.visualization.set", state);

			context.Expect(camera.has_value() && frame.has_value() && tool.has_value() && orientation.has_value() && snapping.has_value() && visualization.has_value(), "Every Scene View control schema should be registered.");
			if (camera.has_value())
			{
				context.Expect(SchemaHasProperty(camera->descriptor.inputSchema, "position"), "Camera control should accept a semantic world-space position.");
				context.Expect(SchemaHasProperty(camera->descriptor.inputSchema, "focus"), "Camera control should accept a semantic authoring focus.");
				context.Expect(!SchemaHasProperty(camera->descriptor.inputSchema, "screenX"), "Scene View control must not expose desktop coordinates.");
			}
			if (frame.has_value())
				context.Expect(SchemaHasProperty(frame->descriptor.inputSchema, "target"), "Framing should resolve a semantic object target.");
			if (tool.has_value())
				context.Expect(SchemaHasProperty(tool->descriptor.inputSchema, "tool"), "Tool control should select a registered gizmo mode.");
			if (orientation.has_value())
				context.Expect(SchemaHasProperty(orientation->descriptor.inputSchema, "orientation"), "Orientation control should select a semantic axis or perspective.");
			if (snapping.has_value())
				context.Expect(SchemaHasProperty(snapping->descriptor.inputSchema, "enabled"), "Snapping control should expose its enabled state.");
			if (visualization.has_value())
				context.Expect(SchemaHasProperty(visualization->descriptor.inputSchema, "mode"), "Visualization control should select an allowlisted rendering mode.");
		});

		registry.Add("Editor.MCP.SceneView.MutationsDeclareProtectedVisibility", [](TestContext& context) {
			EditorAutomationService service;
			AutomationStateSnapshot state;
			state.projectAvailable = true;
			state.sceneAvailable = true;
			state.rendererAvailable = true;
			state.availableServices.insert("SceneView");
			for (const std::string_view operation : {
				"scene_view.camera.set",
				"scene_view.frame_object",
				"scene_view.tool.set",
				"scene_view.orientation.set",
				"scene_view.snapping.set",
				"scene_view.visualization.set"})
			{
				const auto capability = FindSceneViewCapability(service, operation, state);
				context.Expect(capability.has_value(), std::string(operation) + " should be discoverable.");
				if (capability.has_value())
				{
					context.Expect(capability->descriptor.permission == AutomationPermission::ProjectWrite, "Scene View authoring controls should require write mode.");
					context.Expect(capability->descriptor.execution.protectedCommit, "Scene View state should change only at the protected editor boundary.");
					context.Expect(!capability->descriptor.execution.visibilityBoundary.empty(), "Scene View mutations should advertise their visibility boundary.");
				}
			}
		});

		registry.Add("Editor.MCP.SceneView.UnavailableWindowIsReportedByDiscovery", [](TestContext& context) {
			EditorAutomationService service;
			AutomationStateSnapshot state;
			state.projectAvailable = true;
			state.sceneAvailable = true;
			state.rendererAvailable = true;
			const auto capability = FindSceneViewCapability(service, "scene_view.state", state);
			context.Expect(capability.has_value(), "Scene View state should remain discoverable when its window service is unavailable.");
			if (capability.has_value())
			{
				context.Expect(!capability->availability.available, "A missing Scene View surface should be reported as unavailable.");
				context.Expect(!capability->availability.reason.empty(), "Unavailable Scene View discovery should explain the missing surface.");
			}
		});
	}
}
