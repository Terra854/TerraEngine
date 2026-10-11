#pragma once

#include <array>
#include <cstdint>

namespace TerraEngine::Renderer
{
	struct HiZDescriptorOwnerProof
	{
		bool image = false;
		bool view = false;
		bool sampler = false;
		bool shaderReadable = false;
		bool completedHistory = false;
	};
	struct HiZDescriptorPlan
	{
		bool valid = false;
		// -1 selects the device-owned white image; 0/1 select that view's history.
		std::array<int, 2> imageOwners{ -1, -1 };
		int samplerOwner = -1;
		std::uint32_t imageWrites = 0;
	};
	inline HiZDescriptorPlan BuildHiZDescriptorPlan(bool setsReady,
		const std::array<HiZDescriptorOwnerProof, 2>& histories,
		const HiZDescriptorOwnerProof& fallback) noexcept
	{
		HiZDescriptorPlan plan;
		// Descriptor validity is independent of completed occlusion history. A
		// real uploaded far-depth image keeps even dimension queries legal while
		// the existing frame gate continues to disable cold-history occlusion.
		if (!setsReady || !fallback.image || !fallback.view || !fallback.sampler || !fallback.shaderReadable) return plan;
		plan.valid = true;
		plan.imageWrites = 2;
		for (int i = 0; i < 2; ++i)
			if (histories[i].image && histories[i].view && histories[i].sampler && histories[i].shaderReadable && histories[i].completedHistory) {
				plan.imageOwners[i] = i;
				if (plan.samplerOwner == -1) plan.samplerOwner = i;
			}
		return plan;
	}
}
