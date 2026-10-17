#include "Animation/Frame/AnimationFrameRuntime.h"

namespace TerraEngine::Animation {
	std::uint64_t AnimationSystem::Impl::MixWorkFingerprint(std::uint64_t hash, std::uint64_t value) noexcept {
		// FNV-1a over fixed-width values provides a platform-independent grouping
		// fingerprint. This is a compatibility identity, not a fabricated lifecycle
		// generation, and every evaluator/layout input is mixed below.
		for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
			hash ^= (value >> (byte * 8U)) & 0xffU;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	std::uint64_t AnimationSystem::Impl::BuildBindingOutputRequirementsFingerprint(const AnimatorPlaybackBinding& binding) noexcept {
		std::uint64_t hash = 1469598103934665603ULL;
		hash = MixWorkFingerprint(hash, binding.jointStableIds.size());
		hash = MixWorkFingerprint(hash, binding.morphStableIds.size());
		hash = MixWorkFingerprint(hash, binding.curveStableIds.size());
		for(const std::uint64_t stableId : binding.jointStableIds) {
			hash = MixWorkFingerprint(hash, stableId);
		}
		for(const std::uint64_t stableId : binding.morphStableIds) {
			hash = MixWorkFingerprint(hash, stableId);
		}
		for(const std::uint64_t stableId : binding.curveStableIds) {
			hash = MixWorkFingerprint(hash, stableId);
		}
		return hash;
	}

	std::uint64_t AnimationSystem::Impl::BuildBindingResourceLayoutFingerprint(const AnimatorPlaybackBinding& binding) noexcept {
		std::uint64_t hash = 1469598103934665603ULL;
		hash = MixWorkFingerprint(hash, binding.gpuProgramResourceIndex);
		hash = MixWorkFingerprint(hash, binding.residentSegments.size());
		for(const std::uint8_t resident : binding.residentSegments) {
			hash = MixWorkFingerprint(hash, resident);
		}
		if(binding.streamedSegmentPayloads) {
			const AnimationClipSegmentPayloadSet& payloads = *binding.streamedSegmentPayloads;
			hash = MixWorkFingerprint(hash, payloads.generation);
			hash = MixWorkFingerprint(hash, payloads.publicationSequence);
			hash = MixWorkFingerprint(hash, payloads.segments.size());
			for(const AnimationClipSegmentPayload& payload : payloads.segments) {
				hash = MixWorkFingerprint(hash, payload.segmentIndex);
				hash = MixWorkFingerprint(hash, payload.streamOffset);
				hash = MixWorkFingerprint(hash, payload.streamSize);
				hash = MixWorkFingerprint(hash, payload.contentFingerprint);
			}
		} else {
			hash = MixWorkFingerprint(hash, 0U);
		}
		hash = MixWorkFingerprint(hash, binding.authoritativeJointIndices.size());
		hash = MixWorkFingerprint(hash, binding.gpuResidentPayloads ? 1U : 0U);
		hash = MixWorkFingerprint(hash, binding.clipGeneration);
		hash = MixWorkFingerprint(hash, binding.skeletonGeneration);
		hash = MixWorkFingerprint(hash, binding.programGeneration);
		hash = MixWorkFingerprint(hash, binding.retargetProfileGeneration);
		hash = MixWorkFingerprint(hash, binding.retargetSourceSkeletonGeneration);
		hash = MixWorkFingerprint(hash, binding.motionDatabaseGeneration);
		return hash;
	}

	AnimationCompatibilityFingerprint
	AnimationSystem::Impl::BuildFallbackCompatibilityFingerprint(const AnimatorPlaybackBinding& binding) noexcept {
		if(binding.fallbackCompatibilityFingerprint != 0U) {
			return binding.fallbackCompatibilityFingerprint;
		}
		if(binding.skeleton && binding.skeleton->compatibilityFingerprint != 0U) {
			return binding.skeleton->compatibilityFingerprint;
		}

		// An unbound runtime shell has no skeleton fingerprint, but its empty or
		// partially declared pose layout still has a truthful compatibility identity.
		// Hash the copied layout/resource inputs so restart fixtures and authored
		// zero-channel animators can publish a typed safe pose without inventing a
		// lifecycle generation or accepting a differently shaped fallback later.
		std::uint64_t hash = 1469598103934665603ULL;
		hash = MixWorkFingerprint(hash, binding.jointStableIds.size());
		hash = MixWorkFingerprint(hash, binding.morphStableIds.size());
		hash = MixWorkFingerprint(hash, binding.curveStableIds.size());
		hash = MixWorkFingerprint(hash, binding.clipGeneration);
		hash = MixWorkFingerprint(hash, binding.skeletonGeneration);
		hash = MixWorkFingerprint(hash, binding.programGeneration);
		hash = MixWorkFingerprint(hash, binding.gpuProgramResourceIndex);
		for(const std::uint64_t stableId : binding.jointStableIds) {
			hash = MixWorkFingerprint(hash, stableId);
		}
		for(const std::uint64_t stableId : binding.morphStableIds) {
			hash = MixWorkFingerprint(hash, stableId);
		}
		for(const std::uint64_t stableId : binding.curveStableIds) {
			hash = MixWorkFingerprint(hash, stableId);
		}
		return hash;
	}

	bool AnimationSystem::Impl::TryAddMemoryBytes(std::size_t count, std::size_t elementBytes, std::size_t& total) noexcept {
		if(count != 0U && elementBytes > std::numeric_limits<std::size_t>::max() / count) {
			return false;
		}
		const std::size_t bytes = count * elementBytes;
		if(bytes > std::numeric_limits<std::size_t>::max() - total)
			return false;
		total += bytes;
		return true;
	}

	bool AnimationSystem::Impl::TryEstimatePoseBytes(const FrameWorkItem& item, std::size_t& total) noexcept {
		if(!TryAddMemoryBytes(1U, sizeof(PoseBuffer) + sizeof(AnimationSnapshot) + sizeof(AnimationAuthorityRow), total) ||
		   !TryAddMemoryBytes(item.binding.jointStableIds.size(), sizeof(AnimationTransform), total) ||
		   !TryAddMemoryBytes(item.binding.jointStableIds.size(), sizeof(AnimationTransform), total) ||
		   !TryAddMemoryBytes(item.binding.morphStableIds.size(), sizeof(AnimationNamedFloatValue), total) ||
		   !TryAddMemoryBytes(item.binding.curveStableIds.size(), sizeof(AnimationNamedFloatValue), total)) {
			return false;
		}
		if(item.binding.clip) {
			if(!TryAddMemoryBytes(item.binding.clip->events.size(), sizeof(AnimationEventRecord), total)) {
				return false;
			}
			for(const AnimationClipEvent& event : item.binding.clip->events) {
				if(!TryAddMemoryBytes(event.payload.size(), sizeof(char), total))
					return false;
			}
		}
		return true;
	}

	bool AnimationSystem::Impl::TryAdvanceGeneration(std::uint64_t current, bool changed, std::uint64_t& output) noexcept {
		output = current;
		if(!changed)
			return current != 0U;
		if(current == 0U || current == std::numeric_limits<std::uint64_t>::max())
			return false;
		output = current + 1U;
		return true;
	}

	void AnimationSystem::Impl::ResolveUnchangedCacheReuse(FrameWorkItem& item) noexcept {
		const AnimationEvaluationExternalGenerations& external = item.binding.evaluationGenerations;
		if(item.evaluationStatus != AnimationStatus::Ok || !item.activeProceduralGoals.empty() || item.retargetActive ||
		   item.activeMotionDatabaseGeneration != 0U || item.ragdollLifecycle == RagdollLifecycleState::Active ||
		   item.ragdollLifecycle == RagdollLifecycleState::Recovering || !external.unchangedInputReuseEligible || !external.IsComplete()) {
			return;
		}
		const std::uint64_t consumerGeneration =
			item.resolvedConsumers.declarationGeneration != 0U ? item.resolvedConsumers.declarationGeneration : external.consumerGeneration;

		item.cacheKey.animator = item.input.handle;
		item.cacheKey.evaluationTick = item.playback.AbsoluteTick();
		item.cacheKey.stateGeneration = item.candidateStateGeneration;
		item.cacheKey.parameterGeneration = item.candidateParameterGeneration;
		item.cacheKey.consumerGeneration = consumerGeneration;
		item.cacheKey.executionModeGeneration = item.candidateExecutionModeGeneration;
		item.cacheKey.poseGeneration = item.capturedPoseGeneration != 0U ? item.capturedPoseGeneration : item.candidatePoseGeneration;
		item.cacheKey.morphGeneration = external.morphGeneration;
		item.cacheKey.graphGeneration = external.graphGeneration;
		item.cacheKey.viewGeneration = external.viewGeneration;
		item.cacheKey.assetGeneration = external.assetGeneration;
		item.cacheKey.residencyGeneration = external.residencyGeneration;
		item.cacheKey.materialGeneration = external.materialGeneration;
		item.cacheKey.rendererGeneration = external.rendererGeneration;
		item.cacheKey.physicsGeneration = external.physicsGeneration;
		item.cacheKey.deterministicGeneration = external.deterministicGeneration;
		item.cacheRequirements.visible = item.visibleForVisualEvaluation;
		item.cacheRequirements.timeAdvances = item.advance.previousAbsoluteTick != item.advance.currentAbsoluteTick;
		item.cacheRequirements.events = item.cacheRequirements.timeAdvances && item.binding.eventPolicy != AnimatorEventPolicy::Suppress;
		item.cacheRequirements.rootMotion = item.cacheRequirements.timeAdvances &&
											(item.rootMotionEnabled || item.binding.rootMotionPolicy != AnimatorRootMotionPolicy::Ignore);
		item.cacheRequirements.physics = item.executionDecision.consumerClosure.physics;
		item.cacheRequirements.recording = item.executionDecision.consumerClosure.recording;
		item.cacheRequirements.replication = item.executionDecision.consumerClosure.networking;
		item.cacheRequirements.rollback = item.executionDecision.consumerClosure.rollback;
		item.cacheRequirements.deterministic = item.resolvedConsumers.deterministic;
		item.cacheRequirements.automation = item.executionDecision.consumerClosure.automation;
		item.cacheRequirements.detailedDiagnostics = item.executionDecision.consumerClosure.diagnostics;
		// An advancing clock, event/root-motion publication, physics, recording,
		// replication, rollback, automation, or detailed diagnostics requires new
		// authority by contract. Such work can never reuse this cache entry, so do
		// not take the cache mutex or replace its per-animator tree node every frame.
		// A newly paused/stable animator evaluates once to seed its exact key and
		// then resumes ordinary unchanged-result reuse on subsequent frames.
		item.cacheEligible = !item.cacheRequirements.HasAuthoritativeWork();
		if(!item.cacheEligible)
			return;
		if(item.capturedPoseGeneration == 0U || item.priorPublication == nullptr || item.priorPublication->snapshot == nullptr ||
		   item.priorPublication->snapshot->pose == nullptr) {
			return;
		}

		const AnimationEvaluationCacheLookup lookup = persistentReuse.evaluationCache.Lookup(item.cacheKey, item.cacheRequirements);
		const std::uint64_t priorPoseGeneration = item.priorPublication->snapshot->publicationGeneration;
		if(lookup.disposition != AnimationEvaluationCacheDisposition::Reused || lookup.value == nullptr ||
		   lookup.value->poseToken != priorPoseGeneration ||
		   lookup.value->publicationGeneration != item.priorPublication->authorityPublicationGeneration) {
			return;
		}

		// Reuse retains the exact immutable prior generation. It advances neither
		// pose publication nor root motion/events, and the protected boundary will
		// therefore not duplicate last frame's authoritative side effects.
		item.cacheReused = true;
		item.candidatePoseGeneration = item.capturedPoseGeneration;
		item.executionDecision.nextState = item.previousExecutionModeState;
		item.publication = item.priorPublication;
		item.selectedPublication = item.priorPublication;
		item.evaluationStatus = item.priorPublication->status;
		item.safePose = item.priorPublication->safeFallback;
		item.evaluated = true;
		evaluationState.cacheReusedAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
	}

	[[nodiscard]] bool AnimationSystem::Impl::CanSuspendVisualEvaluation(const FrameWorkItem& item) noexcept {
		// Visibility may remove only presentation work. Every gameplay, tooling,
		// deterministic, event, root-motion, procedural, and physics obligation
		// keeps full evaluation regardless of the current camera set.
		return !item.visibleForVisualEvaluation && item.binding.cullingPolicy != AnimatorCullingPolicy::AlwaysUpdate &&
			   item.evaluationStatus == AnimationStatus::Ok && !item.cacheReused && item.resolvedConsumers.status == AnimationStatus::Ok &&
			   !item.resolvedConsumers.deterministic && !item.executionDecision.consumerClosure.HasAuthoritativeSubset() &&
			   item.binding.eventPolicy == AnimatorEventPolicy::Suppress && !item.rootMotionEnabled &&
			   item.binding.rootMotionPolicy == AnimatorRootMotionPolicy::Ignore && item.activeProceduralGoals.empty() &&
			   item.pendingMotionQueries.empty() && item.pendingMotionQueriesV2.empty() && !item.retargetActive &&
			   item.ragdollLifecycle == RagdollLifecycleState::Inactive &&
			   (item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual ||
				(item.priorPublication && item.priorPublication->snapshot && item.priorPublication->snapshot->pose));
	}

	const EvaluationState* AnimationSystem::Impl::FindControllerState(const AnimatorPlaybackBinding& binding,
																	  std::uint64_t stateId) const noexcept {
		if(!binding.evaluationProgram || stateId == 0U)
			return nullptr;
		const auto state = std::find_if(binding.evaluationProgram->states.begin(), binding.evaluationProgram->states.end(),
										[stateId](const EvaluationState& candidate) { return candidate.stableId == stateId; });
		return state == binding.evaluationProgram->states.end() ? nullptr : std::addressof(*state);
	}

	const AnimatorControllerClipBinding* AnimationSystem::Impl::FindControllerClipForState(const AnimatorPlaybackBinding& binding,
																						   std::uint64_t stateId) const noexcept {
		const EvaluationState* state = FindControllerState(binding, stateId);
		if(state == nullptr || state->nodeOpcode != EvaluationOpcode::SampleClip) {
			return nullptr;
		}
		const auto clip = std::lower_bound(
			binding.controllerClips.begin(), binding.controllerClips.end(), state->nodeId,
			[](const AnimatorControllerClipBinding& candidate, std::uint64_t nodeId) { return candidate.nodeStableId < nodeId; });
		return clip == binding.controllerClips.end() || clip->nodeStableId != state->nodeId ? nullptr : std::addressof(*clip);
	}

	AnimationStatus AnimationSystem::Impl::BeginControllerTransition(FrameWorkItem& item) noexcept {
		const StateMachineSnapshot& graph = item.candidateGraphState;
		if(graph.activeTransitionStableId == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(item.transition.ready && item.transition.transitionStableId == graph.activeTransitionStableId &&
		   item.transition.sourceStateId == graph.transitionSourceStateId &&
		   item.transition.destinationStateId == graph.transitionDestinationStateId && !item.restoredFromHistory) {
			return AnimationStatus::Ok;
		}

		const AnimatorControllerClipBinding* destination = FindControllerClipForState(item.binding, graph.transitionDestinationStateId);
		if(destination == nullptr || !destination->clip || destination->clipGeneration == 0U || !item.binding.skeleton) {
			// Non-clip graph nodes retain their existing evaluator path. This
			// transition helper never guesses that an arbitrary dependency is a
			// clip merely because a state references it.
			item.transition = {};
			return AnimationStatus::Ok;
		}

		ControllerTransitionRuntime candidate;
		candidate.transitionStableId = graph.activeTransitionStableId;
		candidate.sourceStateId = graph.transitionSourceStateId;
		candidate.destinationStateId = graph.transitionDestinationStateId;
		try {
			candidate.destination = *destination;
			const bool sourceCompatible =
				item.retargetActive && item.binding.retargetSourceSkeleton &&
				destination->clip->compatibilityFingerprint == item.binding.retargetSourceSkeleton->compatibilityFingerprint &&
				destination->clip->skeletonAssetId == item.binding.retargetSourceSkeleton->assetId;
			const SkeletonAsset* samplingSkeleton =
				sourceCompatible ? item.binding.retargetSourceSkeleton.get() : item.binding.skeleton.get();
			AnimationStatus status = candidate.destinationClipView.Open(*destination->clip, samplingSkeleton, destination->residentSegments,
																		destination->clipGeneration, destination->streamedSegmentPayloads);
			if(status != AnimationStatus::Ok)
				return status;
			status = candidate.destinationEventStream.Configure({destination->clip->assetId, destination->clipGeneration},
																destination->clip->durationTicks, destination->clip->events);
			if(status != AnimationStatus::Ok)
				return status;

			AnimatorPlaybackConfiguration configuration = item.playback.Configuration();
			configuration.durationTicks = destination->clip->durationTicks;
			status = candidate.destinationPlayback.Configure(configuration);
			if(status != AnimationStatus::Ok)
				return status;
			const long double offset =
				static_cast<long double>(configuration.durationTicks) * static_cast<long double>(graph.retainedDestinationOffsetNormalized);
			AnimationFixedTick destinationTick = static_cast<AnimationFixedTick>(
				std::llround(std::clamp(offset, 0.0L, static_cast<long double>(configuration.durationTicks))));
			// Rollback can restore an already-active transition without its
			// transient destination cursor. Rebuild the deterministic elapsed
			// position from the graph tick before the restored frame republishes.
			if(item.restoredFromHistory || item.capturedGraphState.activeTransitionStableId == graph.activeTransitionStableId) {
				const long double elapsedValue =
					std::max(0.0L, static_cast<long double>(graph.lastTick) - static_cast<long double>(graph.transitionStartTick));
				if(elapsedValue > static_cast<long double>(std::numeric_limits<AnimationFixedTick>::max())) {
					return AnimationStatus::RangeOverflow;
				}
				const AnimationFixedTick elapsed = static_cast<AnimationFixedTick>(elapsedValue);
				const long double reconstructed =
					static_cast<long double>(destinationTick) + static_cast<long double>(configuration.reverse ? -elapsed : elapsed);
				if(reconstructed < static_cast<long double>(std::numeric_limits<AnimationFixedTick>::min()) ||
				   reconstructed > static_cast<long double>(std::numeric_limits<AnimationFixedTick>::max())) {
					return AnimationStatus::RangeOverflow;
				}
				destinationTick = static_cast<AnimationFixedTick>(reconstructed);
			}
			status = candidate.destinationPlayback.Seek(destinationTick);
			if(status != AnimationStatus::Ok)
				return status;
			const AnimatorPlaybackRunState sourceRunState = item.playback.RunState();
			if(sourceRunState == AnimatorPlaybackRunState::Playing || sourceRunState == AnimatorPlaybackRunState::Paused ||
			   sourceRunState == AnimatorPlaybackRunState::Completed) {
				status = candidate.destinationPlayback.Play();
				if(status == AnimationStatus::Ok && sourceRunState == AnimatorPlaybackRunState::Paused) {
					status = candidate.destinationPlayback.Pause();
				}
				if(status != AnimationStatus::Ok)
					return status;
			}

			const std::uint64_t priorTransition = item.capturedGraphState.activeTransitionStableId;
			if(priorTransition != 0U && graph.interruptedTransitionStableId == priorTransition && item.priorPublication &&
			   item.priorPublication->snapshot && item.priorPublication->snapshot->pose) {
				const std::span<const AnimationTransform> priorPose = item.priorPublication->snapshot->pose->LocalTransforms();
				if(priorPose.size() == item.binding.jointStableIds.size()) {
					candidate.capturedSourcePose =
						std::make_shared<const std::vector<AnimationTransform>>(priorPose.begin(), priorPose.end());
				}
				if(item.priorPublication->snapshot->pose->RootMotionDelta()) {
					candidate.capturedSourceRootMotion = *item.priorPublication->snapshot->pose->RootMotionDelta();
				}
			}
			candidate.ready = true;
			item.transition = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	void AnimationSystem::Impl::PromoteCompletedControllerTransition(FrameWorkItem& item) noexcept {
		const bool completed = item.transition.ready &&
							   item.capturedGraphState.activeTransitionStableId == item.transition.transitionStableId &&
							   item.candidateGraphState.activeTransitionStableId == 0U &&
							   item.candidateGraphState.activeStateId == item.transition.destinationStateId;
		if(!completed) {
			item.transition = {};
			return;
		}

		// The destination cursor has advanced independently for every blend tick.
		// Promote the complete destination bundle together instead of reopening
		// the clip at its authored offset and visibly rewinding on completion.
		item.binding.clip = item.transition.destination.clip;
		item.binding.clipGeneration = item.transition.destination.clipGeneration;
		item.binding.clipDeterministicContentFingerprint = item.transition.destination.deterministicContentFingerprint;
		item.binding.residentSegments = item.transition.destination.residentSegments;
		item.binding.streamedSegmentPayloads = item.transition.destination.streamedSegmentPayloads;
		item.playback = item.transition.destinationPlayback;
		item.clipView = item.transition.destinationClipView;
		item.eventStream = item.transition.destinationEventStream;
		item.sampledSourceSkeleton =
			item.retargetActive && item.binding.retargetSourceSkeleton && item.binding.clip &&
			item.binding.clip->compatibilityFingerprint == item.binding.retargetSourceSkeleton->compatibilityFingerprint &&
			item.binding.clip->skeletonAssetId == item.binding.retargetSourceSkeleton->assetId;
		item.transition = {};
	}

	AnimationStatus AnimationSystem::Impl::SynchronizeControllerTransition(FrameWorkItem& item) noexcept {
		if(item.candidateGraphState.activeTransitionStableId != 0U) {
			return BeginControllerTransition(item);
		}
		PromoteCompletedControllerTransition(item);
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationSystem::Impl::SelectControllerClipForState(FrameWorkItem& item, std::uint64_t stateId) noexcept {
		if(!item.binding.evaluationProgram || stateId == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		const auto state = std::find_if(item.binding.evaluationProgram->states.begin(), item.binding.evaluationProgram->states.end(),
										[stateId](const EvaluationState& candidate) { return candidate.stableId == stateId; });
		if(state == item.binding.evaluationProgram->states.end()) {
			return AnimationStatus::MissingAsset;
		}
		// Non-clip nodes own dedicated graph evaluation stages. Keep the current
		// sampled base pose until that stage consumes it; never reinterpret an
		// arbitrary dependency as a clip merely because a state selected it.
		if(state->nodeOpcode != EvaluationOpcode::SampleClip) {
			return AnimationStatus::Ok;
		}
		const auto clip = std::lower_bound(
			item.binding.controllerClips.begin(), item.binding.controllerClips.end(), state->nodeId,
			[](const AnimatorControllerClipBinding& candidate, std::uint64_t nodeId) { return candidate.nodeStableId < nodeId; });
		if(clip == item.binding.controllerClips.end() || clip->nodeStableId != state->nodeId || !clip->clip || clip->clipGeneration == 0U ||
		   !item.binding.skeleton) {
			return AnimationStatus::MissingAsset;
		}
		if(item.binding.clip == clip->clip && item.binding.clipGeneration == clip->clipGeneration) {
			return AnimationStatus::Ok;
		}

		const bool sourceCompatible =
			item.retargetActive && item.binding.retargetSourceSkeleton &&
			clip->clip->compatibilityFingerprint == item.binding.retargetSourceSkeleton->compatibilityFingerprint &&
			clip->clip->skeletonAssetId == item.binding.retargetSourceSkeleton->assetId;
		const SkeletonAsset* samplingSkeleton = sourceCompatible ? item.binding.retargetSourceSkeleton.get() : item.binding.skeleton.get();
		AnimationClipView nextView;
		AnimationStatus status =
			nextView.Open(*clip->clip, samplingSkeleton, clip->residentSegments, clip->clipGeneration, clip->streamedSegmentPayloads);
		if(status != AnimationStatus::Ok)
			return status;
		AnimationEventStream nextEvents;
		status = nextEvents.Configure({clip->clip->assetId, clip->clipGeneration}, clip->clip->durationTicks, clip->clip->events);
		if(status != AnimationStatus::Ok)
			return status;

		const AnimatorPlaybackRunState priorRunState = item.playback.RunState();
		AnimatorPlaybackConfiguration configuration = item.playback.Configuration();
		configuration.durationTicks = clip->clip->durationTicks;
		AnimatorPlayback nextPlayback;
		status = nextPlayback.Configure(configuration);
		if(status != AnimationStatus::Ok)
			return status;
		const long double offset = static_cast<long double>(configuration.durationTicks) *
								   static_cast<long double>(item.candidateGraphState.retainedDestinationOffsetNormalized);
		const AnimationFixedTick destinationTick =
			static_cast<AnimationFixedTick>(std::clamp<long double>(offset, 0.0L, static_cast<long double>(configuration.durationTicks)));
		status = nextPlayback.Seek(destinationTick);
		if(status != AnimationStatus::Ok)
			return status;
		if(priorRunState == AnimatorPlaybackRunState::Playing || priorRunState == AnimatorPlaybackRunState::Paused) {
			status = nextPlayback.Play();
			if(status == AnimationStatus::Ok && priorRunState == AnimatorPlaybackRunState::Paused) {
				status = nextPlayback.Pause();
			}
			if(status != AnimationStatus::Ok)
				return status;
		}

		item.binding.clip = clip->clip;
		item.binding.clipGeneration = clip->clipGeneration;
		item.binding.clipDeterministicContentFingerprint = clip->deterministicContentFingerprint;
		item.binding.residentSegments = clip->residentSegments;
		item.binding.streamedSegmentPayloads = clip->streamedSegmentPayloads;
		item.sampledSourceSkeleton = sourceCompatible;
		item.clipView = std::move(nextView);
		item.eventStream = std::move(nextEvents);
		item.playback = nextPlayback;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationSystem::Impl::ResolveRequestedStateLeaf(const EvaluationProgram& program, std::uint64_t requestedStateId,
																	 std::uint64_t& outputLeaf) const noexcept {
		const EvaluationState* current = nullptr;
		for(const EvaluationState& state : program.states) {
			if(state.stableId == requestedStateId) {
				current = &state;
				break;
			}
		}
		if(current == nullptr || current->anyState || current->exit) {
			return AnimationStatus::InvalidArgument;
		}

		// A command may name a state-machine scope. Resolve its single authored
		// entry chain exactly as ordinary state-machine evaluation does.
		for(std::size_t depth = 0U; depth <= GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum; ++depth) {
			const EvaluationState* entryChild = nullptr;
			for(const EvaluationState& candidate : program.states) {
				if(candidate.parentStateId != current->stableId || !candidate.entry)
					continue;
				if(entryChild != nullptr)
					return AnimationStatus::Rejected;
				entryChild = &candidate;
			}
			if(entryChild == nullptr) {
				outputLeaf = current->stableId;
				return AnimationStatus::Ok;
			}
			if(entryChild->anyState || entryChild->exit)
				return AnimationStatus::Rejected;
			current = entryChild;
		}
		return AnimationStatus::HardLimitExceeded;
	}

	std::uint64_t AnimationSystem::Impl::MakeCommandTransitionId(const AnimationCommand& command, std::uint64_t sourceStateId,
																 std::uint64_t destinationStateId) const noexcept {
		// Keep explicit gameplay transitions in a disjoint high-bit namespace.
		// The remaining bits are deterministic from copied command identities.
		std::uint64_t hash = 1469598103934665603ULL;
		for(const std::uint64_t value : {sourceStateId, destinationStateId, command.producerSequence, command.correlationId,
										 static_cast<std::uint64_t>(command.targetTick)}) {
			hash ^= value;
			hash *= 1099511628211ULL;
		}
		return (hash | (1ULL << 63U) | 1ULL);
	}

	AnimationStatus AnimationSystem::Impl::ApplyGraphControlCommand(FrameWorkItem& item, const AnimationCommand& command) noexcept {
		if(command.kind == AnimationCommandKind::SetLayerWeight) {
			if(command.layerIndex >= item.layerWeights.size() || !std::isfinite(command.layerWeight) || command.layerWeight < 0.0F ||
			   command.layerWeight > 1.0F) {
				return AnimationStatus::InvalidArgument;
			}
			item.layerWeights[command.layerIndex] = command.layerWeight;
			return AnimationStatus::Ok;
		}
		if(command.kind == AnimationCommandKind::Play && command.stateId == 0U) {
			const AnimationStatus seekStatus = item.playback.Seek(command.playbackTick);
			return seekStatus == AnimationStatus::Ok ? item.playback.Play() : seekStatus;
		}
		if(command.kind == AnimationCommandKind::Play && !item.binding.evaluationProgram) {
			if(command.stateId != item.binding.stateId) {
				return AnimationStatus::InvalidArgument;
			}
			const AnimationStatus seekStatus = item.playback.Seek(command.playbackTick);
			return seekStatus == AnimationStatus::Ok ? item.playback.Play() : seekStatus;
		}
		if((command.kind != AnimationCommandKind::Play && command.kind != AnimationCommandKind::CrossFade) ||
		   !item.binding.evaluationProgram) {
			return AnimationStatus::NotReady;
		}

		std::uint64_t destinationLeaf = 0U;
		const AnimationStatus resolveStatus = ResolveRequestedStateLeaf(*item.binding.evaluationProgram, command.stateId, destinationLeaf);
		if(resolveStatus != AnimationStatus::Ok)
			return resolveStatus;
		try {
			if(command.kind == AnimationCommandKind::Play || command.transitionTicks == 0 || item.candidateGraphState.activeStateId == 0U ||
			   item.candidateGraphState.activeStateId == destinationLeaf) {
				StateMachineSnapshot next = item.candidateGraphState;
				next.activeStateId = destinationLeaf;
				next.stateEnteredTick = item.input.tick;
				next.activeTransitionStableId = 0U;
				next.transitionSourceStateId = 0U;
				next.transitionDestinationStateId = 0U;
				next.transitionStartTick = 0;
				next.transitionDurationTicks = 0;
				next.transitionProgressNormalized = 0.0F;
				next.retainedDestinationOffsetNormalized = 0.0F;
				next.transitionDestinationEnteredTick = 0;
				next.interruptedTransitionStableId = 0U;
				next.interruptedTransitionProgressNormalized = 0.0F;
				next.retainedTriggerEdges.clear();
				next.syncHistory.clear();
				next.lastTick = item.input.tick;
				item.candidateGraphState = std::move(next);
				item.binding.stateId = destinationLeaf;
				// An instant Play or zero-duration CrossFade replaces any
				// in-flight blend. Clear its private destination cursor before
				// synchronization so it cannot overwrite the newly requested
				// state and playback tick during completion promotion.
				item.transition = {};
				const AnimationStatus clipStatus = SelectControllerClipForState(item, destinationLeaf);
				if(clipStatus != AnimationStatus::Ok)
					return clipStatus;
				const AnimationStatus seekStatus =
					item.playback.Seek(command.kind == AnimationCommandKind::Play ? command.playbackTick : 0);
				return seekStatus == AnimationStatus::Ok ? item.playback.Play() : seekStatus;
			}

			StateMachineSnapshot next = item.candidateGraphState;
			next.interruptedTransitionStableId = next.activeTransitionStableId;
			next.interruptedTransitionProgressNormalized = next.transitionProgressNormalized;
			next.activeTransitionStableId = MakeCommandTransitionId(command, next.activeStateId, destinationLeaf);
			next.transitionSourceStateId = next.activeStateId;
			next.transitionDestinationStateId = destinationLeaf;
			next.transitionStartTick = item.input.tick;
			next.transitionDurationTicks = command.transitionTicks;
			next.transitionProgressNormalized = 0.0F;
			next.retainedDestinationOffsetNormalized = 0.0F;
			next.transitionDestinationEnteredTick = item.input.tick;
			next.lastTick = item.input.tick;
			next.transitionHistory.push_back(
				{item.input.tick, next.activeTransitionStableId, next.transitionSourceStateId, destinationLeaf});
			const std::size_t historyLimit =
				static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum);
			if(next.transitionHistory.size() > historyLimit) {
				next.transitionHistory.erase(next.transitionHistory.begin(),
											 next.transitionHistory.begin() +
												 static_cast<std::ptrdiff_t>(next.transitionHistory.size() - historyLimit));
			}
			item.candidateGraphState = std::move(next);
			item.binding.stateId = item.candidateGraphState.activeStateId;
			return item.playback.Play();
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus AnimationSystem::Impl::ApplyRetainedMotionSelection(FrameWorkItem& item, bool& changed) noexcept {
		changed = false;
		if(!item.hasMotionCandidate || item.latestMotionQueryId == 0U || item.latestMotionQueryId == item.appliedMotionQueryId) {
			return AnimationStatus::Ok;
		}
		// The explicit runtime query API is also useful for observation and AI
		// planning on a direct-clip animator. Without an authored controller there
		// is no motion node/state to drive, so retain the copied result for
		// diagnostics without turning the otherwise valid animator into a failure.
		if(!item.binding.evaluationProgram)
			return AnimationStatus::Ok;
		if(!item.binding.motionDatabase || item.activeMotionDatabaseGeneration == 0U ||
		   item.activeMotionDatabaseGeneration != item.binding.motionDatabaseGeneration) {
			return AnimationStatus::NotReady;
		}

		const auto state = std::find_if(item.binding.evaluationProgram->states.begin(), item.binding.evaluationProgram->states.end(),
										[&item](const EvaluationState& candidate) {
											return candidate.nodeOpcode == EvaluationOpcode::SampleClip &&
												   candidate.nodeId == item.latestMotionCandidate.sourceClipId;
										});
		if(state == item.binding.evaluationProgram->states.end()) {
			return AnimationStatus::MissingAsset;
		}
		try {
			StateMachineSnapshot next = item.candidateGraphState;
			const std::uint64_t priorState = next.activeStateId;
			next.activeStateId = state->stableId;
			next.stateEnteredTick = item.input.tick;
			next.activeTransitionStableId = 0U;
			next.transitionSourceStateId = 0U;
			next.transitionDestinationStateId = 0U;
			next.transitionStartTick = 0;
			next.transitionDurationTicks = 0;
			next.transitionProgressNormalized = 0.0F;
			next.retainedDestinationOffsetNormalized = 0.0F;
			next.transitionDestinationEnteredTick = 0;
			next.interruptedTransitionStableId = 0U;
			next.interruptedTransitionProgressNormalized = 0.0F;
			next.lastTick = item.input.tick;
			if(priorState != 0U && priorState != state->stableId) {
				const std::uint64_t decisionId = item.latestMotionCandidate.stableSampleId | (1ULL << 62U) | 1ULL;
				next.transitionHistory.push_back({item.input.tick, decisionId, priorState, state->stableId});
				// Motion matching may choose a new source every frame. Keep the
				// diagnostic history bounded by the same retained-event limit used
				// by authored transitions so a long-running animator cannot grow
				// this worker-owned vector without limit.
				const std::size_t historyLimit =
					static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum);
				if(next.transitionHistory.size() > historyLimit) {
					next.transitionHistory.erase(next.transitionHistory.begin(),
												 next.transitionHistory.begin() +
													 static_cast<std::ptrdiff_t>(next.transitionHistory.size() - historyLimit));
				}
			}
			item.candidateGraphState = std::move(next);
			item.binding.stateId = state->stableId;
			const AnimationStatus clipStatus = SelectControllerClipForState(item, state->stableId);
			if(clipStatus != AnimationStatus::Ok)
				return clipStatus;
			const AnimationStatus seekStatus = item.playback.Seek(item.latestMotionCandidate.sourceTick);
			if(seekStatus != AnimationStatus::Ok)
				return seekStatus;
			const AnimationStatus playStatus = item.playback.Play();
			if(playStatus != AnimationStatus::Ok)
				return playStatus;
			item.appliedMotionQueryId = item.latestMotionQueryId;
			changed = true;
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	[[nodiscard]] bool AnimationSystem::Impl::StampGpuVisualFrameIdentity(GpuAnimationVisualInput& visual, AnimationFixedTick resolvedTick,
																		  AnimationFixedTick frameTick) const noexcept {
		const GpuAnimationRenderIdentity& renderIdentity = visual.renderIdentity;
		if(!IsGpuAnimationRenderIdentityValid(renderIdentity) ||
		   renderIdentity.meshHeaderVersion > std::numeric_limits<std::uint32_t>::max() ||
		   renderIdentity.rendererGeneration > std::numeric_limits<std::uint32_t>::max() || frameInputs.frameSceneGeneration == 0U ||
		   frameInputs.frameSceneGeneration > std::numeric_limits<std::uint32_t>::max() || resolvedTick < 0 ||
		   static_cast<std::uint64_t>(resolvedTick) > std::numeric_limits<std::uint32_t>::max()) {
			return false;
		}
		visual.generations.meshGeneration = renderIdentity.meshHeaderVersion;
		visual.generations.rendererGeneration = static_cast<std::uint32_t>(renderIdentity.rendererGeneration);
		visual.generations.sceneGeneration = static_cast<std::uint32_t>(frameInputs.frameSceneGeneration);
		visual.sampleTick = static_cast<std::uint32_t>(resolvedTick);
		// The renderer validates this full-width value beside the narrowed shader ABI.
		visual.renderIdentity.sceneGeneration = frameInputs.frameSceneGeneration;
		visual.generations.frameIdLow = static_cast<std::uint32_t>(frameTick);
		visual.generations.frameIdHigh = static_cast<std::uint32_t>(static_cast<std::uint64_t>(frameTick) >> 32U);
		return true;
	}

	void AnimationSystem::Impl::PlanCompactCapture(FrameWorkItem& item, const AnimatorPlaybackTimeInput& timeInput) noexcept {
		// This branch intentionally touches only CompactCapture and scalar authority
		// fields. It is the worker half of the protected early classification above.
		AnimatorPlaybackTimeInput effectiveTime = timeInput;
		if(item.compactCapture.updatePolicy == AnimatorUpdatePolicy::Manual)
			effectiveTime = {};
		const AnimationStatus advanceStatus = item.playback.Advance(effectiveTime, item.advance);
		item.evaluationStatus = advanceStatus;
		const AnimatorPlaybackStateSnapshot playback = item.playback.Snapshot();
		if(!TryAdvanceGeneration(item.capturedStateGeneration,
								 playback.resolvedTick != item.previousExecutionModeState.continuity.stateTimeTick,
								 item.candidateStateGeneration)) {
			item.evaluationStatus = AnimationStatus::RangeOverflow;
		}
		AnimationExecutionModeRequest request;
		request.fixedTick = item.input.tick;
		request.requestedPolicy = item.compactCapture.requestedPolicy;
		request.headless = headlessExecutionRequired;
		request.gpu = item.compactCapture.gpuExecution;
		request.continuity.transitionBoundaryTick = item.input.tick;
		request.continuity.stateTimeTick = item.playback.AbsoluteTick();
		request.continuity.normalizedControllerTime = static_cast<float>(playback.resolvedTick);
		request.continuity.controllerState = 1U;
		request.continuity.parameterSnapshot = item.candidateParameterGeneration;
		request.continuity.synchronizationState = item.candidateStateGeneration;
		request.continuity.synchronizationPhaseTick = playback.resolvedTick;
		request.continuity.pose = item.candidatePoseGeneration;
		item.executionDecision = ResolveAnimationExecutionMode(request, item.previousExecutionModeState);
		if(!TryAdvanceGeneration(item.capturedExecutionModeGeneration,
								 item.executionDecision.nextState.effectiveMode != item.previousExecutionModeState.effectiveMode ||
									 item.executionDecision.nextState.effectiveReason != item.previousExecutionModeState.effectiveReason,
								 item.candidateExecutionModeGeneration)) {
			item.evaluationStatus = AnimationStatus::RangeOverflow;
		}
		if(item.evaluationStatus != AnimationStatus::Ok ||
		   item.executionDecision.effectiveMode != AnimationEffectiveExecutionMode::GpuVisual) {
			item.processingClass = AnimationProcessingClass::Complete;
			item.planned = true;
			return;
		}
		GpuAnimationLoweringInput lowering;
		lowering.program.program = item.compactCapture.program;
		lowering.program.programContentId = item.compactCapture.program->stableHash;
		lowering.program.programResourceIndex = item.compactCapture.fastBinding->programResourceIndex;
		lowering.visual = item.compactCapture.fastBinding->visual;
		lowering.visual.stableInstanceId = (static_cast<std::uint64_t>(item.input.handle.slotIndex) << 32U) | item.input.handle.generation;
		lowering.visual.stableSubmissionOrdinal = item.input.handle.slotIndex;
		lowering.visual.jointCount = item.compactCapture.jointCount;
		lowering.visual.evaluationParameterFingerprint =
			MixWorkFingerprint(item.candidateParameterGeneration, item.candidateStateGeneration);
		lowering.visual.sampleTick = static_cast<std::uint32_t>(std::max<AnimationFixedTick>(0, playback.resolvedTick));
		lowering.visual.generations.instanceSlot = item.input.handle.slotIndex;
		lowering.visual.generations.instanceGeneration = item.input.handle.generation;
		lowering.visual.generations.componentGeneration = item.input.descriptor.componentGeneration;
		lowering.visual.generations.workGeneration = item.candidatePoseGeneration;
		lowering.visual.generations.programGeneration = item.compactCapture.programGeneration;
		lowering.visual.generations.controllerGeneration = item.compactCapture.programGeneration;
		lowering.visual.generations.clipGeneration = item.compactCapture.clipGeneration;
		lowering.visual.generations.skeletonGeneration = item.compactCapture.skeletonGeneration;
		if(!StampGpuVisualFrameIdentity(lowering.visual, playback.resolvedTick, item.input.tick)) {
			item.processingClass = AnimationProcessingClass::Complete;
			item.planned = true;
			return;
		}
		lowering.effectiveMode = item.executionDecision.effectiveMode;
		lowering.allowSharedVisualEvaluation = true;
		lowering.residentPayloads = item.compactCapture.fastBinding->residentPayloads;
		GpuAnimationWorkRequest probe{};
		if(LowerGpuAnimationWorkRequest(lowering, probe) != GpuAnimationLoweringStatus::Ok ||
		   !ValidateGpuAnimationResidentPayloads(probe, *lowering.residentPayloads)) {
			item.processingClass = AnimationProcessingClass::Complete;
			item.planned = true;
			return;
		}
		lowering.residentPayloadsValidated = true;
		lowering.prevalidatedStageMask = probe.batchKey.stageMask;
		item.gpuStageMask = probe.batchKey.stageMask;
		item.gpuLowering = std::move(lowering);
		CompactGpuVisualInput input{};
		input.stableInstanceId = item.gpuLowering->visual.stableInstanceId;
		input.stableSubmissionOrdinal = item.gpuLowering->visual.stableSubmissionOrdinal;
		input.playbackTick = item.playback.AbsoluteTick();
		input.sourcePoseGeneration = item.candidatePoseGeneration;
		input.generations = {input.stableInstanceId,
							 item.input.descriptor.componentGeneration,
							 item.compactCapture.programGeneration,
							 item.compactCapture.clipGeneration,
							 frameInputs.frameSceneGeneration,
							 item.gpuLowering->visual.renderIdentity.rendererGeneration,
							 item.candidateAuthorityPublicationGeneration};
		input.eligibilityReason = item.executionDecision.reason;
		input.gpuLoweringComplete = true;
		// CompactCapture admission positively excludes authority consumers,
		// controller transitions, and advanced animation features. Record that
		// proof explicitly because CompactGpuVisualInput defaults these bits to
		// fail-closed true for every caller that has not performed the capture gate.
		input.hasAuthoritativeConsumer = false;
		input.hasControllerOrTransition = false;
		input.hasAdvancedFeature = false;
		input.hasContinuitySource = item.previousExecutionModeState.initialized;
		item.compactGpuVisualInput = input;
		item.processingClass = ClassifyAnimationProcessing(input);
		item.planned = true;
	}

	void AnimationSystem::Impl::PlanCompactCapture(std::size_t compactIndex, const AnimatorPlaybackTimeInput& timeInput,
												   const CompactGpuVisualCohort*& validatedCohort,
												   std::uint32_t& validatedStageMask) noexcept {
		if(compactIndex >= frameInputs.compactGpuVisualCaptures.size())
			return;
		CompactGpuVisualCaptures& compact = frameInputs.compactGpuVisualCaptures;
		const CompactGpuVisualCohort& cohort = compact.cohort(compactIndex);
		// This is intentionally a direct SoA evaluator. It copies no generic
		// FrameWorkItem and reads only the protected compact capture columns.
		AnimatorPlaybackTimeInput effectiveTime = timeInput;
		if(cohort.updatePolicy == AnimatorUpdatePolicy::Manual)
			effectiveTime = {};
		AnimationStatus& status = compact.evaluationStatuses[compactIndex];
		AnimationPlanRejectionReason& planningFailure = compact.planningFailures[compactIndex];
		planningFailure = AnimationPlanRejectionReason::None;
		status = compact.playbacks[compactIndex].Advance(effectiveTime, compact.advances[compactIndex]);
		if(status != AnimationStatus::Ok) {
			planningFailure = AnimationPlanRejectionReason::PlaybackAdvance;
		}
		const AnimatorPlaybackStateSnapshot playback = compact.playbacks[compactIndex].Snapshot();
		const AnimationExecutionModeState& previous = compact.previousExecutionModes[compactIndex];
		if(!TryAdvanceGeneration(compact.stateGenerations[compactIndex], playback.resolvedTick != previous.continuity.stateTimeTick,
								 compact.stateGenerations[compactIndex])) {
			status = AnimationStatus::RangeOverflow;
			planningFailure = AnimationPlanRejectionReason::GenerationOverflow;
		}
		AnimationModeContinuityToken continuity;
		continuity.transitionBoundaryTick = compact.inputs[compactIndex].tick;
		continuity.stateTimeTick = compact.playbacks[compactIndex].AbsoluteTick();
		continuity.normalizedControllerTime = static_cast<float>(playback.resolvedTick);
		continuity.controllerState = 1U;
		continuity.parameterSnapshot = compact.parameterGenerations[compactIndex];
		continuity.synchronizationState = compact.stateGenerations[compactIndex];
		continuity.synchronizationPhaseTick = playback.resolvedTick;
		continuity.pose = compact.candidatePoseGenerations[compactIndex];
		CompactExecutionDecision& executionDecision = compact.executionDecisions[compactIndex];
		const AnimationGpuExecutionInputs& gpu = cohort.gpuExecution;
		// Protected capture admitted this row only after proving a stable GPU-visual
		// mode, an empty authority closure, and complete resident resources. Refresh
		// that retained mode directly while only replacing its per-frame continuity
		// token. The generic resolver remains the fail-closed path if any copied proof
		// is no longer internally consistent, so new policies cannot accidentally
		// inherit this optimized route.
		const bool stableGpuVisualProof =
			compact.inputs[compactIndex].tick >= 0 && !headlessExecutionRequired &&
			(cohort.requestedPolicy == AnimationExecutionPolicy::Auto ||
			 cohort.requestedPolicy == AnimationExecutionPolicy::ForceGpuVisual) &&
			gpu.rendererAvailable && gpu.animationCapabilityAvailable && gpu.resourcesResident && gpu.evaluationHealthy &&
			std::isfinite(gpu.requiredBudget) && std::isfinite(gpu.availableBudget) && gpu.requiredBudget >= 0.0 &&
			gpu.availableBudget >= 0.0 && gpu.requiredBudget <= gpu.availableBudget && previous.initialized &&
			previous.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual &&
			(previous.effectiveReason == AnimationExecutionModeReason::GpuVisualEligible ||
			 previous.effectiveReason == AnimationExecutionModeReason::ForceGpuRequested) &&
			previous.modeEnteredTick >= 0 && previous.modeEnteredTick <= compact.inputs[compactIndex].tick && !previous.promotionPending;
		if(stableGpuVisualProof) {
			executionDecision = {};
			const AnimationExecutionModeReason reason = cohort.requestedPolicy == AnimationExecutionPolicy::ForceGpuVisual
															? AnimationExecutionModeReason::ForceGpuRequested
															: AnimationExecutionModeReason::GpuVisualEligible;
			executionDecision.effectiveMode = AnimationEffectiveExecutionMode::GpuVisual;
			executionDecision.reason = reason;
			executionDecision.nextState = previous;
			executionDecision.nextState.effectiveReason = reason;
			executionDecision.nextState.continuity = continuity;
			executionDecision.nextState.promotionPending = false;
			executionDecision.nextState.promotionMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
			executionDecision.nextState.promotionReason = AnimationExecutionModeReason::Uninitialized;
			executionDecision.nextState.promotionSinceTick = 0;
			executionDecision.nextState.promotionFingerprint = 0U;
		} else {
			AnimationExecutionModeRequest request;
			request.fixedTick = compact.inputs[compactIndex].tick;
			request.requestedPolicy = cohort.requestedPolicy;
			request.headless = headlessExecutionRequired;
			request.gpu = gpu;
			request.continuity = continuity;
			const AnimationExecutionModeDecision resolved = ResolveAnimationExecutionMode(request, previous);
			executionDecision.effectiveMode = resolved.effectiveMode;
			executionDecision.reason = resolved.reason;
			executionDecision.nextState = resolved.nextState;
		}
		if(!TryAdvanceGeneration(compact.executionModeGenerations[compactIndex],
								 compact.executionDecisions[compactIndex].nextState.effectiveMode != previous.effectiveMode ||
									 compact.executionDecisions[compactIndex].nextState.effectiveReason != previous.effectiveReason,
								 compact.executionModeGenerations[compactIndex])) {
			status = AnimationStatus::RangeOverflow;
			planningFailure = AnimationPlanRejectionReason::GenerationOverflow;
		}
		if(status != AnimationStatus::Ok) {
			compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
			return;
		}
		if(compact.executionDecisions[compactIndex].effectiveMode != AnimationEffectiveExecutionMode::GpuVisual) {
			planningFailure = AnimationPlanRejectionReason::ExecutionModeNotGpuVisual;
			compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
			return;
		}
		GpuAnimationVisualInput visual = cohort.fastBinding->visual;
		const CompactGpuVisualCaptures::InputIdentity& input = compact.inputs[compactIndex];
		visual.stableInstanceId = (static_cast<std::uint64_t>(input.handle.slotIndex) << 32U) | input.handle.generation;
		visual.stableSubmissionOrdinal = input.handle.slotIndex;
		visual.jointCount = cohort.jointCount;
		visual.evaluationParameterFingerprint =
			MixWorkFingerprint(compact.parameterGenerations[compactIndex], compact.stateGenerations[compactIndex]);
		visual.sampleTick = static_cast<std::uint32_t>(std::max<AnimationFixedTick>(0, playback.resolvedTick));
		visual.generations.instanceSlot = input.handle.slotIndex;
		visual.generations.instanceGeneration = input.handle.generation;
		visual.generations.componentGeneration = input.componentGeneration;
		visual.generations.workGeneration = compact.candidatePoseGenerations[compactIndex];
		visual.generations.programGeneration = cohort.programGeneration;
		visual.generations.controllerGeneration = cohort.programGeneration;
		visual.generations.clipGeneration = cohort.clipGeneration;
		visual.generations.skeletonGeneration = cohort.skeletonGeneration;
		if(!StampGpuVisualFrameIdentity(visual, playback.resolvedTick, input.tick)) {
			planningFailure = AnimationPlanRejectionReason::FrameIdentityInvalid;
			compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
			return;
		}
		// A worker range commonly contains many members of one immutable cohort.
		// Once one member proves the shared program shape and resident tables, later
		// members still lower their own identity and generation fields but avoid
		// rescanning the same immutable program and payload vectors.
		const bool sharedInputsValidated = validatedCohort == &cohort && validatedStageMask != 0U;
		GpuAnimationWorkRequest probe{};
		// Shared validation already proved all cohort-owned program, resource, and
		// layout fields. Keep the inexpensive per-instance identity/generation proof,
		// then let presentation lower the complete immutable input once at its normal
		// boundary. A one-member cohort still takes the full validation path below.
		const bool instanceIdentityValid = visual.stableInstanceId != 0U && IsGpuAnimationRenderIdentityValid(visual.renderIdentity) &&
										   IsGpuAnimationGenerationRecordValid(visual.generations);
		GpuAnimationLoweringStatus loweringStatus =
			instanceIdentityValid ? GpuAnimationLoweringStatus::Ok : GpuAnimationLoweringStatus::StaleGeneration;
		if(!sharedInputsValidated && instanceIdentityValid) {
			// Only the first member of an immutable cohort needs the complete
			// program/payload envelope. Later members validate their changing lane
			// above and reuse the cohort proof without touching shared ownership.
			GpuAnimationLoweringInput lowering;
			lowering.program.program = cohort.program;
			lowering.program.programContentId = cohort.program->stableHash;
			lowering.program.programResourceIndex = cohort.fastBinding->programResourceIndex;
			lowering.visual = visual;
			lowering.effectiveMode = compact.executionDecisions[compactIndex].effectiveMode;
			lowering.residentPayloads = cohort.fastBinding->residentPayloads;
			lowering.allowSharedVisualEvaluation = true;
			loweringStatus = LowerGpuAnimationWorkRequest(lowering, probe);
		}
		if(sharedInputsValidated) {
			probe.batchKey.stageMask = validatedStageMask;
		}
		if(loweringStatus != GpuAnimationLoweringStatus::Ok) {
			planningFailure = AnimationPlanRejectionReason::GpuLoweringRejected;
			compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
			return;
		}
		if(!sharedInputsValidated && !ValidateGpuAnimationResidentPayloads(probe, *cohort.fastBinding->residentPayloads)) {
			planningFailure = AnimationPlanRejectionReason::ResidentPayloadInvalid;
			compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
			return;
		}
		if(!sharedInputsValidated) {
			validatedCohort = &cohort;
			validatedStageMask = probe.batchKey.stageMask;
		}
		compact.stageMasks[compactIndex] = probe.batchKey.stageMask;
		compact.gpuPresentationVisuals[compactIndex] = std::move(visual);
		CompactGpuVisualInput compactInput{};
		compactInput.stableInstanceId = compact.gpuPresentationVisuals[compactIndex].stableInstanceId;
		compactInput.stableSubmissionOrdinal = compact.gpuPresentationVisuals[compactIndex].stableSubmissionOrdinal;
		compactInput.playbackTick = compact.playbacks[compactIndex].AbsoluteTick();
		compactInput.sourcePoseGeneration = compact.candidatePoseGenerations[compactIndex];
		compactInput.generations = {
			compactInput.stableInstanceId,	  input.componentGeneration,
			cohort.programGeneration,		  cohort.clipGeneration,
			frameInputs.frameSceneGeneration, compact.gpuPresentationVisuals[compactIndex].renderIdentity.rendererGeneration,
			compact.authorityGeneration};
		compactInput.eligibilityReason = compact.executionDecisions[compactIndex].reason;
		compactInput.gpuLoweringComplete = true;
		// The protected SoA admission gate already proved these exclusion facts.
		// Copy them into the fail-closed value record so classification consumes
		// the captured proof instead of mistaking safe direct clips for unknown work.
		compactInput.hasAuthoritativeConsumer = false;
		compactInput.hasControllerOrTransition = false;
		compactInput.hasAdvancedFeature = false;
		// Direct clips have an explicit, immutable bind-pose origin. They do not
		// need to wait one CPU frame merely to manufacture a previous execution
		// mode: the copied playback and proven direct binding fully determine the
		// first GPU packet. This avoids alternating complete/compact updates when
		// Play Mode begins or presentation completes one scheduler frame later.
		compactInput.hasContinuitySource = previous.initialized || (cohort.program != nullptr && cohort.fastBinding != nullptr);
		compact.compactInputs[compactIndex] = compactInput;
		compact.processingClasses[compactIndex] = ClassifyAnimationProcessing(compactInput);
		if(compact.processingClasses[compactIndex] != AnimationProcessingClass::CompactGpuVisual) {
			planningFailure = AnimationPlanRejectionReason::CompactClassificationRejected;
		}
	}

	void AnimationSystem::Impl::PlanRange(std::size_t begin, std::size_t end, const AnimatorPlaybackTimeInput& timeInput) noexcept {
		if(PollFrameCancellation())
			return;
		const auto rangePlanningStart = std::chrono::steady_clock::now();
		const std::size_t boundedEnd = std::min(end, frameInputs.capturedInputWorkIndices.size());
		std::size_t locallyPlanned = 0U;
		std::size_t locallyCompactPlanned = 0U;
		std::size_t locallyCompletePlanned = 0U;
		const CompactGpuVisualCohort* validatedCompactCohort = nullptr;
		std::uint32_t validatedCompactStageMask = 0U;
		// Identical prefab instances share their immutable program and resident
		// payload objects. Validate each exact shape once per worker range instead of
		// reparsing the same program and morph tables for every crowd member.
		const EvaluationProgram* validatedProgram = nullptr;
		std::uint32_t validatedProgramStageMask = 0U;
		bool validatedStageAllowsProcedural = false;
		bool validatedStageAllowsRagdoll = false;
		std::uint32_t validatedStageMorphCount = 0U;
		struct ResidentValidationKey {
			const GpuAnimationResidentPayloads* payload = nullptr;
			std::uint64_t programContentId = 0U;
			std::uint32_t stageMask = 0U;
			std::uint32_t flags = 0U;
			std::uint32_t programResourceIndex = 0U;
			std::uint32_t maskResourceIndex = 0U;
			std::uint32_t morphResourceIndex = 0U;
			std::uint32_t jointCount = 0U;
			std::uint32_t morphCount = 0U;
			std::uint32_t ragdollTableOffset = 0U;
			AnimationGeneration ragdollResourceGeneration = 0U;
			AnimationGeneration ragdollSkeletonGeneration = 0U;
			AnimationGeneration ragdollComponentGeneration = 0U;

			bool operator==(const ResidentValidationKey&) const noexcept = default;
		};
		std::optional<ResidentValidationKey> residentValidationKey;
		bool residentValidationResult = false;
		for(std::size_t index = begin; index < boundedEnd; ++index) {
			// The lifecycle token is immutable for this worker range. Poll in bounded
			// batches instead of reading the complete multi-boundary latch once per
			// animator; cancellation still stops within at most sixteen items.
			if(((index - begin) & 15U) == 0U && PollFrameCancellation())
				return;
			const CapturedInputWorkIndex& captured = frameInputs.capturedInputWorkIndices[index];
			if(captured.compact) {
				PlanCompactCapture(captured.storageIndex, timeInput, validatedCompactCohort, validatedCompactStageMask);
				++locallyCompactPlanned;
				++locallyPlanned;
				continue;
			}
			if(captured.storageIndex >= frameStorage.frameWork.size())
				return;
			FrameWorkItem& item = frameStorage.frameWork[captured.storageIndex];
			const auto currentPlaybackState = PrepareCompletePlayback(item, timeInput);
			// Mode policy is resolved entirely from this copied frame item.  The
			// resulting next state stays private until the all-or-none authority
			// publication below, so a rejected frame cannot advance hysteresis.
			AnimationExecutionModeRequest modeRequest;
			modeRequest.fixedTick = item.input.tick;
			modeRequest.requestedPolicy = item.binding.requestedExecutionPolicy;
			modeRequest.headless = headlessExecutionRequired;
			if(frameInputs.frameHasConsumerDeclarations) {
				item.resolvedConsumers =
					frameInputs.frameConsumerReadView.Resolve(item.input.handle, item.binding.jointParents, item.input.tick);
			} else {
				// Reset any retained vector capacity from an earlier diagnostic/gameplay
				// declaration without touching the registry's shared publication pointer.
				item.resolvedConsumers = {};
			}
			const AnimationResolvedConsumerRequirements& declaredConsumers = item.resolvedConsumers;
			std::vector<std::uint32_t> mergedAuthoritativeJoints;
			std::span<const std::uint32_t> authoritativeJoints = item.binding.authoritativeJointIndices;
			if(declaredConsumers.status == AnimationStatus::Ok && !declaredConsumers.closure.joints.empty()) {
				// The visual-crowd common case has no CPU joint reader, so borrow
				// the immutable authored list without allocating a temporary vector.
				mergedAuthoritativeJoints = item.binding.authoritativeJointIndices;
				mergedAuthoritativeJoints.insert(mergedAuthoritativeJoints.end(), declaredConsumers.closure.joints.begin(),
												 declaredConsumers.closure.joints.end());
				std::sort(mergedAuthoritativeJoints.begin(), mergedAuthoritativeJoints.end());
				mergedAuthoritativeJoints.erase(std::unique(mergedAuthoritativeJoints.begin(), mergedAuthoritativeJoints.end()),
												mergedAuthoritativeJoints.end());
				authoritativeJoints = mergedAuthoritativeJoints;
			} else if(declaredConsumers.status == AnimationStatus::Ok) {
				// Keep the branch explicit: an empty declaration must preserve the
				// authored span exactly, including its already-validated ordering.
			}
			modeRequest.deterministicRequired = declaredConsumers.deterministic || declaredConsumers.status != AnimationStatus::Ok;
			modeRequest.consumers.requestedJoints = authoritativeJoints;
			modeRequest.consumers.jointParents = item.binding.jointParents;
			modeRequest.consumers.curves = declaredConsumers.closure.curves;
			modeRequest.consumers.morphs = declaredConsumers.closure.morphs;
			modeRequest.consumers.events = declaredConsumers.closure.events;
			// Any authored mode that exposes or applies a delta is gameplay authority.
			// Force Hybrid/CPU closure even when no external consumer declaration was
			// published, otherwise GPU Visual would skip the root track entirely.
			// A policy says how to consume an authored delta; it does not create one.
			// Keeping a root-motion-free clip on GPU Visual avoids evaluating and
			// publishing a duplicate CPU pose for large in-place crowds. Explicit
			// consumers still request CPU authority even when the current clip has no
			// track, while interrupted/cross-fading root motion remains authoritative.
			modeRequest.consumers.rootMotion = RequiresCpuRootMotionAuthority(
				item.rootMotionEnabled ? AnimatorRootMotionPolicy::ApplyToCharacter : item.binding.rootMotionPolicy,
				item.binding.clip != nullptr && item.binding.clip->rootMotion.enabled, item.transition.capturedSourceRootMotion.has_value(),
				item.transition.ready && item.transition.destination.clip != nullptr &&
					item.transition.destination.clip->rootMotion.enabled,
				declaredConsumers.closure.rootMotion);
			modeRequest.consumers.contacts = declaredConsumers.closure.contacts;
			modeRequest.consumers.proceduralResults = declaredConsumers.closure.proceduralResults || !item.activeProceduralGoals.empty();
			modeRequest.consumers.physics = declaredConsumers.closure.physics || item.ragdollLifecycle == RagdollLifecycleState::Active ||
											item.ragdollLifecycle == RagdollLifecycleState::Recovering;
			modeRequest.consumers.attachments = declaredConsumers.closure.attachments;
			modeRequest.consumers.networking = declaredConsumers.closure.networking;
			modeRequest.consumers.rollback = declaredConsumers.closure.rollback;
			modeRequest.consumers.replay = declaredConsumers.closure.replay;
			modeRequest.consumers.recording = declaredConsumers.closure.recording;
			modeRequest.consumers.automation = declaredConsumers.closure.automation;
			modeRequest.consumers.diagnostics = declaredConsumers.closure.diagnostics;
			modeRequest.consumers.completePose =
				declaredConsumers.closure.completePose || item.retargetActive || declaredConsumers.status != AnimationStatus::Ok;
			modeRequest.gpu = item.binding.gpuExecution;
			// Do not require the published GPU envelope to equal the immediately
			// preceding authority generation. Animation presentation and rendering are
			// pipelined, so a complete immutable envelope may legitimately be one
			// generation ahead or behind this capture. RenderSystem validates the exact
			// scene, mesh, component, skeleton, resource, and renderer identities before
			// accepting that envelope; an unavailable or incompatible envelope therefore
			// remains a safe render fallback without forcing thousands of otherwise
			// eligible visual animators back through complete CPU evaluation.
			modeRequest.continuity.transitionBoundaryTick = item.input.tick;
			modeRequest.continuity.stateTimeTick = item.playback.AbsoluteTick();
			modeRequest.continuity.normalizedControllerTime = static_cast<float>(currentPlaybackState.resolvedTick);
			modeRequest.continuity.controllerState = item.binding.stateId;
			modeRequest.continuity.parameterSnapshot = item.candidateParameterGeneration;
			modeRequest.continuity.transition = item.candidateGraphState.activeTransitionStableId;
			modeRequest.continuity.synchronizationState = item.candidateStateGeneration;
			modeRequest.continuity.synchronizationPhaseTick = currentPlaybackState.resolvedTick;
			const AnimationEventStreamDiagnostics& eventDiagnostics = item.eventStream.Diagnostics();
			modeRequest.continuity.eventWatermark = eventDiagnostics.emittedEvents ^ (eventDiagnostics.duplicateEventsSuppressed << 1U) ^
													(eventDiagnostics.retainedEventEvictions << 2U);
			modeRequest.continuity.rootMotion = item.candidatePoseGeneration ^ static_cast<std::uint64_t>(item.playback.AbsoluteTick());
			modeRequest.continuity.motionMatch = item.appliedMotionQueryId;
			modeRequest.continuity.ragdollBlend = static_cast<std::uint64_t>(item.ragdollGeneration.publicationGeneration) ^
												  static_cast<std::uint64_t>(item.ragdollLifecycle);
			modeRequest.continuity.pose = item.candidatePoseGeneration;
			item.executionDecision = ResolveAnimationExecutionMode(modeRequest, item.previousExecutionModeState);
			auto forceCpuFallback = [&item](AnimationExecutionModeReason reason) noexcept {
				// This is a frame-local downgrade after the policy resolver has already
				// copied its continuity token. Keep the persisted mode/transition record
				// coherent so a rejected command cannot later look like GPU authority.
				const AnimationEffectiveExecutionMode previousMode = item.previousExecutionModeState.initialized
																		 ? item.previousExecutionModeState.effectiveMode
																		 : AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				item.executionDecision.effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				item.executionDecision.reason = reason;
				item.executionDecision.eligibleMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				item.executionDecision.eligibilityReason = reason;
				item.executionDecision.modeChanged = previousMode != AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				item.executionDecision.transition.occurred = item.executionDecision.modeChanged;
				item.executionDecision.transition.from = previousMode;
				item.executionDecision.transition.to = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				item.executionDecision.transition.continuity = item.executionDecision.nextState.continuity;
				item.executionDecision.nextState.initialized = true;
				item.executionDecision.nextState.effectiveMode = item.executionDecision.effectiveMode;
				item.executionDecision.nextState.effectiveReason = item.executionDecision.reason;
				item.executionDecision.nextState.modeEnteredTick = item.input.tick;
				item.executionDecision.nextState.promotionPending = false;
				item.executionDecision.nextState.promotionMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				item.executionDecision.nextState.promotionReason = reason;
				item.executionDecision.nextState.promotionSinceTick = 0;
				item.executionDecision.nextState.promotionFingerprint = 0U;
			};
			if(item.evaluationStatus != AnimationStatus::Ok) {
				// Command/clock failures predate evaluation. They must never publish a
				// GPU-only pose, because no authoritative CPU fallback has been built
				// yet and the published status is already non-OK.
				forceCpuFallback(AnimationExecutionModeReason::GpuEvaluationFailure);
			}
			if(item.candidateGraphState.activeTransitionStableId != 0U && item.transition.ready) {
				// The current GPU packet ABI names only one clip cursor. Until its
				// append-only layout carries both transition inputs, publish the
				// complete CPU-blended pose through the existing safe fallback path.
				// This is an explicit capability downgrade, never a silent one-clip
				// approximation that would hold and snap at transition completion.
				forceCpuFallback(AnimationExecutionModeReason::GpuCapabilityUnavailable);
			}
			item.visualEvaluationSuspended = CanSuspendVisualEvaluation(item);
			if(item.planned && (item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual ||
								item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::HybridAuthoritative)) {
				GpuAnimationLoweringInput gpuInput;
				const std::shared_ptr<const AnimatorPlaybackBinding::GpuFastBinding>& fastBinding = item.binding.gpuFastBinding;
				gpuInput.program.program = item.binding.evaluationProgram;
				gpuInput.program.programContentId = item.binding.evaluationProgram ? item.binding.evaluationProgram->stableHash : 0U;
				gpuInput.program.programResourceIndex =
					fastBinding && fastBinding->IsComplete() ? fastBinding->programResourceIndex : kGpuAnimationInvalidResourceIndex;
				gpuInput.visual = fastBinding && fastBinding->IsComplete() ? fastBinding->visual : GpuAnimationVisualInput{};
				if(item.visualEvaluationSuspended) {
					// Suspension is a per-frame generation-lane decision. It must not
					// rewrite immutable bind/hold fallback packet flags, otherwise view
					// changes would rebuild the topology they are intended to preserve.
					// Every GPU stage reads this bit and leaves the last complete pose
					// untouched until the object becomes visible again.
					gpuInput.visual.suspendVisualEvaluation = true;
				}
				// The handle generation is part of the stable producer identity; a
				// reused slot therefore cannot share a packet with stale work.
				gpuInput.visual.stableInstanceId = (static_cast<std::uint64_t>(item.input.handle.slotIndex) << 32U) |
												   static_cast<std::uint64_t>(item.input.handle.generation);
				gpuInput.visual.stableSubmissionOrdinal = item.input.handle.slotIndex;
				gpuInput.visual.jointCount = static_cast<std::uint32_t>(item.binding.jointStableIds.size());
				// The binding coordinator orders GPU morph lanes against the
				// deformable mesh table. Preserve that proven count instead of
				// rebuilding it from clip-facing named-output storage here.
				gpuInput.visual.generations.instanceSlot = item.input.handle.slotIndex;
				gpuInput.visual.generations.instanceGeneration = item.input.handle.generation;
				gpuInput.visual.generations.componentGeneration = item.input.descriptor.componentGeneration;
				gpuInput.visual.generations.workGeneration = item.candidatePoseGeneration;
				gpuInput.visual.generations.programGeneration = item.binding.programGeneration;
				gpuInput.visual.generations.controllerGeneration = item.binding.programGeneration;
				gpuInput.visual.generations.skeletonGeneration = item.binding.skeletonGeneration;
				gpuInput.visual.generations.clipGeneration = item.binding.clipGeneration;
				const bool gpuFrameIdentityRepresentable =
					StampGpuVisualFrameIdentity(gpuInput.visual, currentPlaybackState.resolvedTick, item.input.tick);
				gpuInput.effectiveMode = item.executionDecision.effectiveMode;
				gpuInput.residentPayloads = fastBinding && fastBinding->IsComplete()
												? fastBinding->residentPayloads
												: std::shared_ptr<const GpuAnimationResidentPayloads>{};
				gpuInput.programValidated = validatedProgram != nullptr && validatedProgram == gpuInput.program.program.get();
				const bool stageShapeMatches = gpuInput.programValidated &&
											   validatedStageAllowsProcedural == gpuInput.visual.allowVisualProcedural &&
											   validatedStageAllowsRagdoll == gpuInput.visual.allowVisualRagdoll &&
											   validatedStageMorphCount == gpuInput.visual.morphCount;
				gpuInput.prevalidatedStageMask = stageShapeMatches ? validatedProgramStageMask : 0U;

				// Prove the entire immutable lowering input now, before CPU evaluation
				// begins. Missing programs, descriptor indices, or resident bytes must
				// downgrade this copied frame item to CPU instead of letting a later
				// presentation failure discard the only authoritative pose.
				GpuAnimationWorkRequest probe{};
				const GpuAnimationLoweringStatus loweringStatus = gpuFrameIdentityRepresentable
																	  ? LowerGpuAnimationWorkRequest(gpuInput, probe)
																	  : GpuAnimationLoweringStatus::StaleGeneration;
				if(loweringStatus == GpuAnimationLoweringStatus::Ok && !gpuInput.programValidated) {
					validatedProgram = gpuInput.program.program.get();
				}
				if(loweringStatus == GpuAnimationLoweringStatus::Ok && gpuInput.prevalidatedStageMask == 0U) {
					// Optional visual stages are part of the derivation key. A
					// procedurally enabled hero must never inherit a crowd mask.
					validatedProgramStageMask = probe.batchKey.stageMask;
					validatedStageAllowsProcedural = gpuInput.visual.allowVisualProcedural;
					validatedStageAllowsRagdoll = gpuInput.visual.allowVisualRagdoll;
					validatedStageMorphCount = gpuInput.visual.morphCount;
				}
				const bool ragdollValidation = HasGpuAnimationFlag(probe.flags, GpuAnimationWorkFlag::AllowRagdollVisual);
				const ResidentValidationKey candidateValidationKey{gpuInput.residentPayloads.get(),
																   probe.batchKey.programContentId,
																   probe.batchKey.stageMask,
																   probe.flags,
																   probe.programResourceIndex,
																   probe.maskResourceIndex,
																   probe.morphResourceIndex,
																   probe.jointCount,
																   probe.morphCount,
																   ragdollValidation ? probe.generations.reserved : 0U,
																   ragdollValidation ? probe.generations.resourceGeneration : 0U,
																   ragdollValidation ? probe.generations.skeletonGeneration : 0U,
																   ragdollValidation ? probe.generations.componentGeneration : 0U};
				if(!residentValidationKey.has_value() || *residentValidationKey != candidateValidationKey) {
					residentValidationKey = candidateValidationKey;
					residentValidationResult = loweringStatus == GpuAnimationLoweringStatus::Ok && gpuInput.residentPayloads != nullptr &&
											   ValidateGpuAnimationResidentPayloads(probe, *gpuInput.residentPayloads);
				}
				const bool residentInputsComplete = residentValidationResult;
				if(loweringStatus == GpuAnimationLoweringStatus::Ok && residentInputsComplete) {
					gpuInput.residentPayloadsValidated = true;
					gpuInput.prevalidatedStageMask = probe.batchKey.stageMask;
					item.gpuStageMask = probe.batchKey.stageMask;
					item.gpuLowering = std::move(gpuInput);
				} else {
					forceCpuFallback(loweringStatus == GpuAnimationLoweringStatus::Ok
										 ? AnimationExecutionModeReason::GpuResourcesUnavailable
										 : AnimationExecutionModeReason::GpuEvaluationFailure);
				}
			}
			if(!TryAdvanceGeneration(item.capturedExecutionModeGeneration,
									 item.executionDecision.nextState.initialized != item.previousExecutionModeState.initialized ||
										 item.executionDecision.nextState.effectiveMode != item.previousExecutionModeState.effectiveMode ||
										 item.executionDecision.nextState.effectiveReason !=
											 item.previousExecutionModeState.effectiveReason,
									 item.candidateExecutionModeGeneration)) {
				item.evaluationStatus = AnimationStatus::RangeOverflow;
			}
			CompactGpuVisualInput compactInput;
			compactInput.stableInstanceId =
				(static_cast<std::uint64_t>(item.input.handle.slotIndex) << 32U) | static_cast<std::uint64_t>(item.input.handle.generation);
			compactInput.stableSubmissionOrdinal = item.input.handle.slotIndex;
			compactInput.playbackTick = item.playback.AbsoluteTick();
			compactInput.sourcePoseGeneration = item.candidatePoseGeneration;
			compactInput.generations.membershipGeneration = compactInput.stableInstanceId;
			compactInput.generations.capabilityGeneration = item.input.descriptor.componentGeneration;
			compactInput.generations.bindingGeneration = item.binding.programGeneration;
			compactInput.generations.resourceGeneration = item.binding.clipGeneration;
			compactInput.generations.sceneGeneration = frameInputs.frameSceneGeneration;
			compactInput.generations.rendererGeneration =
				item.gpuLowering ? item.gpuLowering->visual.renderIdentity.rendererGeneration : 0U;
			compactInput.generations.authorityGeneration = item.candidateAuthorityPublicationGeneration;
			compactInput.eligibilityReason = item.executionDecision.reason;
			compactInput.gpuLoweringComplete = item.gpuLowering.has_value();
			compactInput.hasAuthoritativeConsumer = item.executionDecision.consumerClosure.HasAuthoritativeSubset();
			compactInput.hasControllerOrTransition =
				item.binding.controllerProgramRequired || item.candidateGraphState.activeTransitionStableId != 0U;
			compactInput.hasAdvancedFeature = item.retargetActive || item.hasMotionCandidate || !item.activeProceduralGoals.empty() ||
											  item.ragdollLifecycle != RagdollLifecycleState::Inactive;
			compactInput.hasContinuitySource = item.previousExecutionModeState.initialized;
			item.compactGpuVisualInput = compactInput;
			item.processingClass = ClassifyAnimationProcessing(compactInput);
			ResolveUnchangedCacheReuse(item);
			if(item.processingClass == AnimationProcessingClass::CompactGpuVisual) {
				++locallyCompactPlanned;
			} else {
				++locallyCompletePlanned;
			}
			++locallyPlanned;
		}
		if(locallyPlanned != 0U) {
			const std::uint64_t rangePlanningNanoseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - rangePlanningStart).count());
			// Planning telemetry describes worker-owned CPU time; it must not change
			// the cost being measured. Attribute one range timer proportionally when
			// a partition contains both classes, replacing two clock reads and one
			// contended atomic operation for every animator with at most two atomics.
			const std::uint64_t compactRangeNanoseconds =
				static_cast<std::uint64_t>(static_cast<long double>(rangePlanningNanoseconds) *
										   static_cast<long double>(locallyCompactPlanned) / static_cast<long double>(locallyPlanned));
			const std::uint64_t completeRangeNanoseconds = rangePlanningNanoseconds - compactRangeNanoseconds;
			if(compactRangeNanoseconds != 0U) {
				evaluationState.compactPlanningNanoseconds.fetch_add(compactRangeNanoseconds, std::memory_order_relaxed);
			}
			if(completeRangeNanoseconds != 0U) {
				evaluationState.completePlanningNanoseconds.fetch_add(completeRangeNanoseconds, std::memory_order_relaxed);
			}
		}
		// One partition owns this complete contiguous range, so a single atomic
		// publication replaces cache-line contention from every animator.
		if(locallyPlanned != 0U) {
			evaluationState.plannedAnimatorCount.fetch_add(locallyPlanned, std::memory_order_acq_rel);
		}
	}

	bool AnimationSystem::Impl::FinalizeAnimationWorkPlan() noexcept {
		evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::None;
		evaluationState.workPlanRejectionItemIndex = std::numeric_limits<std::size_t>::max();
		if(frameInputs.capturedInputWorkIndices.empty() || PollFrameCancellation() ||
		   evaluationState.plannedAnimatorCount.load(std::memory_order_acquire) != frameInputs.capturedInputWorkIndices.size()) {
			evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::PlanningIncomplete;
			return false;
		}
		std::size_t estimatedPoseBytes = 0U;
		try {
			evaluationState.capturedWorkItems.clear();
			evaluationState.compactGpuVisualWorkItems.clear();
			evaluationState.capturedWorkItems.reserve(frameStorage.frameWork.size());
			evaluationState.compactGpuVisualWorkItems.reserve(frameStorage.frameWork.size());
			for(std::size_t originalIndex = 0U; originalIndex < frameStorage.frameWork.size(); ++originalIndex) {
				const FrameWorkItem& item = frameStorage.frameWork[originalIndex];
				if(item.compactCaptureOnly && item.processingClass != AnimationProcessingClass::CompactGpuVisual) {
					evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::CompactClassificationRejected;
					evaluationState.workPlanRejectionItemIndex = originalIndex;
					frameStorage.workPlan = {};
					frameStorage.workPlan.status = AnimationStatus::Rejected;
					frameStorage.workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
					return false;
				}
				if(item.processingClass == AnimationProcessingClass::CompactGpuVisual) {
					// Classification is a complete immutable proof. Recheck the two facts
					// that bind it to this publication record before omitting the general
					// evaluator; any inconsistency fails closed into the complete path.
					if(!item.gpuLowering || !item.compactGpuVisualInput.IsComplete()) {
						evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::CompactInputIncomplete;
						evaluationState.workPlanRejectionItemIndex = originalIndex;
						frameStorage.workPlan = {};
						frameStorage.workPlan.status = AnimationStatus::Rejected;
						frameStorage.workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
						return false;
					}
					evaluationState.compactGpuVisualWorkItems.append(item.capturedInputIndex, item.input.handle,
																	 item.candidateAuthorityPublicationGeneration,
																	 item.compactGpuVisualInput);
					continue;
				}
				AnimationWorkItem workItem;
				workItem.animator = item.input.handle;
				workItem.compatibility.programGeneration = item.binding.programGeneration;
				workItem.compatibility.skeletonGeneration = item.binding.skeletonGeneration;
				workItem.compatibility.clipBindingGeneration = item.binding.clipGeneration;
				workItem.compatibility.outputRequirementsFingerprint =
					MixWorkFingerprint(item.bindingOutputRequirementsFingerprint, item.resolvedConsumers.fingerprint);
				workItem.compatibility.resourceLayoutFingerprint =
					MixWorkFingerprint(item.bindingResourceLayoutFingerprint, item.retargetActive ? 1U : 0U);
				workItem.compatibility.effectiveMode = item.executionDecision.effectiveMode;
				workItem.stablePublicationOrder = item.capturedInputIndex;
				workItem.sourceGeneration = item.candidateAuthorityPublicationGeneration;
				workItem.scratchBytes = sizeof(AnimationFrameScratchRecord);
				evaluationState.capturedWorkItems.push_back(workItem);
				const bool createsCpuPose =
					!item.cacheReused && !item.visualEvaluationSuspended &&
					!(item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual && item.gpuLowering.has_value());
				if(createsCpuPose && !TryEstimatePoseBytes(item, estimatedPoseBytes)) {
					frameStorage.workPlan = {};
					frameStorage.workPlan.status = AnimationStatus::RangeOverflow;
					frameStorage.workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
					return false;
				}
			}
			for(std::size_t compactIndex = 0U; compactIndex < frameInputs.compactGpuVisualCaptures.size(); ++compactIndex) {
				if(frameInputs.compactGpuVisualCaptures.processingClasses[compactIndex] != AnimationProcessingClass::CompactGpuVisual ||
				   !frameInputs.compactGpuVisualCaptures.compactInputs[compactIndex].IsComplete()) {
					evaluationState.workPlanRejectionReason = frameInputs.compactGpuVisualCaptures.planningFailures[compactIndex];
					if(evaluationState.workPlanRejectionReason == AnimationPlanRejectionReason::None) {
						evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::CompactInputIncomplete;
					}
					evaluationState.workPlanRejectionItemIndex = frameInputs.compactGpuVisualCaptures.inputIndices[compactIndex];
					frameStorage.workPlan = {};
					frameStorage.workPlan.status = AnimationStatus::Rejected;
					frameStorage.workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
					return false;
				}
				evaluationState.compactGpuVisualWorkItems.append(frameInputs.compactGpuVisualCaptures.inputIndices[compactIndex],
																 frameInputs.compactGpuVisualCaptures.inputs[compactIndex].handle,
																 frameInputs.compactGpuVisualCaptures.authorityGeneration,
																 frameInputs.compactGpuVisualCaptures.compactInputs[compactIndex]);
			}
		} catch(const std::bad_alloc&) {
			evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::AllocationFailure;
			frameStorage.workPlan = {};
			frameStorage.workPlan.status = AnimationStatus::InternalError;
			frameStorage.workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
			return false;
		}

		evaluationState.availableWorkerCount = ThreadPool::instance().getThreadCount();
		frameStorage.workPlan = frameStorage.workScheduler.BuildPlan(
			evaluationState.capturedWorkItems, evaluationState.availableWorkerCount, evaluationState.workCancellationSource.CaptureToken());
		if(frameStorage.workPlan.status != AnimationStatus::Ok || PollFrameCancellation()) {
			evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::SchedulerRejected;
			return false;
		}
		// AnimationWorkScheduler::BuildPlan already validates the complete plan.
		// Retain that proof instead of repeating the O(animator-count) scan in
		// every worker partition and again at publication.
		evaluationState.workPlanValidated = true;

		AnimationMemoryLease stagedLease;
		AnimationMemoryLease stagedPoseLease;
		const std::uint64_t planAuthorityGeneration = !frameInputs.compactGpuVisualCaptures.empty()
														  ? frameInputs.compactGpuVisualCaptures.authorityGeneration
														  : frameStorage.frameWork.front().candidateAuthorityPublicationGeneration;
		if(frameStorage.workPlan.totalScratchBytes != 0U) {
			// A frame made entirely of CompactGpuVisual records has no generic
			// evaluator, ranges, or scratch storage. The scheduler deliberately
			// represents that as a valid Omitted plan, while the memory controller
			// correctly rejects zero-byte reservations. Reserve only when generic
			// work actually owns scratch bytes so compact packets can publish their
			// current playback tick through the separate compact completion path.
			const AnimationMemoryReservationRequest memoryRequest{AnimationMemoryClass::ScratchPool,
																  frameStorage.workPlan.totalScratchBytes, planAuthorityGeneration,
																  AnimationMemoryRetention::Required, 0U};
			const AnimationMemoryStatus memoryStatus = persistentReuse.memoryController.TryReserve(memoryRequest, stagedLease);
			if(memoryStatus != AnimationMemoryStatus::Reserved) {
				evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::MemoryReservationRejected;
				frameStorage.workPlan.status =
					memoryStatus == AnimationMemoryStatus::BudgetExceeded || memoryStatus == AnimationMemoryStatus::ReservationTableFull
						? AnimationStatus::HardLimitExceeded
						: AnimationStatus::InternalError;
				frameStorage.workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
				return false;
			}
		}
		if(estimatedPoseBytes != 0U) {
			const AnimationMemoryReservationRequest poseMemoryRequest{AnimationMemoryClass::PosePool, estimatedPoseBytes,
																	  planAuthorityGeneration, AnimationMemoryRetention::Required, 0U};
			const AnimationMemoryStatus poseMemoryStatus = persistentReuse.memoryController.TryReserve(poseMemoryRequest, stagedPoseLease);
			if(poseMemoryStatus != AnimationMemoryStatus::Reserved) {
				evaluationState.workPlanRejectionReason = AnimationPlanRejectionReason::MemoryReservationRejected;
				frameStorage.workPlan.status = poseMemoryStatus == AnimationMemoryStatus::BudgetExceeded ||
													   poseMemoryStatus == AnimationMemoryStatus::ReservationTableFull
												   ? AnimationStatus::HardLimitExceeded
												   : AnimationStatus::InternalError;
				frameStorage.workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
				return false;
			}
		}

		try {
			// Frame scratch and completion storage have no observable state until
			// protected publication succeeds. Resize them in place so a steady crowd
			// reuses capacity instead of allocating two O(animator-count) arrays per
			// frame; a failure still rejects the whole unpublished plan atomically.
			frameStorage.frameScratch.resize(frameStorage.workPlan.totalScratchBytes);
			evaluationState.workCompletions.clear();
			evaluationState.workCompletions.resize(frameStorage.workPlan.items.size());
			evaluationState.compactGpuVisualCompletions.clear();
			evaluationState.compactGpuVisualCompletions.resize(evaluationState.compactGpuVisualWorkItems.size());
			auto stagedRangeLedger = std::make_unique<AnimationWorkRangeLedger>(frameStorage.workPlan);
			std::shared_ptr<const AnimationMemoryLease> stagedPoseReservation;
			if(stagedPoseLease.IsActive()) {
				stagedPoseReservation = std::make_shared<AnimationMemoryLease>(std::move(stagedPoseLease));
			}
			// The scheduler retains the checked scratch reservation for evaluators that
			// need it, while this typed path publishes directly into workCompletions.
			// Avoid initializing a second terminal-status record that no consumer reads.
			frameStorage.workRangeLedger = std::move(stagedRangeLedger);
			frameStorage.frameScratchLease = std::move(stagedLease);
			frameStorage.framePoseReservation = std::move(stagedPoseReservation);
		} catch(const std::bad_alloc&) {
			frameStorage.workPlan.status = AnimationStatus::InternalError;
			frameStorage.workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
			return false;
		}
		return !PollFrameCancellation();
	}

	bool AnimationSystem::Impl::EnsureAnimationWorkPlanFinalized() noexcept {
		{
			std::unique_lock lock(evaluationState.workPlanFinalizeMutex);
			if(evaluationState.workPlanFinalizeState == WorkPlanFinalizeState::Ready)
				return true;
			if(evaluationState.workPlanFinalizeState == WorkPlanFinalizeState::Failed)
				return false;
			if(evaluationState.workPlanFinalizeState == WorkPlanFinalizeState::Building) {
				evaluationState.workPlanFinalizeCondition.wait(
					lock, [this]() { return evaluationState.workPlanFinalizeState != WorkPlanFinalizeState::Building; });
				return evaluationState.workPlanFinalizeState == WorkPlanFinalizeState::Ready;
			}
			evaluationState.workPlanFinalizeState = WorkPlanFinalizeState::Building;
		}

		const bool finalized = FinalizeAnimationWorkPlan();
		{
			std::scoped_lock lock(evaluationState.workPlanFinalizeMutex);
			evaluationState.workPlanFinalizeState = finalized ? WorkPlanFinalizeState::Ready : WorkPlanFinalizeState::Failed;
		}
		evaluationState.workPlanFinalizeCondition.notify_all();
		return finalized;
	}

	AnimatorPlaybackStateSnapshot AnimationSystem::Impl::PrepareCompletePlayback(FrameWorkItem& item,
																				 const AnimatorPlaybackTimeInput& timeInput) noexcept {
		AnimationStatus commandStatus = AnimationStatus::Ok;
		bool stateCommandApplied = false;
		bool parameterCommandApplied = false;
		bool directStateOverrideQueued = false;
		std::vector<StateMachineTriggerEdge> triggerEdges;
		for(std::size_t commandIndex = 0U; commandIndex < frameInputs.capturedCommands.size(); ++commandIndex) {
			const AnimationCommand& command = frameInputs.capturedCommands[commandIndex];
			if(command.target != item.input.handle)
				continue;
			if(frameInputs.capturedCommandStatuses[commandIndex] != AnimationStatus::NotReady) {
				if(commandStatus == AnimationStatus::Ok)
					commandStatus = frameInputs.capturedCommandStatuses[commandIndex];
				continue;
			}
			if(command.expectedController.IsValid() && command.expectedController != item.input.descriptor.controller) {
				frameInputs.capturedCommandStatuses[commandIndex] = AnimationStatus::StaleHandle;
				if(commandStatus == AnimationStatus::Ok)
					commandStatus = AnimationStatus::StaleHandle;
				continue;
			}
			if(command.kind == AnimationCommandKind::Play || command.kind == AnimationCommandKind::CrossFade ||
			   command.kind == AnimationCommandKind::SetLayerWeight) {
				directStateOverrideQueued = directStateOverrideQueued || command.kind == AnimationCommandKind::Play ||
											command.kind == AnimationCommandKind::CrossFade;
				continue;
			}
			AnimationStatus current =
				command.kind == AnimationCommandKind::SetParameter || command.kind == AnimationCommandKind::FireTrigger
					? ApplyParameterCommand(item.parameters, command)
					: ApplyPlaybackCommand(item.playback, command);
			if(current == AnimationStatus::Ok && command.kind == AnimationCommandKind::FireTrigger) {
				if(command.parameterId == 0U || command.producerSequence == 0U) {
					current = AnimationStatus::InvalidArgument;
				} else {
					try {
						triggerEdges.push_back({command.parameterId, command.producerSequence, true});
					} catch(const std::bad_alloc&) {
						current = AnimationStatus::InternalError;
					}
				}
			}
			frameInputs.capturedCommandStatuses[commandIndex] = current;
			if(current == AnimationStatus::Ok) {
				if(command.kind == AnimationCommandKind::SetParameter || command.kind == AnimationCommandKind::FireTrigger) {
					parameterCommandApplied = true;
				} else {
					stateCommandApplied = true;
				}
			}
			if(current != AnimationStatus::Ok && commandStatus == AnimationStatus::Ok) {
				commandStatus = current;
			}
		}
		const AnimationStatus preGraphCommandStatus = commandStatus;
		if(commandStatus == AnimationStatus::Ok && item.binding.evaluationProgram && !item.restoredFromHistory) {
			StateMachineStepInput graphInput;
			graphInput.tick = item.input.tick;
			graphInput.triggerEdges = triggerEdges;
			// Capture already copied the last-good graph into this worker-owned
			// candidate. Advance it directly so synchronized crowds reuse retained
			// history capacity instead of allocating and copying a third snapshot.
			const AnimationStatus graphStatus =
				StepStateMachineTrustedInPlace(*item.binding.evaluationProgram, item.candidateGraphState, graphInput);
			if(graphStatus == AnimationStatus::Ok) {
				stateCommandApplied = stateCommandApplied || item.candidateGraphState != item.capturedGraphState;
				item.binding.stateId = item.candidateGraphState.activeStateId;
				const AnimationStatus clipStatus = SelectControllerClipForState(item, item.binding.stateId);
				if(clipStatus != AnimationStatus::Ok)
					commandStatus = clipStatus;
			} else if(!directStateOverrideQueued) {
				// The in-place evaluator may have advanced part of its private candidate
				// before an allocation failed. Restore the last-good state so fallback
				// publication remains atomic and can never commit a partial graph.
				item.candidateGraphState = item.capturedGraphState;
				commandStatus = graphStatus;
			} else {
				// A direct Play/CrossFade may intentionally recover this graph below,
				// but it must start from the protected last-good snapshot.
				item.candidateGraphState = item.capturedGraphState;
			}
		}
		// Direct state and layer commands intentionally run after the ordinary
		// graph step. A Play/CrossFade can recover an otherwise uninitialized
		// graph, while parameter validation failures still reject the command set.
		if(preGraphCommandStatus == AnimationStatus::Ok) {
			for(std::size_t commandIndex = 0U; commandIndex < frameInputs.capturedCommands.size(); ++commandIndex) {
				const AnimationCommand& command = frameInputs.capturedCommands[commandIndex];
				if(command.target != item.input.handle ||
				   (command.kind != AnimationCommandKind::Play && command.kind != AnimationCommandKind::CrossFade &&
					command.kind != AnimationCommandKind::SetLayerWeight))
					continue;
				const bool layerChanged = command.kind == AnimationCommandKind::SetLayerWeight &&
										  command.layerIndex < item.layerWeights.size() &&
										  item.layerWeights[command.layerIndex] != command.layerWeight;
				const AnimationStatus current = ApplyGraphControlCommand(item, command);
				frameInputs.capturedCommandStatuses[commandIndex] = current;
				if(current == AnimationStatus::Ok) {
					stateCommandApplied = stateCommandApplied || command.kind != AnimationCommandKind::SetLayerWeight || layerChanged;
					if(command.kind == AnimationCommandKind::Play || command.kind == AnimationCommandKind::CrossFade) {
						commandStatus = AnimationStatus::Ok;
					}
				} else if(commandStatus == AnimationStatus::Ok || directStateOverrideQueued) {
					commandStatus = current;
				}
			}
		}
		if(commandStatus == AnimationStatus::Ok) {
			bool motionSelectionChanged = false;
			const AnimationStatus motionSelectionStatus = ApplyRetainedMotionSelection(item, motionSelectionChanged);
			if(motionSelectionStatus != AnimationStatus::Ok) {
				commandStatus = motionSelectionStatus;
			} else {
				stateCommandApplied = stateCommandApplied || motionSelectionChanged;
			}
		}
		if(commandStatus == AnimationStatus::Ok) {
			const AnimationStatus transitionStatus = SynchronizeControllerTransition(item);
			if(transitionStatus != AnimationStatus::Ok) {
				commandStatus = transitionStatus;
			}
		}
		AnimatorPlaybackTimeInput effectiveTime = timeInput;
		if(item.binding.updatePolicy == AnimatorUpdatePolicy::Manual || item.restoredFromHistory) {
			// Manual animators still consume explicit commands and publish their
			// resulting snapshot, but automatic frame time never advances them. A
			// restored animator likewise republishes the exact retained fixed-point
			// state once before ordinary advancement resumes.
			effectiveTime = {};
		}
		if(item.binding.updatePolicy != AnimatorUpdatePolicy::Manual && !item.restoredFromHistory) {
			switch(item.binding.playback.timePolicy) {
			case AnimatorTimePolicy::Scaled:
				item.physicalEvaluationDurationSeconds = effectiveTime.scaledDeltaSeconds;
				break;
			case AnimatorTimePolicy::Unscaled:
				item.physicalEvaluationDurationSeconds = effectiveTime.unscaledDeltaSeconds;
				break;
			case AnimatorTimePolicy::Fixed:
				item.physicalEvaluationDurationSeconds =
					effectiveTime.fixedDeltaTicks > 0
						? static_cast<double>(effectiveTime.fixedDeltaTicks) / static_cast<double>(kAnimationFixedTicksPerSecond)
						: 0.0;
				break;
			}
		}
		const AnimationStatus advanceStatus = item.playback.Advance(effectiveTime, item.advance);
		AnimationStatus transitionAdvanceStatus = AnimationStatus::Ok;
		if(item.candidateGraphState.activeTransitionStableId != 0U && item.transition.ready) {
			transitionAdvanceStatus = item.transition.destinationPlayback.Advance(effectiveTime, item.transition.destinationAdvance);
		}
		item.evaluationStatus = commandStatus != AnimationStatus::Ok   ? commandStatus
								: advanceStatus != AnimationStatus::Ok ? advanceStatus
																	   : transitionAdvanceStatus;
		const AnimatorPlaybackStateSnapshot currentPlaybackState = item.playback.Snapshot();
		const bool playbackChanged = stateCommandApplied || currentPlaybackState != item.capturedPlaybackState;
		if(!TryAdvanceGeneration(item.capturedStateGeneration, playbackChanged, item.candidateStateGeneration) ||
		   !TryAdvanceGeneration(item.capturedParameterGeneration, parameterCommandApplied, item.candidateParameterGeneration)) {
			item.evaluationStatus = AnimationStatus::RangeOverflow;
		}
		// A planning failure is still a complete worker result. Evaluation selects a
		// prior or bind-pose fallback instead of stalling the whole frame forever.
		item.planned = true;
		return currentPlaybackState;
	}

	void AnimationSystem::Impl::PlanAnimationEvaluationStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
															 const AnimatorPlaybackTimeInput& timeInput,
															 const TerraEngine::Scheduling::FrameWorkloadPartition& partition,
															 TerraEngine::Scheduling::JobGraphContext& context) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Plan Evaluation Range", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(context.IsCancellationRequested()) {
			this->RequestFrameCancellation();
			return;
		}
		if(!this->runtimeActive.load(std::memory_order_acquire) ||
		   this->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch ||
		   this->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch)
			return;
		this->PlanRange(partition.itemBegin, partition.itemEnd, timeInput);
	}

	void AnimationSystem::Impl::FinalizeAnimationWorkPlanStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
															   TerraEngine::Scheduling::JobGraphContext& context) {

		TERRAENGINE_PROFILE_SCOPE("Animation: Finalize Work Plan", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(context.IsCancellationRequested()) {
			this->RequestFrameCancellation();
			return;
		}
		if(!this->runtimeActive.load(std::memory_order_acquire) ||
		   this->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch ||
		   this->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch)
			return;
		(void)this->EnsureAnimationWorkPlanFinalized();
	}

} // namespace TerraEngine::Animation
