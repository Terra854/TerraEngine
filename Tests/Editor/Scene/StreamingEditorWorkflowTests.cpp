#include "TestHarness.h"

#include "Resource/ResourceStreaming.h"

namespace TerraEngine::EditorTests {
	void RegisterStreamingEditorWorkflowTests(TestRegistry& registry)
	{
		registry.Add("Editor.Streaming.Workflow.EditorNeedsAreProtected", [](TestContext& context) {
			const Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();

			context.Expect(Resource::IsProtectedNeed(Resource::StreamingNeedType::EditorSelection, policy), "Selected offscreen objects should protect their streamable assets.");
			context.Expect(Resource::IsProtectedNeed(Resource::StreamingNeedType::PrefabEditing, policy), "Prefab editing should protect its preview assets.");
			context.Expect(Resource::IsProtectedNeed(Resource::StreamingNeedType::AssetPreview, policy), "Asset previews should protect inspected payloads.");
		});
	}
}
