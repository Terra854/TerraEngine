#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanPresentationLifecycle.h"

namespace TerraEngine::Tests
{
	void RegisterVulkanPresentationLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanPresentationLifecycle.ImageCountsAndGenerations", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			for (std::uint32_t imageCount = 2; imageCount <= 8; ++imageCount) {
				PresentationLifecycle lifecycle;
				context.Expect(lifecycle.Configure(3, 2, 2, imageCount), "Supported image counts should configure successfully.");
				for (std::uint32_t image = 0; image < imageCount; ++image) {
					context.Expect(lifecycle.Validate({ image % 3, image, lifecycle.Generation() }), "Every actual image index should validate.");
				}
				context.Expect(!lifecycle.Validate({ 0, imageCount, lifecycle.Generation() }), "The first out-of-range image must be rejected.");
				const std::uint64_t oldGeneration = lifecycle.Generation();
				lifecycle.PublishRecreation(PresentationRecreationOutcome::Success, 2, 2, imageCount);
				context.Expect(!lifecycle.Validate({ 0, 0, oldGeneration }), "A pre-recreation acquisition must be stale.");
			}
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.AllCountTransitions", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			for (std::uint32_t oldCount = 2; oldCount <= 8; ++oldCount) {
				for (std::uint32_t newCount = 2; newCount <= 8; ++newCount) {
					PresentationLifecycle lifecycle;
					lifecycle.Configure(3, 2, 2, oldCount);
					lifecycle.RequestRecreation();
					lifecycle.PublishRecreation(PresentationRecreationOutcome::Success, 2, 2, newCount);
					context.ExpectEqual(newCount, lifecycle.ActualImages(), "Recreation must publish the complete candidate image count.");
				}
			}
		});

		registry.Add("Renderer.VulkanPresentationLifecycle.ZeroExtentAndPresentRetirement", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			PresentationLifecycle lifecycle;
			lifecycle.Configure(3, 2, 2, 3);
			const auto generation = lifecycle.Generation();
			context.Expect(!lifecycle.CanReleasePresentSynchronization(generation - 1), "A render fence alone must not retire present waits.");
			context.Expect(lifecycle.CanReleasePresentSynchronization(generation), "A presentation completion proof may retire image synchronization.");
			lifecycle.PublishRecreation(PresentationRecreationOutcome::DeferredZeroExtent, 2, 2, 3);
			context.Expect(lifecycle.State() == PresentationLifecycleState::DeferredZeroExtent, "Minimize should defer without blocking.");
		});
	}
}
