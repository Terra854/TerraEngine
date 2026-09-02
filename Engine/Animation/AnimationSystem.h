// AnimationSystem.h - pre-physics animation authority scheduler owner.
//
// The system owns live animator instances and typed command submission. Frame
// work is split into protected capture/publication and worker-safe preparation
// so worker jobs never touch live ObjectManager or component storage.

#pragma once

#include "Animation/Runtime/AnimationCommandQueue.h"
#include "Animation/Runtime/AdvancedAnimationCommandStore.h"
#include "Animation/Runtime/AnimationCancellation.h"
#include "Animation/Runtime/AnimationAuthorityPublication.h"
#include "Animation/Runtime/AnimationEvaluationCache.h"
#include "Animation/Runtime/AnimationFallback.h"
#include "Animation/Runtime/AnimationMemory.h"
#include "Animation/Runtime/AnimationSnapshots.h"
#include "Animation/Runtime/AnimationWorkScheduler.h"
#include "Animation/Runtime/AnimatorPlayback.h"
#include "Animation/Runtime/RootMotion.h"
#include "Animation/Runtime/SharedCopyVector.h"
#include "Animation/Procedural/ProceduralCommandStore.h"
#include "Animation/Gpu/GpuAnimationPlanner.h"
#include "Core/SystemBase.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Animation {
	class AnimationClipView;
	class AnimationEventStream;
	struct MotionDatabaseAsset;
	struct RagdollProfileAsset;
	struct RetargetCompiledProfile;
	struct RetargetProfileAsset;

	// Controller graphs may reference several immutable clips. Each node binding
	// owns its payload and resident-segment bitmap so a worker can change states
	// without resolving ResourceManager storage or touching an editor asset.
	struct AnimatorControllerClipBinding {
		std::uint64_t nodeStableId = 0U;
		std::shared_ptr<const AnimationClipAsset> clip;
		std::vector<std::uint8_t> residentSegments;
		AnimationGeneration clipGeneration = 0U;
		// Computed once at bind from the complete cooked payload. Rollback and
		// replication can then identify clip bytes without rehashing them per frame.
		std::uint64_t deterministicContentFingerprint = 0U;
		// Controller nodes retain their own immutable segment publication because
		// state changes may select a different clip while worker work is in flight.
		std::shared_ptr<const AnimationClipSegmentPayloadSet> streamedSegmentPayloads;
	};

	// These generations originate outside AnimationSystem and therefore cannot be
	// inferred from a clip or pose pointer. Producers opt into unchanged-instance
	// reuse only after supplying every correctness domain with a truthful value.
	struct AnimationEvaluationExternalGenerations {
		std::uint64_t consumerGeneration = 0U;
		std::uint64_t morphGeneration = 0U;
		std::uint64_t graphGeneration = 0U;
		std::uint64_t viewGeneration = 0U;
		std::uint64_t assetGeneration = 0U;
		std::uint64_t residencyGeneration = 0U;
		std::uint64_t materialGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::uint64_t physicsGeneration = 0U;
		std::uint64_t deterministicGeneration = 0U;
		bool unchangedInputReuseEligible = false;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return consumerGeneration != 0U && morphGeneration != 0U && graphGeneration != 0U
				&& viewGeneration != 0U && assetGeneration != 0U
				&& residencyGeneration != 0U && materialGeneration != 0U
				&& rendererGeneration != 0U && physicsGeneration != 0U
				&& deterministicGeneration != 0U;
		}

		auto operator<=>(const AnimationEvaluationExternalGenerations&) const = default;
	};

	struct AnimatorPlaybackBinding {
		// Shared immutable assets keep every worker view alive until the protected
		// publication completes, even if a hot reload replaces the live binding.
		std::shared_ptr<const AnimationClipAsset> clip;
		std::shared_ptr<const SkeletonAsset> skeleton;
		// Advanced authored assets are copied together with their ResourceManager
		// generation. Runtime requests can therefore validate a stable UUID and
		// generation without reparsing files or borrowing cache-owned pointers.
		std::shared_ptr<const RetargetProfileAsset> retargetProfile;
		// The checked mapping table is compiled at the protected bind boundary.
		// Workers may therefore evaluate an active retarget profile without parsing
		// authored mappings or borrowing mutable publisher state each frame.
		std::shared_ptr<const RetargetCompiledProfile> compiledRetargetProfile;
		std::shared_ptr<const MotionDatabaseAsset> motionDatabase;
		std::shared_ptr<const RagdollProfileAsset> ragdollProfile;
		std::shared_ptr<const SkeletonAsset> retargetSourceSkeleton;
		std::vector<AnimatorControllerClipBinding> controllerClips;
		SharedCopyVector<std::uint8_t> residentSegments;
		SharedCopyVector<std::uint64_t> jointStableIds;
		// Source-rig order is retained separately from the target publication layout.
		// A retarget worker samples this exact immutable order, then publishes only
		// target-rig joints through jointStableIds.
		SharedCopyVector<std::uint64_t> retargetSourceJointStableIds;
		SharedCopyVector<std::uint64_t> morphStableIds;
		SharedCopyVector<std::uint64_t> curveStableIds;
		// GPU presentation uses only immutable, binding-owned values.  The vectors
		// below replace borrowed consumer spans and remain valid through worker
		// planning and protected publication.
		std::shared_ptr<const EvaluationProgram> evaluationProgram;
		// An authored controller must never silently degrade into an unrelated
		// default-clip animator. While its worker compile is pending or rejected,
		// the binding carries that explicit status and publishes a safe pose.
		bool controllerProgramRequired = false;
		AnimationStatus controllerProgramStatus = AnimationStatus::Ok;
		AnimationExecutionPolicy requestedExecutionPolicy = AnimationExecutionPolicy::Auto;
		SharedCopyVector<std::uint32_t> authoritativeJointIndices;
		SharedCopyVector<std::int32_t> jointParents;
		AnimationGpuExecutionInputs gpuExecution{};
		// The coordinator builds this one immutable visual binding at protected
		// bind/residency refresh. Compact workers copy only playback/continuity
		// values into packets and share this resident program/payload proof.
		struct GpuFastBinding {
			std::uint32_t programResourceIndex = kGpuAnimationInvalidResourceIndex;
			GpuAnimationVisualInput visual{};
			std::shared_ptr<const GpuAnimationResidentPayloads> residentPayloads;
			// The coordinator computes this once from the immutable resident buffers.
			// Sharing it with the binding prevents a cache hit from re-walking every
			// payload vector merely to restore the scheduler budget declaration.
			std::size_t residentByteCount = 0U;

			[[nodiscard]] bool IsComplete() const noexcept
			{
				return programResourceIndex != kGpuAnimationInvalidResourceIndex
					&& residentPayloads != nullptr && residentByteCount != 0U;
			}
		};
		std::shared_ptr<const GpuFastBinding> gpuFastBinding;
		// Legacy value fields remain copied for complete-path compatibility and are
		// kept equal to gpuFastBinding whenever a compact binding is admitted.
		std::uint32_t gpuProgramResourceIndex = kGpuAnimationInvalidResourceIndex;
		GpuAnimationVisualInput gpuVisual{};
		std::shared_ptr<const GpuAnimationResidentPayloads> gpuResidentPayloads;
		AnimationGeneration clipGeneration = 1U;
		std::uint64_t clipDeterministicContentFingerprint = 0U;
		AnimationGeneration skeletonGeneration = 1U;
		AnimationGeneration programGeneration = 1U;
		AnimationGeneration retargetProfileGeneration = 0U;
		AnimationGeneration motionDatabaseGeneration = 0U;
		AnimationGeneration ragdollProfileGeneration = 0U;
		AnimationGeneration retargetSourceSkeletonGeneration = 0U;
		// Cache participation is deliberately disabled by default. Asset, renderer,
		// view, material, residency, physics, and determinism owners must forward
		// their real publication generations before reuse can be correctness-safe.
		AnimationEvaluationExternalGenerations evaluationGenerations{};
		AnimationFallbackPolicy fallbackPolicy = AnimationFallbackPolicy::CpuThenHoldLast;
		// Missing-resource recovery cannot derive compatibility from a null asset.
		// A producer may carry the last known compatible skeleton/program identity;
		// otherwise hold-last is intentionally unavailable and bind pose is attempted.
		AnimationCompatibilityFingerprint fallbackCompatibilityFingerprint = 0U;
		std::uint64_t stateId = 1U;
		// Authored initial values cross the binding boundary as copied JSON and are
		// parsed once into typed runtime snapshots before worker evaluation begins.
		std::string initialParametersJson = "{}";
		// Visual-only animation should not acquire CPU authority unless the author
		// explicitly enables root-motion extraction or application.
		AnimatorRootMotionPolicy rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
		AnimatorUpdatePolicy updatePolicy = AnimatorUpdatePolicy::EveryFrame;
		AnimatorEventPolicy eventPolicy = AnimatorEventPolicy::Dispatch;
		AnimatorCullingPolicy cullingPolicy = AnimatorCullingPolicy::Automatic;
		AnimatorPlaybackConfiguration playback{};
		// This copied authored cursor is applied immediately after Configure and
		// before optional autoplay. It therefore selects both the frozen editor pose
		// and the first runtime pose without placing mutable cursor state in a component.
		AnimationFixedTick initialPlaybackTick = 0;
		// Component-authored animators are expected to evaluate their default clip or
		// controller entry state immediately. Direct runtime bindings remain stopped
		// unless their caller explicitly opts in, preserving command-driven control.
		bool playOnBind = false;
		// This snapshot pins the exact segment bytes represented by residentSegments.
		// Null preserves legacy/full-resident callers that decode clip->streamData.
		std::shared_ptr<const AnimationClipSegmentPayloadSet> streamedSegmentPayloads;
	};

	// The coordinator owns every pointer for the duration of one protected batch.
	// Grouping identical direct-clip promotions avoids reacquiring runtime/cache
	// locks once per prefab instance while preserving an individual result status.
	struct AnimatorDirectClipStreamingRefresh {
		AnimatorInstanceHandle handle;
		const AnimatorPlaybackBinding* streamedBinding = nullptr;
		AnimationClipView* sharedValidatedClipView = nullptr;
		// The authored UUID lets a previously unavailable runtime record accept only
		// the exact direct clip that the coordinator validated for this component.
		UUID expectedClipAssetId = EMPTY_UUID;
		// A ResourceManager hot publication of the same authored direct clip may
		// replace its immutable generation without rebuilding unrelated mesh,
		// skeleton, parameter, and policy arrays for every prefab instance.
		// The protected batch may repair a lagging runtime record when the same
		// direct-clip asset has already advanced to a newer immutable generation.
		bool allowClipPublicationReplacement = false;
		bool playOnBind = false;
	};

	// The component coordinator owns each binding pointer for the duration of one
	// protected batch. Direct-clip prefab crowds share an immutable prepared
	// template while retaining independent runtime clocks and publication state.
	struct AnimatorPlaybackBindRequest {
		AnimatorInstanceHandle handle;
		const AnimatorPlaybackBinding* binding = nullptr;
		// The coordinator validates one direct-clip publication per shared authored
		// signature. Each runtime record adopts that checked view state instead of
		// reopening identical codec and skeleton tables thousands of times.
		const AnimationClipView* sharedValidatedClipView = nullptr;
		bool playOnBind = false;
	};

	// A rejected frame remains atomic, but diagnostics still need to explain which
	// positive proof failed. The values are deliberately coarse and allocation-free
	// so production captures can expose the first failed compact record cheaply.
	enum class AnimationPlanRejectionReason : std::uint8_t {
		None = 0U,
		PlanningIncomplete,
		PlaybackAdvance,
		GenerationOverflow,
		ExecutionModeNotGpuVisual,
		FrameIdentityInvalid,
		GpuLoweringRejected,
		ResidentPayloadInvalid,
		CompactClassificationRejected,
		CompactInputIncomplete,
		SchedulerRejected,
		MemoryReservationRejected,
		AllocationFailure
	};

	[[nodiscard]] TERRAENGINE_API std::string_view ToString(
		AnimationPlanRejectionReason reason) noexcept;

	// This copied diagnostic snapshot exposes the selected task granularity and
	// bounded-memory/cancellation outcome without exposing mutable runtime storage.
	struct AnimationProductionIntegrationSnapshot {
		AnimationStatus planStatus = AnimationStatus::NotReady;
		AnimationPlanRejectionReason rejectionReason = AnimationPlanRejectionReason::None;
		std::size_t rejectionItemIndex = std::numeric_limits<std::size_t>::max();
		AnimationWorkExecutionPolicy executionPolicy = AnimationWorkExecutionPolicy::Omitted;
		std::size_t availableWorkerCount = 0U;
		std::size_t selectedWorkerCount = 0U;
		std::size_t itemCount = 0U;
		std::size_t rangeCount = 0U;
		std::size_t scratchBytes = 0U;
		std::size_t cacheEntryCount = 0U;
		std::size_t reusedItemCount = 0U;
		std::size_t fallbackItemCount = 0U;
		// Authored-component reconciliation is a distinct protected preparation
		// episode. Publishing its latest completed non-empty episode lets editor
		// diagnostics distinguish real binding work from dormant frame timing.
		std::uint64_t bindingEpisodeCount = 0U;
		std::uint64_t bindingReconciliationNanoseconds = 0U;
		std::size_t bindingReconciledCount = 0U;
		std::size_t bindingFailureCount = 0U;
		AnimationMemorySnapshot memory{};
		bool deterministicPublicationValidated = false;
		bool cancellationRequested = false;
	};

	class TERRAENGINE_API AnimationSystem final : public SystemsBase {
	public:
		static AnimationSystem& Instance();

		AnimationSystem(const AnimationSystem&) = delete;
		AnimationSystem& operator=(const AnimationSystem&) = delete;

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		// These protected-lifecycle wrappers keep AnimatorInstanceStore private to
		// its owning system. Component, scene, and future managed integrations can
		// request lifecycle changes without receiving mutable store access.
		[[nodiscard]] AnimatorInstanceHandle CreateAnimatorInstance(AnimatorInstanceDescriptor descriptor);
		[[nodiscard]] bool InvalidateAnimatorInstance(AnimatorInstanceHandle handle);
		// Scene reconciliation commonly retires thousands of component-owned handles
		// together. The batch path copies runtime ownership once, advances lifecycle
		// identity once, and retires the complete set as one protected transaction.
		[[nodiscard]] std::size_t InvalidateAnimatorInstances(
			std::span<const AnimatorInstanceHandle> handles);
		// Standalone runtimes enable authored playback by default. The editor
		// disables it outside Play Mode so a stopped scene has no component-owned
		// runtime palette and renders the mesh's authored bind geometry.
		void SetAuthoredComponentPlaybackEnabled(bool enabled);
		[[nodiscard]] bool IsAuthoredComponentPlaybackEnabled() const noexcept;
		// Engine startup sets this before scheduler initialization for runtimes
		// that intentionally omit RenderSystem. The copied value becomes an
		// explicit execution-mode input, so a renderer-free process cannot retain
		// a stale GPU/Hybrid label from asset capability metadata.
		void SetHeadlessExecutionRequired(bool required);
		[[nodiscard]] bool IsHeadlessExecutionRequired() const noexcept;
		// Edit Mode keeps a tick-zero pose published for skinned imports whose
		// authored bind geometry requires a skeleton palette, while Play Mode
		// advances the same component-owned playback normally.
		void SetAuthoredComponentPlaybackAdvancing(bool advancing);
		[[nodiscard]] bool IsAuthoredComponentPlaybackAdvancing() const noexcept;
		// Scene, script, renderer, physics, device, and Play Mode owners call this
		// only from their protected lifecycle boundary. It invalidates in-flight
		// tokens and retires authority, sidecar, and pending-frame roots so fresh
		// consumers cannot acquire a result from the previous lifecycle identity.
		[[nodiscard]] AnimationStatus NotifyLifecycleInvalidation(
			AnimationLifecycleBoundary boundary);
		[[nodiscard]] bool IsAnimatorInstanceAlive(AnimatorInstanceHandle handle) const;
		// Binding synchronization uses this revision to validate its retained handle
		// set without reopening the private instance store for every crowd member.
		[[nodiscard]] std::uint64_t AnimatorInstanceMembershipRevision() const noexcept;
		// Binding copies immutable ownership and validates the dependency generation
		// before any worker sees it. Missing or incompatible clip data is retained as
		// an explicit safe-pose status rather than partially mixing generations.
		[[nodiscard]] AnimationStatus BindAnimatorPlayback(
			AnimatorInstanceHandle handle,
			AnimatorPlaybackBinding binding);
		// Bind a crowd under one runtime-ownership lock and one global lifecycle
		// invalidation. Each record is still validated and updated independently;
		// statuses preserve the request order and failures cannot alias another owner.
		void BindAnimatorPlaybackBatch(
			std::span<const AnimatorPlaybackBindRequest> requests,
			std::span<AnimationStatus> statuses);
		// Promote newly resident segment bytes for an unchanged direct-clip binding.
		// This preserves playback, graph, parameter, and publication continuity while
		// replacing only the immutable stream/GPU resource views.
		[[nodiscard]] AnimationStatus RefreshAnimatorDirectClipStreaming(
			AnimatorInstanceHandle handle,
			const AnimatorPlaybackBinding& streamedBinding,
			AnimationClipView* sharedValidatedClipView = nullptr);
		void RefreshAnimatorDirectClipStreamingBatch(
			std::span<const AnimatorDirectClipStreamingRefresh> refreshes,
			std::span<AnimationStatus> statuses);
		// Dynamic view/residency/material/renderer/physics owners refresh their
		// immutable generations through this protected narrow API. A changed bundle
		// cancels the captured frame and invalidates only this animator's cache.
		[[nodiscard]] AnimationStatus UpdateEvaluationGenerations(
			AnimatorInstanceHandle handle,
			AnimationEvaluationExternalGenerations generations);
		[[nodiscard]] AnimationCommandReceipt SubmitCommand(AnimationCommand command);
		// Automation and other cross-system producers use this atomic batch path so
		// a rejected later command cannot leave earlier parameter edits queued.
		[[nodiscard]] AnimationCommandBatchReceipt SubmitCommandBatch(
			std::span<const AnimationCommand> commands);
		[[nodiscard]] AnimationCommandReceipt CancelCommand(
			std::uint64_t cancellationToken,
			std::uint64_t correlationId);
		[[nodiscard]] AnimationCommandReceipt ReadCommandReceipt(
			std::uint32_t producerId,
			std::uint64_t producerSequence,
			std::uint64_t correlationId) const;
		// Gameplay requests carry only copied identities. The system resolves the
		// latest exact immutable root delta, then routes typed deferred work without
		// exposing PoseBuffer or live component references.
		[[nodiscard]] AnimationRootMotionApplicationReceipt SubmitRootMotionApplication(
			AnimationRootMotionDeferredApplicationRequest request);
		// Procedural gameplay input remains a copied, typed deferred path. The
		// protected animation boundary validates publication generations before a
		// goal becomes active, and copied result reads never expose evaluator storage.
		[[nodiscard]] NativeAnimationProceduralGoalReceipt SubmitProceduralGoal(
			const NativeAnimationProceduralGoalCommand& command);
		[[nodiscard]] AnimationStatus CopyProceduralResults(
			AnimatorInstanceHandle handle,
			std::span<NativeAnimationProceduralResultSnapshot> destination,
			std::uint32_t& requiredCount) const;
		// Scene View and native diagnostics use the same bounded copy protocol as
		// managed callers. Active goals remain store-owned and are never exposed by
		// reference while an editor handle is being dragged.
		[[nodiscard]] AnimationStatus CopyProceduralGoals(
			AnimatorInstanceHandle handle,
			std::span<NativeAnimationProceduralGoalCommand> destination,
			std::uint32_t& requiredCount) const;
		// Advanced managed requests share the same bounded deferred ownership as
		// ordinary commands. Receipts report queue admission immediately; copied
		// snapshots become visible only after the protected animation/physics
		// publication boundaries have processed the request.
		[[nodiscard]] NativeAnimationRetargetReceipt SubmitRetargetRequest(
			const NativeAnimationRetargetRequest& request);
		[[nodiscard]] NativeAnimationMotionMatchReceipt SubmitMotionMatchRequest(
			const NativeAnimationMotionMatchRequest& request);
		// V2 carries the complete deterministic search query inline. The runtime
		// retains that copied value until protected capture, so callers never lend
		// feature or exclusion storage to animation workers.
		[[nodiscard]] NativeAnimationMotionMatchQueryReceiptV2 SubmitMotionMatchQueryV2(
			const NativeAnimationMotionMatchQueryRequestV2& request);
		[[nodiscard]] NativeAnimationRagdollReceipt SubmitRagdollRequest(
			const NativeAnimationRagdollRequest& request);
		[[nodiscard]] NativeAnimationDeterminismReceipt SubmitDeterminismRequest(
			const NativeAnimationDeterminismRequest& request);
		[[nodiscard]] AnimationStatus CopyRetargetSnapshots(
			AnimatorInstanceHandle handle,
			std::span<NativeAnimationRetargetSnapshot> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimationStatus CopyMotionMatchSnapshots(
			AnimatorInstanceHandle handle,
			std::span<NativeAnimationMotionMatchSnapshot> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimationStatus CopyMotionMatchResultsV2(
			AnimatorInstanceHandle handle,
			std::span<NativeAnimationMotionMatchResultV2> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimationStatus CopyRagdollSnapshots(
			AnimatorInstanceHandle handle,
			std::span<NativeAnimationRagdollSnapshot> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimationStatus CopyDeterminismSnapshots(
			AnimatorInstanceHandle handle,
			std::span<NativeAnimationDeterminismSnapshot> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimatorInstanceHandle ResolveAnimatorForObject(
			const UUID& ownerObjectId) const noexcept;
		[[nodiscard]] std::shared_ptr<const AnimationSnapshot> LoadAnimatorSnapshot(
			AnimatorInstanceHandle handle) const noexcept;
		[[nodiscard]] AnimatorPlaybackStateSnapshot LoadAnimatorPlaybackState(
			AnimatorInstanceHandle handle) const noexcept;
		[[nodiscard]] std::size_t PendingCommandCount() const;
		[[nodiscard]] std::size_t CapturedCommandCount() const;

		// AnimationPresentation reads the cached count from the same coordinator
		// membership check that activated this frame. It must not scan the runtime
		// store a second time on a dormant frame.
		[[nodiscard]] std::size_t ScheduledAnimatorCount() const noexcept;
		[[nodiscard]] std::size_t PublishedAnimatorCount() const noexcept;
		[[nodiscard]] std::uint64_t AuthorityPublicationGeneration() const noexcept;
		// Gameplay and scripting read one immutable pre-physics authority root.
		// This replaces per-record pose loads and independent generation assembly.
		[[nodiscard]] std::shared_ptr<const AnimationAuthorityFrame>
			LoadPublishedAuthorityFrame() const noexcept;
		[[nodiscard]] AnimationProductionIntegrationSnapshot
			ReadProductionIntegrationSnapshot() const noexcept;

	private:
		AnimationSystem();
		~AnimationSystem() override;
		// A prefab cohort shares immutable authored defaults. Batch binding parses
		// and validates those defaults once, then copies only the small mutable
		// per-instance state into each protected runtime record.
		struct ImmutableBindPreparation {
			std::vector<AnimationParameterSnapshot> initialParameters;
			std::vector<float> initialLayerWeights;
		};
		[[nodiscard]] AnimationStatus BindAnimatorPlaybackLocked(
			AnimatorInstanceHandle handle,
			AnimatorPlaybackBinding binding,
			const AnimationClipView* sharedValidatedClipView,
			ImmutableBindPreparation* sharedPreparation,
			bool immutableBindingValidated,
			bool deferEvaluationCacheInvalidation,
			bool publishGlobalInvalidation,
			bool& outMutated);
		[[nodiscard]] AnimationStatus RefreshAnimatorDirectClipStreamingLocked(
			AnimatorInstanceHandle handle,
			const AnimatorPlaybackBinding& streamedBinding,
			AnimationClipView* sharedValidatedClipView,
			UUID expectedClipAssetId,
			bool allowClipPublicationReplacement,
			bool playOnBind,
			std::uint64_t replacementClipFingerprint,
			const AnimationEventStream* preparedReplacementEventStream,
			const AnimatorPlayback* preparedReplacementPlayback,
			const AnimatorPlaybackConfiguration* preparedReplacementConfiguration,
			bool& outChanged,
			bool& outReplacedClipPublication);

		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
