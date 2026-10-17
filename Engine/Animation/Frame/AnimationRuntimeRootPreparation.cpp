#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	[[nodiscard]] bool AnimationSystem::Impl::PrepareRuntimeRootCandidate(std::uint64_t authorityGeneration) {
		frameInputs.commandFrameFailurePhase = "RuntimeCandidateCompletionGate";
		// Runtime evaluator state is internal mutable state, but it must advance with
		// the same all-or-none visibility as the immutable authority frame.  Build a
		// copy-on-write root on this worker: unchanged slots retain their shared
		// record, while every participating animator receives a private next record.
		// The protected publisher below then performs only pointer/generation checks
		// and two root exchanges, independent of crowd size.
		if(authorityGeneration == 0U || PollFrameCancellation() || !evaluationState.workPlanValidated ||
		   frameStorage.workRangeLedger == nullptr || !frameStorage.workRangeLedger->AllCompleted() ||
		   frameInputs.capturedInputWorkIndices.size() != evaluationState.plannedAnimatorCount.load(std::memory_order_acquire) ||
		   frameInputs.capturedInputWorkIndices.size() != evaluationState.evaluatedAnimatorCount.load(std::memory_order_acquire) ||
		   evaluationState.workCompletions.size() != frameStorage.workPlan.items.size() ||
		   evaluationState.compactGpuVisualCompletedCount.load(std::memory_order_acquire) !=
			   evaluationState.compactGpuVisualWorkItems.size())
			return false;

		try {
			const std::shared_ptr<RuntimeRecordRoot> expected = runtimeRecords.load(std::memory_order_acquire);
			if(!expected)
				return false;
			const std::shared_ptr<const CompactRuntimeStateRoot> expectedCompact = compactRuntimeStates.load(std::memory_order_acquire);
			if(expectedCompact != frameInputs.compactRuntimeStateCaptureRoot) {
				frameInputs.commandFrameFailurePhase = "RuntimeCandidateCompactRootChanged";
				return false;
			}
			auto nextCompact = std::make_shared<CompactRuntimeStateRoot>(expected->size());
			const std::uint64_t liveSceneGeneration = ObjectManager::instance().GetSceneGeneration();
			evaluationState.deterministicPublicationOrder.resize(frameInputs.capturedInputWorkIndices.size());
			std::iota(evaluationState.deterministicPublicationOrder.begin(), evaluationState.deterministicPublicationOrder.end(), 0U);
			evaluationState.deterministicPublicationValidated = true;
			auto specialistSidecars = std::make_shared<SealedSpecialistSidecarBatch>();
			// Explicit root-motion requests arrive at protected capture. Copy them
			// into this candidate so a rejected root leaves the live queue intact.
			specialistSidecars->explicitRootMotion = pendingRootMotionApplications;
			const bool allInputsCompact =
				frameInputs.capturedInputWorkIndices.size() == frameInputs.compactGpuVisualCaptures.size() &&
				std::all_of(frameInputs.capturedInputWorkIndices.begin(), frameInputs.capturedInputWorkIndices.end(),
							[](const CapturedInputWorkIndex& captured) { return captured.compact; });
			// Compact candidates must still refer to the exact root captured before
			// evaluation; otherwise none of their private state may become visible.
			if(allInputsCompact && expected != frameInputs.compactRuntimeCaptureRoot)
				return false;
			// A few thousand scalar rows fit comfortably in cache and complete faster
			// in one pass than through dozens of task submissions and future joins.
			// Preserve the worker partition for truly large crowds where each task has
			// enough independent rows to repay that coordination cost.
			constexpr std::size_t kMinimumParallelCompactRuntimeRows = 8192U;
			if(allInputsCompact && (frameInputs.capturedInputWorkIndices.size() < kMinimumParallelCompactRuntimeRows ||
									evaluationState.availableWorkerCount <= 1U)) {
				TERRAENGINE_PROFILE_SCOPE("Animation: Build Compact Runtime Root Serial",
										  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				std::vector<std::uint8_t> claimedSlots(nextCompact->size(), 0U);
				for(std::size_t inputIndex = 0U; inputIndex < frameInputs.capturedInputWorkIndices.size(); ++inputIndex) {
					const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
					if(captured.storageIndex >= frameInputs.compactGpuVisualCaptures.size())
						return false;
					const std::size_t compactIndex = captured.storageIndex;
					const AnimatorInstanceHandle handle = frameInputs.compactGpuVisualCaptures.inputs[compactIndex].handle;
					if(!handle.IsValid() || handle.slotIndex >= expected->size() || handle.slotIndex >= nextCompact->size() ||
					   claimedSlots[handle.slotIndex] != 0U)
						return false;
					const std::shared_ptr<RuntimeRecord>& current = expected->at(handle.slotIndex);
					const CompactRuntimeState* priorCompactState =
						FindCompactRuntimeState(frameInputs.compactRuntimeStateCaptureRoot, handle);
					const AnimationGeneration priorPoseGeneration =
						priorCompactState ? priorCompactState->poseGeneration : (current ? current->poseGeneration : 0U);
					if(!current || current->handle != handle ||
					   frameInputs.compactGpuVisualCaptures.candidatePoseGenerations[compactIndex] != priorPoseGeneration + 1U ||
					   frameInputs.compactGpuVisualCaptures.authorityGeneration != authorityGeneration ||
					   frameInputs.compactGpuVisualCaptures.evaluationStatuses[compactIndex] != AnimationStatus::Ok)
						return false;

					claimedSlots[handle.slotIndex] = 1U;
					CompactRuntimeState& state = nextCompact->at(handle.slotIndex);
					state.handle = handle;
					state.playback = frameInputs.compactGpuVisualCaptures.playbacks[compactIndex];
					state.poseGeneration = frameInputs.compactGpuVisualCaptures.candidatePoseGenerations[compactIndex];
					state.stateGeneration = frameInputs.compactGpuVisualCaptures.stateGenerations[compactIndex];
					state.executionModeGeneration = frameInputs.compactGpuVisualCaptures.executionModeGenerations[compactIndex];
					state.bindingContentGeneration = current->contentGeneration;
					state.executionModeState = frameInputs.compactGpuVisualCaptures.executionDecisions[compactIndex].nextState;
					if(current->rollbackHistoryActive) {
						specialistSidecars->rollbackHistoryAnimators.push_back(handle);
					}
				}
				authorityState.publishedAuthorityCommit.expectedRuntimeRoot = std::move(expected);
				authorityState.publishedAuthorityCommit.nextRuntimeRoot = authorityState.publishedAuthorityCommit.expectedRuntimeRoot;
				authorityState.publishedAuthorityCommit.expectedCompactRuntimeRoot = expectedCompact;
				authorityState.publishedAuthorityCommit.nextCompactRuntimeRoot = std::move(nextCompact);
				authorityState.publishedAuthorityCommit.specialistSidecars = std::move(specialistSidecars);
				return true;
			}
			if(allInputsCompact) {
				TERRAENGINE_PROFILE_SCOPE("Animation: Build Compact Runtime Root", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				// Validate unique target slots before workers write disjoint vector
				// elements. This preserves the atomic failure behavior while allowing the
				// expensive RuntimeRecord copies to scale across the available pool.
				std::vector<AnimatorInstanceHandle> compactHandles(frameInputs.capturedInputWorkIndices.size());
				std::vector<std::uint8_t> claimedSlots(nextCompact->size(), 0U);
				std::vector<std::uint8_t> rollbackActive(frameInputs.capturedInputWorkIndices.size(), 0U);
				for(std::size_t inputIndex = 0U; inputIndex < frameInputs.capturedInputWorkIndices.size(); ++inputIndex) {
					const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
					if(captured.storageIndex >= frameInputs.compactGpuVisualCaptures.size())
						return false;
					const AnimatorInstanceHandle handle = frameInputs.compactGpuVisualCaptures.inputs[captured.storageIndex].handle;
					if(!handle.IsValid() || handle.slotIndex >= expected->size() || handle.slotIndex >= nextCompact->size() ||
					   claimedSlots[handle.slotIndex] != 0U)
						return false;
					const std::shared_ptr<RuntimeRecord>& current = expected->at(handle.slotIndex);
					const CompactRuntimeState* priorCompactState =
						FindCompactRuntimeState(frameInputs.compactRuntimeStateCaptureRoot, handle);
					const AnimationGeneration priorPoseGeneration =
						priorCompactState ? priorCompactState->poseGeneration : (current ? current->poseGeneration : 0U);
					if(!current || current->handle != handle ||
					   frameInputs.compactGpuVisualCaptures.candidatePoseGenerations[captured.storageIndex] != priorPoseGeneration + 1U ||
					   frameInputs.compactGpuVisualCaptures.authorityGeneration != authorityGeneration ||
					   frameInputs.compactGpuVisualCaptures.evaluationStatuses[captured.storageIndex] != AnimationStatus::Ok)
						return false;
					claimedSlots[handle.slotIndex] = 1U;
					compactHandles[inputIndex] = handle;
					rollbackActive[inputIndex] = current->rollbackHistoryActive ? 1U : 0U;
				}

				const std::size_t workerCount =
					std::min<std::size_t>(evaluationState.availableWorkerCount, frameInputs.capturedInputWorkIndices.size());
				const std::size_t chunkSize = (frameInputs.capturedInputWorkIndices.size() + workerCount - 1U) / workerCount;
				std::vector<std::future<void>> futures;
				futures.reserve(workerCount);
				// This guard dies before every input captured by admitted children.
				TerraEngine::Tasks::ScopedTaskSettlement<void> futuresSettlement(
					futures, TerraEngine::Tasks::NestedTaskGroup::AnimationCompactRuntime);
				for(std::size_t workerIndex = 0U; workerIndex < workerCount; ++workerIndex) {
					const std::size_t begin = workerIndex * chunkSize;
					if(begin >= frameInputs.capturedInputWorkIndices.size())
						break;
					const std::size_t end = std::min(frameInputs.capturedInputWorkIndices.size(), begin + chunkSize);
					futuresSettlement.CreateAndEnqueue(ThreadPool::instance(), [this, begin, end, &compactHandles, &nextCompact]() {
						TERRAENGINE_PROFILE_SCOPE("Animation Task: Build Compact Runtime State",
												  TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
						for(std::size_t inputIndex = begin; inputIndex < end; ++inputIndex) {
							const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
							const AnimatorInstanceHandle handle = compactHandles[inputIndex];
							const std::size_t compactIndex = captured.storageIndex;
							CompactRuntimeState& state = nextCompact->at(handle.slotIndex);
							state.handle = handle;
							state.playback = frameInputs.compactGpuVisualCaptures.playbacks[compactIndex];
							state.poseGeneration = frameInputs.compactGpuVisualCaptures.candidatePoseGenerations[compactIndex];
							state.stateGeneration = frameInputs.compactGpuVisualCaptures.stateGenerations[compactIndex];
							state.executionModeGeneration = frameInputs.compactGpuVisualCaptures.executionModeGenerations[compactIndex];
							state.bindingContentGeneration =
								frameInputs.compactGpuVisualCaptures.runtimeRecords[compactIndex]->contentGeneration;
							state.executionModeState = frameInputs.compactGpuVisualCaptures.executionDecisions[compactIndex].nextState;
						}
					});
				}
				futuresSettlement.GetAll();
				if(PollFrameCancellation())
					return false;
				for(std::size_t inputIndex = 0U; inputIndex < rollbackActive.size(); ++inputIndex) {
					if(rollbackActive[inputIndex] != 0U) {
						specialistSidecars->rollbackHistoryAnimators.push_back(compactHandles[inputIndex]);
					}
				}
				authorityState.publishedAuthorityCommit.expectedRuntimeRoot = std::move(expected);
				authorityState.publishedAuthorityCommit.nextRuntimeRoot = authorityState.publishedAuthorityCommit.expectedRuntimeRoot;
				authorityState.publishedAuthorityCommit.expectedCompactRuntimeRoot = expectedCompact;
				authorityState.publishedAuthorityCommit.nextCompactRuntimeRoot = std::move(nextCompact);
				authorityState.publishedAuthorityCommit.specialistSidecars = std::move(specialistSidecars);
				return true;
			}

			// Only complete/specialist rows clone their broad evaluator record.
			// Compact rows publish into the narrow scalar root below, so thousands
			// of visual animators do not copy bindings, graphs, or event vectors.
			auto next = std::make_shared<RuntimeRecordRoot>(*expected);
			if(!PrepareCompleteRuntimeRecords(authorityGeneration, expected, next, nextCompact, specialistSidecars, liveSceneGeneration))
				return false;

			authorityState.publishedAuthorityCommit.expectedRuntimeRoot = std::move(expected);
			authorityState.publishedAuthorityCommit.nextRuntimeRoot = std::move(next);
			authorityState.publishedAuthorityCommit.expectedCompactRuntimeRoot = expectedCompact;
			authorityState.publishedAuthorityCommit.nextCompactRuntimeRoot = std::move(nextCompact);
			authorityState.publishedAuthorityCommit.specialistSidecars = std::move(specialistSidecars);
			frameInputs.commandFrameFailurePhase = nullptr;
			return true;
		} catch(const std::bad_alloc&) {
			// The token owns every candidate sidecar. Dropping it makes a failed
			// worker seal unobservable without touching any live specialist queue.
			authorityState.publishedAuthorityCommit = {};
			return false;
		}
	}

	bool AnimationSystem::Impl::PrepareCompleteRuntimeRecords(std::uint64_t authorityGeneration,
															  const std::shared_ptr<RuntimeRecordRoot>& expected,
															  const std::shared_ptr<RuntimeRecordRoot>& next,
															  const std::shared_ptr<CompactRuntimeStateRoot>& nextCompact,
															  const std::shared_ptr<SealedSpecialistSidecarBatch>& specialistSidecars,
															  std::uint64_t liveSceneGeneration) {
		for(std::size_t inputIndex = 0U; inputIndex < frameInputs.capturedInputWorkIndices.size(); ++inputIndex) {
			const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[inputIndex];
			const AnimatorInstanceHandle handle = captured.compact
													  ? frameInputs.compactGpuVisualCaptures.inputs[captured.storageIndex].handle
													  : frameStorage.frameWork[captured.storageIndex].input.handle;
			if(!handle.IsValid() || handle.slotIndex >= expected->size() || handle.slotIndex >= next->size())
				return false;
			const std::shared_ptr<RuntimeRecord>& current = expected->at(handle.slotIndex);
			if(!current || current->handle != handle)
				return false;
			if(current->rollbackHistoryActive) {
				specialistSidecars->rollbackHistoryAnimators.push_back(handle);
			}

			if(captured.compact) {
				const std::size_t compactIndex = captured.storageIndex;
				const CompactRuntimeState* priorCompactState = FindCompactRuntimeState(frameInputs.compactRuntimeStateCaptureRoot, handle);
				const AnimationGeneration priorPoseGeneration =
					priorCompactState ? priorCompactState->poseGeneration : current->poseGeneration;
				if(compactIndex >= frameInputs.compactGpuVisualCaptures.size() ||
				   frameInputs.compactGpuVisualCaptures.candidatePoseGenerations[compactIndex] != priorPoseGeneration + 1U ||
				   frameInputs.compactGpuVisualCaptures.authorityGeneration != authorityGeneration ||
				   frameInputs.compactGpuVisualCaptures.evaluationStatuses[compactIndex] != AnimationStatus::Ok)
					return false;
				CompactRuntimeState& state = nextCompact->at(handle.slotIndex);
				state.handle = handle;
				state.playback = frameInputs.compactGpuVisualCaptures.playbacks[compactIndex];
				state.poseGeneration = frameInputs.compactGpuVisualCaptures.candidatePoseGenerations[compactIndex];
				state.stateGeneration = frameInputs.compactGpuVisualCaptures.stateGenerations[compactIndex];
				state.executionModeGeneration = frameInputs.compactGpuVisualCaptures.executionModeGenerations[compactIndex];
				state.bindingContentGeneration = current->contentGeneration;
				state.executionModeState = frameInputs.compactGpuVisualCaptures.executionDecisions[compactIndex].nextState;
				continue;
			}

			if(captured.storageIndex >= frameStorage.frameWork.size())
				return false;
			// Complete/specialist animation owns mutable graph, event, and pose
			// state, so only these uncommon rows require a COW record clone.
			auto record = std::make_shared<RuntimeRecord>(*current);
			FrameWorkItem& item = frameStorage.frameWork[captured.storageIndex];
			const bool preservesPrior = item.cacheReused || item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration;
			if(preservesPrior) {
				item.liveFallbackGenerations = BuildLiveFallbackGenerations(*current, item, liveSceneGeneration);
			}
			if(current->descriptor.componentGeneration != item.input.descriptor.componentGeneration ||
			   (preservesPrior ? item.selectedPublication == nullptr || item.selectedPublication->poseGeneration != current->poseGeneration
							   : item.candidatePoseGeneration != current->poseGeneration + 1U) ||
			   item.candidateAuthorityPublicationGeneration != authorityGeneration || !item.evaluated) {
				frameInputs.commandFrameFailurePhase = "RuntimeCandidateCompleteGenerationGate";
				return false;
			}

			record->playback = item.playback;
			if(item.compactCaptureOnly) {
				record->poseGeneration = item.candidatePoseGeneration;
				record->stateGeneration = item.candidateStateGeneration;
				record->executionModeGeneration = item.candidateExecutionModeGeneration;
				record->executionModeState = item.executionDecision.nextState;
				next->Set(handle.slotIndex, std::move(record));
				continue;
			}
			if(item.eventStateCaptured)
				record->eventStream = item.eventStream;
			if(item.controllerStateCaptured) {
				record->graphState = item.candidateGraphState;
				record->transition = item.transition;
			}
			record->binding.stateId = item.binding.stateId;
			const bool clipBindingChanged =
				record->binding.clip != item.binding.clip || record->binding.clipGeneration != item.binding.clipGeneration ||
				record->binding.clipDeterministicContentFingerprint != item.binding.clipDeterministicContentFingerprint ||
				record->binding.streamedSegmentPayloads != item.binding.streamedSegmentPayloads;
			if(clipBindingChanged) {
				record->binding.clip = item.binding.clip;
				record->binding.clipGeneration = item.binding.clipGeneration;
				record->binding.clipDeterministicContentFingerprint = item.binding.clipDeterministicContentFingerprint;
				record->binding.residentSegments = item.binding.residentSegments;
				record->binding.streamedSegmentPayloads = item.binding.streamedSegmentPayloads;
			}
			record->latestRetargetMetrics = item.retargetActive ? item.retargetMetrics : RetargetEvaluationMetrics{};
			record->latestRetargetDeterministicHash = item.retargetActive ? item.retargetDeterministicHash : 0U;
			// A retained GPU-visual authority row intentionally has no CPU pose
			// snapshot. Its row generation is still the exact published owner
			// generation, so keep that identity during a one-frame asset fallback.
			// Requiring a snapshot here would permanently reject every later root.
			record->poseGeneration = preservesPrior && item.selectedPublication ? item.selectedPublication->poseGeneration
									 : item.selectedPublication && item.selectedPublication->snapshot
										 ? item.selectedPublication->snapshot->publicationGeneration
										 : item.candidatePoseGeneration;
			record->stateGeneration = item.candidateStateGeneration;
			const bool parametersChanged = record->parameterGeneration != item.candidateParameterGeneration;
			record->parameterGeneration = item.candidateParameterGeneration;
			record->executionModeGeneration = item.candidateExecutionModeGeneration;
			record->executionModeState = item.executionDecision.nextState;
			if(parametersChanged) {
				record->parameters = item.parameters;
				record->layerWeights = item.layerWeights;
			}
			item.capturedStateGeneration = record->stateGeneration;
			item.capturedParameterGeneration = record->parameterGeneration;
			item.capturedExecutionModeGeneration = record->executionModeGeneration;
			item.bindingCaptureContentGeneration = record->contentGeneration;
			item.runtimeStateCaptureCommitted = true;
			record->appliedMotionQueryId = item.appliedMotionQueryId;
			if(item.ragdollPhysicsPose)
				record->ragdollGeneration = item.ragdollGeneration;
			if(item.ragdollGeneration.IsValid() &&
			   (item.ragdollLifecycle == RagdollLifecycleState::Active || item.ragdollLifecycle == RagdollLifecycleState::Recovering)) {
				PreparedRagdollPublication publication;
				publication.runtimeRecord = record;
				publication.motorDriveCount = static_cast<std::uint32_t>(
					std::min<std::size_t>(item.ragdollMotorDrives.size(), std::numeric_limits<std::uint32_t>::max()));
				if(!item.ragdollMotorDrives.empty()) {
					RagdollPhysicsRequest request;
					request.kind = RagdollPhysicsRequestKind::UpdateMotors;
					request.ragdollId = record->ragdollId;
					request.generation = item.ragdollGeneration;
					request.expectedCurrentGeneration = item.ragdollGeneration;
					request.motorDrives = item.ragdollMotorDrives;
					request.correlationId = record->ragdollId ^ authorityGeneration ^ 0x4D4F544F52000000ULL;
					if(request.correlationId == 0U)
						request.correlationId = 1U;
					request.producerOrder = 1U;
					publication.motorRequest = std::move(request);
				}
				if(item.ragdollRecoveryBlendComplete && record->pendingRagdollCorrelationId == 0U) {
					RagdollGenerationStamp generation = item.ragdollGeneration;
					generation.publicationGeneration = authorityGeneration;
					generation.fixedStep = std::max<AnimationFixedTick>(1, frameInputs.frameTick);
					RagdollPhysicsRequest request;
					request.kind = RagdollPhysicsRequestKind::Deactivate;
					request.ragdollId = record->ragdollId;
					request.generation = generation;
					request.expectedCurrentGeneration = item.ragdollGeneration;
					request.correlationId = record->ragdollId ^ authorityGeneration ^ 0x5245434F56455259ULL;
					if(request.correlationId == 0U)
						request.correlationId = 1U;
					request.producerOrder = 2U;
					publication.deactivateRequest = std::move(request);
				}
				specialistSidecars->ragdoll.push_back(std::move(publication));
			}
			if(item.proceduralStateObserved) {
				specialistSidecars->procedural.push_back(
					{{item.input.handle.slotIndex, item.input.handle.generation}, item.proceduralResults});
			}
			if(item.retargetActive && item.sampledSourceSkeleton) {
				NativeAnimationRetargetSnapshot snapshot{};
				snapshot.header = MakeNativeHeader(NativeAnimationRecordKind::RetargetSnapshot, sizeof(snapshot));
				snapshot.status = ToNativeStatus(item.retargetEvaluationStatus);
				snapshot.animator = {item.input.handle.slotIndex, item.input.handle.generation};
				snapshot.profileAssetId = item.activeRetargetProfileId;
				snapshot.profileGeneration = item.activeRetargetProfileGeneration;
				snapshot.sourceSkeletonGeneration = item.binding.retargetSourceSkeletonGeneration;
				snapshot.targetSkeletonGeneration = item.binding.skeletonGeneration;
				snapshot.poseGeneration = item.candidatePoseGeneration;
				snapshot.contactErrorMeters = item.retargetMetrics.constraints.maximumContactErrorMeters;
				snapshot.reachableIkErrorMeters = item.retargetMetrics.constraints.maximumEndEffectorErrorMeters;
				snapshot.observationTick = item.input.tick;
				snapshot.publicationGeneration = authorityGeneration;
				specialistSidecars->retarget.push_back({std::move(snapshot)});
			}
			if(AnimationAttachmentSnapshotPublisher::Instance().IsPublicationRequested(item.input.handle)) {
				PreparedAttachmentPublication publication;
				publication.animator = item.input.handle;
				if(!item.cacheReused && item.fallbackDecision.source != AnimationFallbackSource::HeldPriorGeneration &&
				   item.selectedPublication && item.selectedPublication->snapshot && item.selectedPublication->snapshot->pose &&
				   item.binding.skeleton && item.sourceObjectGeneration != 0U && item.sourceObjectWorldTransformCaptured) {
					AnimationBoneSocketSnapshotRequest request;
					request.animator = item.input.handle;
					request.sourceObjectId = item.input.descriptor.ownerObjectId;
					request.sourceObjectGeneration = item.sourceObjectGeneration;
					request.sourceComponentGeneration = item.input.descriptor.componentGeneration;
					request.sourceTick = item.selectedPublication->snapshot->tick;
					request.publicationGeneration = item.selectedPublication->snapshot->publicationGeneration;
					request.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
					request.pose = item.selectedPublication->snapshot->pose;
					request.skeleton = item.binding.skeleton;
					AnimationBoneSocketSnapshot snapshot;
					if(CaptureAnimationBoneSocketSnapshot(request, snapshot) == AnimationStatus::Ok) {
						publication.snapshot = std::move(snapshot);
					}
				}
				specialistSidecars->attachments.push_back(std::move(publication));
			}
			if(!item.cacheReused && item.fallbackDecision.source != AnimationFallbackSource::HeldPriorGeneration &&
			   item.selectedPublication && item.selectedPublication->snapshot && item.selectedPublication->snapshot->pose &&
			   item.selectedPublication->snapshot->pose->RootMotionDelta().has_value()) {
				PreparedAutomaticRootMotionPublication publication;
				publication.request.delta = *item.selectedPublication->snapshot->pose->RootMotionDelta();
				publication.request.ownerObjectId = item.input.descriptor.ownerObjectId;
				publication.request.objectGeneration = item.sourceObjectGeneration;
				publication.request.componentGeneration = item.input.descriptor.componentGeneration;
				publication.request.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
				publication.request.sourceObjectWorldTransformCaptured = item.sourceObjectWorldTransformCaptured;
				switch(item.rootMotionEnabled ? AnimatorRootMotionPolicy::ApplyToCharacter : item.binding.rootMotionPolicy) {
				case AnimatorRootMotionPolicy::Ignore:
					publication.request.mode = AnimationRootMotionApplicationMode::Ignore;
					break;
				case AnimatorRootMotionPolicy::ExtractOnly:
					publication.request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
					break;
				case AnimatorRootMotionPolicy::ApplyToObject:
					publication.request.mode = AnimationRootMotionApplicationMode::ApplyToTransform;
					break;
				case AnimatorRootMotionPolicy::ApplyToCharacter:
					publication.request.mode = AnimationRootMotionApplicationMode::ApplyToCharacter;
					break;
				default:
					publication.request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
					break;
				}
				const bool activatesRagdollAtThisBoundary =
					std::any_of(frameInputs.capturedRagdollRequests.begin(), frameInputs.capturedRagdollRequests.end(),
								[&item](const NativeAnimationRagdollRequest& request) {
									return request.animator.slotIndex == item.input.handle.slotIndex &&
										   request.animator.generation == item.input.handle.generation &&
										   request.operation == NativeAnimationRagdollOperation::Activate;
								});
				if(item.ragdollLifecycle != RagdollLifecycleState::Inactive || activatesRagdollAtThisBoundary) {
					publication.request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
				}
				specialistSidecars->automaticRootMotion.push_back(std::move(publication));
			}
			if(item.cacheEligible && item.fallbackDecision.source == AnimationFallbackSource::Current &&
			   item.evaluationStatus == AnimationStatus::Ok && item.selectedPublication && item.selectedPublication->snapshot) {
				specialistSidecars->evaluationCacheWorkIndices.push_back(inputIndex);
			}
			next->Set(handle.slotIndex, std::move(record));
		}
		return true;
	}

} // namespace TerraEngine::Animation
