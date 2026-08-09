#include "../TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

namespace TerraEngine::Tests {
	void RegisterLiveGpuSubmissionBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Engine.LiveGpuSubmissionBoundary.RenderSystemIsPartialWorkerSafe", [](TestContext& context) {
			const TerraEngine::Scheduling::SystemDeclaration declaration = RenderSystem::Instance().SchedulerDeclaration();

			context.Expect(declaration.hardBoundary, "RenderSystem should advertise the live GPU hard boundary.");
			context.Expect(declaration.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "Live GPU submission should be partial worker-safe.");
			context.Expect(!declaration.mainThread, "Render preparation should remain worker eligible.");
			context.Expect(declaration.safeSubwork.find("snapshots") != std::string::npos, "Safe subwork should describe render snapshot preparation.");
			context.Expect(declaration.protectedCommit.find("present") != std::string::npos, "Protected commit should describe present ownership.");
		});

		registry.Add("Engine.LiveGpuSubmissionBoundary.RetiresOnlyCompletedFrameResources",
			[](TestContext& context) {
				TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache cache;
				cache.EnqueueResourceForRetirement({ 17u, 42u, "publication-range-buffer" });
				const auto beforeFence = cache.RetireCompletedResources(41u);
				const auto atFence = cache.RetireCompletedResources(42u);
				context.Expect(beforeFence.empty(),
					"An in-flight frame slot must retain every resource referenced by its publication.");
				context.Expect(atFence.size() == 1u
					&& atFence.front().resourceId == 17u,
					"The matching completed-frame boundary must retire the resource exactly once.");
				context.Expect(cache.RetireCompletedResources(99u).empty(),
					"A completed retirement must not be published again by later frame slots.");
			});
	}
}
