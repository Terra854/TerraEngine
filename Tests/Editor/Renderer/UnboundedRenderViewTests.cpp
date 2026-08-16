#include "../TestHarness.h"

#include "Renderer/RenderTypes.h"

namespace TerraEngine::EditorTests
{
	void RegisterUnboundedRenderViewTests(TestRegistry& registry)
	{
		registry.Add("Editor.UnboundedRenderView.PickRequiresCurrentReferenceFrame", [](TestContext& context) {
			VirtualGeometryVisibilityPick stale;
			stale.objectId[15] = 1u;
			stale.meshId[15] = 2u;
			context.Expect(!stale.IsValid(), "A pick without a published renderer frame identity must be rejected.");
			stale.frameId = 17u;
			context.Expect(stale.IsValid(), "A completed pick still requires explicit frame qualification before selection.");

			TerraEngine::Renderer::RenderReferenceFrame frame;
			frame.frameId = 17u;
			frame.viewId = 3u;
			frame.outputId = 5u;
			frame.originGeneration = 7u;
			frame.spatialGeneration = 11u;
			frame.residencyGeneration = 13u;
			context.Expect(!stale.IsQualifiedFor(frame), "A pick without origin/spatial/residency identity must fail closed.");
			stale.originGeneration = frame.originGeneration;
			stale.spatialGeneration = frame.spatialGeneration;
			stale.residencyGeneration = frame.residencyGeneration;
			context.Expect(stale.IsQualifiedFor(frame), "Only a pick stamped by the current reference frame may select an object.");
			++frame.residencyGeneration;
			context.Expect(!stale.IsQualifiedFor(frame), "A residency publication change must reject stale picking.");
		});
	}
}
