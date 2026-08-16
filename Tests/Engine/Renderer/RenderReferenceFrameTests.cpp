#include "../TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"

namespace TerraEngine::Tests
{
	void RegisterRenderReferenceFrameTests(TestRegistry& registry)
	{
		registry.Add("Renderer.RenderReferenceFrame.LocalizesCanonicalOriginWithoutGlobalFloat", [](TestContext& context)
		{
			TerraEngine::Renderer::RenderReferenceFrame frame;
			frame.frameId = 1U;
			frame.viewId = 7U;
			frame.outputId = 2U;
			frame.canonicalOrigin = { { 15, -4, 8 }, { 0.25, -1.0, 2.0 } };
			frame.originGeneration = 3U;
			frame.spatialGeneration = 4U;
			frame.residencyGeneration = 5U;

			const TerraEngine::Scene::WorldPose pose{
				{ { 15, -4, 8 }, { 1.251, -1.0, 2.0 } }, {}, { 1.0F, 1.0F, 1.0F }, 0U, 0U };
			const auto localized = TerraEngine::Renderer::BuildLocalizedRenderPose(pose, frame);
			context.Expect(localized.Succeeded(), "A nearby canonical pose must localize in the view frame.");
			context.ExpectEqual(1.001F, localized.offsetMetres.x, "Localization must retain millimetre local precision.");
		});

		registry.Add("Renderer.RenderReferenceFrame.RejectsDistantAndStaleOrigins", [](TestContext& context)
		{
			TerraEngine::Renderer::RenderReferenceFrame frame;
			frame.frameId = 1U;
			frame.viewId = 1U;
			frame.outputId = 1U;
			frame.originGeneration = 1U;
			frame.spatialGeneration = 1U;
			frame.residencyGeneration = 1U;
			const TerraEngine::Scene::WorldPose distant{
				{ { 1'000'000'000'000LL, 0, 0 }, {} }, {}, { 1.0F, 1.0F, 1.0F }, 0U, 0U };
			context.Expect(!TerraEngine::Renderer::BuildLocalizedRenderPose(distant, frame).Succeeded(),
				"A pose outside the bounded local range must fail closed.");

			TerraEngine::Renderer::RenderReferenceFrame changed = frame;
			++changed.originGeneration;
			const auto stale = TerraEngine::Renderer::BuildLocalizedRenderPose(
				TerraEngine::Scene::WorldPose{ frame.canonicalOrigin, {}, { 1.0F, 1.0F, 1.0F }, 0U, 0U }, frame, &changed);
			context.Expect(stale.staleReferenceFrame, "A changed origin generation must reject previous-frame reuse.");
		});
	}
}
