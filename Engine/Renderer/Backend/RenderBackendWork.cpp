#include "RenderBackendWork.h"
#include "Renderer/Graph/RenderFramePublication.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Renderer {
	AnimationBackendWorkPlan BuildDenseAnimationBackendWorkPlan(
		std::uint64_t frameId,
		std::size_t validatedOwnerCount,
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutablePlan)
	{
		AnimationBackendWorkPlan plan;
		plan.frameId = frameId;
		if(frameId == 0U || validatedOwnerCount == 0U || immutablePlan == nullptr
			|| immutablePlan->generation == 0U
			|| immutablePlan->topologyInputSignature == 0U
			|| immutablePlan->topologyRendererGeneration == 0U
			|| immutablePlan->WorkPackets().size() != validatedOwnerCount
			|| immutablePlan->GenerationRecordCount() != validatedOwnerCount
			|| immutablePlan->RenderIdentityCount() != validatedOwnerCount) {
			plan.valid = false;
			plan.rejectionReason =
				"Dense animation publication does not match its sealed owner topology.";
			return plan;
		}
		const TerraEngine::Animation::GpuAnimationWorkPacket& firstPacket =
			immutablePlan->WorkPackets().front();
		if(firstPacket.generationRecordIndex >= immutablePlan->GenerationRecordCount()
			|| immutablePlan->GenerationRecord(firstPacket.generationRecordIndex)
				.rendererGeneration == 0U) {
			plan.valid = false;
			plan.rejectionReason =
				"Dense animation publication has no current renderer-session generation.";
			return plan;
		}
		plan.gpuAnimatorCount = validatedOwnerCount;
		plan.fullImmutablePlanCoverage = true;
		// topologyRendererGeneration is a retained aggregate cache key, not the
		// renderer session number expected by Vulkan generation records. Read the
		// current dynamic lane in O(1); backend validation still checks every packet
		// against this session before touching descriptors or command buffers.
		plan.rendererGeneration = immutablePlan->GenerationRecord(
			firstPacket.generationRecordIndex).rendererGeneration;
		plan.immutablePlan = std::move(immutablePlan);
		return plan;
	}

	AnimationBackendWorkPlan BuildAnimationBackendWorkPlan(
		std::uint64_t frameId,
		const RenderDynamicStateRoot& dynamicState,
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutablePlan)
	{
		AnimationBackendWorkPlan plan;
		plan.frameId = frameId;
		// A crowd normally contributes one GPU range per animator. Stable identities
		// encode a dense slot, so use that lane directly and retain a hash fallback
		// for sparse or externally authored identities.
		std::size_t laneCount = 0u;
		for(const std::shared_ptr<const RenderDynamicPage>& page : dynamicState.pages)
		{
			laneCount += page != nullptr ? page->lanes.size() : 0u;
		}
		// Dense GPU crowds already carry one immutable work packet per animator.
		// Prove that the visible lanes form an exact permutation of that plan before
		// allocating and copying a second crowd-sized range table. Any ambiguity
		// falls through to the general deduplicating path below.
		if(immutablePlan != nullptr && immutablePlan->generation != 0U
			&& !immutablePlan->WorkPackets().empty())
		{
			std::vector<std::uint8_t> visitedPackets(
				immutablePlan->WorkPackets().size(), 0U);
			std::size_t coveredPacketCount = 0U;
			std::size_t cpuOrFallbackAnimatorCount = 0U;
			std::uint32_t rendererGeneration = 0U;
			bool completeCoverage = true;
			for(const std::shared_ptr<const RenderDynamicPage>& page : dynamicState.pages)
			{
				if(page == nullptr)
				{
					completeCoverage = false;
					break;
				}
				for(const RenderDynamicLane& lane : page->lanes)
				{
					if(!lane.occupied) continue;
					const RenderItemRecord::PublishedAnimationGeneration& animation =
						lane.animation;
					const AnimationRenderEvaluationSource source =
						static_cast<AnimationRenderEvaluationSource>(animation.source);
					if(source == AnimationRenderEvaluationSource::None) continue;
					const bool identityComplete = animation.stableInstanceId != 0U
						&& animation.deformablePayloadGeneration != 0U
						&& animation.rendererGeneration != 0U;
					if(source == AnimationRenderEvaluationSource::SafeFallback)
					{
						if(!identityComplete || animation.complete
							|| animation.gpuWorkPacketCount != 0U)
						{
							completeCoverage = false;
							break;
						}
						++cpuOrFallbackAnimatorCount;
						continue;
					}
					const bool evaluatedIdentityComplete = identityComplete
						&& animation.complete && animation.poseGeneration != 0U
						&& animation.paletteGeneration != 0U
						&& animation.programGeneration != 0U;
					if(source == AnimationRenderEvaluationSource::CpuPose)
					{
						if(!evaluatedIdentityComplete
							|| animation.gpuWorkPacketCount != 0U
							|| animation.gpuStageMask != 0U)
						{
							completeCoverage = false;
							break;
						}
						++cpuOrFallbackAnimatorCount;
						continue;
					}
					if((source != AnimationRenderEvaluationSource::GpuVisual
							&& source != AnimationRenderEvaluationSource::HybridAuthoritative)
						|| !evaluatedIdentityComplete
						|| animation.animationPlanGeneration != immutablePlan->generation
						|| animation.gpuWorkPacketCount != 1U
						|| animation.gpuStageMask == 0U
						|| animation.firstGpuWorkPacket >= visitedPackets.size()
						|| visitedPackets[animation.firstGpuWorkPacket] != 0U)
					{
						completeCoverage = false;
						break;
					}
					const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
						immutablePlan->WorkPackets()[animation.firstGpuWorkPacket];
					if(packet.generationRecordIndex
							>= immutablePlan->GenerationRecordCount()
						|| packet.stableInstanceId != animation.stableInstanceId
						|| packet.stageMask != animation.gpuStageMask
						|| immutablePlan->GenerationRecord(packet.generationRecordIndex)
							.rendererGeneration != animation.rendererGeneration
						|| (rendererGeneration != 0U
							&& rendererGeneration != animation.rendererGeneration))
					{
						completeCoverage = false;
						break;
					}
					rendererGeneration = animation.rendererGeneration;
					visitedPackets[animation.firstGpuWorkPacket] = 1U;
					++coveredPacketCount;
				}
				if(!completeCoverage) break;
			}
			if(completeCoverage && coveredPacketCount == visitedPackets.size())
			{
				plan.gpuAnimatorCount = coveredPacketCount;
				plan.cpuOrFallbackAnimatorCount = cpuOrFallbackAnimatorCount;
				plan.fullImmutablePlanCoverage = true;
				plan.rendererGeneration = rendererGeneration;
				plan.immutablePlan = std::move(immutablePlan);
				return plan;
			}
		}
		plan.ranges.reserve(laneCount);
		struct DenseRangeIndex
		{
			std::uint64_t stableInstanceId = 0u;
			std::size_t rangeIndex = 0u;
		};
		std::vector<DenseRangeIndex> rangeIndexByAnimatorSlot(
			laneCount + 1u);
		std::unordered_map<std::uint64_t, std::size_t> sparseRangeIndices;
		sparseRangeIndices.reserve(std::min<std::size_t>(
			laneCount, 64u));
		const auto reject = [&plan](std::string reason)
		{
			plan.valid = false;
			plan.rejectionReason = std::move(reason);
			plan.ranges.clear();
			plan.gpuAnimatorCount = 0U;
			plan.fullImmutablePlanCoverage = false;
			plan.rendererGeneration = 0U;
			plan.immutablePlan.reset();
		};
		for(const std::shared_ptr<const RenderDynamicPage>& page : dynamicState.pages)
		{
			if(page == nullptr)
			{
				reject("The dynamic renderer publication contains a missing page.");
				return plan;
			}
			for(const RenderDynamicLane& lane : page->lanes)
			{
				if(!lane.occupied)
				{
					continue;
				}
				const RenderItemRecord::PublishedAnimationGeneration& animation =
					lane.animation;
				const AnimationRenderEvaluationSource source =
					static_cast<AnimationRenderEvaluationSource>(animation.source);
				if(source == AnimationRenderEvaluationSource::None)
				{
					continue;
				}
				const bool identityComplete = animation.stableInstanceId != 0U
					&& animation.deformablePayloadGeneration != 0U
					&& animation.rendererGeneration != 0U;
				if(source == AnimationRenderEvaluationSource::SafeFallback)
				{
					if(!identityComplete || animation.complete
						|| animation.gpuWorkPacketCount != 0U)
					{
						reject("An animated fallback carried an incomplete or mixed generation record.");
						return plan;
					}
					++plan.cpuOrFallbackAnimatorCount;
					continue;
				}
				const bool evaluatedIdentityComplete = identityComplete
					&& animation.complete && animation.poseGeneration != 0U
					&& animation.paletteGeneration != 0U
					&& animation.programGeneration != 0U;
				if(!evaluatedIdentityComplete)
				{
					reject("An animated renderable carried an incomplete or mixed generation record.");
					return plan;
				}
				if(source == AnimationRenderEvaluationSource::CpuPose)
				{
					if(animation.gpuWorkPacketCount != 0U
						|| animation.gpuStageMask != 0U)
					{
						reject("A CPU animation record unexpectedly referenced GPU work.");
						return plan;
					}
					++plan.cpuOrFallbackAnimatorCount;
					continue;
				}
				if((source != AnimationRenderEvaluationSource::GpuVisual
					&& source
						!= AnimationRenderEvaluationSource::HybridAuthoritative)
					|| animation.animationPlanGeneration == 0U
					|| animation.gpuWorkPacketCount == 0U
					|| animation.gpuStageMask == 0U)
				{
					reject("A GPU animation record carried no complete work range.");
					return plan;
				}

				const std::uint64_t animatorSlot = animation.stableInstanceId >> 32u;
				std::size_t existingRangeIndex = 0u;
				bool inserted = false;
				if(animatorSlot < rangeIndexByAnimatorSlot.size())
			{
				DenseRangeIndex& dense = rangeIndexByAnimatorSlot[
					static_cast<std::size_t>(animatorSlot)];
				if(dense.stableInstanceId == 0u)
				{
					dense.stableInstanceId = animation.stableInstanceId;
					dense.rangeIndex = plan.ranges.size();
					existingRangeIndex = dense.rangeIndex;
					inserted = true;
				}
				else if(dense.stableInstanceId == animation.stableInstanceId)
				{
					existingRangeIndex = dense.rangeIndex;
				}
				else
				{
					const auto [sparse, sparseInserted] =
						sparseRangeIndices.emplace(animation.stableInstanceId,
							plan.ranges.size());
					existingRangeIndex = sparse->second;
					inserted = sparseInserted;
				}
			}
			else
			{
				const auto [sparse, sparseInserted] = sparseRangeIndices.emplace(
					animation.stableInstanceId, plan.ranges.size());
				existingRangeIndex = sparse->second;
				inserted = sparseInserted;
			}
			if (!inserted)
			{
				const AnimationBackendWorkRange& duplicate =
					plan.ranges[existingRangeIndex];
				// Multiple meshes may reference one animator. They consume one palette
				// generation and must not submit its compute work once per mesh/view.
				if (duplicate.animationPlanGeneration != animation.animationPlanGeneration
					|| duplicate.paletteGeneration != animation.paletteGeneration
					|| duplicate.rendererGeneration != animation.rendererGeneration
					|| duplicate.firstGpuWorkPacket != animation.firstGpuWorkPacket
					|| duplicate.gpuWorkPacketCount != animation.gpuWorkPacketCount
					|| duplicate.gpuStageMask != animation.gpuStageMask)
				{
					reject("One animator was referenced with conflicting GPU work generations.");
					return plan;
				}
				continue;
			}
			plan.ranges.push_back({
				animation.stableInstanceId,
				animation.animationPlanGeneration,
				animation.paletteGeneration,
				animation.rendererGeneration,
				animation.firstGpuWorkPacket,
				animation.gpuWorkPacketCount,
				animation.gpuStageMask});
				++plan.gpuAnimatorCount;
			}
		}
		const auto rangeBefore = [](const AnimationBackendWorkRange& left,
			const AnimationBackendWorkRange& right)
		{
			return left.stableInstanceId < right.stableInstanceId;
		};
		if(!std::is_sorted(plan.ranges.begin(), plan.ranges.end(), rangeBefore))
		{
			std::sort(plan.ranges.begin(), plan.ranges.end(), rangeBefore);
		}
		if(plan.ranges.empty()) return plan;
		if(!immutablePlan || immutablePlan->generation == 0U) {
			reject("GPU animation ranges require their complete immutable presentation plan.");
			return plan;
		}
		for(const AnimationBackendWorkRange& range : plan.ranges) {
			const std::size_t first = range.firstGpuWorkPacket;
			const std::size_t count = range.gpuWorkPacketCount;
			if(range.animationPlanGeneration != immutablePlan->generation
				|| first > immutablePlan->WorkPackets().size()
				|| count > immutablePlan->WorkPackets().size() - first) {
				reject("GPU animation packet ranges do not belong to the retained plan generation.");
				return plan;
			}
			for(std::size_t packetIndex = first; packetIndex < first + count; ++packetIndex) {
				const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
					immutablePlan->WorkPackets()[packetIndex];
				if(packet.stableInstanceId != range.stableInstanceId
					|| packet.generationRecordIndex >= immutablePlan->GenerationRecordCount()
					|| immutablePlan->GenerationRecord(packet.generationRecordIndex).rendererGeneration
						!= range.rendererGeneration) {
					reject("GPU animation packet identity or renderer generation is stale.");
					return plan;
				}
			}
		}
		plan.immutablePlan = std::move(immutablePlan);
		plan.rendererGeneration = plan.ranges.front().rendererGeneration;
		return plan;
	}

	DeformationBackendWorkPlan BuildDeformationBackendWorkPlan(
		std::uint64_t frameId,
		std::shared_ptr<const GpuDeformationPlan> immutablePlan)
	{
		DeformationBackendWorkPlan result;
		result.frameId = frameId;
		if(!immutablePlan)
		{
			result.valid = true;
			return result;
		}
		result.valid = immutablePlan->frameId == frameId
			&& immutablePlan->IsComplete();
		result.rejectionReason = result.valid
			? immutablePlan->rejectionReason
			: "The immutable deformation plan is incomplete or belongs to another frame.";
		result.uniquePageCount = immutablePlan->work.size();
		result.reusedDemandCount = immutablePlan->reusedDemandCount;
		result.immutablePlan = std::move(immutablePlan);
		return result;
	}

}
