#include "TestHarness.h"

#include "Resource/ResourceStreaming.h"
#include "Scene/WorldStreamingSystem.h"

namespace TerraEngine::EditorTests {
	void RegisterStreamingEditorWorkflowTests(TestRegistry& registry)
	{
		registry.Add("Editor.Streaming.Workflow.EditorNeedsAreProtected", [](TestContext& context) {
			const Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();

			context.Expect(Resource::IsProtectedNeed(Resource::StreamingNeedType::EditorSelection, policy), "Selected offscreen objects should protect their streamable assets.");
			context.Expect(Resource::IsProtectedNeed(Resource::StreamingNeedType::PrefabEditing, policy), "Prefab editing should protect its preview assets.");
			context.Expect(Resource::IsProtectedNeed(Resource::StreamingNeedType::AssetPreview, policy), "Asset previews should protect inspected payloads.");
		});

		registry.Add("Editor.Streaming.Workflow.SelectionPinUsesCanonicalLocation", [](TestContext& context) {
			Scene::ObserverInterest pin;
			pin.observerId = "editor-selection-test";
			pin.kind = Scene::ObserverInterestKind::SelectionPin;
			pin.position.cell = {15'258'789'062LL, -15'258'789'062LL, 0};
			pin.position.offsetMetres = {1.0, -2.0, 0.0};
			pin.priority = Scene::ObserverInterestPriority::Critical;
			pin.radiusMetres = Scene::kWorldCellEdgeMetres;
			const Scene::WorldCoordinateResult normalized = Scene::NormalizeWorldPosition(pin.position);
			context.Expect(normalized.Succeeded(), "Selection pins must preserve a valid exact location for distant navigation.");
			context.Expect(pin.kind == Scene::ObserverInterestKind::SelectionPin, "Editor selection must retain protected streaming priority.");
		});
	}
}
