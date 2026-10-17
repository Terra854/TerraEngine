#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	AnimationStatus AnimationSystem::BindAnimatorPlayback(AnimatorInstanceHandle handle, AnimatorPlaybackBinding binding) {
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		bool mutated = false;
		return BindAnimatorPlaybackLocked(handle, std::move(binding), nullptr, nullptr, false, false, true, mutated);
	}

	AnimationStatus AnimationSystem::BindAnimatorPlaybackLocked(AnimatorInstanceHandle handle, AnimatorPlaybackBinding binding,
																const AnimationClipView* sharedValidatedClipView,
																ImmutableBindPreparation* sharedPreparation, bool immutableBindingValidated,
																bool deferEvaluationCacheInvalidation, bool publishGlobalInvalidation,
																bool& outMutated) {
		outMutated = false;
		Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || record == nullptr || !impl_->instances->IsAlive(handle)) {
			return AnimationStatus::StaleHandle;
		}
		// Rebinding would retire the old ragdoll proof, recovery pose, and profile.
		// An external character owner may survive a controller-target replacement,
		// so this low-frequency protected check scans Physics by owner identity rather
		// than trusting the current animator-to-controller binding table alone.
		if(record->bindingInitialized &&
		   (record->pendingExternalRecoveryToken.has_value() ||
			Physics::PhysicsSystem::Instance().HasExternalCharacterMotionOwner(RagdollExternalOwnerId(handle)))) {
			return AnimationStatus::NotReady;
		}
		if(binding.clipGeneration == 0U || binding.skeletonGeneration == 0U || binding.programGeneration == 0U || binding.stateId == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(!immutableBindingValidated && binding.controllerProgramRequired && binding.evaluationProgram) {
			const AnimationStatus stateProgramStatus = ValidateStateMachineProgram(*binding.evaluationProgram);
			if(stateProgramStatus != AnimationStatus::Ok)
				return stateProgramStatus;
			const bool initialStateExists =
				std::any_of(binding.evaluationProgram->states.begin(), binding.evaluationProgram->states.end(),
							[&binding](const EvaluationState& state) { return state.stableId == binding.stateId; });
			if(!initialStateExists)
				return AnimationStatus::InvalidArgument;
		}
		if(!immutableBindingValidated &&
		   ((binding.retargetProfile && (binding.retargetProfileGeneration == 0U ||
										 binding.retargetProfile->runtimeGeneration != binding.retargetProfileGeneration)) ||
			(binding.motionDatabase &&
			 (binding.motionDatabaseGeneration == 0U || binding.motionDatabase->runtimeGeneration != binding.motionDatabaseGeneration)) ||
			(binding.ragdollProfile &&
			 (binding.ragdollProfileGeneration == 0U || binding.ragdollProfile->runtimeGeneration != binding.ragdollProfileGeneration)))) {
			return AnimationStatus::InvalidArgument;
		}
		if(record->contentGeneration == std::numeric_limits<AnimationGeneration>::max() ||
		   record->historyGeneration == std::numeric_limits<AnimationGeneration>::max() ||
		   record->stateGeneration == std::numeric_limits<std::uint64_t>::max() ||
		   record->parameterGeneration == std::numeric_limits<std::uint64_t>::max() ||
		   record->executionModeGeneration == std::numeric_limits<std::uint64_t>::max()) {
			return AnimationStatus::RangeOverflow;
		}
		std::vector<AnimationParameterSnapshot> localInitialParameters;
		std::vector<float> localInitialLayerWeights;
		std::vector<AnimationParameterSnapshot>& initialParameters =
			sharedPreparation ? sharedPreparation->initialParameters : localInitialParameters;
		std::vector<float>& initialLayerWeights = sharedPreparation ? sharedPreparation->initialLayerWeights : localInitialLayerWeights;
		if(!immutableBindingValidated) {
			initialParameters.clear();
			if(const AnimationStatus status = ParseInitialParameters(binding.initialParametersJson, initialParameters);
			   status != AnimationStatus::Ok) {
				return status;
			}
			initialLayerWeights.clear();
		}
		try {
			if(!immutableBindingValidated && binding.evaluationProgram) {
				initialLayerWeights.reserve(binding.evaluationProgram->layers.size());
				for(const EvaluationLayer& layer : binding.evaluationProgram->layers) {
					if(!std::isfinite(layer.defaultWeight) || layer.defaultWeight < 0.0F || layer.defaultWeight > 1.0F) {
						return AnimationStatus::InvalidArgument;
					}
					initialLayerWeights.push_back(layer.defaultWeight);
				}
			}
			if(sharedPreparation != nullptr) {
				// The shared template is copied into each actor only after the
				// controller generation changes. Reserve both destinations now so
				// that late allocation cannot fail after the instance descriptor
				// has advanced to the new controller publication.
				record->parameters.reserve(initialParameters.size());
				record->layerWeights.reserve(initialLayerWeights.size());
			}
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		// Compile the complete source/target mapping once at this protected bind
		// boundary. A malformed profile remains an unavailable optional dependency;
		// it cannot expose a partially compiled table to worker evaluation.
		binding.compiledRetargetProfile.reset();
		if(binding.retargetProfile && binding.retargetSourceSkeleton && binding.skeleton) {
			RetargetValidationResult validation;
			RetargetCompiledProfile compiled;
			if(CompileRetargetProfile(*binding.retargetProfile, *binding.retargetSourceSkeleton, *binding.skeleton, validation, compiled) ==
			   AnimationStatus::Ok) {
				try {
					// The compiler canonicalizes authored content and clears the runtime
					// generation. Restore the exact ResourceManager publication identity
					// before workers validate this immutable compiled table.
					compiled.profile.runtimeGeneration = binding.retargetProfileGeneration;
					binding.compiledRetargetProfile = std::make_shared<const RetargetCompiledProfile>(std::move(compiled));
				} catch(const std::bad_alloc&) {
					return AnimationStatus::InternalError;
				}
			}
		}
		const bool sourceRetargetAvailable = binding.compiledRetargetProfile && binding.retargetSourceSkeleton;

		AnimationStatus dependencyStatus = AnimationStatus::Ok;
		if(binding.controllerProgramRequired && !binding.evaluationProgram) {
			dependencyStatus =
				binding.controllerProgramStatus == AnimationStatus::Ok || binding.controllerProgramStatus == AnimationStatus::Queued
					? AnimationStatus::NotReady
					: binding.controllerProgramStatus;
		} else if(binding.evaluationProgram) {
			EvaluationDispatchState dispatchValidation;
			const AnimationStatus programStatus = DispatchEvaluationProgramCpu(*binding.evaluationProgram, dispatchValidation);
			if(programStatus != AnimationStatus::Ok) {
				dependencyStatus = programStatus;
			} else if(binding.skeleton &&
					  binding.evaluationProgram->compatibilityFingerprint != binding.skeleton->compatibilityFingerprint &&
					  (!sourceRetargetAvailable ||
					   binding.evaluationProgram->compatibilityFingerprint != binding.retargetSourceSkeleton->compatibilityFingerprint)) {
				dependencyStatus = AnimationStatus::IncompatibleAsset;
			}
		}
		if(dependencyStatus == AnimationStatus::Ok && (!binding.skeleton || !binding.clip)) {
			dependencyStatus = AnimationStatus::MissingAsset;
		} else if(dependencyStatus == AnimationStatus::Ok && (!binding.skeleton->assetId.IsValid() || !binding.clip->assetId.IsValid())) {
			return AnimationStatus::InvalidArgument;
		}
		const bool clipTargetsOutputSkeleton = dependencyStatus == AnimationStatus::Ok && binding.skeleton && binding.clip &&
											   binding.clip->compatibilityFingerprint == binding.skeleton->compatibilityFingerprint &&
											   binding.clip->skeletonAssetId == binding.skeleton->assetId;
		const bool clipTargetsRetargetSource =
			dependencyStatus == AnimationStatus::Ok && sourceRetargetAvailable && binding.clip &&
			binding.clip->compatibilityFingerprint == binding.retargetSourceSkeleton->compatibilityFingerprint &&
			binding.clip->skeletonAssetId == binding.retargetSourceSkeleton->assetId;
		if(dependencyStatus == AnimationStatus::Ok &&
		   (binding.skeleton->compatibilityFingerprint == 0U || (!clipTargetsOutputSkeleton && !clipTargetsRetargetSource))) {
			dependencyStatus = AnimationStatus::IncompatibleAsset;
		}

		if(binding.jointStableIds.empty() && binding.skeleton) {
			try {
				binding.jointStableIds.reserve(binding.skeleton->joints.size());
				for(const SkeletonJoint& joint : binding.skeleton->joints) {
					binding.jointStableIds.push_back(joint.stableId);
				}
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}
		if(binding.retargetSourceJointStableIds.empty() && binding.retargetSourceSkeleton) {
			try {
				binding.retargetSourceJointStableIds.reserve(binding.retargetSourceSkeleton->joints.size());
				for(const SkeletonJoint& joint : binding.retargetSourceSkeleton->joints) {
					binding.retargetSourceJointStableIds.push_back(joint.stableId);
				}
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}
		auto validStableIds = [](const std::vector<std::uint64_t>& values) {
			std::vector<std::uint64_t> sorted = values;
			if(std::any_of(sorted.begin(), sorted.end(), [](std::uint64_t value) { return value == 0U; })) {
				return false;
			}
			std::sort(sorted.begin(), sorted.end());
			return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
		};
		try {
			// A component crowd shares the exact prepared binding pointer. Once one
			// live record has validated these immutable arrays, repeating four copied
			// sorts for every identical prefab instance only adds protected-frame work.
			if(!immutableBindingValidated &&
			   (!validStableIds(binding.jointStableIds) || !validStableIds(binding.retargetSourceJointStableIds) ||
				!validStableIds(binding.morphStableIds) || !validStableIds(binding.curveStableIds))) {
				return AnimationStatus::InvalidArgument;
			}
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		const auto resolveClipFingerprint = [this](const AnimationClipAsset& clip, AnimationGeneration generation, std::uint64_t& output) {
			const AnimationAssetHandle key{clip.assetId, generation};
			if(const auto found = impl_->persistentReuse.clipContentFingerprints.find(key);
			   found != impl_->persistentReuse.clipContentFingerprints.end()) {
				output = found->second;
				return AnimationStatus::Ok;
			}
			std::uint64_t fingerprint = 0U;
			const AnimationStatus status = ComputeClipDeterministicContentFingerprint(clip, fingerprint);
			if(status != AnimationStatus::Ok)
				return status;
			try {
				impl_->persistentReuse.clipContentFingerprints.emplace(key, fingerprint);
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
			output = fingerprint;
			return AnimationStatus::Ok;
		};
		if(binding.clip) {
			const AnimationStatus fingerprintStatus =
				resolveClipFingerprint(*binding.clip, binding.clipGeneration, binding.clipDeterministicContentFingerprint);
			if(fingerprintStatus != AnimationStatus::Ok)
				return fingerprintStatus;
		}
		for(AnimatorControllerClipBinding& controllerClip : binding.controllerClips) {
			if(!controllerClip.clip)
				continue;
			const AnimationStatus fingerprintStatus =
				resolveClipFingerprint(*controllerClip.clip, controllerClip.clipGeneration, controllerClip.deterministicContentFingerprint);
			if(fingerprintStatus != AnimationStatus::Ok)
				return fingerprintStatus;
		}

		const bool replacingExistingBinding = record->bindingInitialized;
		AnimationClipView clipView;
		AnimationEventStream eventStream;
		if(dependencyStatus == AnimationStatus::Ok) {
			const SkeletonAsset* samplingSkeleton =
				clipTargetsRetargetSource ? binding.retargetSourceSkeleton.get() : binding.skeleton.get();
			const bool sharedViewMatches =
				sharedValidatedClipView != nullptr &&
				sharedValidatedClipView->Matches(*binding.clip, samplingSkeleton, std::span<const std::uint8_t>(binding.residentSegments),
												 binding.clipGeneration, binding.streamedSegmentPayloads);
			dependencyStatus = sharedViewMatches
								   ? clipView.AdoptValidatedPublicationFrom(*sharedValidatedClipView)
								   : clipView.Open(*binding.clip, samplingSkeleton, std::span<const std::uint8_t>(binding.residentSegments),
												   binding.clipGeneration, binding.streamedSegmentPayloads);
			if(dependencyStatus == AnimationStatus::Ok && binding.eventPolicy != AnimatorEventPolicy::Suppress) {
				const AnimationAssetHandle clipHandle{binding.clip->assetId, binding.clipGeneration};
				dependencyStatus = eventStream.Configure(clipHandle, binding.clip->durationTicks, binding.clip->events);
			}
		}
		binding.playback.durationTicks = binding.clip ? binding.clip->durationTicks : 0;
		if(binding.updatePolicy == AnimatorUpdatePolicy::FixedStep) {
			// Fixed-step update intent is enforced at bind time so the playback clock
			// cannot accidentally consume render-frame seconds.
			binding.playback.timePolicy = AnimatorTimePolicy::Fixed;
		}
		AnimatorPlayback playback;
		if(const AnimationStatus status = playback.Configure(binding.playback); status != AnimationStatus::Ok)
			return status;
		// Seek before optional autoplay so a stopped editor binding and a running
		// runtime binding publish the same configured first pose. The coordinator
		// already clamps authored values to the currently published clip duration.
		if(const AnimationStatus status = playback.Seek(binding.initialPlaybackTick); status != AnimationStatus::Ok)
			return status;
		if(binding.playOnBind && dependencyStatus == AnimationStatus::Ok) {
			// Autoplay is applied before the new binding is published, keeping the
			// clip, event stream, and clock transition atomic at this protected
			// boundary. Missing dependencies stay stopped and retry on a later bind.
			if(const AnimationStatus status = playback.Play(); status != AnimationStatus::Ok)
				return status;
		}

		if(binding.fallbackCompatibilityFingerprint == 0U && binding.skeleton) {
			binding.fallbackCompatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
		}
		AnimationAssetHandle reboundController{};
		if(record->descriptor.controller.IsValid() && binding.evaluationProgram) {
			reboundController = record->descriptor.controller;
			reboundController.generation = binding.programGeneration;
			// This is the last generation check before the successful bind commits.
			// CaptureFrameInputs copies this store descriptor, not the runtime record.
			if(!impl_->instances->UpdateControllerGeneration(handle, reboundController)) {
				return AnimationStatus::StaleHandle;
			}
		}
		if(publishGlobalInvalidation) {
			// A direct-clip crowd batch reaches this point under one protected lock.
			// The first record invalidates shared authority/presentation ownership;
			// later records still retire their individual caches below.
			(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Asset);
			(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(AnimationLifecycleBoundary::Asset);
		}
		if(replacingExistingBinding && !deferEvaluationCacheInvalidation) {
			(void)impl_->persistentReuse.evaluationCache.InvalidateAnimator(handle);
		}
		record->binding = std::move(binding);
		record->bindingInitialized = true;
		if(reboundController.IsValid()) {
			record->descriptor.controller = reboundController;
		}
		record->clipView = std::move(clipView);
		record->eventStream = std::move(eventStream);
		record->playback = playback;
		if(sharedPreparation != nullptr) {
			// Retain each record's capacity across rebinds while reusing the one parsed
			// cohort template. This removes per-instance JSON parsing and layer walks.
			record->parameters.assign(initialParameters.begin(), initialParameters.end());
			record->layerWeights.assign(initialLayerWeights.begin(), initialLayerWeights.end());
		} else {
			record->parameters = std::move(localInitialParameters);
			record->layerWeights = std::move(localInitialLayerWeights);
		}
		record->graphState = {};
		record->transition = {};
		record->dependencyStatus = dependencyStatus;
		++record->contentGeneration;
		++record->historyGeneration;
		++record->stateGeneration;
		++record->parameterGeneration;
		++record->executionModeGeneration;
		// Rebinding replaces the immutable program/resource identity, so retaining
		// promotion hysteresis from the previous binding could select a stale mode.
		record->executionModeState = {};
		// A hot-rebind invalidates cached evaluator state.  Consumers retain only a
		// previously acquired authority root until the next sealed root is published.
		if(replacingExistingBinding) {
			// Socket/bone attachment copies belong to the retired pose generation too.
			// Remove them in the same protected rebind operation so a fresh ABI read can
			// never pair an old transform set with the new resource identity.
			if(!deferEvaluationCacheInvalidation) {
				AnimationAttachmentSnapshotPublisher::Instance().Retire(handle);
				if(impl_->advancedCommands != nullptr) {
					impl_->advancedCommands->RetireAnimator({handle.slotIndex, handle.generation});
				}
			}
			if(record->ragdollGeneration.IsValid() && record->ragdollLifecycle != RagdollLifecycleState::Inactive) {
				RagdollPhysicsRequest teardown;
				teardown.kind = RagdollPhysicsRequestKind::Teardown;
				teardown.ragdollId = record->ragdollId;
				teardown.generation = record->ragdollGeneration;
				teardown.expectedCurrentGeneration = record->ragdollGeneration;
				teardown.correlationId = record->ragdollId;
				teardown.producerOrder = record->ragdollId;
				(void)TerraEngine::Physics::PhysicsSystem::Instance().SubmitRagdollPhysicsRequest(std::move(teardown));
			}
		}
		record->activeRetargetProfileId = {};
		record->activeRetargetProfileGeneration = 0U;
		record->latestRetargetMetrics = {};
		record->latestRetargetDeterministicHash = 0U;
		record->activeMotionDatabaseId = {};
		record->activeMotionDatabaseGeneration = 0U;
		record->latestMotionQueryId = 0U;
		record->appliedMotionQueryId = 0U;
		record->latestMotionCandidate = {};
		record->hasMotionCandidate = false;
		record->ragdollGeneration = {};
		record->activeRagdollProfileId = {};
		record->ragdollCreated = false;
		record->ragdollLifecycle = RagdollLifecycleState::Inactive;
		record->ragdollBlendWeight = 0.0F;
		record->ragdollLifecycleStartTick = 0;
		record->ragdollRecoveryCandidateId = 0U;
		record->ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
		record->ragdollRecoveryAlignedRootPosition = {};
		record->ragdollRecoveryDurationSeconds = 0.0F;
		record->ragdollMotorDriveCount = 0U;
		record->ragdollMotorStatus = AnimationStatus::NotReady;
		record->pendingRagdollCorrelationId = 0U;
		record->pendingRagdollPreviousGeneration = {};
		if(replacingExistingBinding && !deferEvaluationCacheInvalidation) {
			std::erase_if(impl_->pendingRootMotionApplications,
						  [handle](const AnimationRootMotionApplicationRequest& value) { return value.delta.animator == handle; });
			AnimationRootMotionResultPublisher::Instance().Retire(handle);
			impl_->persistentReuse.rollbackHistory.Clear(handle);
		}
		if(publishGlobalInvalidation) {
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		}
		outMutated = true;
		return dependencyStatus;
	}

	void AnimationSystem::BindAnimatorPlaybackBatch(std::span<const AnimatorPlaybackBindRequest> requests,
													std::span<AnimationStatus> statuses) {
		impl_->AssertCoordinatorThread();
		if(statuses.size() != requests.size()) {
			std::fill(statuses.begin(), statuses.end(), AnimationStatus::InvalidArgument);
			return;
		}
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		bool globalInvalidationPublished = false;
		struct PreparedBinding {
			const AnimatorPlaybackBinding* source = nullptr;
			ImmutableBindPreparation immutable;
		};
		std::vector<PreparedBinding> preparedBindings;
		std::vector<AnimatorInstanceHandle> mutatedHandles;
		std::vector<AnimatorInstanceHandle> retiredHandles;
		try {
			preparedBindings.reserve(requests.size());
			mutatedHandles.reserve(requests.size());
			retiredHandles.reserve(requests.size());
		} catch(const std::bad_alloc&) {
			std::fill(statuses.begin(), statuses.end(), AnimationStatus::InternalError);
			return;
		}
		for(std::size_t index = 0U; index < requests.size(); ++index) {
			const AnimatorPlaybackBindRequest& request = requests[index];
			if(request.binding == nullptr) {
				statuses[index] = AnimationStatus::InvalidArgument;
				continue;
			}
			AnimatorPlaybackBinding binding = *request.binding;
			binding.playOnBind = request.playOnBind;
			const Impl::RuntimeRecord* priorRecord = impl_->FindRuntimeRecordLocked(request.handle);
			const bool replacingExistingBinding = priorRecord != nullptr && priorRecord->bindingInitialized;
			auto prepared = std::find_if(preparedBindings.begin(), preparedBindings.end(),
										 [request](const PreparedBinding& value) { return value.source == request.binding; });
			const bool immutableBindingValidated = prepared != preparedBindings.end();
			if(!immutableBindingValidated) {
				preparedBindings.push_back({request.binding, {}});
				prepared = std::prev(preparedBindings.end());
			}
			bool mutated = false;
			statuses[index] =
				BindAnimatorPlaybackLocked(request.handle, std::move(binding), request.sharedValidatedClipView, &prepared->immutable,
										   immutableBindingValidated, true, !globalInvalidationPublished, mutated);
			globalInvalidationPublished = globalInvalidationPublished || mutated;
			if(mutated) {
				mutatedHandles.push_back(request.handle);
				if(replacingExistingBinding) {
					retiredHandles.push_back(request.handle);
				}
			} else if(!immutableBindingValidated) {
				// Failed first-use preparation must not make the next request skip
				// validation or consume an incomplete authored-default template.
				preparedBindings.erase(prepared);
			}
		}
		if(!retiredHandles.empty()) {
			std::sort(retiredHandles.begin(), retiredHandles.end());
			retiredHandles.erase(std::unique(retiredHandles.begin(), retiredHandles.end()), retiredHandles.end());
			// Every per-record replacement above already cleared its local runtime state.
			// Retire cross-subsystem publications once for the sorted crowd so attachment
			// and root-motion vectors are compacted linearly rather than once per owner.
			AnimationAttachmentSnapshotPublisher::Instance().RetireMany(retiredHandles);
			if(impl_->advancedCommands != nullptr) {
				impl_->advancedCommands->RetireAnimators(retiredHandles);
			}
			std::erase_if(impl_->pendingRootMotionApplications, [&retiredHandles](const AnimationRootMotionApplicationRequest& value) {
				return std::binary_search(retiredHandles.begin(), retiredHandles.end(), value.delta.animator);
			});
			AnimationRootMotionResultPublisher::Instance().RetireMany(retiredHandles);
			impl_->persistentReuse.rollbackHistory.ClearMany(retiredHandles);
		}
		if(!mutatedHandles.empty()) {
			// Evaluation cache keys are animator-contiguous. One bulk operation keeps
			// its mutex for the complete crowd instead of reacquiring it thousands of
			// times during one protected resource publication.
			(void)impl_->persistentReuse.evaluationCache.InvalidateAnimators(mutatedHandles);
		}
	}

	AnimationStatus AnimationSystem::RefreshAnimatorDirectClipStreaming(AnimatorInstanceHandle handle,
																		const AnimatorPlaybackBinding& streamedBinding,
																		AnimationClipView* sharedValidatedClipView) {
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		bool changed = false;
		bool replacedClipPublication = false;
		const AnimationStatus status =
			RefreshAnimatorDirectClipStreamingLocked(handle, streamedBinding, sharedValidatedClipView, EMPTY_UUID, false, false, 0U,
													 nullptr, nullptr, nullptr, changed, replacedClipPublication);
		if(changed) {
			(void)impl_->persistentReuse.evaluationCache.InvalidateAnimator(handle);
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		}
		return status;
	}

	AnimationStatus AnimationSystem::RefreshAnimatorDirectClipStreamingLocked(
		AnimatorInstanceHandle handle, const AnimatorPlaybackBinding& streamedBinding, AnimationClipView* sharedValidatedClipView,
		UUID expectedClipAssetId, bool allowClipPublicationReplacement, bool playOnBind, std::uint64_t replacementClipFingerprint,
		const AnimationEventStream* preparedReplacementEventStream, const AnimatorPlayback* preparedReplacementPlayback,
		const AnimatorPlaybackConfiguration* preparedReplacementConfiguration, bool& outChanged, bool& outReplacedClipPublication) {
		outChanged = false;
		outReplacedClipPublication = false;
		Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || record == nullptr) {
			return AnimationStatus::StaleHandle;
		}
		// The caller holds runtimeOwnershipMutex. Invalidation clears this exact
		// generation's runtime-record slot while holding the same mutex, so the
		// successful lookup already proves liveness. Rechecking the private store
		// mutex here serialized every member of a crowd-wide streaming refresh.
		const AnimatorPlaybackBinding& current = record->binding;
		const bool replacingPublishedClip =
			allowClipPublicationReplacement && current.clip && streamedBinding.clip && expectedClipAssetId != EMPTY_UUID &&
			streamedBinding.clip->assetId.value == expectedClipAssetId && current.clip->assetId == streamedBinding.clip->assetId &&
			current.clipGeneration != streamedBinding.clipGeneration;
		const bool publishingPreviouslyUnavailableClip = allowClipPublicationReplacement && !current.clip && streamedBinding.clip &&
														 expectedClipAssetId != EMPTY_UUID &&
														 streamedBinding.clip->assetId.value == expectedClipAssetId;
		const bool publishingClip = replacingPublishedClip || publishingPreviouslyUnavailableClip;
		// The narrow refresh path is valid only when every authored/resource identity
		// is unchanged. Controllers and retargeting may change several active clips or
		// sampling skeletons, so they continue through the full atomic bind path.
		const char* rejectionReason = nullptr;
		if(current.controllerProgramRequired || streamedBinding.controllerProgramRequired) {
			rejectionReason = "ControllerProgram";
		} else if(current.retargetProfile || streamedBinding.retargetProfile) {
			rejectionReason = "RetargetProfile";
		} else if(!streamedBinding.clip || (!current.clip && !publishingPreviouslyUnavailableClip)) {
			rejectionReason = "MissingClip";
		} else if(!current.skeleton || !streamedBinding.skeleton) {
			rejectionReason = "MissingSkeleton";
		} else if(current.clip && current.clip->assetId != streamedBinding.clip->assetId) {
			rejectionReason = "ClipAsset";
		} else if(current.clip && !publishingClip && current.clipGeneration != streamedBinding.clipGeneration) {
			rejectionReason = "ClipGeneration";
		} else if(publishingClip && replacementClipFingerprint == 0U) {
			rejectionReason = "ClipFingerprint";
		} else if(current.skeleton->assetId != streamedBinding.skeleton->assetId) {
			rejectionReason = "SkeletonAsset";
		} else if(current.skeletonGeneration != streamedBinding.skeletonGeneration) {
			rejectionReason = "SkeletonGeneration";
		} else if(!publishingClip && current.programGeneration != streamedBinding.programGeneration) {
			rejectionReason = "ProgramGeneration";
		} else if(!publishingClip && current.stateId != streamedBinding.stateId) {
			rejectionReason = "State";
		} else if(current.requestedExecutionPolicy != streamedBinding.requestedExecutionPolicy) {
			rejectionReason = "ExecutionPolicy";
		} else if(current.rootMotionPolicy != streamedBinding.rootMotionPolicy) {
			rejectionReason = "RootMotionPolicy";
		} else if(current.eventPolicy != streamedBinding.eventPolicy) {
			rejectionReason = "EventPolicy";
		}
		if(rejectionReason != nullptr) {
			return AnimationStatus::InvalidArgument;
		}
		const bool streamingInputsUnchanged =
			!publishingClip && current.residentSegments == streamedBinding.residentSegments &&
			current.streamedSegmentPayloads == streamedBinding.streamedSegmentPayloads &&
			current.gpuResidentPayloads == streamedBinding.gpuResidentPayloads &&
			current.gpuFastBinding == streamedBinding.gpuFastBinding &&
			current.gpuProgramResourceIndex == streamedBinding.gpuProgramResourceIndex &&
			current.evaluationGenerations.residencyGeneration == streamedBinding.evaluationGenerations.residencyGeneration &&
			current.gpuExecution.rendererAvailable == streamedBinding.gpuExecution.rendererAvailable &&
			current.gpuExecution.animationCapabilityAvailable == streamedBinding.gpuExecution.animationCapabilityAvailable &&
			current.gpuExecution.resourcesResident == streamedBinding.gpuExecution.resourcesResident &&
			current.gpuExecution.evaluationHealthy == streamedBinding.gpuExecution.evaluationHealthy &&
			current.gpuExecution.requiredBudget == streamedBinding.gpuExecution.requiredBudget &&
			current.gpuExecution.availableBudget == streamedBinding.gpuExecution.availableBudget;
		if(streamingInputsUnchanged) {
			// A shared clip publication can wake several retained requests at once.
			// Some components may already expose the final immutable state; do not
			// invalidate their clip views, history, or evaluation cache a second time.
			return AnimationStatus::Ok;
		}
		if(record->contentGeneration == std::numeric_limits<AnimationGeneration>::max() ||
		   record->historyGeneration == std::numeric_limits<AnimationGeneration>::max() ||
		   (publishingClip && record->stateGeneration == std::numeric_limits<std::uint64_t>::max()) ||
		   record->executionModeGeneration == std::numeric_limits<std::uint64_t>::max()) {
			return AnimationStatus::RangeOverflow;
		}

		AnimationClipView localValidatedView;
		AnimationClipView* validatedView = sharedValidatedClipView;
		AnimationStatus viewStatus = AnimationStatus::NotReady;
		const AnimationClipAsset& validatedClip = publishingClip ? *streamedBinding.clip : *current.clip;
		const SkeletonAsset* validatedSkeleton = publishingClip ? streamedBinding.skeleton.get() : current.skeleton.get();
		const AnimationGeneration validatedClipGeneration = publishingClip ? streamedBinding.clipGeneration : current.clipGeneration;
		if(sharedValidatedClipView != nullptr) {
			if(!sharedValidatedClipView->Matches(validatedClip, validatedSkeleton,
												 std::span<const std::uint8_t>(streamedBinding.residentSegments), validatedClipGeneration,
												 streamedBinding.streamedSegmentPayloads)) {
				// Preparation can publish a fresh shared_ptr wrapper for unchanged asset
				// bytes. Bind the scratch view once to the first live runtime pointer pair;
				// subsequent identical prefab records then share that completed validation.
				viewStatus = sharedValidatedClipView->Open(validatedClip, validatedSkeleton,
														   std::span<const std::uint8_t>(streamedBinding.residentSegments),
														   validatedClipGeneration, streamedBinding.streamedSegmentPayloads);
			} else {
				viewStatus = AnimationStatus::Ok;
			}
		} else {
			viewStatus =
				localValidatedView.Open(validatedClip, validatedSkeleton, std::span<const std::uint8_t>(streamedBinding.residentSegments),
										validatedClipGeneration, streamedBinding.streamedSegmentPayloads);
			validatedView = &localValidatedView;
		}
		if(viewStatus != AnimationStatus::Ok) {
			return viewStatus;
		}

		AnimationEventStream replacementEventStream;
		AnimatorPlayback replacementPlayback;
		std::vector<AnimatorControllerClipBinding> replacementControllerClips;
		std::vector<std::uint64_t> replacementMorphStableIds;
		std::vector<std::uint64_t> replacementCurveStableIds;
		AnimatorPlaybackConfiguration replacementPlaybackConfiguration =
			preparedReplacementConfiguration != nullptr ? *preparedReplacementConfiguration : streamedBinding.playback;
		if(publishingClip) {
			try {
				// The direct program and named-output directories are derived from the
				// immutable clip publication. Stage their copies before mutating the live
				// record so allocation failure cannot expose a half-promoted binding.
				replacementControllerClips = streamedBinding.controllerClips;
				replacementMorphStableIds = streamedBinding.morphStableIds;
				replacementCurveStableIds = streamedBinding.curveStableIds;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
			if(preparedReplacementEventStream != nullptr) {
				// Batch preparation performs invariant clip-event sorting and playback
				// setup once per direct-clip cohort before runtime ownership is locked.
				// Copies retain independent cursors while sharing immutable event arrays.
				replacementEventStream = *preparedReplacementEventStream;
			} else if(streamedBinding.eventPolicy != AnimatorEventPolicy::Suppress) {
				const AnimationAssetHandle clipHandle{streamedBinding.clip->assetId, streamedBinding.clipGeneration};
				if(const AnimationStatus eventStatus =
					   replacementEventStream.Configure(clipHandle, streamedBinding.clip->durationTicks, streamedBinding.clip->events);
				   eventStatus != AnimationStatus::Ok) {
					return eventStatus;
				}
			}
			if(preparedReplacementPlayback != nullptr) {
				replacementPlayback = *preparedReplacementPlayback;
			} else {
				replacementPlaybackConfiguration.durationTicks = streamedBinding.clip->durationTicks;
				if(streamedBinding.updatePolicy == AnimatorUpdatePolicy::FixedStep) {
					replacementPlaybackConfiguration.timePolicy = AnimatorTimePolicy::Fixed;
				}
				if(const AnimationStatus playbackStatus = replacementPlayback.Configure(replacementPlaybackConfiguration);
				   playbackStatus != AnimationStatus::Ok) {
					return playbackStatus;
				}
				if(const AnimationStatus seekStatus = replacementPlayback.Seek(streamedBinding.initialPlaybackTick);
				   seekStatus != AnimationStatus::Ok) {
					return seekStatus;
				}
				if(playOnBind) {
					if(const AnimationStatus playStatus = replacementPlayback.Play(); playStatus != AnimationStatus::Ok) {
						return playStatus;
					}
				}
			}
		}

		viewStatus = publishingClip ? record->clipView.AdoptValidatedPublicationFrom(*validatedView)
									: record->clipView.CopyStreamingStateFrom(*validatedView);
		if(viewStatus != AnimationStatus::Ok) {
			return viewStatus;
		}
		if(publishingClip) {
			// Reuse skeleton/mesh policy storage, but atomically publish every value
			// derived from the direct clip. This also promotes records created while the
			// shared clip was unavailable, without 1,000 independent full binds.
			record->binding.clip = streamedBinding.clip;
			record->binding.skeleton = streamedBinding.skeleton;
			record->binding.clipGeneration = streamedBinding.clipGeneration;
			record->binding.clipDeterministicContentFingerprint = replacementClipFingerprint;
			record->binding.evaluationProgram = streamedBinding.evaluationProgram;
			record->binding.controllerClips = std::move(replacementControllerClips);
			record->binding.programGeneration = streamedBinding.programGeneration;
			record->binding.stateId = streamedBinding.stateId;
			record->binding.morphStableIds = std::move(replacementMorphStableIds);
			record->binding.curveStableIds = std::move(replacementCurveStableIds);
			record->binding.playback = replacementPlaybackConfiguration;
			record->binding.initialPlaybackTick = streamedBinding.initialPlaybackTick;
			record->eventStream = std::move(replacementEventStream);
			record->eventStateCaptured = false;
			record->playback = replacementPlayback;
			record->graphState = {};
			record->transition = {};
			++record->stateGeneration;
			record->executionModeState = {};
			AnimationAttachmentSnapshotPublisher::Instance().Retire(handle);
			impl_->persistentReuse.rollbackHistory.Clear(handle);
		}
		// The cohort-prepared binding owns an immutable copy-on-write bitmap. Share it
		// across runtime records rather than detaching and copying it once per member.
		record->binding.residentSegments = streamedBinding.residentSegments;
		record->binding.streamedSegmentPayloads = streamedBinding.streamedSegmentPayloads;
		record->binding.gpuExecution = streamedBinding.gpuExecution;
		record->binding.gpuProgramResourceIndex = streamedBinding.gpuProgramResourceIndex;
		record->binding.gpuVisual = streamedBinding.gpuVisual;
		record->binding.gpuResidentPayloads = streamedBinding.gpuResidentPayloads;
		record->binding.gpuFastBinding = streamedBinding.gpuFastBinding;
		if(publishingClip) {
			record->binding.evaluationGenerations = streamedBinding.evaluationGenerations;
		} else {
			record->binding.evaluationGenerations.residencyGeneration = streamedBinding.evaluationGenerations.residencyGeneration;
		}
		record->dependencyStatus = AnimationStatus::Ok;
		++record->contentGeneration;
		++record->historyGeneration;
		++record->executionModeGeneration;
		outChanged = true;
		outReplacedClipPublication = publishingClip;
		return AnimationStatus::Ok;
	}

	void AnimationSystem::RefreshAnimatorDirectClipStreamingBatch(std::span<const AnimatorDirectClipStreamingRefresh> refreshes,
																  std::span<AnimationStatus> statuses) {
		impl_->AssertCoordinatorThread();
		if(statuses.size() != refreshes.size()) {
			std::fill(statuses.begin(), statuses.end(), AnimationStatus::InvalidArgument);
			return;
		}
		struct PreparedDirectClipReplacement {
			const AnimatorPlaybackBinding* binding = nullptr;
			UUID expectedClipAssetId = EMPTY_UUID;
			bool playOnBind = false;
			bool hasEventStream = false;
			AnimationEventStream eventStream;
			AnimatorPlayback playback;
			AnimatorPlaybackConfiguration playbackConfiguration{};
			AnimationStatus status = AnimationStatus::NotReady;
		};
		std::vector<PreparedDirectClipReplacement> preparedReplacements;
		std::vector<std::uint32_t> preparedReplacementIndices;
		std::vector<AnimatorInstanceHandle> changedAnimators;
		try {
			// Reserve and prepare every cohort-invariant replacement before publishing
			// record changes. Allocation or validation failure then leaves the complete
			// protected batch untouched and reports a bounded error per item.
			preparedReplacements.reserve(refreshes.size());
			preparedReplacementIndices.assign(refreshes.size(), std::numeric_limits<std::uint32_t>::max());
			changedAnimators.reserve(refreshes.size());
			for(std::size_t index = 0U; index < refreshes.size(); ++index) {
				const AnimatorDirectClipStreamingRefresh& refresh = refreshes[index];
				if(!refresh.allowClipPublicationReplacement || refresh.streamedBinding == nullptr) {
					continue;
				}
				if(!preparedReplacements.empty()) {
					const PreparedDirectClipReplacement& prior = preparedReplacements.back();
					if(prior.binding == refresh.streamedBinding && prior.expectedClipAssetId == refresh.expectedClipAssetId &&
					   prior.playOnBind == refresh.playOnBind) {
						preparedReplacementIndices[index] = static_cast<std::uint32_t>(preparedReplacements.size() - 1U);
						continue;
					}
				}
				PreparedDirectClipReplacement prepared{};
				prepared.binding = refresh.streamedBinding;
				prepared.expectedClipAssetId = refresh.expectedClipAssetId;
				prepared.playOnBind = refresh.playOnBind;
				if(!refresh.streamedBinding->clip) {
					prepared.status = AnimationStatus::MissingAsset;
				} else if(refresh.expectedClipAssetId == EMPTY_UUID ||
						  refresh.streamedBinding->clip->assetId.value != refresh.expectedClipAssetId) {
					prepared.status = AnimationStatus::InvalidArgument;
				} else {
					prepared.playbackConfiguration = refresh.streamedBinding->playback;
					prepared.playbackConfiguration.durationTicks = refresh.streamedBinding->clip->durationTicks;
					if(refresh.streamedBinding->updatePolicy == AnimatorUpdatePolicy::FixedStep) {
						prepared.playbackConfiguration.timePolicy = AnimatorTimePolicy::Fixed;
					}
					prepared.status = prepared.playback.Configure(prepared.playbackConfiguration);
					if(prepared.status == AnimationStatus::Ok) {
						prepared.status = prepared.playback.Seek(refresh.streamedBinding->initialPlaybackTick);
					}
					if(prepared.status == AnimationStatus::Ok && refresh.playOnBind) {
						prepared.status = prepared.playback.Play();
					}
					if(prepared.status == AnimationStatus::Ok && refresh.streamedBinding->eventPolicy != AnimatorEventPolicy::Suppress) {
						const AnimationAssetHandle clipHandle{refresh.streamedBinding->clip->assetId,
															  refresh.streamedBinding->clipGeneration};
						prepared.status = prepared.eventStream.Configure(clipHandle, refresh.streamedBinding->clip->durationTicks,
																		 refresh.streamedBinding->clip->events);
						prepared.hasEventStream = prepared.status == AnimationStatus::Ok;
					}
				}
				preparedReplacements.push_back(std::move(prepared));
				preparedReplacementIndices[index] = static_cast<std::uint32_t>(preparedReplacements.size() - 1U);
			}
		} catch(const std::bad_alloc&) {
			std::fill(statuses.begin(), statuses.end(), AnimationStatus::InternalError);
			return;
		}

		std::unique_lock lock(impl_->runtimeOwnershipMutex, std::defer_lock);
		{
			// Measure only contention for runtime ownership. The surrounding batch
			// scope also includes record refresh and presentation invalidation, which
			// otherwise makes an occasional cross-frame lock wait look like copy work.
			TERRAENGINE_PROFILE_SCOPE("Animation: Direct Clip Batch Runtime Lock Wait",
									  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			lock.lock();
		}
		bool replacedClipPublication = false;
		const AnimatorPlaybackBinding* priorReplacementBinding = nullptr;
		UUID priorExpectedClipAssetId = EMPTY_UUID;
		AnimationStatus priorReplacementStatus = AnimationStatus::NotReady;
		std::uint64_t priorReplacementClipFingerprint = 0U;
		{
			TERRAENGINE_PROFILE_SCOPE("Animation: Refresh Direct Clip Runtime Records",
									  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for(std::size_t index = 0U; index < refreshes.size(); ++index) {
				const AnimatorDirectClipStreamingRefresh& refresh = refreshes[index];
				if(refresh.streamedBinding == nullptr) {
					statuses[index] = AnimationStatus::InvalidArgument;
					continue;
				}
				const std::uint32_t preparedIndex = preparedReplacementIndices[index];
				const PreparedDirectClipReplacement* prepared =
					preparedIndex < preparedReplacements.size() ? &preparedReplacements[preparedIndex] : nullptr;
				if(prepared != nullptr && prepared->status != AnimationStatus::Ok) {
					statuses[index] = prepared->status;
					continue;
				}
				std::uint64_t replacementClipFingerprint = 0U;
				if(refresh.allowClipPublicationReplacement) {
					if(priorReplacementBinding == refresh.streamedBinding && priorExpectedClipAssetId == refresh.expectedClipAssetId) {
						// The coordinator groups prefab instances by one immutable prepared
						// binding. Reuse its completed identity/fingerprint result instead of
						// repeating an ordered-map lookup for every member of a large crowd.
						if(priorReplacementStatus != AnimationStatus::Ok) {
							statuses[index] = priorReplacementStatus;
							continue;
						}
						replacementClipFingerprint = priorReplacementClipFingerprint;
					} else {
						priorReplacementBinding = refresh.streamedBinding;
						priorExpectedClipAssetId = refresh.expectedClipAssetId;
						priorReplacementStatus = AnimationStatus::Ok;
						priorReplacementClipFingerprint = 0U;
						if(!refresh.streamedBinding->clip) {
							priorReplacementStatus = AnimationStatus::MissingAsset;
						} else if(refresh.expectedClipAssetId == EMPTY_UUID ||
								  refresh.streamedBinding->clip->assetId.value != refresh.expectedClipAssetId) {
							priorReplacementStatus = AnimationStatus::InvalidArgument;
						} else {
							const AnimationAssetHandle clipKey{refresh.streamedBinding->clip->assetId,
															   refresh.streamedBinding->clipGeneration};
							const auto cachedFingerprint = impl_->persistentReuse.clipContentFingerprints.find(clipKey);
							if(cachedFingerprint != impl_->persistentReuse.clipContentFingerprints.end()) {
								priorReplacementClipFingerprint = cachedFingerprint->second;
							} else {
								priorReplacementStatus = ComputeClipDeterministicContentFingerprint(*refresh.streamedBinding->clip,
																									priorReplacementClipFingerprint);
								if(priorReplacementStatus == AnimationStatus::Ok) {
									try {
										impl_->persistentReuse.clipContentFingerprints.emplace(clipKey, priorReplacementClipFingerprint);
									} catch(const std::bad_alloc&) {
										priorReplacementStatus = AnimationStatus::InternalError;
									}
								}
							}
						}
						if(priorReplacementStatus != AnimationStatus::Ok) {
							statuses[index] = priorReplacementStatus;
							continue;
						}
						replacementClipFingerprint = priorReplacementClipFingerprint;
					}
				}
				bool changed = false;
				bool replacedThisClipPublication = false;
				statuses[index] = RefreshAnimatorDirectClipStreamingLocked(
					refresh.handle, *refresh.streamedBinding, refresh.sharedValidatedClipView, refresh.expectedClipAssetId,
					refresh.allowClipPublicationReplacement, refresh.playOnBind, replacementClipFingerprint,
					prepared != nullptr && prepared->hasEventStream ? &prepared->eventStream : nullptr,
					prepared != nullptr ? &prepared->playback : nullptr, prepared != nullptr ? &prepared->playbackConfiguration : nullptr,
					changed, replacedThisClipPublication);
				if(changed) {
					changedAnimators.push_back(refresh.handle);
					replacedClipPublication = replacedClipPublication || replacedThisClipPublication;
				}
			}
		}
		if(replacedClipPublication) {
			// A shared generation publication invalidates the captured asset world
			// once for the complete batch, not once per prefab runtime record.
			TERRAENGINE_PROFILE_SCOPE("Animation: Invalidate Direct Clip Lifecycle", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Asset);
		}
		if(!changedAnimators.empty()) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Invalidate Streamed Evaluation Cache",
									  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			const std::size_t activeCount = impl_->instances->ActiveCount();
			const std::size_t halfActiveCount = activeCount / 2U + activeCount % 2U;
			if(changedAnimators.size() >= halfActiveCount) {
				// A shared publication commonly promotes nearly the complete crowd. One
				// linear clear is much cheaper than thousands of ordered-map lower_bounds;
				// broader eviction is correctness-safe because cache entries are optional.
				impl_->persistentReuse.evaluationCache.InvalidateAll();
			} else {
				(void)impl_->persistentReuse.evaluationCache.InvalidateAnimators(changedAnimators);
			}
			// One immutable stream publication changes one runtime declaration epoch,
			// regardless of how many prefab records consumed the shared bytes.
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		}
		lock.unlock();
		if(replacedClipPublication) {
			// Presentation owns a separate protected publication. Notify it only after
			// releasing runtime ownership so its retirement path cannot form a lock
			// cycle with animation capture or render publication.
			TERRAENGINE_PROFILE_SCOPE("Animation: Invalidate Direct Clip Presentation",
									  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(AnimationLifecycleBoundary::Asset);
		}
	}

	AnimationStatus AnimationSystem::UpdateEvaluationGenerations(AnimatorInstanceHandle handle,
																 AnimationEvaluationExternalGenerations generations) {
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || record == nullptr || !impl_->instances->IsAlive(handle)) {
			return AnimationStatus::StaleHandle;
		}
		if(generations.unchangedInputReuseEligible && !generations.IsComplete()) {
			return AnimationStatus::InvalidArgument;
		}
		if(record->binding.evaluationGenerations == generations) {
			return AnimationStatus::Ok;
		}

		record->binding.evaluationGenerations = generations;
		(void)impl_->persistentReuse.evaluationCache.InvalidateAnimator(handle);
		(void)impl_->evaluationState.evaluationCancellationSource.Cancel(AnimationCancellationReason::Explicit);
		impl_->evaluationState.workCancellationSource.RequestCancellation();
		impl_->evaluationState.frameCancellationRequested.store(true, std::memory_order_release);
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		return AnimationStatus::Ok;
	}

	std::shared_ptr<const AnimationSnapshot> AnimationSystem::LoadAnimatorSnapshot(AnimatorInstanceHandle handle) const noexcept {
		const std::shared_ptr<const AnimationAuthorityFrame> authority = LoadPublishedAuthorityFrame();
		const AnimationAuthorityRow* row = authority ? authority->Find(handle) : nullptr;
		return row ? row->snapshot : std::shared_ptr<const AnimationSnapshot>{};
	}

	AnimatorPlaybackStateSnapshot AnimationSystem::LoadAnimatorPlaybackState(AnimatorInstanceHandle handle) const noexcept {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		const std::shared_ptr<const Impl::CompactRuntimeStateRoot> compactRoot =
			impl_->compactRuntimeStates.load(std::memory_order_acquire);
		if(const Impl::CompactRuntimeState* compact = Impl::FindCompactRuntimeState(compactRoot, handle)) {
			return compact->playback.Snapshot();
		}
		const Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		return record != nullptr && impl_->instances->IsAlive(handle) ? record->playback.Snapshot() : AnimatorPlaybackStateSnapshot{};
	}

	AnimationCommandReceipt AnimationSystem::SubmitCommand(AnimationCommand command) {
		// Command submission is the intentional cross-producer entry point. The
		// short lifecycle guard makes queue/store ownership atomic with Shutdown;
		// command ordering and payload storage remain inside AnimationCommandQueue.
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->commands == nullptr) {
			// Only Initialize may begin another runtime session. A producer arriving
			// during or after shutdown gets a copied terminal receipt and cannot
			// resurrect queue ownership on its thread.
			AnimationCommandReceipt receipt;
			receipt.target = command.target;
			receipt.producerSequence = command.producerSequence;
			receipt.correlationId = command.correlationId;
			receipt.status = AnimationStatus::Cancelled;
			receipt.targetTick = command.targetTick;
			return receipt;
		}
		return impl_->commands->Enqueue(std::move(command), *impl_->instances);
	}

	AnimationCommandBatchReceipt AnimationSystem::SubmitCommandBatch(std::span<const AnimationCommand> commands) {
		// The lifecycle lock keeps the instance store and command queue from being
		// retired between validation and the queue's all-or-nothing insertion.
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire) && impl_->commands != nullptr) {
			return impl_->commands->EnqueueBatch(commands, *impl_->instances);
		}

		AnimationCommandBatchReceipt batch;
		batch.status = commands.empty() ? AnimationStatus::InvalidArgument : AnimationStatus::Cancelled;
		try {
			batch.commands.reserve(commands.size());
			for(const AnimationCommand& command : commands) {
				AnimationCommandReceipt receipt;
				receipt.target = command.target;
				receipt.producerId = command.producerId;
				receipt.producerSequence = command.producerSequence;
				receipt.correlationId = command.correlationId;
				receipt.idempotencyKey = command.idempotencyKey;
				receipt.cancellationToken = command.cancellationToken;
				receipt.status = batch.status;
				receipt.targetTick = command.targetTick;
				batch.commands.push_back(receipt);
			}
		} catch(const std::bad_alloc&) {
			batch.commands.clear();
			batch.status = AnimationStatus::InternalError;
		}
		return batch;
	}

	AnimationCommandReceipt AnimationSystem::CancelCommand(std::uint64_t cancellationToken, std::uint64_t correlationId) {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->commands == nullptr) {
			AnimationCommandReceipt receipt;
			receipt.cancellationToken = cancellationToken;
			receipt.correlationId = correlationId;
			receipt.status = AnimationStatus::Cancelled;
			return receipt;
		}
		return impl_->commands->Cancel(cancellationToken, correlationId);
	}

	AnimationCommandReceipt AnimationSystem::ReadCommandReceipt(std::uint32_t producerId, std::uint64_t producerSequence,
																std::uint64_t correlationId) const {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->commands == nullptr) {
			AnimationCommandReceipt receipt;
			receipt.producerId = producerId;
			receipt.producerSequence = producerSequence;
			receipt.correlationId = correlationId;
			receipt.status = AnimationStatus::Cancelled;
			return receipt;
		}
		return impl_->commands->ReadReceipt(producerId, producerSequence, correlationId);
	}

	AnimationRootMotionApplicationReceipt
	AnimationSystem::SubmitRootMotionApplication(AnimationRootMotionDeferredApplicationRequest request) {
		AnimationRootMotionApplicationReceipt receipt;
		receipt.animator = request.animator;
		receipt.mode = request.mode;
		receipt.generation = request.expectedPoseGeneration;
		receipt.producerId = request.producerId;
		receipt.producerSequence = request.producerSequence;
		receipt.correlationId = request.correlationId;
		receipt.idempotencyKey = request.idempotencyKey;
		receipt.publicationGeneration = request.expectedPublicationGeneration;
		AnimationRootMotionApplicationRequest resolved;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		{
			if(!impl_->runtimeActive.load(std::memory_order_acquire)) {
				receipt.status = AnimationStatus::Cancelled;
				return receipt;
			}
			const Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(request.animator);
			if(record == nullptr || !impl_->instances->IsAlive(request.animator)) {
				receipt.status = AnimationStatus::StaleHandle;
				return receipt;
			}
			const std::shared_ptr<const AnimationAuthorityFrame> authority = impl_->authorityPublisher.Load();
			const AnimationAuthorityRow* published = authority ? authority->Find(request.animator) : nullptr;
			if(!published || !published->snapshot || !published->snapshot->pose || !published->snapshot->pose->RootMotionDelta()) {
				receipt.status = AnimationStatus::NotReady;
				return receipt;
			}
			const AnimationRootMotionDelta& delta = *published->snapshot->pose->RootMotionDelta();
			if(request.expectedPoseGeneration == 0U || request.expectedPublicationGeneration == 0U) {
				receipt.status = AnimationStatus::InvalidArgument;
				return receipt;
			}
			if(delta.generation != request.expectedPoseGeneration ||
			   published->authorityPublicationGeneration != request.expectedPublicationGeneration) {
				receipt.status = AnimationStatus::StaleHandle;
				return receipt;
			}
			resolved.delta = delta;
			resolved.ownerObjectId = published->ownerObjectId;
			resolved.objectGeneration = published->objectGeneration;
			resolved.componentGeneration = published->componentGeneration;
			resolved.sourceObjectWorldTransform = published->sourceObjectWorldTransform;
			resolved.sourceObjectWorldTransformCaptured = published->sourceObjectWorldTransformCaptured;
			if(resolved.sourceObjectWorldTransformCaptured) {
				const AnimationFloat3 scaled{delta.localTranslation.x * resolved.sourceObjectWorldTransform.scale.x,
											 delta.localTranslation.y * resolved.sourceObjectWorldTransform.scale.y,
											 delta.localTranslation.z * resolved.sourceObjectWorldTransform.scale.z};
				resolved.delta.worldTranslation = RotateVector(resolved.sourceObjectWorldTransform.rotation, scaled);
				resolved.delta.worldYawRadians = delta.localYawRadians;
			}
			resolved.mode = request.mode;
			resolved.producerId = request.producerId;
			resolved.producerSequence = request.producerSequence;
			resolved.correlationId = request.correlationId;
			resolved.idempotencyKey = request.idempotencyKey;
			resolved.publicationGeneration = request.expectedPublicationGeneration;
			resolved.fixedDurationSeconds = request.fixedDurationSeconds;
		}
		if(impl_->pendingRootMotionApplications.size() >= kMaximumPendingRootMotionApplications) {
			receipt.status = AnimationStatus::HardLimitExceeded;
			return receipt;
		}
		try {
			// Grow the copied queue before reserving the once-only publisher record.
			// The request itself owns no dynamic storage, so push_back cannot allocate
			// after this point and the reservation remains fail-atomic.
			impl_->pendingRootMotionApplications.reserve(impl_->pendingRootMotionApplications.size() + 1U);
		} catch(const std::bad_alloc&) {
			receipt.status = AnimationStatus::InternalError;
			return receipt;
		}
		bool shouldQueue = false;
		receipt = AnimationRootMotionResultPublisher::Instance().ReserveApplication(resolved, shouldQueue);
		if(!shouldQueue)
			return receipt;
		impl_->pendingRootMotionApplications.push_back(resolved);
		return receipt;
	}

	NativeAnimationProceduralGoalReceipt AnimationSystem::SubmitProceduralGoal(const NativeAnimationProceduralGoalCommand& command) {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->proceduralCommands == nullptr || impl_->instances == nullptr) {
			NativeAnimationProceduralGoalReceipt receipt{};
			receipt.header = {static_cast<std::uint32_t>(sizeof(receipt)), kNativeAnimationAbiVersion,
							  static_cast<std::uint16_t>(NativeAnimationRecordKind::ProceduralGoalReceipt)};
			receipt.status = NativeAnimationStatus::Cancelled;
			receipt.animator = command.animator;
			receipt.command = command.command;
			receipt.producerId = command.producerId;
			receipt.producerSequence = command.producerSequence;
			receipt.correlationId = command.correlationId;
			receipt.idempotencyKey = command.idempotencyKey;
			receipt.cancellationToken = command.cancellationToken;
			receipt.goalId = command.goalId;
			receipt.observableTick = command.targetTick;
			return receipt;
		}
		return impl_->proceduralCommands->Submit(command, *impl_->instances);
	}

	AnimationStatus AnimationSystem::CopyProceduralResults(AnimatorInstanceHandle handle,
														   std::span<NativeAnimationProceduralResultSnapshot> destination,
														   std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->proceduralCommands == nullptr || impl_->instances == nullptr) {
			return AnimationStatus::Cancelled;
		}
		if(!impl_->instances->IsAlive(handle))
			return AnimationStatus::StaleHandle;
		return impl_->proceduralCommands->CopyResults({handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyProceduralGoals(AnimatorInstanceHandle handle,
														 std::span<NativeAnimationProceduralGoalCommand> destination,
														 std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->proceduralCommands == nullptr || impl_->instances == nullptr) {
			return AnimationStatus::Cancelled;
		}
		if(!impl_->instances->IsAlive(handle))
			return AnimationStatus::StaleHandle;
		return impl_->proceduralCommands->CopyActiveGoals({handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	NativeAnimationRetargetReceipt AnimationSystem::SubmitRetargetRequest(const NativeAnimationRetargetRequest& request) {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire) && impl_->advancedCommands && impl_->instances) {
			NativeAnimationRetargetReceipt receipt = impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration = impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationRetargetReceipt receipt{};
		receipt.header = MakeNativeHeader(NativeAnimationRecordKind::RetargetReceipt, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.operation = request.operation;
		receipt.profileGeneration = request.profileGeneration;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	NativeAnimationMotionMatchReceipt AnimationSystem::SubmitMotionMatchRequest(const NativeAnimationMotionMatchRequest& request) {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire) && impl_->advancedCommands && impl_->instances) {
			NativeAnimationMotionMatchReceipt receipt = impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration = impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationMotionMatchReceipt receipt{};
		receipt.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchReceipt, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.operation = request.operation;
		receipt.databaseGeneration = request.databaseGeneration;
		receipt.queryId = request.queryId;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	NativeAnimationMotionMatchQueryReceiptV2
	AnimationSystem::SubmitMotionMatchQueryV2(const NativeAnimationMotionMatchQueryRequestV2& request) {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire) && impl_->advancedCommands && impl_->instances) {
			NativeAnimationMotionMatchQueryReceiptV2 receipt = impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration = impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationMotionMatchQueryReceiptV2 receipt{};
		receipt.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchQueryReceiptV2, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.databaseGeneration = request.databaseGeneration;
		receipt.schemaFingerprint = request.schemaFingerprint;
		receipt.queryId = request.queryId;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	NativeAnimationRagdollReceipt AnimationSystem::SubmitRagdollRequest(const NativeAnimationRagdollRequest& request) {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire) && impl_->advancedCommands && impl_->instances) {
			std::shared_ptr<const AnimationRagdollExclusionSnapshot> stagedExclusion;
			if(request.operation == NativeAnimationRagdollOperation::Activate) {
				try {
					auto next = std::make_shared<AnimationRagdollExclusionSnapshot>();
					const auto current = impl_->ragdollExclusions.load(std::memory_order_acquire);
					if(current)
						next->entries = current->entries;
					if(current && current->publicationGeneration == std::numeric_limits<std::uint64_t>::max())
						throw std::bad_alloc{};
					next->publicationGeneration = current ? current->publicationGeneration + 1U : 1U;
					const AnimatorInstanceHandle handle{request.animator.slotIndex, request.animator.generation};
					const auto found = std::find_if(next->entries.begin(), next->entries.end(),
													[handle](const AnimationRagdollExclusion& entry) { return entry.animator == handle; });
					if(found == next->entries.end()) {
						if(next->entries.size() >= 4096U)
							throw std::bad_alloc{};
						next->entries.push_back({handle, AnimationRagdollExclusionState::PendingActivate, request.correlationId,
												 RagdollExternalOwnerId(handle)});
					} else {
						found->state = AnimationRagdollExclusionState::PendingActivate;
						found->correlationId = request.correlationId;
						found->externalOwnerId = RagdollExternalOwnerId(handle);
					}
					stagedExclusion = std::move(next);
				} catch(const std::bad_alloc&) {
					// Do not accept an Activate that cannot become visible to the
					// preceding movement authority boundary in the same frame.
					NativeAnimationRagdollReceipt rejected{};
					rejected.header = MakeNativeHeader(NativeAnimationRecordKind::RagdollReceipt, sizeof(rejected));
					rejected.status = NativeAnimationStatus::HardLimitExceeded;
					rejected.animator = request.animator;
					rejected.operation = request.operation;
					rejected.profileGeneration = request.profileGeneration;
					rejected.correlationId = request.correlationId;
					rejected.observableTick = request.targetTick;
					return rejected;
				}
			}
			NativeAnimationRagdollReceipt receipt = impl_->advancedCommands->Submit(request, *impl_->instances);
			if(receipt.status == NativeAnimationStatus::Queued && stagedExclusion)
				impl_->ragdollExclusions.store(std::move(stagedExclusion), std::memory_order_release);
			receipt.publicationGeneration = impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationRagdollReceipt receipt{};
		receipt.header = MakeNativeHeader(NativeAnimationRecordKind::RagdollReceipt, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.operation = request.operation;
		receipt.profileGeneration = request.profileGeneration;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	NativeAnimationDeterminismReceipt AnimationSystem::SubmitDeterminismRequest(const NativeAnimationDeterminismRequest& request) {
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire) && impl_->advancedCommands && impl_->instances) {
			NativeAnimationDeterminismReceipt receipt = impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration = impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationDeterminismReceipt receipt{};
		receipt.header = MakeNativeHeader(NativeAnimationRecordKind::DeterminismReceipt, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.operation = request.operation;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	AnimationStatus AnimationSystem::CopyRetargetSnapshots(AnimatorInstanceHandle handle,
														   std::span<NativeAnimationRetargetSnapshot> destination,
														   std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || !impl_->advancedCommands || !impl_->instances)
			return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle))
			return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy({handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyMotionMatchSnapshots(AnimatorInstanceHandle handle,
															  std::span<NativeAnimationMotionMatchSnapshot> destination,
															  std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || !impl_->advancedCommands || !impl_->instances)
			return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle))
			return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy({handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyMotionMatchResultsV2(AnimatorInstanceHandle handle,
															  std::span<NativeAnimationMotionMatchResultV2> destination,
															  std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || !impl_->advancedCommands || !impl_->instances)
			return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle))
			return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy({handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyRagdollSnapshots(AnimatorInstanceHandle handle,
														  std::span<NativeAnimationRagdollSnapshot> destination,
														  std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || !impl_->advancedCommands || !impl_->instances)
			return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle))
			return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy({handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyDeterminismSnapshots(AnimatorInstanceHandle handle,
															  std::span<NativeAnimationDeterminismSnapshot> destination,
															  std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || !impl_->advancedCommands || !impl_->instances)
			return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle))
			return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy({handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	void AnimationSystem::PublishRootControllerBindings(std::shared_ptr<const AnimationRootControllerBindingSnapshot> bindings) noexcept {
		impl_->rootControllerBindings.store(std::move(bindings), std::memory_order_release);
	}

	std::shared_ptr<const AnimationRootControllerBindingSnapshot> AnimationSystem::ReadRootControllerBindings() const noexcept {
		return impl_->rootControllerBindings.load(std::memory_order_acquire);
	}

	std::shared_ptr<const AnimationRagdollExclusionSnapshot> AnimationSystem::ReadRagdollExclusions() const noexcept {
		return impl_->ragdollExclusions.load(std::memory_order_acquire);
	}

	AnimatorInstanceHandle AnimationSystem::ResolveAnimatorForObject(const UUID& ownerObjectId) const noexcept {
		if(ownerObjectId == EMPTY_UUID)
			return {};
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		const std::shared_ptr<Impl::RuntimeRecordRoot> root = impl_->runtimeRecords.load(std::memory_order_acquire);
		if(root == nullptr)
			return {};
		AnimatorInstanceHandle resolved{};
		root->ForEach([&](const std::shared_ptr<Impl::RuntimeRecord>& record) {
			if(!resolved.IsValid() && record && record->descriptor.ownerObjectId == ownerObjectId &&
			   impl_->instances->IsAlive(record->handle)) {
				resolved = record->handle;
			}
		});
		return resolved;
	}
} // namespace TerraEngine::Animation
