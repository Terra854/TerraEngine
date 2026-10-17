#include "Animation/Frame/AnimationFrameRuntime.h"

// The coordinator retains bootstrap and the nine existing scheduler declarations.
namespace TerraEngine::Animation {
	AnimationSystem& AnimationSystem::Instance() {
		static AnimationSystem system;
		return system;
	}

	AnimationSystem::AnimationSystem() : impl_(std::make_unique<Impl>()) {}

	AnimationSystem::~AnimationSystem() = default;

	void AnimationSystem::Initialize() {
		impl_->AssertCoordinatorThread();
		{
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			// SystemsBase initialization is idempotent. Reinitializing an already
			// active singleton must not erase published counts or captured state while
			// leaving their public generation identity unchanged.
			if(impl_->runtimeActive.load(std::memory_order_acquire))
				return;
			impl_->ActivateRuntimeOwnershipLocked();
			impl_->frameInputs.capturedFrameInputs.clear();
			impl_->frameInputs.capturedCommands.clear();
			impl_->frameInputs.qualifiedControllerResourceRevision = 0U;
			impl_->frameInputs.capturedCommandStatuses.clear();
			impl_->frameInputs.capturedProceduralCommands.clear();
			impl_->frameInputs.capturedProceduralCommandStatuses.clear();
			impl_->frameInputs.capturedProceduralGoalRanges.clear();
			impl_->frameInputs.capturedProceduralGoals.clear();
			if(impl_->proceduralCommands != nullptr)
				impl_->proceduralCommands->Clear();
			impl_->frameInputs.capturedRetargetRequests.clear();
			impl_->frameInputs.capturedMotionMatchRequests.clear();
			impl_->frameInputs.capturedMotionMatchQueriesV2.clear();
			impl_->frameInputs.capturedRagdollRequests.clear();
			impl_->frameInputs.capturedDeterminismRequests.clear();
			if(impl_->advancedCommands != nullptr)
				impl_->advancedCommands->Clear();
			impl_->pendingRootMotionApplications.clear();
			impl_->frameStorage.frameWork.clear();
			impl_->persistentReuse.candidateFrameWork.clear();
			impl_->persistentReuse.gpuPresentationFramePool = {};
			impl_->persistentReuse.gpuPresentationFramePoolCursor = 0U;
			impl_->runtimeRecords.store(std::make_shared<Impl::RuntimeRecordRoot>(), std::memory_order_release);
			impl_->compactRuntimeStates.store(std::make_shared<Impl::CompactRuntimeStateRoot>(), std::memory_order_release);
			impl_->persistentReuse.rollbackHistory.ClearAll();
			impl_->frameInputs.frameTick = 0;
			impl_->hadScheduledAnimatorMembership = false;
		}
		impl_->evaluationState.scheduledAnimatorCount.store(0U, std::memory_order_release);
		impl_->evaluationState.plannedAnimatorCount.store(0U, std::memory_order_release);
		impl_->evaluationState.evaluatedAnimatorCount.store(0U, std::memory_order_release);
		impl_->evaluationState.publishedAnimatorCount.store(0U, std::memory_order_release);
		AnimationAttachmentSnapshotPublisher::Instance().Clear();
		AnimationRootMotionResultPublisher::Instance().Clear();
		// Publication generations are public stale-work identities and intentionally
		// remain monotonic across runtime sessions.
	}

	void AnimationSystem::Update(float) {
		// SystemScheduler executes the explicit declarations below. Keeping this
		// compatibility entry empty prevents a direct legacy call from evaluating
		// or publishing animation outside the declared protected boundaries.
	}

