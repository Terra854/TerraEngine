#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

namespace TerraEngine::Tests {

	void RegisterRenderShadowParallelParityTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ShadowParallel.PointLightUsesSixFaces", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(7u, 64u, 1u);
			const auto publication = BuildRendererThreadingPublication(snapshot);
			auto profile = MakeThreadedRendererProfile();
			const auto plan = TerraEngine::Renderer::Vulkan::BuildShadowRecordingPreparationPlan(publication, profile);
			context.ExpectEqual(static_cast<std::size_t>(6u), plan.secondaryRecordingCount, "Point-light shadow work should partition into six deterministic face recordings.");
			context.Expect(plan.workerEligible, "Shadow work with multiple faces should be worker eligible.");
		});
	}

}
