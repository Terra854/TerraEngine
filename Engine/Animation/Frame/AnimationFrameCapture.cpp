#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	bool AnimationSystem::Impl::CaptureFrameLocked(AnimationFixedTick captureTick, std::uint64_t candidateAuthorityGeneration) {
		if(instances == nullptr || commands == nullptr || proceduralCommands == nullptr || advancedCommands == nullptr ||
		   lifecycleLatch == nullptr || candidateAuthorityGeneration == 0U)
			return false;
		// A thrown/cancelled graph may skip its finalizer. Complete its drained
		// commands before the next capture replaces the copied command vector.
		PublishCommittedOrdinaryCommandsLocked();
		SettleFailedFrameCommandsLocked(AnimationStatus::Cancelled, "NextCapture");
		frameInputs.commandFrameFailurePhase = nullptr;
		ResetFrameIntegrationStateLocked();
		if(evaluationState.evaluationCancellationSource.Initialize(AnimationCancellationOperation::CpuEvaluation, *lifecycleLatch) !=
		   AnimationStatus::Ok) {
			evaluationState.frameCancellationRequested.store(true, std::memory_order_release);
			return false;
		}
		evaluationState.evaluationCancellation = evaluationState.evaluationCancellationSource.GetToken();
		frameInputs.frameLifecycleGenerations = lifecycleLatch->Capture();
		frameInputs.frameSceneGeneration = ObjectManager::instance().GetSceneGeneration();
		frameInputs.frameConsumerReadView = AnimationConsumerRegistry::Instance().CaptureReadView();
		const auto rootBindings = rootControllerBindings.load(std::memory_order_acquire);

		frameInputs.frameHasConsumerDeclarations = frameInputs.frameConsumerReadView.HasDeclarations();
		const std::uint64_t frameSpatialRevision = ObjectManager::instance().GetSpatialRevision();
		try {
			if(!CaptureQueuedFrameInputs(captureTick))
				return false;
			if(!CaptureRuntimeFrameInputs(candidateAuthorityGeneration, frameSpatialRevision, rootBindings))
				return false;
			{
				// Scene access stays in one protected pass. This sample
				// exposes hierarchy lookup cost separately from
				// animation-owned state copying.
				TERRAENGINE_PROFILE_SCOPE("Animation: Capture Object Transforms", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				ObjectManager& objects = ObjectManager::instance();
				for(FrameWorkItem& item : persistentReuse.candidateFrameWork) {
					const AnimationFrameInput& input = item.input;
					if(item.sourceObjectWorldTransformCaptured)
						continue;
					if(const std::optional<Object> object = objects.getObject(input.descriptor.ownerObjectId)) {
						item.sourceObjectGeneration = TerraEngine::CompressObjectGeneration(objects.getObjectLifetimeGeneration(*object));
						item.sourceObjectWorldTransformCaptured =
							CaptureObjectWorldTransform(objects, *object, item.sourceObjectWorldTransform);
						if(item.sourceObjectGeneration != 0U && item.sourceObjectWorldTransformCaptured) {
							item.sourceObjectSceneGeneration = frameInputs.frameSceneGeneration;
							item.sourceObjectSpatialRevision = frameSpatialRevision;
						}
					}
				}
			}
			{
				// Clip views retain validated immutable payloads across
				// alternating frame slots. Keep their validation cost
				// visible without emitting one sample per animator, which
				// would make detailed captures unnecessarily expensive.
				TERRAENGINE_PROFILE_SCOPE("Animation: Open Clip Views", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				struct ReusableClipView {
					const AnimationClipAsset* clip = nullptr;
					const SkeletonAsset* skeleton = nullptr;
					const AnimationClipSegmentPayloadSet* streamedPayloads = nullptr;
					AnimationGeneration generation = 0U;
					AnimationClipView view;
				};
				// Identical prefab instances consume the same immutable clip, skeleton,
				// and streamed payload publication. Validate that tuple once per capture
				// and copy the checked view instead of reopening the codec thousands of
				// times after a shared residency promotion.
				std::vector<ReusableClipView> reusableClipViews;
				reusableClipViews.reserve(32U);
				for(FrameWorkItem& item : persistentReuse.candidateFrameWork) {
					if(item.dependencyStatus == AnimationStatus::Ok && item.binding.clip && item.binding.skeleton) {
						const bool sourceCompatible =
							item.retargetActive && item.binding.retargetSourceSkeleton &&
							item.binding.clip->compatibilityFingerprint == item.binding.retargetSourceSkeleton->compatibilityFingerprint &&
							item.binding.clip->skeletonAssetId == item.binding.retargetSourceSkeleton->assetId;
						const SkeletonAsset* samplingSkeleton =
							sourceCompatible ? item.binding.retargetSourceSkeleton.get() : item.binding.skeleton.get();
						item.sampledSourceSkeleton = sourceCompatible;
						const std::span<const std::uint8_t> residentSegments(item.binding.residentSegments);
						const auto reusable = std::find_if(
							reusableClipViews.begin(), reusableClipViews.end(),
							[&item, samplingSkeleton, residentSegments](const ReusableClipView& candidate) {
								return candidate.clip == item.binding.clip.get() && candidate.skeleton == samplingSkeleton &&
									   candidate.streamedPayloads == item.binding.streamedSegmentPayloads.get() &&
									   candidate.generation == item.binding.clipGeneration &&
									   candidate.view.Matches(*item.binding.clip, samplingSkeleton, residentSegments,
															  item.binding.clipGeneration, item.binding.streamedSegmentPayloads);
							});
						if(reusable != reusableClipViews.end()) {
							item.clipView = reusable->view;
							item.dependencyStatus = AnimationStatus::Ok;
						} else {
							item.dependencyStatus = item.clipView.Open(*item.binding.clip, samplingSkeleton, residentSegments,
																	   item.binding.clipGeneration, item.binding.streamedSegmentPayloads);
							if(item.dependencyStatus == AnimationStatus::Ok) {
								reusableClipViews.push_back({item.binding.clip.get(), samplingSkeleton,
															 item.binding.streamedSegmentPayloads.get(), item.binding.clipGeneration,
															 item.clipView});
							}
						}
					}
				}
			}
			frameStorage.frameWork.swap(persistentReuse.candidateFrameWork);
			frameInputs.compactGpuVisualCaptures.swap(persistentReuse.candidateCompactGpuVisualCaptures);
			frameInputs.compactRuntimeCaptureRoot.swap(persistentReuse.candidateCompactRuntimeCaptureRoot);
			frameInputs.compactRuntimeStateCaptureRoot.swap(persistentReuse.candidateCompactRuntimeStateCaptureRoot);
			frameInputs.capturedInputWorkIndices.swap(persistentReuse.candidateInputWorkIndices);
			frameInputs.frameTick = captureTick;
			return true;
		} catch(const std::bad_alloc&) {
			return false;
		}
	}

	void AnimationSystem::Impl::CopyCompleteRuntimeInput(std::size_t inputIndex, const AnimationFrameInput& input, RuntimeRecord* record,
														 std::size_t& candidateCompleteCount, std::size_t& proceduralRangeIndex,
														 std::uint64_t frameSpatialRevision, std::uint64_t candidateAuthorityGeneration,
														 bool visibleForPresentation, bool rootMotionEnabled) {
		if(candidateCompleteCount == persistentReuse.candidateFrameWork.size()) {
			persistentReuse.candidateFrameWork.emplace_back();
		}
		FrameWorkItem& item = persistentReuse.candidateFrameWork[candidateCompleteCount];
		item.capturedInputIndex = inputIndex;
		persistentReuse.candidateInputWorkIndices.push_back({candidateCompleteCount, false});
		++candidateCompleteCount;
		const bool sameAnimator = item.bindingCaptureHandle == input.handle;
		const bool bindingStorageReusable =
			sameAnimator && item.runtimeStateCaptureCommitted && item.bindingCaptureContentGeneration == record->contentGeneration;
		const bool parameterStorageReusable =
			sameAnimator && item.runtimeStateCaptureCommitted && item.capturedParameterGeneration == record->parameterGeneration;
		const bool sourceObjectTransformReusable = sameAnimator && item.runtimeStateCaptureCommitted &&
												   item.sourceObjectWorldTransformCaptured &&
												   item.sourceObjectSceneGeneration == frameInputs.frameSceneGeneration &&
												   item.sourceObjectSpatialRevision == frameSpatialRevision;
		const bool priorEventStateCaptured = item.eventStateCaptured;
		const bool priorControllerStateCaptured = item.controllerStateCaptured;
		// The previous frame and its admitted children have settled before this
		// protected capture reuses the alternating slot and its retained capacity.
		item.ResetForCaptureReuse();
		item.input = input;
		item.rootMotionEnabled = rootMotionEnabled;
		if(!sourceObjectTransformReusable) {
			item.sourceObjectGeneration = 0U;
			item.sourceObjectWorldTransform = {};
			item.sourceObjectWorldTransformCaptured = false;
			item.sourceObjectSceneGeneration = 0U;
			item.sourceObjectSpatialRevision = 0U;
		}
		// Missing, first-frame, or stale renderer
		// data fails open. Only an exact
		// scene-generation publication may suppress
		// visual evaluation.
		item.visibleForVisualEvaluation = visibleForPresentation;
		if(!bindingStorageReusable) {
			// Rebind and hot-reload advance
			// contentGeneration, so this is the only
			// steady-state path that copies the
			// binding's large immutable vectors.
			item.binding = record->binding;
			item.bindingOutputRequirementsFingerprint = 0U;
			item.bindingResourceLayoutFingerprint = 0U;
		} else {
			// Controller transitions change only this
			// small dynamic tail. Preserve the cached
			// immutable binding body and refresh the
			// fields that can be committed without a
			// full asset rebind.
			item.binding.stateId = record->binding.stateId;
			const bool clipChanged =
				item.binding.clip != record->binding.clip || item.binding.clipGeneration != record->binding.clipGeneration ||
				item.binding.clipDeterministicContentFingerprint != record->binding.clipDeterministicContentFingerprint ||
				item.binding.streamedSegmentPayloads != record->binding.streamedSegmentPayloads;
			item.binding.clip = record->binding.clip;
			item.binding.clipGeneration = record->binding.clipGeneration;
			item.binding.clipDeterministicContentFingerprint = record->binding.clipDeterministicContentFingerprint;
			item.binding.streamedSegmentPayloads = record->binding.streamedSegmentPayloads;
			if(clipChanged) {
				item.binding.residentSegments = record->binding.residentSegments;
				item.bindingResourceLayoutFingerprint = 0U;
			}
			item.binding.compiledRetargetProfile = record->binding.compiledRetargetProfile;
			item.binding.evaluationGenerations = record->binding.evaluationGenerations;
		}
		item.bindingCaptureHandle = input.handle;
		item.bindingCaptureContentGeneration = record->contentGeneration;
		// Mutable playback/controller state is
		// always recopied from the live protected
		// record. A generation can remain equal
		// after a cancelled or test-driven partial
		// frame, so retaining a worker-mutated
		// candidate here could otherwise consume a
		// command without ever publishing it.
		item.playback = record->playback;
		item.eventStateCaptured =
			item.binding.clip && item.binding.eventPolicy != AnimatorEventPolicy::Suppress && !item.binding.clip->events.empty();
		if(item.eventStateCaptured) {
			item.eventStream = record->eventStream;
		} else if(priorEventStateCaptured || !sameAnimator) {
			// A slot that previously belonged to an
			// event-bearing clip must not leak its
			// delivery watermark into a later
			// direct-clip fast path.
			item.eventStream = {};
		}
		item.controllerStateCaptured = item.binding.evaluationProgram != nullptr;
		if(item.controllerStateCaptured) {
			item.capturedGraphState = record->graphState;
			item.candidateGraphState = record->graphState;
			// Both private copies are now complete,
			// so exchange the candidate and live
			// buffers without changing either value.
			// Workers mutate the original allocation
			// while the runtime keeps an identical
			// rollback copy; successful publication
			// can later swap the candidate back in
			// without deep-copying three history
			// vectors for every animator.
			std::swap(item.candidateGraphState, record->graphState);
			item.transition = record->transition;
		} else if(priorControllerStateCaptured || !sameAnimator) {
			// Clear a reused slot once when it leaves
			// controller evaluation. Later
			// direct-clip frames retain these
			// already-empty allocations.
			item.capturedGraphState = {};
			item.candidateGraphState = {};
			item.transition = {};
		}
		item.dependencyStatus = record->dependencyStatus;
		item.previousExecutionModeState = record->executionModeState;
		if(!parameterStorageReusable) {
			// Generation equality proves these
			// vectors still match the live protected
			// record. Preserve their retained
			// allocations on the common command-free
			// crowd frame.
			item.parameters = record->parameters;
			item.layerWeights = record->layerWeights;
		}
		item.activeRetargetProfileId = record->activeRetargetProfileId;
		item.activeRetargetProfileGeneration = record->activeRetargetProfileGeneration;
		item.retargetActive = item.binding.retargetProfile && item.binding.compiledRetargetProfile && item.binding.retargetSourceSkeleton &&
							  item.binding.skeleton &&
							  MatchesNativeUuid(item.activeRetargetProfileId, item.binding.retargetProfile->assetId.value) &&
							  item.activeRetargetProfileGeneration == item.binding.retargetProfileGeneration;
		if(item.bindingOutputRequirementsFingerprint == 0U) {
			item.bindingOutputRequirementsFingerprint = BuildBindingOutputRequirementsFingerprint(item.binding);
		}
		if(item.bindingResourceLayoutFingerprint == 0U) {
			item.bindingResourceLayoutFingerprint = BuildBindingResourceLayoutFingerprint(item.binding);
		}
		item.activeMotionDatabaseId = record->activeMotionDatabaseId;
		item.activeMotionDatabaseGeneration = record->activeMotionDatabaseGeneration;
		item.latestMotionQueryId = record->latestMotionQueryId;
		item.appliedMotionQueryId = record->appliedMotionQueryId;
		item.latestMotionCandidate = record->latestMotionCandidate;
		item.hasMotionCandidate = record->hasMotionCandidate;
		item.restoredFromHistory =
			std::any_of(frameInputs.determinismRequestCompletions.begin(), frameInputs.determinismRequestCompletions.end(),
						[&input](const DeterminismRequestCompletion& completion) {
							return completion.stateRestored && completion.request.animator.slotIndex == input.handle.slotIndex &&
								   completion.request.animator.generation == input.handle.generation;
						});
		for(const NativeAnimationMotionMatchRequest& request : frameInputs.capturedMotionMatchRequests) {
			if(request.operation == NativeAnimationMotionMatchOperation::Query && request.animator.slotIndex == input.handle.slotIndex &&
			   request.animator.generation == input.handle.generation) {
				item.pendingMotionQueries.push_back(request);
			}
		}
		for(const NativeAnimationMotionMatchQueryRequestV2& request : frameInputs.capturedMotionMatchQueriesV2) {
			if(request.animator.slotIndex == input.handle.slotIndex && request.animator.generation == input.handle.generation) {
				item.pendingMotionQueriesV2.push_back(request);
			}
		}
		const NativeAnimationAnimatorHandle nativeHandle{input.handle.slotIndex, input.handle.generation};
		while(proceduralRangeIndex < frameInputs.capturedProceduralGoalRanges.size() &&
			  std::tie(frameInputs.capturedProceduralGoalRanges[proceduralRangeIndex].animator.slotIndex,
					   frameInputs.capturedProceduralGoalRanges[proceduralRangeIndex].animator.generation) <
				  std::tie(nativeHandle.slotIndex, nativeHandle.generation)) {
			++proceduralRangeIndex;
		}
		if(proceduralRangeIndex < frameInputs.capturedProceduralGoalRanges.size() &&
		   frameInputs.capturedProceduralGoalRanges[proceduralRangeIndex].animator.slotIndex == nativeHandle.slotIndex &&
		   frameInputs.capturedProceduralGoalRanges[proceduralRangeIndex].animator.generation == nativeHandle.generation) {
			const ActiveProceduralGoalRange& range = frameInputs.capturedProceduralGoalRanges[proceduralRangeIndex];
			item.proceduralStateObserved = true;
			item.activeProceduralGoals.assign(frameInputs.capturedProceduralGoals.begin() + range.firstGoal,
											  frameInputs.capturedProceduralGoals.begin() + range.firstGoal + range.goalCount);
		}
		item.ragdollGeneration = record->ragdollGeneration;
		item.ragdollLifecycle = record->ragdollLifecycle;
		item.ragdollBlendWeight = record->ragdollBlendWeight;
		item.ragdollLifecycleStartTick = record->ragdollLifecycleStartTick;
		item.ragdollRecoveryAlignedRootPosition = record->ragdollRecoveryAlignedRootPosition;
		item.ragdollRecoveryDurationSeconds = record->ragdollRecoveryDurationSeconds;
		if(record->ragdollGeneration.IsValid() &&
		   (record->ragdollLifecycle == RagdollLifecycleState::Active || record->ragdollLifecycle == RagdollLifecycleState::Recovering)) {
			item.ragdollPhysicsPose =
				Physics::PhysicsSystem::Instance().LatestRagdollPhysicsPoseAtOrAfter(record->ragdollId, record->ragdollGeneration);
			if(item.ragdollPhysicsPose) {
				item.ragdollGeneration = item.ragdollPhysicsPose->generation;
			}
		}
		// A complete candidate owns a copied row
		// while workers run.  The source is the
		// immutable authority root, never a runtime
		// record.
		const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
		const AnimationAuthorityRow* prior = authority ? authority->Find(record->handle) : nullptr;
		item.priorPublication =
			prior ? std::make_shared<const AnimationAuthorityRow>(*prior) : std::shared_ptr<const AnimationAuthorityRow>{};
		item.capturedPoseGeneration = record->poseGeneration;
		item.capturedStateGeneration = record->stateGeneration;
		item.capturedParameterGeneration = record->parameterGeneration;
		item.capturedExecutionModeGeneration = record->executionModeGeneration;
		item.contentGeneration = record->contentGeneration;
		item.candidateStateGeneration = record->stateGeneration;
		item.candidateParameterGeneration = record->parameterGeneration;
		item.candidateExecutionModeGeneration = record->executionModeGeneration;
		item.capturedPlaybackState = record->playback.Snapshot();
		// Triggers are edge commands. Their copied
		// snapshot may be observed for one
		// publication, but they never remain armed
		// in a later frame.
		std::erase_if(item.parameters,
					  [](const AnimationParameterSnapshot& value) { return value.type == AnimationParameterType::Trigger; });
		item.candidatePoseGeneration = record->poseGeneration + 1U;
		item.candidateAuthorityPublicationGeneration = candidateAuthorityGeneration;
	}

	bool
	AnimationSystem::Impl::CaptureRuntimeFrameInputs(std::uint64_t candidateAuthorityGeneration, std::uint64_t frameSpatialRevision,
													 const std::shared_ptr<const AnimationRootControllerBindingSnapshot>& rootBindings) {
		const auto rootEnabledFor = [&rootBindings](AnimatorInstanceHandle handle) noexcept {
			if(!rootBindings)
				return false;
			const auto found =
				std::lower_bound(rootBindings->active.begin(), rootBindings->active.end(), handle,
								 [](const AnimationRootControllerBinding& row, AnimatorInstanceHandle key) { return row.animator < key; });
			return found != rootBindings->active.end() && found->animator == handle && found->rootMotionEnabled;
		};

		// Keep two alternating item arrays alive.
		// Copy assignment can then reuse binding,
		// parameter, controller, and event-vector
		// capacity instead of allocating a fresh
		// nested object graph for every animator
		// every frame.
		TERRAENGINE_PROFILE_SCOPE("Animation: Copy Runtime State", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const std::shared_ptr<const TerraEngine::Renderer::AnimatedObjectVisibilitySnapshot> visibility =
			TerraEngine::Renderer::LoadAnimatedObjectVisibilitySnapshot();
		const bool visibilityUsable = visibility && visibility->valid && visibility->sceneGeneration == frameInputs.frameSceneGeneration;
		// candidateFrameWork is the retired frame
		// buffer after the previous swap. Reuse each
		// existing record in place so its nested
		// binding, parameter, controller, and event
		// allocations survive stable complete-mode
		// frames.
		std::size_t candidateCompleteCount = 0U;
		persistentReuse.candidateFrameWork.reserve(frameInputs.capturedFrameInputs.size());
		persistentReuse.candidateCompactGpuVisualCaptures.clear();
		persistentReuse.candidateCompactGpuVisualCaptures.reserve(frameInputs.capturedFrameInputs.size());
		persistentReuse.candidateCompactGpuVisualCaptures.authorityGeneration = candidateAuthorityGeneration;
		persistentReuse.candidateInputWorkIndices.clear();
		persistentReuse.candidateInputWorkIndices.reserve(frameInputs.capturedFrameInputs.size());
		const bool compactFrameHasAdvancedRequests = !frameInputs.capturedMotionMatchRequests.empty() ||
													 !frameInputs.capturedMotionMatchQueriesV2.empty() ||
													 !frameInputs.capturedProceduralGoals.empty();
		const bool visibleForPresentation = !visibilityUsable || visibility->hasActiveView;
		const bool compactAdmissionHasExternalWork =
			!frameInputs.capturedCommands.empty() || compactFrameHasAdvancedRequests || frameInputs.frameHasConsumerDeclarations;
		// The protected capture owns one immutable
		// runtime-root identity for the whole frame.
		// Loading an atomic shared_ptr for every
		// animator needlessly serializes
		// reference-count traffic across a large
		// crowd; retain the root once and perform
		// exact slot/generation checks against that
		// same owner.
		const std::shared_ptr<RuntimeRecordRoot> capturedRuntimeRoot = runtimeRecords.load(std::memory_order_acquire);
		if(!capturedRuntimeRoot)
			return false;
		const std::shared_ptr<const CompactRuntimeStateRoot> capturedCompactRuntimeRoot =
			compactRuntimeStates.load(std::memory_order_acquire);
		persistentReuse.candidateCompactRuntimeCaptureRoot = capturedRuntimeRoot;
		persistentReuse.candidateCompactRuntimeStateCaptureRoot = capturedCompactRuntimeRoot;
		ProveCompactCaptureRanges(capturedRuntimeRoot, capturedCompactRuntimeRoot, compactFrameHasAdvancedRequests);
		std::size_t proceduralRangeIndex = 0U;
		for(std::size_t inputIndex = 0U; inputIndex < frameInputs.capturedFrameInputs.size(); ++inputIndex) {
			const AnimationFrameInput& input = frameInputs.capturedFrameInputs[inputIndex];
			RuntimeRecord* record = input.handle.IsValid() && input.handle.slotIndex < capturedRuntimeRoot->size()
										? capturedRuntimeRoot->at(input.handle.slotIndex).get()
										: nullptr;
			if(record != nullptr && record->handle != input.handle)
				record = nullptr;
			const CompactRuntimeState* compactRuntimeState = FindCompactRuntimeState(capturedCompactRuntimeRoot, input.handle);
			const AnimationGeneration currentPoseGeneration =
				compactRuntimeState ? compactRuntimeState->poseGeneration : (record ? record->poseGeneration : 0U);
			if(record == nullptr || currentPoseGeneration == std::numeric_limits<AnimationGeneration>::max()) {
				return false;
			}
			const AnimatorPlaybackBinding& liveBinding = record->binding;
			if(frameInputs.compactCaptureAdmissionProof[inputIndex] != 0U) {
				persistentReuse.candidateCompactGpuVisualCaptures.append(inputIndex, input, *record, compactRuntimeState);
				persistentReuse.candidateInputWorkIndices.push_back({persistentReuse.candidateCompactGpuVisualCaptures.size() - 1U, true});
				evaluationState.compactPreCopyCaptureCount.fetch_add(1U, std::memory_order_acq_rel);
				continue;
			}
			const AnimationGpuExecutionInputs& gpu = liveBinding.gpuExecution;
			const AnimationExecutionModeState& modeState =
				compactRuntimeState ? compactRuntimeState->executionModeState : record->executionModeState;
			const bool compactPolicyEligible = liveBinding.requestedExecutionPolicy == AnimationExecutionPolicy::Auto ||
											   liveBinding.requestedExecutionPolicy == AnimationExecutionPolicy::ForceGpuVisual;
			const bool compactModeProven =
				input.tick >= 0 && !headlessExecutionRequired && compactPolicyEligible && gpu.rendererAvailable &&
				gpu.animationCapabilityAvailable && gpu.resourcesResident && gpu.evaluationHealthy && std::isfinite(gpu.requiredBudget) &&
				std::isfinite(gpu.availableBudget) && gpu.requiredBudget >= 0.0 && gpu.requiredBudget <= gpu.availableBudget &&
				modeState.initialized && modeState.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual &&
				(modeState.effectiveReason == AnimationExecutionModeReason::GpuVisualEligible ||
				 modeState.effectiveReason == AnimationExecutionModeReason::ForceGpuRequested) &&
				modeState.modeEnteredTick >= 0 && modeState.modeEnteredTick <= input.tick && !modeState.promotionPending;
			const bool retainedCompactAdmissionProof =
				compactRuntimeState != nullptr && compactRuntimeState->bindingContentGeneration != 0U &&
				compactRuntimeState->bindingContentGeneration == record->contentGeneration && !compactAdmissionHasExternalWork &&
				compactModeProven && liveBinding.gpuFastBinding && liveBinding.gpuFastBinding->IsComplete() &&
				liveBinding.gpuFastBinding->visual.renderIdentity.rendererGeneration != 0U && frameInputs.frameSceneGeneration != 0U &&
				record->dependencyStatus == AnimationStatus::Ok && record->ragdollLifecycle == RagdollLifecycleState::Inactive;
			if(retainedCompactAdmissionProof) {
				// The preceding committed compact root proves every immutable
				// admission fact for this exact binding generation. Append the
				// current scalar lanes directly after rechecking all live inputs.
				persistentReuse.candidateCompactGpuVisualCaptures.append(inputIndex, input, *record, compactRuntimeState);
				persistentReuse.candidateInputWorkIndices.push_back({persistentReuse.candidateCompactGpuVisualCaptures.size() - 1U, true});
				evaluationState.compactPreCopyCaptureCount.fetch_add(1U, std::memory_order_acq_rel);
				continue;
			}
			const bool hasTargetedCommand =
				std::any_of(frameInputs.capturedCommands.begin(), frameInputs.capturedCommands.end(),
							[&input](const AnimationCommand& command) { return command.target == input.handle; });
			// A registry publication can contain a
			// gameplay declaration for one animator
			// while a large render-only crowd has no
			// consumers at all. Resolve only this
			// immutable animator range before deciding
			// whether its direct clip may bypass the
			// complete capture record. Any matching
			// declaration (even one with no current
			// joint fields) remains a fail-closed
			// authoritative observation and therefore
			// uses Complete.
			const AnimationResolvedConsumerRequirements compactConsumers =
				frameInputs.frameHasConsumerDeclarations
					? frameInputs.frameConsumerReadView.Resolve(input.handle, liveBinding.jointParents, input.tick)
					: AnimationResolvedConsumerRequirements{};
			const bool compactConsumerClosureProven =
				compactConsumers.status == AnimationStatus::Ok && compactConsumers.attribution.empty();
			// The prior renderer snapshot is complete
			// for views that already existed, but an
			// automation/custom capture may introduce
			// another camera later in this same frame.
			// Do not use per-owner absence as a hard
			// publication gate: that would leave a
			// newly visible compact crowd in bind pose.
			// A valid no-view snapshot may still
			// suspend all visual work; any active
			// output conservatively retains
			// presentation and lets GPU culling discard
			// owners. A root-motion track is not itself
			// CPU authority. Ignore explicitly
			// suppresses extraction, while
			// Extract/Apply policies and declared
			// readers still require the complete
			// capture path. Reuse the execution-mode
			// rule so the early SoA gate cannot
			// disagree with the later policy decision.
			const bool compactRootMotionRequiresCpuAuthority = RequiresCpuRootMotionAuthority(
				liveBinding.rootMotionPolicy, liveBinding.clip != nullptr && liveBinding.clip->rootMotion.enabled, false, false,
				compactConsumers.closure.rootMotion);
			// Capture needs only a fail-closed
			// admission proof; the worker still runs
			// the full execution-mode resolver before
			// publication. Checking the exact stable
			// GPU state here avoids building and
			// hashing a duplicate empty consumer
			// closure for every crowd member on the
			// coordinator thread.
			const bool compactCaptureProven =
				compactConsumerClosureProven && !hasTargetedCommand && !compactFrameHasAdvancedRequests && liveBinding.clip &&
				liveBinding.clip->events.empty() && !compactRootMotionRequiresCpuAuthority && !liveBinding.controllerProgramRequired &&
				!liveBinding.retargetProfile && !liveBinding.motionDatabase && !liveBinding.ragdollProfile &&
				liveBinding.evaluationProgram && liveBinding.gpuFastBinding && liveBinding.gpuFastBinding->IsComplete() &&
				liveBinding.clipGeneration != 0U && liveBinding.skeletonGeneration != 0U && liveBinding.programGeneration != 0U &&
				liveBinding.evaluationGenerations.residencyGeneration != 0U
				// A direct compact capture omits the
				// broad CPU fallback state. Admit it
				// only after a protected complete frame
				// has committed GpuVisual mode;
				// first-frame promotion and any
				// hysteresis demotion must retain that
				// state so planning can safely choose
				// the complete evaluator instead.
				&& modeState.initialized && modeState.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual &&
				compactModeProven
				// Renderer and deterministic external
				// generations are cache-reuse domains.
				// Direct authored clips intentionally
				// leave them unset until a producer
				// opts into reuse, and the compact
				// packet never reads them. Its copied
				// render identity below is the
				// authoritative renderer-lifetime proof
				// for GPU presentation.
				&& frameInputs.frameSceneGeneration != 0U && liveBinding.gpuFastBinding->visual.renderIdentity.rendererGeneration != 0U &&
				record->dependencyStatus == AnimationStatus::Ok && record->ragdollLifecycle == RagdollLifecycleState::Inactive;
			if(compactCaptureProven) {
				persistentReuse.candidateCompactGpuVisualCaptures.append(inputIndex, input, *record, compactRuntimeState);
				persistentReuse.candidateInputWorkIndices.push_back({persistentReuse.candidateCompactGpuVisualCaptures.size() - 1U, true});
				evaluationState.compactPreCopyCaptureCount.fetch_add(1U, std::memory_order_acq_rel);
				continue;
			}
			if(compactRuntimeState) {
				// A compact owner entering a complete path needs its latest
				// scalar state exactly once. The protected capture precedes all
				// frame workers, and the mixed commit replaces this broad root.
				record->playback = compactRuntimeState->playback;
				record->poseGeneration = compactRuntimeState->poseGeneration;
				record->stateGeneration = compactRuntimeState->stateGeneration;
				record->executionModeGeneration = compactRuntimeState->executionModeGeneration;
				record->executionModeState = compactRuntimeState->executionModeState;
			}
			CopyCompleteRuntimeInput(inputIndex, input, record, candidateCompleteCount, proceduralRangeIndex, frameSpatialRevision,
									 candidateAuthorityGeneration, visibleForPresentation, rootEnabledFor(input.handle));
		}
		persistentReuse.candidateFrameWork.resize(candidateCompleteCount);

		return true;
	}

	bool AnimationSystem::Impl::CaptureQueuedFrameInputs(AnimationFixedTick captureTick) {

		// This scope separates external queue/store snapshots
		// from per-animator state capture, making profiler
		// sessions actionable without per-item noise.
		TERRAENGINE_PROFILE_SCOPE("Animation: Capture Queues And Inputs", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		instances->CaptureFrameInputs(captureTick, frameInputs.capturedFrameInputs);
		commands->DrainThrough(captureTick, frameInputs.capturedCommands, *instances);
		frameInputs.capturedCommandStatuses.assign(frameInputs.capturedCommands.size(), AnimationStatus::NotReady);
		const std::uint64_t controllerRevisionBefore = ResourceManager::Instance().GetAnimationBindingResourceRevision();
		frameInputs.qualifiedControllerResourceRevision = 0U;
		// Qualified commands are tied to the exact resource publication
		// captured by their producer. Reject a hot-reloaded or tombstoned
		// controller before any worker can mutate copied parameters.
		for(std::size_t commandIndex = 0U; commandIndex < frameInputs.capturedCommands.size(); ++commandIndex) {
			const AnimationAssetHandle expected = frameInputs.capturedCommands[commandIndex].expectedController;
			if(!expected.IsValid())
				continue;
			const auto current =
				ResourceManager::Instance().TryGetAnimationAssetRuntimeGeneration(expected.assetId.value, AssetType::AnimationController);
			if(!current || *current != expected.generation ||
			   !ResourceManager::Instance().IsAnimationAssetPublished(expected.assetId.value, AssetType::AnimationController))
				frameInputs.capturedCommandStatuses[commandIndex] = AnimationStatus::StaleHandle;
			else
				frameInputs.qualifiedControllerResourceRevision = controllerRevisionBefore;
		}
		proceduralCommands->DrainThrough(captureTick, frameInputs.capturedProceduralCommands, *instances);
		if(proceduralCommands->CaptureActiveGoals(frameInputs.capturedProceduralGoalRanges, frameInputs.capturedProceduralGoals) !=
		   AnimationStatus::Ok) {
			return false;
		}
		frameInputs.capturedProceduralCommandStatuses.assign(frameInputs.capturedProceduralCommands.size(), AnimationStatus::NotReady);
		advancedCommands->DrainThrough(captureTick, frameInputs.capturedRetargetRequests, frameInputs.capturedMotionMatchRequests,
									   frameInputs.capturedMotionMatchQueriesV2, frameInputs.capturedRagdollRequests,
									   frameInputs.capturedDeterminismRequests);
		// Exact history restores happen only while runtime
		// ownership is protected and before worker-owned frame
		// values are copied. No worker mutates live playback,
		// controller, parameter, or event state.
		PrepareDeterminismRequestsLocked();
		for(std::size_t index = 0U; index < frameInputs.capturedProceduralCommands.size(); ++index) {
			const NativeAnimationProceduralGoalCommand& command = frameInputs.capturedProceduralCommands[index];
			const AnimatorInstanceHandle handle{command.animator.slotIndex, command.animator.generation};
			const RuntimeRecord* record = FindRuntimeRecordLocked(handle);
			if(record == nullptr || !instances->IsAlive(handle)) {
				frameInputs.capturedProceduralCommandStatuses[index] = AnimationStatus::StaleHandle;
				continue;
			}
			if(command.command == NativeAnimationProceduralCommandKind::ClearGoal) {
				frameInputs.capturedProceduralCommandStatuses[index] = AnimationStatus::Ok;
				continue;
			}
			const std::shared_ptr<const AnimationAuthorityFrame> authority = authorityPublisher.Load();
			const AnimationAuthorityRow* published = authority ? authority->Find(record->handle) : nullptr;
			if(!published || !published->snapshot || published->snapshot->publicationGeneration != command.expectedPoseGeneration ||
			   published->authorityPublicationGeneration != command.expectedPublicationGeneration) {
				frameInputs.capturedProceduralCommandStatuses[index] = AnimationStatus::StaleHandle;
				continue;
			}
			const bool jointExists = std::find(record->binding.jointStableIds.begin(), record->binding.jointStableIds.end(),
											   command.targetJointStableId) != record->binding.jointStableIds.end();
			frameInputs.capturedProceduralCommandStatuses[index] = jointExists ? AnimationStatus::Ok : AnimationStatus::InvalidArgument;
		}

		return true;
	}

	void AnimationSystem::Impl::ProveCompactCaptureRanges(const std::shared_ptr<RuntimeRecordRoot>& capturedRuntimeRoot,
														  const std::shared_ptr<const CompactRuntimeStateRoot>& capturedCompactRuntimeRoot,
														  bool compactFrameHasAdvancedRequests) {
		frameInputs.compactCaptureAdmissionProof.assign(frameInputs.capturedFrameInputs.size(), std::uint8_t{0U});
		const std::size_t captureWorkerCapacity = ThreadPool::instance().getThreadCount();
		// A worker proof is a second read-only pass followed by ordered SoA
		// assembly. Measurements on the 4,015-owner authored crowd show that
		// queue/join overhead exceeds the saved serial checks at that size;
		// enable it only once each worker receives enough rows to amortize
		// scheduling and immutable consumer-resolution costs.
		constexpr std::size_t kMinimumParallelCompactCaptureRows = 8192U;
		const bool canProveCompactInParallel = frameInputs.capturedFrameInputs.size() >= kMinimumParallelCompactCaptureRows &&
											   captureWorkerCapacity > 1U && frameInputs.capturedCommands.empty() &&
											   !compactFrameHasAdvancedRequests;
		const auto proveCompactRange = [this, &capturedRuntimeRoot, &capturedCompactRuntimeRoot](std::size_t begin,
																								 std::size_t end) noexcept {
			for(std::size_t inputIndex = begin; inputIndex < end; ++inputIndex) {
				const AnimationFrameInput& input = frameInputs.capturedFrameInputs[inputIndex];
				RuntimeRecord* record = input.handle.IsValid() && input.handle.slotIndex < capturedRuntimeRoot->size()
											? capturedRuntimeRoot->at(input.handle.slotIndex).get()
											: nullptr;
				if(!record || record->handle != input.handle)
					continue;
				const CompactRuntimeState* state = FindCompactRuntimeState(capturedCompactRuntimeRoot, input.handle);
				const AnimationGeneration poseGeneration = state ? state->poseGeneration : record->poseGeneration;
				const AnimationExecutionModeState& mode = state ? state->executionModeState : record->executionModeState;
				const AnimatorPlaybackBinding& binding = record->binding;
				const AnimationGpuExecutionInputs& gpu = binding.gpuExecution;
				// The read view pins one immutable, sorted declaration root for
				// every worker. A few gameplay consumers can therefore reject only
				// their matching animators instead of forcing the entire visual crowd
				// through the serial complete-admission path.
				const AnimationResolvedConsumerRequirements consumers =
					frameInputs.frameHasConsumerDeclarations
						? frameInputs.frameConsumerReadView.Resolve(input.handle, binding.jointParents, input.tick)
						: AnimationResolvedConsumerRequirements{};
				const bool consumerClosureProven = consumers.status == AnimationStatus::Ok && consumers.attribution.empty();
				const bool policyEligible = binding.requestedExecutionPolicy == AnimationExecutionPolicy::Auto ||
											binding.requestedExecutionPolicy == AnimationExecutionPolicy::ForceGpuVisual;
				const bool modeProven = input.tick >= 0 && !headlessExecutionRequired && policyEligible && gpu.rendererAvailable &&
										gpu.animationCapabilityAvailable && gpu.resourcesResident && gpu.evaluationHealthy &&
										std::isfinite(gpu.requiredBudget) && std::isfinite(gpu.availableBudget) &&
										gpu.requiredBudget >= 0.0 && gpu.requiredBudget <= gpu.availableBudget && mode.initialized &&
										mode.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual &&
										(mode.effectiveReason == AnimationExecutionModeReason::GpuVisualEligible ||
										 mode.effectiveReason == AnimationExecutionModeReason::ForceGpuRequested) &&
										mode.modeEnteredTick >= 0 && mode.modeEnteredTick <= input.tick && !mode.promotionPending;
				const bool rootMotionRequiresCpu = RequiresCpuRootMotionAuthority(
					binding.rootMotionPolicy, binding.clip && binding.clip->rootMotion.enabled, false, false, consumers.closure.rootMotion);
				const bool proven =
					poseGeneration != std::numeric_limits<AnimationGeneration>::max() && consumerClosureProven && binding.clip &&
					binding.clip->events.empty() && !rootMotionRequiresCpu && !binding.controllerProgramRequired &&
					!binding.retargetProfile && !binding.motionDatabase && !binding.ragdollProfile && binding.evaluationProgram &&
					binding.gpuFastBinding && binding.gpuFastBinding->IsComplete() && binding.clipGeneration != 0U &&
					binding.skeletonGeneration != 0U && binding.programGeneration != 0U &&
					binding.evaluationGenerations.residencyGeneration != 0U && modeProven && frameInputs.frameSceneGeneration != 0U &&
					binding.gpuFastBinding->visual.renderIdentity.rendererGeneration != 0U &&
					record->dependencyStatus == AnimationStatus::Ok && record->ragdollLifecycle == RagdollLifecycleState::Inactive;
				frameInputs.compactCaptureAdmissionProof[inputIndex] = proven ? 1U : 0U;
			}
		};
		if(canProveCompactInParallel) {
			constexpr std::size_t kCompactCaptureRowsPerTask = 1024U;
			const std::size_t workerCount =
				std::min<std::size_t>(captureWorkerCapacity, (frameInputs.capturedFrameInputs.size() + kCompactCaptureRowsPerTask - 1U) /
																 kCompactCaptureRowsPerTask);
			const std::size_t chunkSize = (frameInputs.capturedFrameInputs.size() + workerCount - 1U) / workerCount;
			std::vector<std::future<void>> captureFutures;
			captureFutures.reserve(workerCount - 1U);
			// Join children before the captured proof inputs can unwind.
			TerraEngine::Tasks::ScopedTaskSettlement<void> captureFuturesSettlement(
				captureFutures, TerraEngine::Tasks::NestedTaskGroup::AnimationCompactCapture);
			for(std::size_t workerIndex = 1U; workerIndex < workerCount; ++workerIndex) {
				const std::size_t begin = workerIndex * chunkSize;
				const std::size_t end = std::min(frameInputs.capturedFrameInputs.size(), begin + chunkSize);
				captureFuturesSettlement.CreateAndEnqueue(ThreadPool::instance(),
														  [&proveCompactRange, begin, end]() { proveCompactRange(begin, end); });
			}
			captureFuturesSettlement.RunInline([&] { proveCompactRange(0U, std::min(frameInputs.capturedFrameInputs.size(), chunkSize)); });
			captureFuturesSettlement.GetAll();
		}
	}

	void AnimationSystem::Impl::CaptureAnimationInputsStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
															TerraEngine::Scheduling::JobGraphContext& context) {

		if(context.IsCancellationRequested()) {
			this->RequestFrameCancellation();
			return;
		}
		if(!this->runtimeActive.load(std::memory_order_acquire) ||
		   this->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch ||
		   this->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) {
			return;
		}
		this->evaluationState.plannedAnimatorCount.store(0U, std::memory_order_release);
		this->evaluationState.evaluatedAnimatorCount.store(0U, std::memory_order_release);
		std::scoped_lock lock(this->runtimeOwnershipMutex);
		const AnimationFixedTick captureTick = this->frameInputs.frameTick == std::numeric_limits<AnimationFixedTick>::max()
												   ? this->frameInputs.frameTick
												   : this->frameInputs.frameTick + 1;
		const std::uint64_t currentAuthorityGeneration =
			this->authorityPublicationGeneration.load(std::memory_order_relaxed);
		const std::uint64_t candidateAuthorityGeneration =
			currentAuthorityGeneration == std::numeric_limits<std::uint64_t>::max() ? 0U : currentAuthorityGeneration + 1U;
		(void)this->CaptureFrameLocked(captureTick, candidateAuthorityGeneration);
	}

} // namespace TerraEngine::Animation