	void AnimationSystem::Shutdown() {
		impl_->AssertCoordinatorThread();
		// Authored/runtime mappings re-enter the public invalidation API, so retire
		// them before taking the nonrecursive lifecycle mutex below. This also makes
		// every component-owned handle stale before the runtime session is retired.
		impl_->componentBindings.InvalidateAll(*this);
		bool retiredRuntimeSession = false;
		{
			// Holding the lifecycle guard while the queue is released and every live
			// slot is invalidated makes shutdown atomic with concurrent producers.
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Shutdown);
			const AnimationLifecycleGenerationSnapshot lifecycle =
				impl_->lifecycleLatch ? impl_->lifecycleLatch->Capture() : AnimationLifecycleGenerationSnapshot{};
			impl_->authorityPublisher.Clear(impl_->runtimeSessionEpoch.load(std::memory_order_acquire),
														   lifecycle.Generation(AnimationLifecycleBoundary::Scene));
			impl_->ResetFrameIntegrationStateLocked();
			impl_->persistentReuse.evaluationCache.InvalidateAll();
			impl_->persistentReuse.memoryController.Shutdown();
			impl_->frameInputs.capturedFrameInputs.clear();
			impl_->frameInputs.capturedCommands.clear();
			impl_->frameInputs.qualifiedControllerResourceRevision = 0U;
			impl_->frameInputs.capturedCommandStatuses.clear();
			impl_->frameInputs.capturedProceduralCommands.clear();
			impl_->frameInputs.capturedProceduralCommandStatuses.clear();
			if(impl_->proceduralCommands != nullptr)
				impl_->proceduralCommands->Clear();
			impl_->frameInputs.capturedRetargetRequests.clear();
			impl_->frameInputs.capturedMotionMatchRequests.clear();
			impl_->frameInputs.capturedMotionMatchQueriesV2.clear();
			impl_->frameInputs.capturedRagdollRequests.clear();
			impl_->frameInputs.capturedDeterminismRequests.clear();
			if(impl_->advancedCommands != nullptr)
				impl_->advancedCommands->Clear();
			impl_->pendingRootMotionApplications.clear();
			impl_->frameStorage.frameWork.clear();
			impl_->persistentReuse.candidateFrameWork.clear();
			impl_->persistentReuse.rollbackHistory.ClearAll();
			impl_->hadScheduledAnimatorMembership = false;
			impl_->rootControllerBindings.store({}, std::memory_order_release);
			impl_->ragdollExclusions.store({}, std::memory_order_release);
			if(impl_->runtimeActive.exchange(false, std::memory_order_acq_rel)) {
				// Make dormant membership visible before invalidating the protected
				// store, then retire all handles while producers remain behind the guard.
				impl_->instances->InvalidateAll();
				impl_->commands.reset();
				impl_->runtimeRecords.store(std::make_shared<Impl::RuntimeRecordRoot>(), std::memory_order_release);
				impl_->compactRuntimeStates.store(std::make_shared<Impl::CompactRuntimeStateRoot>(), std::memory_order_release);
				impl_->persistentReuse.clipContentFingerprints.clear();
				impl_->runtimeSessionEpoch.fetch_add(1U, std::memory_order_acq_rel);
				impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
				retiredRuntimeSession = true;
			}
		}
		// Published attachment snapshots borrow the runtime session's animator
		// identity. Clear them only after releasing lifecycle ownership so readers
		// cannot create a lock-order cycle with shutdown.
		AnimationAttachmentSnapshotPublisher::Instance().Clear();
		AnimationRootMotionResultPublisher::Instance().Clear();
		impl_->evaluationState.scheduledAnimatorCount.store(0U, std::memory_order_release);
		impl_->evaluationState.plannedAnimatorCount.store(0U, std::memory_order_release);
		impl_->evaluationState.evaluatedAnimatorCount.store(0U, std::memory_order_release);
		impl_->evaluationState.publishedAnimatorCount.store(0U, std::memory_order_release);
		if(retiredRuntimeSession) {
			// Retire the complete authority identity even when no animator happened
			// to publish in this session. Initialize preserves this monotonic value.
			impl_->authorityPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
		}
	}

	std::string AnimationSystem::SystemName() {
		return "AnimationSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration AnimationSystem::SchedulerDeclaration() {
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = "AnimationSystem";
		declaration.phase = "Animation";
		declaration.after = {"ScriptingSystem"};
		declaration.before = {"PhysicsSystem"};
		declaration.reads = {kResourceAnimatorComponents,
							 kResourceAnimationCommandQueues,
							 "RuntimeTime",
							 "RuntimeLayout",
							 "Resources",
							 kResourceAnimationEvaluationPrograms,
							 kResourceAnimationResidencyCapabilities,
							 kResourceAnimationClipSegments,
							 kResourceAnimationRollbackState,
							 kResourceAnimatedObjectVisibilitySnapshot,
							 kResourceLocomotionAnimatorBindings,
							 kResourceRootMotionResults,
							 kResourceAnimationRagdollMappings,
							 kResourcePhysicsCharacterMotionLeases,
							 "PhysicsWorld",
							 "RagdollPhysicsSnapshots",
							 "RagdollPhysicsResults"};
		declaration.writes = {kResourceAnimationFrameInputs,
							  kResourceAnimationExecutionModes,
							  kResourceAnimationProcessingClasses,
							  kResourceAnimationConsumerMasks,
							  kResourceAnimationWorkPackets,
							  kResourceAnimationVisualPackets,
							  kResourceAnimationGpuVisualFastPackets,
							  kResourceAnimationGpuVisualFastPrepared,
							  kResourceAnimationAuthoritativeOutputs,
							  kResourceAnimationAuthorityPages,
							  kResourceAnimationFrameSidecars,
							  kResourceAnimationAuthorityFrame,
							  kResourceAnimationEvaluationCache,
							  kResourceAnimationRuntimeState,
							  kResourceAnimationPhysicsHandoffQueue,
							  kResourceAnimationManagedCallbackBatches,
							  kResourceRootMotionIntervals,
							  kResourceRootMotionResults,
							  kResourcePhysicsCharacterMotionLeases,
							  kResourceAnimationRagdollMappings,
							  "PhysicsWorld",
							  "RagdollPhysicsQueue",
							  "SceneMutationQueue",
							  "SceneGraph",
							  "ObjectManager"};
		declaration.mainThread = true;
		// AnimatorComponent membership follows the engine's normal structural rule:
		// mutations submitted by scripts flush after scripting and are synchronized
		// into the next frame's frozen workload count before live input capture.
		declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork =
			"Immutable input planning and range-disjoint authoritative evaluation can run across deterministic worker partitions.";
		declaration.protectedCommit =
			"Live animator/component capture and complete generation-validated authority, physics, and copied managed handoff publication.";
		declaration.protectedReason = declaration.protectedCommit;
		declaration.profilingLabel = "Animation";
		declaration.omitWhenNoFrameWorkloads = true;
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> AnimationSystem::DeclareFrameWorkloads(float deltaTime) {
		using namespace TerraEngine::Scheduling;
		impl_->AssertCoordinatorThread();
		const ScriptingSystem::RuntimeTimeSnapshot runtimeTime = ScriptingSystem::Instance().GetRuntimeTimeSnapshot();
		const bool pausedPlayModeWithoutStep =
			runtimeTime.playSessionActive && runtimeTime.playSessionPaused && !runtimeTime.advancesSimulation;
		if(pausedPlayModeWithoutStep) {
			// Play Mode pause owns the simulation clock rather than merely setting a
			// zero delta.  Retaining existing binding membership and publications keeps
			// generation-stamped automation targets valid until an explicit Next step;
			// importantly, no protected capture can consume a queued mutation early.
			// Editor previews remain active because they do not have a running Play
			// session and therefore never take this branch.
			return {};
		}
		if(impl_->runtimeActive.load(std::memory_order_acquire) && impl_->authoredComponentPlaybackEnabled) {
			ObjectManager& objects = ObjectManager::instance();
			const bool hadScheduledMembership = impl_->evaluationState.scheduledAnimatorCount.load(std::memory_order_acquire) != 0U;
			const bool hasAuthoredComponentStorage = objects.HasAnyObjectWithComponentsIncludingInactive<AnimatorComponent>();
			// Declaration runs on the protected coordinator after the previous flush.
			// Synchronize before freezing membership so zero-to-one authored changes
			// can create workloads and every deterministic partition uses one count.
			// The editor disables this authored-component path outside Play Mode;
			// preview-window instances are independent and remain schedulable.
			//
			// A completely static project first probes EnTT's component storage.
			// That constant-time empty check avoids walking every active object each
			// frame. Existing membership still synchronizes once after removal so
			// handles, streaming retains, and published state are retired atomically.
			if(hadScheduledMembership || hasAuthoredComponentStorage) {
				const auto bindingStart = std::chrono::steady_clock::now();
				const AnimatorComponentSynchronizationSummary binding =
					impl_->componentBindings.Synchronize(*this, objects, ResourceManager::Instance());
				const auto bindingNanoseconds =
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - bindingStart).count();
				// Stable frames intentionally return a zero-work summary. Retain the
				// latest real reconciliation instead of replacing it with a dormant poll.
				if(binding.reconciledBindingCount != 0U) {
					impl_->bindingReconciliationNanoseconds.store(static_cast<std::uint64_t>(std::max<std::int64_t>(0, bindingNanoseconds)),
																  std::memory_order_release);
					impl_->bindingReconciledCount.store(binding.reconciledBindingCount, std::memory_order_release);
					impl_->bindingFailureCount.store(binding.failedCount, std::memory_order_release);
					impl_->bindingEpisodeCount.fetch_add(1U, std::memory_order_acq_rel);
				}
			}
		}

		// This is the sole dormant membership check. AnimationPresentation reads
		// the cached count below and never scans AnimatorInstanceStore again.
		const std::size_t animatorCount = impl_->runtimeActive.load(std::memory_order_acquire) ? impl_->instances->ActiveCount() : 0U;
		// Presentation only needs a new release publication when membership changes.
		// A steady static project therefore performs a read instead of rewriting the
		// same activation cache line on every frame.
		if(impl_->evaluationState.scheduledAnimatorCount.load(std::memory_order_relaxed) != animatorCount) {
			impl_->evaluationState.scheduledAnimatorCount.store(animatorCount, std::memory_order_release);
		}
		if(animatorCount == 0U) {
			// The membership query already proves the fixed dormant facts. Profiler's
			// specialized fast path avoids building and validating a full diagnostics
			// snapshot, and remains a no-op when capture is inactive.
			TerraEngine::Profiling::Profiler::Instance().RecordDormantAnimationFacts(1U);
			if(impl_->hadScheduledAnimatorMembership) {
				impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
				const AnimationLifecycleGenerationSnapshot lifecycle =
					impl_->lifecycleLatch ? impl_->lifecycleLatch->Capture() : AnimationLifecycleGenerationSnapshot{};
				// The first dormant frame retires the complete immutable authority root;
				// later static frames retain the allocation-free dormant fast path.
				impl_->authorityPublisher.Clear(impl_->runtimeSessionEpoch.load(std::memory_order_acquire),
															   lifecycle.Generation(AnimationLifecycleBoundary::Scene));
				// Retire the last active global copies once. Steady dormant frames keep
				// the zero-work fast path and never take diagnostics publication locks.
				ClearPublishedAnimationDiagnostics();
				ClearPublishedAnimationDetailedDiagnostics();
				// Captured vectors can retain active-frame storage until the one active to
				// dormant transition. Steady zero-membership frames do no lifecycle work.
				if(impl_->evaluationState.publishedAnimatorCount.exchange(0U, std::memory_order_acq_rel) != 0U) {
					// Retire the last complete active publication so later consumers cannot
					// mistake its generation for a current static-project result.
					impl_->authorityPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
				}
				std::scoped_lock lock(impl_->runtimeOwnershipMutex);
				// Last-membership retirement also releases the copied command batch.
				impl_->PublishCommittedOrdinaryCommandsLocked();
				impl_->SettleFailedFrameCommandsLocked(AnimationStatus::Cancelled, "DormantRetirement");
				impl_->ResetFrameIntegrationStateLocked();
				impl_->frameInputs.capturedFrameInputs.clear();
				impl_->frameInputs.capturedCommands.clear();
				impl_->frameInputs.qualifiedControllerResourceRevision = 0U;
				impl_->frameInputs.capturedCommandStatuses.clear();
				impl_->pendingRootMotionApplications.clear();
				impl_->frameStorage.frameWork.clear();
				// Dormancy retires the reusable candidate as well as the active view so
				// old asset owners do not remain pinned until another animator appears.
				impl_->persistentReuse.candidateFrameWork.clear();
				impl_->hadScheduledAnimatorMembership = false;
			}
			return {};
		}
		impl_->hadScheduledAnimatorMembership = true;
		const std::uint64_t runtimeSessionEpoch = impl_->runtimeSessionEpoch.load(std::memory_order_acquire);
		const std::uint64_t frameDeclarationEpoch = impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel) + 1U;
		AnimatorPlaybackTimeInput timeInput;
		timeInput.scaledDeltaSeconds = impl_->authoredComponentPlaybackAdvancing && std::isfinite(deltaTime) && deltaTime >= 0.0F
										   ? static_cast<double>(deltaTime)
										   : 0.0;
		timeInput.unscaledDeltaSeconds = timeInput.scaledDeltaSeconds;
		timeInput.fixedDeltaTicks = 1;
		const bool useLatchedSimulationDelta = runtimeTime.playSessionActive && runtimeTime.advancesSimulation &&
											   std::isfinite(runtimeTime.simulationDeltaTime) && runtimeTime.simulationDeltaTime >= 0.0F;
		if(useLatchedSimulationDelta) {
			// A paused explicit step must advance animation by the same fixed duration
			// as scripting and physics, not by the editor's render-frame duration.
			timeInput.scaledDeltaSeconds = runtimeTime.simulationDeltaTime;
			timeInput.unscaledDeltaSeconds = runtimeTime.simulationDeltaTime;
		}
		if(runtimeTime.frameCount != 0U && !useLatchedSimulationDelta && std::isfinite(runtimeTime.unscaledDeltaTime) &&
		   runtimeTime.unscaledDeltaTime >= 0.0F) {
			timeInput.unscaledDeltaSeconds = runtimeTime.unscaledDeltaTime;
		}

		std::array<FrameWorkloadDeclaration, 9> descriptors;
		ConfigureProtectedWorkload(
			descriptors[0], "CaptureAnimationInputs", WorkerSystemPhaseKind::Snapshot,
			{kResourceAnimatorComponents, kResourceAnimationCommandQueues, "RuntimeTime", "RuntimeLayout", "Resources", "SceneGraph",
			 "ObjectManager", kResourceAnimatedObjectVisibilitySnapshot, kResourceLocomotionAnimatorBindings,
			 kResourceAnimationRagdollMappings},
			{kResourceAnimationFrameInputs, kResourceAnimationCommandQueues},
			"Live animator membership, component configuration, commands, transforms, and resource views are copied before worker access.");
		ConfigureWorkerWorkload(
			descriptors[1], "PlanAnimationEvaluation",
			{kResourceAnimationFrameInputs, kResourceAnimationEvaluationPrograms, kResourceAnimationResidencyCapabilities},
			{kResourceAnimationExecutionModes, kResourceAnimationProcessingClasses, kResourceAnimationConsumerMasks,
			 kResourceAnimationWorkPackets, kResourceAnimationVisualPackets, kResourceAnimationGpuVisualFastPackets},
			"Reads immutable frame/program/capability views and writes only deterministic owned plan ranges.");
		ConfigureWorkerWorkload(descriptors[2], "FinalizeAnimationWorkPlan",
								{kResourceAnimationExecutionModes, kResourceAnimationProcessingClasses, kResourceAnimationConsumerMasks,
								 kResourceAnimationWorkPackets, kResourceAnimationVisualPackets, kResourceAnimationGpuVisualFastPackets},
								{kResourceAnimationWorkPackets, kResourceAnimationGpuVisualFastPackets},
								"Builds the complete scheduler plan and frozen compact packets after every planning range has completed.");
		// Finalization is a single atomic plan build. It must never be partitioned,
		// because evaluation ranges consume one shared ordering and scratch layout.
		descriptors[2].partitionPolicy = FrameWorkloadPartitionPolicy::Single;
		descriptors[2].partitionWorkItemCount = 0U;
		descriptors[2].inlineWorkItemThreshold = 0U;
		ConfigureWorkerWorkload(
			descriptors[3], "EvaluateCompleteAnimation",
			{kResourceAnimationWorkPackets, kResourceAnimationClipSegments, kResourceAnimationRollbackState},
			{kResourceAnimationAuthoritativeOutputs},
			"Reads immutable complete packets and writes range-disjoint authoritative output without live scene or component mutation.");
		ConfigureWorkerWorkload(
			descriptors[4], "EvaluateCompactGpuVisualAnimation", {kResourceAnimationGpuVisualFastPackets},
			{kResourceAnimationGpuVisualFastPrepared},
			"Reads minimal visual-only packets and writes disjoint compact completions without entering the complete evaluator.");
		ConfigureWorkerWorkload(descriptors[5], "SealAnimationAuthority",
								{kResourceAnimationAuthoritativeOutputs, kResourceAnimationGpuVisualFastPrepared},
								{kResourceAnimationAuthorityPages, kResourceAnimationFrameSidecars},
								"Merges final immutable authority pages and ordered sparse sidecars, reserves every fallible candidate "
								"value, and freezes one root candidate.");
		descriptors[5].partitionPolicy = FrameWorkloadPartitionPolicy::Single;
		descriptors[5].partitionWorkItemCount = 0U;
		descriptors[5].inlineWorkItemThreshold = 0U;
		ConfigureProtectedWorkload(
			descriptors[6], "PublishAnimationAuthority", WorkerSystemPhaseKind::ProtectedCommit,
			{kResourceAnimationAuthorityPages, kResourceAnimationFrameSidecars, kResourceAnimationAuthorityFrame, "Resources"},
			{kResourceAnimationAuthorityFrame},
			"Performs the constant-time session/predecessor check and exchanges one sealed authority root.");
		ConfigureWorkerWorkload(
			descriptors[7], "CommitPublishedAnimationEvaluationCache", {kResourceAnimationAuthorityFrame},
			{kResourceAnimationEvaluationCache},
			"Commits generation-keyed optional cache entries on a worker only after the matching authority root is accepted.");
		descriptors[7].partitionPolicy = FrameWorkloadPartitionPolicy::Single;
		descriptors[7].partitionWorkItemCount = 0U;
		descriptors[7].inlineWorkItemThreshold = 0U;
		ConfigureProtectedWorkload(descriptors[8], "FinalizePublishedAnimationAuthority", WorkerSystemPhaseKind::ProtectedCommit,
								   {kResourceAnimationAuthorityFrame, kResourceAnimationFrameSidecars, "SceneGraph", "ObjectManager",
									kResourceLocomotionAnimatorBindings, kResourceRootMotionResults, kResourceAnimationRagdollMappings,
									kResourcePhysicsCharacterMotionLeases, "PhysicsWorld", "RagdollPhysicsSnapshots",
									"RagdollPhysicsResults"},
								   {kResourceAnimationRuntimeState, kResourceAnimationCommandQueues, kResourceAnimationPhysicsHandoffQueue,
									kResourceAnimationManagedCallbackBatches, kResourceRootMotionIntervals, kResourceRootMotionResults,
									kResourcePhysicsCharacterMotionLeases, kResourceAnimationRagdollMappings, "PhysicsWorld",
									"RagdollPhysicsQueue", "SceneMutationQueue", "SceneGraph", "ObjectManager"},
								   "Applies the already accepted immutable commit token and its sparse specialist batches.");

		descriptors[1].explicitDependencies = {descriptors[0].stableDescriptorId};
		descriptors[2].explicitDependencies = {descriptors[1].stableDescriptorId};
		descriptors[3].explicitDependencies = {descriptors[2].stableDescriptorId};
		descriptors[4].explicitDependencies = {descriptors[2].stableDescriptorId};
		descriptors[5].explicitDependencies = {descriptors[3].stableDescriptorId, descriptors[4].stableDescriptorId};
		descriptors[6].explicitDependencies = {descriptors[5].stableDescriptorId};
		descriptors[7].explicitDependencies = {descriptors[6].stableDescriptorId};
		descriptors[8].explicitDependencies = {descriptors[7].stableDescriptorId};
		for(FrameWorkloadDeclaration& descriptor : descriptors) {
			descriptor.active = animatorCount != 0U;
			descriptor.ready = true;
			descriptor.estimatedWorkItemCount = animatorCount;
		}
		descriptors[2].estimatedWorkItemCount = 1U;
		descriptors[5].estimatedWorkItemCount = 1U;
		descriptors[6].sameFrameVisibilityRequired = animatorCount != 0U;
		descriptors[7].sameFrameVisibilityRequired = animatorCount != 0U;
		descriptors[8].sameFrameVisibilityRequired = animatorCount != 0U;

		descriptors[0].work = [this, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			impl_->CaptureAnimationInputsStage(runtimeSessionEpoch, frameDeclarationEpoch, context);
		};
		descriptors[1].rangeWork = [this, runtimeSessionEpoch, frameDeclarationEpoch, timeInput](const FrameWorkloadPartition& partition,
																								 JobGraphContext& context) {
			impl_->PlanAnimationEvaluationStage(runtimeSessionEpoch, frameDeclarationEpoch, timeInput, partition, context);
		};
		descriptors[2].work = [this, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			impl_->FinalizeAnimationWorkPlanStage(runtimeSessionEpoch, frameDeclarationEpoch, context);
		};
		descriptors[3].rangeWork = [this, runtimeSessionEpoch, frameDeclarationEpoch](const FrameWorkloadPartition& partition,
																					  JobGraphContext& context) {
			impl_->EvaluateCompleteAnimationStage(runtimeSessionEpoch, frameDeclarationEpoch, partition, context);
		};
		descriptors[4].rangeWork = [this, runtimeSessionEpoch, frameDeclarationEpoch](const FrameWorkloadPartition& partition,
																					  JobGraphContext& context) {
			impl_->EvaluateCompactAnimationStage(runtimeSessionEpoch, frameDeclarationEpoch, partition, context);
		};
		descriptors[5].work = [this, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			impl_->SealAnimationAuthorityStage(runtimeSessionEpoch, frameDeclarationEpoch, context);
		};
		descriptors[6].work = [this, animatorCount, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			impl_->PublishAnimationAuthorityStage(animatorCount, runtimeSessionEpoch, frameDeclarationEpoch, context);
		};
		descriptors[7].work = [this, animatorCount, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			impl_->CommitPublishedAnimationCacheStage(animatorCount, runtimeSessionEpoch, frameDeclarationEpoch, context);
		};
		descriptors[8].work = [this, animatorCount, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			impl_->FinalizePublishedAnimationStage(animatorCount, runtimeSessionEpoch, frameDeclarationEpoch, context);
		};
		return {descriptors.begin(), descriptors.end()};
	}
} // namespace TerraEngine::Animation
