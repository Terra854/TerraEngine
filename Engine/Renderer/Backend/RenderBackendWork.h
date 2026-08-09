// RenderBackendWork.h - immutable backend-owned animation/deformation work.

#pragma once

#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Renderer/Deformation/GpuDeformationPlan.h"
#include "Renderer/RenderTypes.h"
#include "Core/Export.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer
{
	struct RenderDynamicStateRoot;

	struct AnimationBackendWorkRange
	{
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t animationPlanGeneration = 0U;
		std::uint64_t paletteGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::uint32_t firstGpuWorkPacket = 0U;
		std::uint32_t gpuWorkPacketCount = 0U;
		std::uint32_t gpuStageMask = 0U;
	};

	struct AnimationBackendWorkPlan
	{
		std::uint64_t frameId = 0U;
		std::vector<AnimationBackendWorkRange> ranges;
		std::size_t gpuAnimatorCount = 0U;
		std::size_t cpuOrFallbackAnimatorCount = 0U;
		// A fully visible one-packet-per-animator crowd can submit the already
		// immutable animation plan directly. The producer still proves a complete
		// packet permutation before setting this flag; mixed and sparse frames keep
		// the explicit range table below.
		bool fullImmutablePlanCoverage = false;
		std::uint32_t rendererGeneration = 0U;
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutablePlan;
		std::shared_ptr<const CpuAnimationDeformationPlan> cpuDeformationPlan;
		bool valid = true;
		std::string rejectionReason;
	};

	struct DeformationBackendWorkPlan
	{
		std::uint64_t frameId = 0u;
		std::size_t uniquePageCount = 0u;
		std::size_t reusedDemandCount = 0u;
		std::shared_ptr<const GpuDeformationPlan> immutablePlan;
		bool valid = true;
		std::string rejectionReason;
	};

	TERRAENGINE_API AnimationBackendWorkPlan BuildAnimationBackendWorkPlan(
		std::uint64_t frameId,
		const RenderDynamicStateRoot& dynamicState,
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutablePlan = {});
	// A producer-sealed dense presentation already proves a one-to-one owner/packet
	// topology. This O(1) path publishes that proof directly instead of rescanning
	// every render lane merely to reconstruct the same complete coverage result.
	TERRAENGINE_API AnimationBackendWorkPlan BuildDenseAnimationBackendWorkPlan(
		std::uint64_t frameId,
		std::size_t validatedOwnerCount,
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutablePlan);
	TERRAENGINE_API DeformationBackendWorkPlan BuildDeformationBackendWorkPlan(
		std::uint64_t frameId,
		std::shared_ptr<const GpuDeformationPlan> immutablePlan = {});
}
