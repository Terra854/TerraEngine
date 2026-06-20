#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

namespace TerraEngine::Tests {

	void RegisterRenderDeferredRetirementTests(TestRegistry& registry)
	{
		registry.Add("Renderer.DeferredRetirement.WaitsForCompletedFrame", [](TestContext& context) {
			TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache cache;
			for (std::uint64_t index = 0u; index < 100u; ++index)
			{
				cache.EnqueueResourceForRetirement({ index + 1u, 50u + index, "resource" });
			}
			context.ExpectEqual(static_cast<std::size_t>(0u), cache.RetireCompletedResources(49u).size(), "Retirement must not run before the first fence frame.");
			context.ExpectEqual(static_cast<std::size_t>(1u), cache.RetireCompletedResources(50u).size(), "Exactly eligible resources should retire.");
			context.Expect(cache.PendingRetirementCount() > 0u, "Later frame retirements should remain queued.");
		});
	}

}
