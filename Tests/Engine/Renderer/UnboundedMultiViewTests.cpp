#include "../TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"

namespace TerraEngine::Tests
{
	void RegisterUnboundedMultiViewTests(TestRegistry& registry)
	{
		registry.Add("Renderer.UnboundedMultiView.SharedCanonicalPoseProducesIndependentMatrices", [](TestContext& context)
		{
			TerraEngine::Scene::WorldPose pose{
				{ { 0, 0, 0 }, { 10.0, 2.0, -3.0 } }, {}, { 1.0F, 1.0F, 1.0F }, 0U, 0U };
			TerraEngine::Renderer::RenderReferenceFrame nearFrame;
			nearFrame.frameId = 1U; nearFrame.viewId = 1U; nearFrame.outputId = 1U;
			nearFrame.originGeneration = 1U; nearFrame.spatialGeneration = 1U; nearFrame.residencyGeneration = 1U;
			TerraEngine::Renderer::RenderReferenceFrame farFrame = nearFrame;
			farFrame.viewId = 2U; farFrame.outputId = 2U;
			farFrame.canonicalOrigin.cell.x = 1'000'000'000LL;
			const auto nearMatrices = TerraEngine::Renderer::BuildLocalizedRenderPoseMatrices(pose, nearFrame);
			const auto farMatrices = TerraEngine::Renderer::BuildLocalizedRenderPoseMatrices(pose, farFrame);
			context.Expect(nearMatrices.valid, "The near view must produce a valid local matrix.");
			context.Expect(!farMatrices.valid, "A distant view must reject an unbounded local matrix request.");
			context.Expect(nearMatrices.currentMatrix == nearMatrices.previousMatrix,
				"Current and previous matrices must be explicitly owned by the same view lane.");
		});

		registry.Add("Renderer.UnboundedMultiView.ReferenceFrameIdentityIncludesResidencyAndSpatialGenerations", [](TestContext& context)
		{
			TerraEngine::Renderer::RenderReferenceFrame first;
			first.frameId = 10U; first.viewId = 3U; first.outputId = 4U;
			first.originGeneration = 2U; first.spatialGeneration = 5U; first.residencyGeneration = 8U;
			TerraEngine::Renderer::RenderReferenceFrame changed = first;
			++changed.residencyGeneration;
			context.Expect(!TerraEngine::Renderer::IsRenderReferenceFrameCurrent(first, changed),
				"Residency generation changes must invalidate per-view matrices and caches.");
		});
	}
}
