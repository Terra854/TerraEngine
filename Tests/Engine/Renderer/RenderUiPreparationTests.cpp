#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

namespace TerraEngine::Tests {

	void RegisterRenderUiPreparationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.UiPreparation.RespectsWorkerThreshold", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(8u, 1u);
			auto profile = MakeThreadedRendererProfile();
			const auto smallPublication = BuildRendererThreadingPublication(snapshot);
			const auto small = TerraEngine::Renderer::Vulkan::BuildUiGeometryPreparationPlan(smallPublication, profile);
			context.Expect(!small.workerEligible, "Tiny UI geometry should stay inline.");
			for (std::size_t index = 0u; index < 16u; ++index)
			{
				snapshot.viewRequests.push_back({ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(128u, 128u), static_cast<std::uint32_t>(index + 10u) });
			}
			const auto largePublication = BuildRendererThreadingPublication(snapshot);
			const auto large = TerraEngine::Renderer::Vulkan::BuildUiGeometryPreparationPlan(largePublication, profile);
			context.Expect(large.workerEligible, "Larger UI geometry should become worker eligible.");
		});
	}

}
