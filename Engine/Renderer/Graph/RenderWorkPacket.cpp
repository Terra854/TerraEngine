#include "RenderWorkPacket.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Renderer {

	const char* ToString(RenderBackendWorkKind workKind)
	{
		switch (workKind)
		{
		case RenderBackendWorkKind::Geometry: return "geometry";
		case RenderBackendWorkKind::Shadow: return "shadow";
		case RenderBackendWorkKind::UI: return "ui";
		case RenderBackendWorkKind::Culling: return "culling";
		case RenderBackendWorkKind::Descriptor: return "descriptor";
		case RenderBackendWorkKind::Upload: return "upload";
		case RenderBackendWorkKind::Transition: return "transition";
		case RenderBackendWorkKind::Validation: return "validation";
		case RenderBackendWorkKind::Stats: return "stats";
		default: return "unknown";
		}
	}

	BackendRecordingBatchPlan BuildBackendRecordingBatchPlan(
		const RenderFrameSnapshot& snapshot,
		const RenderWorkPacketPlan& packetPlan,
		const BackendCapabilityProfile& capabilities,
		RenderBackendWorkKind workKind)
	{
		BackendRecordingBatchPlan plan;
		for (const RenderWorkPacket& packet : packetPlan.packets)
		{
			BackendRecordingBatch batch;
			batch.batchId = "frame-" + std::to_string(snapshot.frameId) + "." + packet.packetId + "." + ToString(workKind);
			batch.frameId = snapshot.frameId;
			batch.passId = packet.passId;
			batch.workKind = workKind;
			batch.begin = packet.begin;
			batch.end = packet.end;
			batch.workerEligible = packet.executionPolicy == RenderPacketExecutionPolicy::Worker && capabilities.supportsParallelRecording;
			if (batch.workerEligible)
			{
				++plan.workerBatchCount;
			}
			else
			{
				++plan.serialBatchCount;
				batch.fallbackReason = "Backend recording batch requires coordinator execution or a serial fallback.";
				plan.serializedReasons.push_back(batch.fallbackReason);
			}
			plan.batches.push_back(std::move(batch));
		}
		return plan;
	}

	AnimationBackendWorkPlan BuildAnimationBackendWorkPlan(
		const RenderFrameSnapshot& snapshot,
		std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> immutablePlan)
	{
		AnimationBackendWorkPlan plan;
		plan.frameId = snapshot.frameId;
		// A crowd normally contributes one GPU range per animator. Stable identities
		// encode a dense slot, so use that lane directly and retain a hash fallback
		// for sparse or externally authored identities.
		plan.ranges.reserve(snapshot.renderables.size());
		struct DenseRangeIndex
		{
			std::uint64_t stableInstanceId = 0u;
			std::size_t rangeIndex = 0u;
		};
		std::vector<DenseRangeIndex> rangeIndexByAnimatorSlot(
			snapshot.renderables.size() + 1u);
		std::unordered_map<std::uint64_t, std::size_t> sparseRangeIndices;
		sparseRangeIndices.reserve(std::min<std::size_t>(
			snapshot.renderables.size(), 64u));
		const auto reject = [&plan](std::string reason)
		{
			plan.valid = false;
			plan.rejectionReason = std::move(reason);
			plan.ranges.clear();
			plan.gpuAnimatorCount = 0U;
			plan.immutablePlan.reset();
		};
		for (const RenderFrameRenderableRecord& renderable : snapshot.renderables)
		{
			const AnimationRenderGenerationRecord& animation = renderable.animation;
			if (animation.source == AnimationRenderEvaluationSource::None)
			{
				continue;
			}
			if (!ValidateAnimationRenderGenerationRecord(animation))
			{
				reject("An animated renderable carried an incomplete or mixed generation record.");
				return plan;
			}
			if (animation.source == AnimationRenderEvaluationSource::CpuPose
				|| animation.source == AnimationRenderEvaluationSource::SafeFallback)
			{
				++plan.cpuOrFallbackAnimatorCount;
				continue;
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
				|| first > immutablePlan->workPackets.size()
				|| count > immutablePlan->workPackets.size() - first) {
				reject("GPU animation packet ranges do not belong to the retained plan generation.");
				return plan;
			}
			for(std::size_t packetIndex = first; packetIndex < first + count; ++packetIndex) {
				const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
					immutablePlan->workPackets[packetIndex];
				if(packet.stableInstanceId != range.stableInstanceId
					|| packet.generationRecordIndex >= immutablePlan->generationRecords.size()
					|| immutablePlan->generationRecords[packet.generationRecordIndex].rendererGeneration
						!= range.rendererGeneration) {
					reject("GPU animation packet identity or renderer generation is stale.");
					return plan;
				}
			}
		}
		plan.immutablePlan = std::move(immutablePlan);
		return plan;
	}

	DeformationBackendWorkPlan BuildDeformationBackendWorkPlan(
		const RenderFrameSnapshot& snapshot,
		std::shared_ptr<const GpuDeformationPlan> immutablePlan)
	{
		DeformationBackendWorkPlan result;
		result.frameId = snapshot.frameId;
		if(!immutablePlan && snapshot.animatedGeometryPageDemands.empty())
		{
			return result;
		}
		if(!immutablePlan)
		{
			const std::uint64_t rendererGeneration =
				snapshot.animatedGeometryPageDemands.front().key.deformation.rendererGeneration;
			immutablePlan = std::make_shared<GpuDeformationPlan>(BuildGpuDeformationPlan(
				snapshot.frameId,
				rendererGeneration,
				snapshot.animatedGeometryPageDemands));
		}
		result.valid = immutablePlan->frameId == snapshot.frameId
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
