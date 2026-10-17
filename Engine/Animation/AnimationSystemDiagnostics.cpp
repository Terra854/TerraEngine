#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	AnimationDiagnosticsSnapshot AnimationSystem::Impl::BuildActiveDiagnosticsLocked(std::uint64_t publicationGeneration,
																					 std::size_t detailedRecordCount) const noexcept {
		AnimationDiagnosticsSnapshot snapshot;
		snapshot.frameIndex = publicationGeneration;
		snapshot.candidateGeneration = publicationGeneration;
		snapshot.acceptedGeneration = publicationGeneration;
		snapshot.presentationGeneration = AnimationPresentationSystem::Instance().PresentationPublicationGeneration();
		snapshot.sceneGeneration = frameInputs.frameSceneGeneration;
		snapshot.fixedTick = frameInputs.frameTick;
		// Summary counters remain cheap on every active frame. Only the detail
		// level and bounded record count reflect a separately requested copied
		// publication; row construction itself happens in the method below.
		snapshot.detailLevel =
			detailedRecordCount == 0U ? AnimationDiagnosticsDetailLevel::Summary : AnimationDiagnosticsDetailLevel::Detailed;
		snapshot.dormant = false;
		AnimationDiagnosticsCounters& counters = snapshot.counters;
		// The indexed capture mapping is the authoritative full-frame population:
		// early compact records intentionally never construct FrameWorkItem entries.
		counters.activeAnimators = frameInputs.capturedInputWorkIndices.size();
		// This is the protected pre-copy admission count. It proves compact
		// candidates bypassed broad runtime-state capture even if a later immutable
		// lowering proof rejects the complete frame.
		counters.compactEligibleAnimators = evaluationState.compactPreCopyCaptureCount.load(std::memory_order_acquire);
		counters.compactProcessedAnimators = evaluationState.compactGpuVisualCompletedCount.load(std::memory_order_acquire);
		// FrameWorkItem is constructed only for the complete-capture branch, so its
		// count cannot be derived by subtracting compact packets (which also include
		// direct SoA entries and would underflow for an all-compact crowd).
		counters.completeCapturedAnimators = frameStorage.frameWork.size();
		counters.completeProcessedAnimators = evaluationState.workCompletions.size();
		// Finalization creates exactly one compact plan envelope for all proven
		// packets. This is a real plan-group count, not a per-animator estimate.
		counters.compactPlanGroups = evaluationState.compactGpuVisualWorkItems.empty() ? 0U : 1U;
		constexpr double kNanosecondsPerMillisecond = 1'000'000.0;
		snapshot.timings.compactCaptureMs =
			static_cast<double>(evaluationState.compactPlanningNanoseconds.load(std::memory_order_acquire)) / kNanosecondsPerMillisecond;
		snapshot.timings.completeCaptureMs =
			static_cast<double>(evaluationState.completePlanningNanoseconds.load(std::memory_order_acquire)) / kNanosecondsPerMillisecond;
		snapshot.timings.compactEvaluationMs =
			static_cast<double>(evaluationState.compactEvaluationNanoseconds.load(std::memory_order_acquire)) / kNanosecondsPerMillisecond;
		snapshot.timings.completeEvaluationMs =
			static_cast<double>(evaluationState.completeEvaluationNanoseconds.load(std::memory_order_acquire)) / kNanosecondsPerMillisecond;
		counters.schedulerMembershipChecks = 1U;
		counters.workerJobs =
			frameStorage.workPlan.executionPolicy == AnimationWorkExecutionPolicy::WorkerTasks ? frameStorage.workPlan.ranges.size() : 0U;
		counters.commandQueueDepth = commands == nullptr ? 0U : commands->PendingCount();
		counters.detailedRecords = detailedRecordCount;
		counters.fallbackCount = evaluationState.fallbackAnimatorCount.load(std::memory_order_acquire);

		for(const FrameWorkItem& item : frameStorage.frameWork) {
			if(item.evaluated)
				++counters.evaluatedAnimators;
			const AnimatorPlaybackRunState runState = item.playback.RunState();
			if(item.visualEvaluationSuspended || runState == AnimatorPlaybackRunState::Paused ||
			   item.binding.updatePolicy == AnimatorUpdatePolicy::Manual) {
				++counters.suspendedAnimators;
			}
			const bool successfulGpuVisual = item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual &&
											 item.gpuLowering.has_value() && item.evaluationStatus == AnimationStatus::Ok && !item.safePose;
			if(item.executionDecision.transition.occurred) {
				if(item.executionDecision.transition.from == AnimationEffectiveExecutionMode::GpuVisual) {
					++counters.compactToCompleteTransitions;
				} else if(item.executionDecision.transition.to == AnimationEffectiveExecutionMode::GpuVisual) {
					++counters.completeToCompactTransitions;
				}
			}
			if(item.evaluationStatus != AnimationStatus::Ok || item.safePose ||
			   (!successfulGpuVisual && item.fallbackDecision.source != AnimationFallbackSource::Current)) {
				++counters.degradedAnimators;
			}
			if(item.evaluationStatus == AnimationStatus::StaleHandle) {
				++counters.staleWorkItems;
			}
			if(item.pose)
				++counters.poseAllocations;
			if(!item.binding.gpuExecution.resourcesResident)
				++counters.residencyMisses;
			else
				++counters.residencyHits;
			if(item.executionDecision.modeChanged)
				++counters.modeSwitchCount;

			const std::uint64_t jointCount = item.binding.jointStableIds.size();
			switch(item.executionDecision.effectiveMode) {
			case AnimationEffectiveExecutionMode::GpuVisual:
				++counters.gpuVisualAnimators;
				if(!item.visualEvaluationSuspended) {
					counters.gpuEvaluatedJoints += jointCount;
				}
				break;
			case AnimationEffectiveExecutionMode::HybridAuthoritative:
				++counters.hybridAuthoritativeAnimators;
				counters.cpuSampledJoints += jointCount;
				counters.gpuEvaluatedJoints += jointCount;
				break;
			case AnimationEffectiveExecutionMode::CpuDeterministicFallback:
			default:
				++counters.cpuFallbackAnimators;
				if(!item.visualEvaluationSuspended) {
					counters.cpuSampledJoints += jointCount;
				}
				break;
			}
			// GPU work is counted from the protected presentation envelope below.
			// A lowering attempt is not necessarily accepted, batched, or recorded by
			// the active graphics backend, so it cannot inflate observable plan counters.
			if(item.ragdollLifecycle == RagdollLifecycleState::Active || item.ragdollLifecycle == RagdollLifecycleState::Recovering) {
				++counters.physicsHandoffs;
			}
			if(item.selectedPublication && item.selectedPublication->snapshot && item.selectedPublication->snapshot->pose) {
				const PoseBuffer& pose = *item.selectedPublication->snapshot->pose;
				counters.eventQueueDepth += pose.Events().size();
				counters.managedCallbacks += pose.Events().size();
				if(pose.RootMotionDelta().has_value())
					++counters.rootMotionQueueDepth;
			}
			const std::size_t fallbackReason = static_cast<std::size_t>(item.fallbackDecision.reason);
			if(fallbackReason < counters.fallbackReasonCounts.size() && item.fallbackDecision.source != AnimationFallbackSource::Current) {
				++counters.fallbackReasonCounts[fallbackReason];
			}
		}
		std::uint32_t compactStageMask = 0U;
		for(std::size_t compactIndex = 0U; compactIndex < evaluationState.compactGpuVisualWorkItems.size(); ++compactIndex) {
			const std::size_t inputIndex = evaluationState.compactGpuVisualWorkItems.frameWorkIndices[compactIndex];
			if(inputIndex >= frameInputs.capturedInputWorkIndices.size())
				continue;
			const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
			if(captured.compact) {
				// Early compact records own no FrameWorkItem; their validated stage
				// mask remains in the compact SoA column through publication.
				if(captured.storageIndex < frameInputs.compactGpuVisualCaptures.size()) {
					compactStageMask |= frameInputs.compactGpuVisualCaptures.stageMasks[captured.storageIndex];
				}
			} else if(captured.storageIndex < frameStorage.frameWork.size()) {
				compactStageMask |= frameStorage.frameWork[captured.storageIndex].gpuStageMask;
			}
		}
		while(compactStageMask != 0U) {
			++counters.compactDispatches;
			compactStageMask &= compactStageMask - 1U;
		}
		const std::shared_ptr<const PublishedGpuAnimationPresentationPlan> presentation =
			AnimationPresentationSystem::Instance().LoadPublishedGpuAnimationPresentationPlan();
		if(presentation && presentation->topology.IsComplete()) {
			snapshot.topologyGeneration = presentation->topology.topologyGeneration;
			// Compact-only crowds do not own FrameWorkItem lowering records. The
			// accepted retained topology is therefore the authoritative renderer
			// generation for the same immutable presentation envelope.
			snapshot.rendererGeneration = presentation->topology.generations.rendererGeneration;
			snapshot.timings.topologyBuildMs = presentation->topologyBuildMs;
			snapshot.timings.topologyRefreshMs = presentation->topologyRefreshMs;
			if(presentation->topologyReused)
				++counters.retainedTopologyHits;
			else
				++counters.retainedTopologyMisses;
		}
		if(presentation && !presentation->safeFallback && presentation->loweringStatus == GpuAnimationLoweringStatus::Ok &&
		   presentation->planStatus == GpuAnimationPlanStatus::Ok && presentation->sourceGeneration == publicationGeneration) {
			// Command recording copies each dispatchCommands entry into the backend
			// plan and emits one compute dispatch. Shader payload records may outnumber
			// those actual dispatch calls, so report the executable plan instead.
			counters.gpuBatches = presentation->plan.Batches().size();
			counters.gpuDispatches = presentation->plan.DispatchCommands().size();
		}
		for(const FrameWorkItem& item : frameStorage.frameWork) {
			if(item.gpuLowering && item.gpuLowering->visual.generations.rendererGeneration != 0U) {
				const std::uint64_t candidateRendererGeneration = item.gpuLowering->visual.generations.rendererGeneration;
				if(snapshot.rendererGeneration == 0U) {
					snapshot.rendererGeneration = candidateRendererGeneration;
				} else if(snapshot.rendererGeneration != candidateRendererGeneration) {
					// Mixed renderer generations cannot name one retained topology.
					snapshot.rendererGeneration = 0U;
					break;
				}
			}
			if(item.evaluationStatus != AnimationStatus::Ok || !item.evaluated) {
				++counters.rejectedCandidateCount;
			}
		}
		if(counters.rejectedCandidateCount != 0U) {
			snapshot.rejectedGeneration = publicationGeneration;
		}
		for(const AnimationCommand& command : frameInputs.capturedCommands) {
			if(command.kind == AnimationCommandKind::SetParameter || command.kind == AnimationCommandKind::FireTrigger) {
				++counters.parameterQueueDepth;
			}
		}
		for(const AnimationStatus status : frameInputs.capturedCommandStatuses) {
			if(status != AnimationStatus::Ok && status != AnimationStatus::NotReady) {
				++counters.rejectedBatches;
			}
		}
		return snapshot;
	}

	AnimationDetailedDiagnosticsSnapshot
	AnimationSystem::Impl::BuildActiveDetailedDiagnosticsLocked(std::uint64_t publicationGeneration) const {
		// This method is called only at ProtectedCommit while a scoped consumer
		// lease is active. It copies frame-owned values after worker evaluation;
		// no worker or editor reader touches live RuntimeRecord storage here.
		AnimationDetailedDiagnosticsSnapshot snapshot;
		snapshot.frameIndex = publicationGeneration;
		snapshot.collapsed = false;
		snapshot.animators.reserve(frameStorage.frameWork.size());
		for(std::size_t index = 0U; index < frameStorage.frameWork.size(); ++index) {
			const FrameWorkItem& item = frameStorage.frameWork[index];
			AnimationAnimatorDiagnosticRecord record;
			record.stableInstanceId =
				(static_cast<std::uint64_t>(item.input.handle.slotIndex) << 32U) | static_cast<std::uint64_t>(item.input.handle.generation);
			record.instanceSlot = item.input.handle.slotIndex;
			record.instanceGeneration = item.input.handle.generation;
			record.requestedPolicy = item.binding.requestedExecutionPolicy;
			record.effectiveMode = item.executionDecision.effectiveMode;
			record.selectionReason = item.executionDecision.reason;
			record.fallbackReason = item.executionDecision.reason;
			record.poseResidency =
				item.binding.gpuExecution.resourcesResident
					? (item.gpuLowering.has_value() ? AnimationPoseResidencyState::GpuResident : AnimationPoseResidencyState::UploadPending)
					: (item.pose ? AnimationPoseResidencyState::CpuOnly : AnimationPoseResidencyState::Unavailable);
			// GPU Visual owns no CPU-authoritative pose subset. Hybrid and CPU
			// modes report the already-resolved bounded consumer closure instead.
			record.cpuAuthoritativeJointCount =
				item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
					? 0U
					: static_cast<std::uint32_t>(
						  std::min<std::size_t>(item.resolvedConsumers.closure.joints.size(), std::numeric_limits<std::uint32_t>::max()));
			record.gpuStageMask = item.gpuStageMask;
			record.batchOrdinal = static_cast<std::uint32_t>(index);
			if(item.gpuLowering) {
				record.gpuEvaluatedElements = item.binding.jointStableIds.size() + item.binding.morphStableIds.size();
			}
			record.modeSwitchCount = item.executionDecision.modeChanged ? 1U : 0U;
			snapshot.animators.push_back(record);

			if(item.executionDecision.transition.occurred &&
			   snapshot.modeSwitchHistory.size() < GetAnimationRuntimeLimit(AnimationLimit::DetailedDiagnosticHistoryFrames).hardMaximum) {
				const AnimationModeContinuityToken& continuity = item.executionDecision.transition.continuity;
				// This compact opaque fingerprint ties a copied transition to its
				// continuity token without exposing mutable controller state.
				const std::uint64_t fingerprint = record.stableInstanceId ^ static_cast<std::uint64_t>(continuity.transitionBoundaryTick) ^
												  static_cast<std::uint64_t>(continuity.stateTimeTick) ^ continuity.controllerState ^
												  continuity.parameterSnapshot ^ continuity.transition ^ continuity.synchronizationState ^
												  continuity.eventWatermark ^ continuity.rootMotion ^ continuity.motionMatch ^
												  continuity.ragdollBlend ^ continuity.pose;
				snapshot.modeSwitchHistory.push_back({record.stableInstanceId, item.input.tick, item.executionDecision.transition.from,
													  item.executionDecision.transition.to, item.executionDecision.reason, fingerprint});
			}
		}
		return snapshot;
	}

	std::size_t AnimationSystem::PendingCommandCount() const {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		return impl_->commands == nullptr ? 0U : impl_->commands->PendingCount();
	}

	std::size_t AnimationSystem::CapturedCommandCount() const {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		return impl_->frameInputs.capturedCommands.size();
	}

	std::size_t AnimationSystem::ScheduledAnimatorCount() const noexcept {
		return impl_->evaluationState.scheduledAnimatorCount.load(std::memory_order_acquire);
	}

	std::size_t AnimationSystem::PublishedAnimatorCount() const noexcept {
		return impl_->evaluationState.publishedAnimatorCount.load(std::memory_order_acquire);
	}

	std::uint64_t AnimationSystem::AuthorityPublicationGeneration() const noexcept {
		return impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
	}

	std::shared_ptr<const AnimationAuthorityFrame> AnimationSystem::LoadPublishedAuthorityFrame() const noexcept {
		return impl_->authorityPublisher.Load();
	}

	AnimationProductionIntegrationSnapshot AnimationSystem::ReadProductionIntegrationSnapshot() const noexcept {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		std::scoped_lock finalizeLock(impl_->evaluationState.workPlanFinalizeMutex);
		AnimationProductionIntegrationSnapshot snapshot;
		snapshot.planStatus = impl_->frameStorage.workPlan.status;
		snapshot.rejectionReason = impl_->evaluationState.workPlanRejectionReason;
		snapshot.rejectionItemIndex = impl_->evaluationState.workPlanRejectionItemIndex;
		snapshot.executionPolicy = impl_->frameStorage.workPlan.executionPolicy;
		snapshot.availableWorkerCount = impl_->evaluationState.availableWorkerCount;
		snapshot.selectedWorkerCount = impl_->frameStorage.workPlan.selectedWorkerCount;
		snapshot.itemCount = impl_->frameStorage.workPlan.items.size();
		snapshot.rangeCount = impl_->frameStorage.workPlan.ranges.size();
		snapshot.scratchBytes = impl_->frameStorage.workPlan.totalScratchBytes;
		snapshot.cacheEntryCount = impl_->persistentReuse.evaluationCache.Size();
		snapshot.reusedItemCount = impl_->evaluationState.cacheReusedAnimatorCount.load(std::memory_order_acquire) +
								   impl_->evaluationState.sharedPoseAnimatorCount.load(std::memory_order_acquire);
		snapshot.fallbackItemCount = impl_->evaluationState.fallbackAnimatorCount.load(std::memory_order_acquire);
		snapshot.bindingEpisodeCount = impl_->bindingEpisodeCount.load(std::memory_order_acquire);
		snapshot.bindingReconciliationNanoseconds = impl_->bindingReconciliationNanoseconds.load(std::memory_order_acquire);
		snapshot.bindingReconciledCount = impl_->bindingReconciledCount.load(std::memory_order_acquire);
		snapshot.bindingFailureCount = impl_->bindingFailureCount.load(std::memory_order_acquire);
		snapshot.memory = impl_->persistentReuse.memoryController.Snapshot();
		snapshot.deterministicPublicationValidated = impl_->evaluationState.deterministicPublicationValidated;
		snapshot.cancellationRequested = impl_->evaluationState.frameCancellationRequested.load(std::memory_order_acquire);
		return snapshot;
	}

	std::string_view ToString(AnimationPlanRejectionReason reason) noexcept {
		switch(reason) {
		case AnimationPlanRejectionReason::None:
			return "None";
		case AnimationPlanRejectionReason::PlanningIncomplete:
			return "PlanningIncomplete";
		case AnimationPlanRejectionReason::PlaybackAdvance:
			return "PlaybackAdvance";
		case AnimationPlanRejectionReason::GenerationOverflow:
			return "GenerationOverflow";
		case AnimationPlanRejectionReason::ExecutionModeNotGpuVisual:
			return "ExecutionModeNotGpuVisual";
		case AnimationPlanRejectionReason::FrameIdentityInvalid:
			return "FrameIdentityInvalid";
		case AnimationPlanRejectionReason::GpuLoweringRejected:
			return "GpuLoweringRejected";
		case AnimationPlanRejectionReason::ResidentPayloadInvalid:
			return "ResidentPayloadInvalid";
		case AnimationPlanRejectionReason::CompactClassificationRejected:
			return "CompactClassificationRejected";
		case AnimationPlanRejectionReason::CompactInputIncomplete:
			return "CompactInputIncomplete";
		case AnimationPlanRejectionReason::SchedulerRejected:
			return "SchedulerRejected";
		case AnimationPlanRejectionReason::MemoryReservationRejected:
			return "MemoryReservationRejected";
		case AnimationPlanRejectionReason::AllocationFailure:
			return "AllocationFailure";
		}
		return "Unknown";
	}
} // namespace TerraEngine::Animation
