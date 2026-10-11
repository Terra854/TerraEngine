#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanHiZDescriptorPlan.h"

namespace TerraEngine::Tests
{
	void RegisterVulkanHiZDescriptorPlanTests(TestRegistry& registry)
	{
		using namespace Renderer;
		registry.Add("Renderer.Efficiency.HiZDescriptors.ColdAndMixedKeepEverySlotValid", [](TestContext& context) {
			const HiZDescriptorOwnerProof white{ true, true, true, true, false };
			std::array<HiZDescriptorOwnerProof, 2> histories{};
			auto plan = BuildHiZDescriptorPlan(true, histories, white);
			context.Expect(plan.valid && plan.imageWrites == 2, "Cold initialization must write both sampled-image slots.");
			context.Expect(plan.imageOwners == std::array<int, 2>{-1,-1}, "Cold slots use the real white image, not undefined pyramids.");
			context.Expect(!histories[0].completedHistory && !histories[1].completedHistory, "Descriptor validity must not manufacture history readiness.");
			histories[0] = { true, true, true, true, true };
			plan = BuildHiZDescriptorPlan(true, histories, white);
			context.Expect(plan.valid && plan.imageOwners == std::array<int,2>{0,-1}, "Mixed views retain own history and safe missing-view fallback.");
			histories[1] = histories[0];
			plan = BuildHiZDescriptorPlan(true, histories, white);
			context.Expect(plan.imageOwners == std::array<int,2>{0,1}, "Both completed views use their exact own image.");
		});
		registry.Add("Renderer.Efficiency.HiZDescriptors.RecoveryAndInvalidOwnersFailClosed", [](TestContext& context) {
			std::array<HiZDescriptorOwnerProof, 2> histories{};
			const HiZDescriptorOwnerProof white{ true, true, true, true, false };
			context.Expect(!BuildHiZDescriptorPlan(false, histories, white).valid, "Missing native descriptor sets cannot be published.");
			for (int fault = 0; fault < 4; ++fault) {
				auto invalid = white;
				if (fault == 0) invalid.image = false;
				if (fault == 1) invalid.view = false;
				if (fault == 2) invalid.sampler = false;
				if (fault == 3) invalid.shaderReadable = false;
				context.Expect(!BuildHiZDescriptorPlan(true, histories, invalid).valid, "Invalid fallback owners must reject readiness.");
			}
			histories[0] = { false, true, true, true, true };
			auto plan = BuildHiZDescriptorPlan(true, histories, white);
			context.Expect(plan.valid && plan.imageOwners[0] == -1, "A stale view without its native image cannot qualify as history.");
			histories = {}; // Fresh backend/resize has no inherited completed owner.
			plan = BuildHiZDescriptorPlan(true, histories, white);
			context.Expect(plan.valid && plan.imageWrites == 2 && plan.imageOwners[1] == -1, "Recovery initializes every fresh slot without reusing old history.");
		});
	}
}
