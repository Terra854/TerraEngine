#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	void AnimationSystem::Impl::EvaluateCompactGpuVisualPartition(std::size_t itemBegin, std::size_t itemEnd) noexcept {
		if(itemBegin > itemEnd || !evaluationState.compactGpuVisualWorkItems.IsConsistent() ||
		   !evaluationState.compactGpuVisualCompletions.IsConsistent() ||
		   evaluationState.compactGpuVisualCompletions.size() != evaluationState.compactGpuVisualWorkItems.size()) {
			return;
		}
		const auto evaluationStart = std::chrono::steady_clock::now();
		std::size_t locallyCompleted = 0U;
		const std::size_t boundedBegin = std::min(itemBegin, evaluationState.compactGpuVisualWorkItems.size());
		const std::size_t boundedEnd = std::min(itemEnd, evaluationState.compactGpuVisualWorkItems.size());
		// Scheduler partitions are contiguous. Adjacent completion records therefore
		// stay on one worker instead of being interleaved across cache lines by every
		// worker, which removes false sharing in large compact crowds.
		bool cancellationObserved = false;
		for(std::size_t compactIndex = boundedBegin; compactIndex < boundedEnd; ++compactIndex) {
			evaluationState.compactGpuVisualCompletions.compactItemIndices[compactIndex] = compactIndex;
			evaluationState.compactGpuVisualCompletions.animators[compactIndex] =
				evaluationState.compactGpuVisualWorkItems.animators[compactIndex];
			evaluationState.compactGpuVisualCompletions.sourceGenerations[compactIndex] =
				evaluationState.compactGpuVisualWorkItems.sourceGenerations[compactIndex];
			// The cancellation state is monotonic for this frame. Poll once per
			// sixteen compact packets and latch the result, matching the complete
			// evaluator's bounded response without repeated shared-state reads.
			if(!cancellationObserved && ((compactIndex - boundedBegin) & 15U) == 0U) {
				cancellationObserved = PollFrameCancellation();
			}
			if(cancellationObserved) {
				evaluationState.compactGpuVisualCompletions.statuses[compactIndex] = AnimationStatus::Cancelled;
				continue;
			}
			// The compact packet has no CPU pose, event, graph, or mutation storage.
			// Its identity is self-contained; protected publication performs the one
			// live-record check before committing the corresponding authority state.
			CompactGpuVisualResult result;
			const CompactGpuVisualInput& input = evaluationState.compactGpuVisualWorkItems.inputs[compactIndex];
			result.stableInstanceId = input.stableInstanceId;
			result.stableSubmissionOrdinal = input.stableSubmissionOrdinal;
			result.sourcePoseGeneration = input.sourcePoseGeneration;
			result.presentationPoseGeneration = input.sourcePoseGeneration;
			result.generations = input.generations;
			result.reason = input.eligibilityReason;
			result.complete = true;
			if(!result.IsPublishableFor(input)) {
				evaluationState.compactGpuVisualCompletions.statuses[compactIndex] = AnimationStatus::Rejected;
				continue;
			}
			evaluationState.compactGpuVisualCompletions.results[compactIndex] = result;
			evaluationState.compactGpuVisualCompletions.statuses[compactIndex] = AnimationStatus::Ok;
			// A command needs broad capture to prepare its private playback state,
			// but can still evaluate as a GPU-only packet. Publish that packet's
			// real completion back to its owned broad row, so the later COW runtime
			// candidate can commit the seek without expecting a CPU evaluator run.
			if(auto* item = CompleteWorkForInputIndex(evaluationState.compactGpuVisualWorkItems.frameWorkIndices[compactIndex])) {
				if(item->input.handle != evaluationState.compactGpuVisualWorkItems.animators[compactIndex] ||
				   item->candidateAuthorityPublicationGeneration !=
					   evaluationState.compactGpuVisualWorkItems.sourceGenerations[compactIndex] ||
				   item->processingClass != AnimationProcessingClass::CompactGpuVisual) {
					evaluationState.compactGpuVisualCompletions.statuses[compactIndex] = AnimationStatus::Rejected;
					continue;
				}
				item->safePose = false;
				item->evaluationStatus = AnimationStatus::Ok;
				item->evaluated = true;
			}
			++locallyCompleted;
		}
		if(locallyCompleted != 0U) {
			evaluationState.compactGpuVisualCompletedCount.fetch_add(locallyCompleted, std::memory_order_acq_rel);
			evaluationState.evaluatedAnimatorCount.fetch_add(locallyCompleted, std::memory_order_acq_rel);
		}
		evaluationState.compactEvaluationNanoseconds.fetch_add(
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - evaluationStart).count()),
			std::memory_order_acq_rel);
	}

	void AnimationSystem::Impl::EvaluateCompactAnimationStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
															  const TerraEngine::Scheduling::FrameWorkloadPartition& partition,
															  TerraEngine::Scheduling::JobGraphContext& context) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Evaluate Compact GPU Visual Range", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(context.IsCancellationRequested()) {
			this->RequestFrameCancellation();
			return;
		}
		if(!this->runtimeActive.load(std::memory_order_acquire) ||
		   this->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch ||
		   this->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch)
			return;
		this->EvaluateCompactGpuVisualPartition(partition.itemBegin, partition.itemEnd);
	}

} // namespace TerraEngine::Animation
