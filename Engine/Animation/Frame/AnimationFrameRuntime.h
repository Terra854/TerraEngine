// Private runtime types and domain owners shared by the animation stage modules.

#pragma once

#include "Animation/AnimationSystem.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Animation/Determinism/AnimationHistory.h"
#include "Animation/Determinism/AnimationReplay.h"
#include "Animation/Evaluation/CpuClipEvaluator.h"
#include "Animation/Evaluation/PoseBlend.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"
#include "Animation/Ragdoll/RagdollActivation.h"
#include "Animation/Ragdoll/RagdollRecovery.h"
#include "Animation/Ragdoll/RagdollState.h"
#include "Animation/Graph/StateMachineEvaluator.h"
#include "Animation/Graph/ProceduralNodeEvaluator.h"
#include "Animation/Runtime/AnimationEventStream.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"
#include "Animation/Runtime/AnimationAttachments.h"
#include "Animation/Runtime/AnimatorComponentBindingCoordinator.h"
#include "Animation/Runtime/RootMotion.h"
#include "Animation/Ragdoll/RagdollBlend.h"
#include "Animation/Retargeting/RetargetEvaluator.h"
#include "Animation/Retargeting/RetargetValidator.h"
#include "Animation/MotionMatching/MotionMatchCpu.h"
#include "Components/AnimatorComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "Physics/PhysicsSystem.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptingSystem.h"
#include "ThreadPool/ThreadPool.h"
#include "ThreadPool/ScopedTaskSettlement.h"
#include "Core/Logger/Logger.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

// Private animation runtime declarations shared only by compiled stage modules.
namespace TerraEngine::Animation {
	namespace FrameDetail {
		constexpr std::size_t kInlineAnimatorThreshold = 16U;
		// A 128-animator partition keeps a large crowd at roughly one
		// planning/evaluation job per worker on the production 32-worker pool. The
		// former 64-item grain created two dispatch waves and paid queue, profiler,
		// and dependency bookkeeping twice without exposing more useful parallelism.
		constexpr std::size_t kAnimatorsPerWorkerPartition = 128U;
		constexpr std::size_t kMaximumPendingRootMotionApplications = 4096U;
		constexpr std::size_t kMaximumAnimationScratchBytes = 64U * 1024U * 1024U;
		constexpr std::size_t kMaximumAnimationPoseBytes = 64U * 1024U * 1024U;
		constexpr std::size_t kMaximumAnimationMemoryBytes = 128U * 1024U * 1024U;
		constexpr std::size_t kMaximumAnimationMemoryReservations = 4096U;
		constexpr std::uint32_t kMotionMatchFeatureCountV1 = 48U;
		// A scheduled item owns exactly one of these records in the frame scratch
		// allocation. Planning and evaluation phases transfer only copied terminal
		// state through the record; no worker receives another item's byte interval.
		struct AnimationFrameScratchRecord {
			std::uint64_t sourceGeneration = 0U;
			std::size_t scheduledItemIndex = 0U;
			AnimationStatus evaluationStatus = AnimationStatus::NotReady;
			bool evaluated = false;
		};
		AnimationMemoryConfiguration MakeAnimationMemoryConfiguration() noexcept;
		NativeAnimationAbiHeader MakeNativeHeader(NativeAnimationRecordKind kind, std::size_t size) noexcept;
		NativeAnimationStatus ToNativeStatus(AnimationStatus status) noexcept;
		NativeAnimationUuid ToNativeUuid(const UUID& value) noexcept;
		bool MatchesNativeUuid(NativeAnimationUuid value, const UUID& expected) noexcept;
		AnimationTransform FromNativeTransform(const NativeAnimationTransform& value) noexcept;
		NativeAnimationTransform ToNativeTransform(const AnimationTransform& value) noexcept;
		std::uint64_t MixDeterministicChecksum(std::uint64_t checksum, std::uint64_t value) noexcept;
		std::uint64_t QuantizedFloat(float value) noexcept;
		std::uint64_t HashDeterministicTransform(std::uint64_t checksum, const AnimationTransform& transform) noexcept;
		std::uint64_t HashAnimationGuid(std::uint64_t checksum, const AnimationGuid& value) noexcept;
		AnimationStatus ComputeClipDeterministicContentFingerprint(const AnimationClipAsset& clip, std::uint64_t& output) noexcept;
		AnimationWorkSchedulerConfiguration MakeAnimationWorkSchedulerConfiguration() noexcept;
		AnimationFallbackReason FallbackReasonForStatus(AnimationStatus status) noexcept;
		void ConfigureProtectedWorkload(TerraEngine::Scheduling::FrameWorkloadDeclaration& workload, std::string name,
										TerraEngine::Scheduling::WorkerSystemPhaseKind phaseKind, std::vector<std::string> reads,
										std::vector<std::string> writes, std::string protectedReason);
		void ConfigureWorkerWorkload(TerraEngine::Scheduling::FrameWorkloadDeclaration& workload, std::string name,
									 std::vector<std::string> reads, std::vector<std::string> writes, std::string safeSubwork);
		AnimationStatus ApplyPlaybackCommand(AnimatorPlayback& playback, const AnimationCommand& command) noexcept;
		AnimationStatus ApplyParameterCommand(std::vector<AnimationParameterSnapshot>& parameters, const AnimationCommand& command);
		std::uint32_t StableParameterId(std::string_view name) noexcept;
		AnimationStatus ParseInitialParameters(std::string_view payload, std::vector<AnimationParameterSnapshot>& output);
		AnimationQuaternion MultiplyRotation(const AnimationQuaternion& left, const AnimationQuaternion& right) noexcept;
		AnimationQuaternion ConjugateRotation(AnimationQuaternion value) noexcept;
		AnimationFloat3 RotateVector(const AnimationQuaternion& rotation, const AnimationFloat3& vector) noexcept;
		AnimationTransform CombineTransform(const AnimationTransform& parent, const AnimationTransform& local) noexcept;
		bool TryMapRagdollBodyWorldToJointModel(const AnimationTransform& objectWorld, const AnimationTransform& animationJointModel,
												const RagdollBodyRecord& body, const AnimationTransform& bodyWorld,
												AnimationTransform& output) noexcept;
		bool CaptureObjectWorldTransform(ObjectManager& objects, Object object, AnimationTransform& output) noexcept;
		AnimationPlaybackSnapshot MakePublishedPlaybackSnapshot(const AnimatorPlaybackStateSnapshot& source) noexcept;
	} // namespace FrameDetail
	using namespace FrameDetail;
	struct AnimationSystem::Impl {
		enum class WorkPlanFinalizeState : std::uint8_t { Pending, Building, Ready, Failed };
		// A controller transition must advance its destination clip independently
		// while the ordinary playback cursor continues sampling the source clip.
		// This copied worker-owned state is committed with the graph snapshot, so a
		// script crossfade cannot expose a new state with an old or reset clip clock.
		struct ControllerTransitionRuntime {
			std::uint64_t transitionStableId = 0U;
			std::uint64_t sourceStateId = 0U;
			std::uint64_t destinationStateId = 0U;
			AnimatorControllerClipBinding destination;
			AnimatorPlayback destinationPlayback;
			AnimationClipView destinationClipView;
			AnimationEventStream destinationEventStream;
			AnimatorPlaybackAdvanceResult destinationAdvance;
			// Replacing an active transition captures the last complete blended pose.
			// A shared immutable copy avoids duplicating a full skeleton on every
			// worker capture while keeping interruption progress visually continuous.
			std::shared_ptr<const std::vector<AnimationTransform>> capturedSourcePose;
			std::optional<AnimationRootMotionDelta> capturedSourceRootMotion;
			bool ready = false;
		};
		struct RuntimeRecord {
			AnimatorInstanceHandle handle;
			AnimatorInstanceDescriptor descriptor;
			AnimatorPlaybackBinding binding;
			// A newly allocated runtime slot has no published pose, attachment, rollback,
			// or advanced-command state to retire. Remember the first successful bind so
			// crowd creation can avoid thousands of empty cross-subsystem retire calls.
			bool bindingInitialized = false;
			AnimatorPlayback playback;
			AnimationClipView clipView;
			AnimationEventStream eventStream;
			// Direct clips without authored events never mutate delivery history.
			// Keeping this explicit lets protected capture and publication skip the
			// event stream's deque/set copy while preserving the live record unchanged.
			bool eventStateCaptured = false;
			AnimationStatus dependencyStatus = AnimationStatus::MissingAsset;
			AnimationGeneration poseGeneration = 0U;
			std::uint64_t stateGeneration = 1U;
			std::uint64_t parameterGeneration = 1U;
			std::uint64_t executionModeGeneration = 1U;
			AnimationGeneration contentGeneration = 1U;
			// A history generation names one compatible rollback ring. Rebind and
			// lifecycle invalidation advance it before any replacement snapshot is kept.
			AnimationGeneration historyGeneration = 1U;
			// Ordinary visual animation does not need a deep deterministic snapshot on
			// every frame. The first valid rollback/replication request opts this
			// animator into continuous bounded retention until its runtime record dies.
			bool rollbackHistoryActive = false;
			AnimationExecutionModeState executionModeState{};
			std::vector<AnimationParameterSnapshot> parameters;
			// Runtime layer weights start from immutable authored defaults, then change
			// only through copied commands at the protected animation publication.
			std::vector<float> layerWeights;
			// Controller state advances on worker-owned copies and is committed only
			// with the same all-or-none authority publication as the sampled pose.
			StateMachineSnapshot graphState;
			ControllerTransitionRuntime transition;
			// Published pose state lives exclusively in AnimationAuthorityFrame.  Runtime
			// records retain mutable evaluator state only; workers capture prior rows from
			// the immutable root held for the whole frame.
			// Advanced control state changes only at PublishAnimationAuthority. These
			// copied identities let later frames validate managed requests without
			// allowing scripting workers to mutate evaluator or physics ownership.
			NativeAnimationUuid activeRetargetProfileId{};
			AnimationGeneration activeRetargetProfileGeneration = 0U;
			RetargetEvaluationMetrics latestRetargetMetrics{};
			std::uint64_t latestRetargetDeterministicHash = 0U;
			NativeAnimationUuid activeMotionDatabaseId{};
			AnimationGeneration activeMotionDatabaseGeneration = 0U;
			std::uint64_t latestMotionQueryId = 0U;
			std::uint64_t appliedMotionQueryId = 0U;
			MotionMatchCandidate latestMotionCandidate{};
			bool hasMotionCandidate = false;
			std::uint64_t ragdollId = 0U;
			RagdollGenerationStamp ragdollGeneration{};
			NativeAnimationUuid activeRagdollProfileId{};
			bool ragdollCreated = false;
			RagdollLifecycleState ragdollLifecycle = RagdollLifecycleState::Inactive;
			float ragdollBlendWeight = 0.0F;
			AnimationFixedTick ragdollLifecycleStartTick = 0;
			std::uint64_t ragdollRecoveryCandidateId = 0U;
			RagdollRecoveryBlockReason ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
			AnimationFloat3 ragdollRecoveryAlignedRootPosition{};
			float ragdollRecoveryDurationSeconds = 0.0F;
			std::uint32_t ragdollMotorDriveCount = 0U;
			AnimationStatus ragdollMotorStatus = AnimationStatus::NotReady;
			std::uint64_t pendingRagdollCorrelationId = 0U;
			RagdollGenerationStamp pendingRagdollPreviousGeneration{};
			RagdollLifecycleState pendingRagdollPreviousLifecycle = RagdollLifecycleState::Inactive;
			NativeAnimationUuid pendingRagdollPreviousProfileId{};
			float pendingRagdollPreviousBlendWeight = 0.0F;
			NativeAnimationRagdollOperation pendingRagdollOperation = NativeAnimationRagdollOperation::Deactivate;
			// Physics retains the completed recovery ticket after the ragdoll body
			// is deactivated. An unsuccessful character handoff can be retried at
			// later protected boundaries without reviving stale root motion.
			std::optional<Physics::RagdollExternalRecoveryToken> pendingExternalRecoveryToken;
			bool pendingRagdollExternalRecovery = false;
			bool pendingExternalRecoveryNeedsRefresh = false;
			// A rejected Activate does not implicitly return an already-granted
			// physical ExternalExclusive lease to locomotion. A later explicit
			// abort/recovery request must name this terminal failure.
			std::uint64_t failedRagdollActivateCorrelationId = 0U;
			RagdollGenerationStamp failedRagdollActivateGeneration{};
			std::uint64_t failedRagdollActivatePublicationGeneration = 0U;
			AnimationFixedTick failedRagdollActivateFrameTick = 0;
		};
		// Runtime configuration remains protected while it is authored, but frame
		// finalization owns a retained immutable root.  Replacing this root is one
		// atomic operation, so a worker can prepare a complete next crowd state
		// without exposing partial per-record updates.
		// Runtime records change much less frequently than compact GPU-visual state.
		// Keep them in copy-on-write pages so a mixed crowd with a handful of CPU
		// exceptions clones only the pages containing those exceptions. The previous
		// flat vector copied every shared_ptr in a 4,000-owner crowd before changing
		// seven rows, even though protected publication ultimately swaps one root.
		class RuntimeRecordRoot {
		  public:
			using Record = std::shared_ptr<RuntimeRecord>;
			static constexpr std::size_t kPageCapacity = 128U;

			[[nodiscard]] std::size_t size() const noexcept {
				return size_;
			}

			[[nodiscard]] const Record& at(std::size_t index) const {
				if(index >= size_)
					throw std::out_of_range("runtime record slot");
				return pages_.at(index / kPageCapacity)->records[index % kPageCapacity];
			}

			void resize(std::size_t size) {
				const std::size_t requiredPages = (size + kPageCapacity - 1U) / kPageCapacity;
				pages_.reserve(requiredPages);
				while(pages_.size() < requiredPages) {
					pages_.push_back(std::make_shared<Page>());
				}
				size_ = size;
			}

			void Set(std::size_t index, Record record) {
				if(index >= size_)
					throw std::out_of_range("runtime record slot");
				const std::size_t pageIndex = index / kPageCapacity;
				// A copied root initially shares every immutable page. Clone the one page
				// before replacing a slot so rejected candidates cannot alter the live root.
				if(pages_[pageIndex].use_count() != 1U) {
					pages_[pageIndex] = std::make_shared<Page>(*pages_[pageIndex]);
				}
				pages_[pageIndex]->records[index % kPageCapacity] = std::move(record);
			}

			template <typename Visitor> void ForEach(Visitor&& visitor) const {
				for(std::size_t index = 0U; index < size_; ++index) {
					visitor(at(index));
				}
			}

		  private:
			struct Page {
				std::array<Record, kPageCapacity> records{};
			};

			std::vector<std::shared_ptr<Page>> pages_;
			std::size_t size_ = 0U;
		};
		// Visual-only animators advance a small scalar state while their binding and
		// specialist configuration stay immutable. Publishing this dense root avoids
		// writing through thousands of scattered RuntimeRecord allocations after the
		// authority root is visible. A complete-path transition materializes only its
		// own row before modifying the broad runtime record.
		struct CompactRuntimeState {
			AnimatorInstanceHandle handle{};
			AnimatorPlayback playback{};
			AnimationGeneration poseGeneration = 0U;
			std::uint64_t stateGeneration = 0U;
			std::uint64_t executionModeGeneration = 0U;
			// This generation identifies the immutable binding whose clip, program,
			// root-motion policy, and resource layout passed compact admission. A warm
			// frame may reuse that proof only while the live runtime record still names
			// the exact same binding generation; dynamic GPU health and mode are always
			// checked again before the owner bypasses complete evaluation.
			AnimationGeneration bindingContentGeneration = 0U;
			AnimationExecutionModeState executionModeState{};

			[[nodiscard]] bool IsValidFor(AnimatorInstanceHandle expected) const noexcept {
				return handle == expected && poseGeneration != 0U && stateGeneration != 0U && executionModeGeneration != 0U;
			}
		};
		using CompactRuntimeStateRoot = std::vector<CompactRuntimeState>;
		struct DeterminismRequestCompletion {
			NativeAnimationDeterminismRequest request{};
			AnimationStatus status = AnimationStatus::NotReady;
			AnimationRollbackSnapshot sourceSnapshot{};
			bool stateRestored = false;
		};
		struct MotionMatchQueryCompletionV2 {
			// correlationId is retained only inside the runtime. The public result
			// record stays ABI-frozen while publication can still pair duplicate query
			// identifiers with the exact copied request that produced this result.
			std::uint64_t correlationId = 0U;
			NativeAnimationMotionMatchResultV2 result{};
			MotionMatchCandidate selectedCandidate{};
			bool hasSelectedCandidate = false;
		};
		// Each vector element is owned by one deterministic worker index. Planning
		// and evaluation never append to shared storage, so scheduler partitions are
		// disjoint even when many jobs complete out of order.
		struct FrameWorkItem {
			// Original capture position remains stable after complete records are packed
			// around the separate compact SoA column for deterministic publication.
			std::size_t capturedInputIndex = 0U;
			AnimationFrameInput input;
			// Captured from Locomotion's immutable sorted binding table. Worker
			// evaluation never calls back into the live authority coordinator.
			bool rootMotionEnabled = false;
			// Compact captures keep this authority slot only for deterministic commit
			// order. Their broad evaluator state below is never populated; the tagged
			// payload owns the only data a visual-only GPU packet may observe.
			struct CompactCapture {
				std::shared_ptr<const EvaluationProgram> program;
				std::shared_ptr<const AnimatorPlaybackBinding::GpuFastBinding> fastBinding;
				AnimationGpuExecutionInputs gpuExecution{};
				AnimationExecutionPolicy requestedPolicy = AnimationExecutionPolicy::Auto;
				AnimatorUpdatePolicy updatePolicy = AnimatorUpdatePolicy::EveryFrame;
				AnimationGeneration programGeneration = 0U;
				AnimationGeneration clipGeneration = 0U;
				AnimationGeneration skeletonGeneration = 0U;
				std::uint32_t jointCount = 0U;
			} compactCapture{};
			bool compactCaptureOnly = false;
			AnimatorPlaybackBinding binding;
			// The two frame-work arrays alternate ownership. Remember which immutable
			// binding generation each slot already contains so an unchanged animator
			// does not deep-copy controller clips, joint ids, morph ids, and resident
			// segment tables again when this slot returns two frames later.
			AnimatorInstanceHandle bindingCaptureHandle{};
			AnimationGeneration bindingCaptureContentGeneration = 0U;
			// These hashes cover only immutable binding data. Alternating frame slots
			// retain them with the deep-copied binding, while per-frame consumer and
			// retarget bits are appended during finalization.
			std::uint64_t bindingOutputRequirementsFingerprint = 0U;
			std::uint64_t bindingResourceLayoutFingerprint = 0U;
			// Generation equality is reusable only after protected publication commits
			// this item's worker mutations back to the live runtime record.
			bool runtimeStateCaptureCommitted = false;
			AnimatorPlayback playback;
			AnimationClipView clipView;
			AnimationEventStream eventStream;
			// Direct-clip crowds usually have no authored events. Track whether this
			// frame actually captured the retained event cursor so capture and commit
			// can avoid copying an otherwise dormant deque and identifier set.
			bool eventStateCaptured = false;
			AnimationStatus dependencyStatus = AnimationStatus::MissingAsset;
			AnimatorPlaybackAdvanceResult advance;
			// Physical interval time is selected before clip playback speed changes
			// source ticks. It is never reconstructed from advancedTicks.
			double physicalEvaluationDurationSeconds = 0.0;
			std::shared_ptr<PoseBuffer> pose;
			// CPU visual-only culling reuses this immutable prior pose while still
			// publishing current playback state and generations for the animator.
			std::shared_ptr<const PoseBuffer> retainedVisualPose;
			std::shared_ptr<const AnimationAuthorityRow> publication;
			AnimationStatus evaluationStatus = AnimationStatus::NotReady;
			AnimationGeneration candidatePoseGeneration = 0U;
			AnimationGeneration capturedPoseGeneration = 0U;
			std::uint64_t candidateAuthorityPublicationGeneration = 0U;
			std::uint64_t visualPoseContentIdLow = 0U;
			std::uint64_t visualPoseContentIdHigh = 0U;
			std::uint64_t capturedStateGeneration = 0U;
			std::uint64_t capturedParameterGeneration = 0U;
			std::uint64_t capturedExecutionModeGeneration = 0U;
			AnimationGeneration contentGeneration = 0U;
			std::uint64_t candidateStateGeneration = 0U;
			std::uint64_t candidateParameterGeneration = 0U;
			std::uint64_t candidateExecutionModeGeneration = 0U;
			AnimationExecutionModeState previousExecutionModeState{};
			AnimatorPlaybackStateSnapshot capturedPlaybackState{};
			std::vector<AnimationParameterSnapshot> parameters;
			std::vector<float> layerWeights;
			StateMachineSnapshot capturedGraphState;
			StateMachineSnapshot candidateGraphState;
			ControllerTransitionRuntime transition;
			// Controller snapshots are copied only when an immutable evaluation
			// program can consume them. Direct-clip crowds otherwise carried three
			// dormant history vectors through both capture and commit every frame.
			bool controllerStateCaptured = false;
			NativeAnimationUuid activeRetargetProfileId{};
			AnimationGeneration activeRetargetProfileGeneration = 0U;
			bool retargetActive = false;
			bool sampledSourceSkeleton = false;
			RetargetEvaluationMetrics retargetMetrics{};
			std::uint64_t retargetDeterministicHash = 0U;
			AnimationStatus retargetEvaluationStatus = AnimationStatus::NotReady;
			NativeAnimationUuid activeMotionDatabaseId{};
			AnimationGeneration activeMotionDatabaseGeneration = 0U;
			std::uint64_t latestMotionQueryId = 0U;
			std::uint64_t appliedMotionQueryId = 0U;
			MotionMatchCandidate latestMotionCandidate{};
			bool hasMotionCandidate = false;
			std::vector<NativeAnimationMotionMatchRequest> pendingMotionQueries;
			std::vector<NativeAnimationMotionMatchSnapshot> evaluatedMotionQueries;
			// V2 stays separate from the frozen V1 records while sharing the retained
			// candidate that drives the next authoritative controller frame.
			std::vector<NativeAnimationMotionMatchQueryRequestV2> pendingMotionQueriesV2;
			std::vector<MotionMatchQueryCompletionV2> evaluatedMotionQueriesV2;
			// A rollback restore is committed before this frame's worker copies are
			// built. The first evaluation must reproduce that exact retained state;
			// ordinary frame-time advancement resumes on the following frame.
			bool restoredFromHistory = false;
			std::vector<NativeAnimationProceduralGoalCommand> activeProceduralGoals;
			std::vector<NativeAnimationProceduralResultSnapshot> proceduralResults;
			bool proceduralStateObserved = false;
			std::shared_ptr<const PhysicsPoseSnapshot> ragdollPhysicsPose;
			RagdollGenerationStamp ragdollGeneration{};
			RagdollLifecycleState ragdollLifecycle = RagdollLifecycleState::Inactive;
			float ragdollBlendWeight = 0.0F;
			AnimationFixedTick ragdollLifecycleStartTick = 0;
			AnimationFloat3 ragdollRecoveryAlignedRootPosition{};
			float ragdollRecoveryDurationSeconds = 0.0F;
			std::vector<RagdollMotorDrive> ragdollMotorDrives;
			bool ragdollRecoveryBlendComplete = false;
			std::shared_ptr<const AnimationAuthorityRow> priorPublication;
			AnimationGeneration sourceObjectGeneration = 0U;
			AnimationTransform sourceObjectWorldTransform{};
			bool sourceObjectWorldTransformCaptured = false;
			// These revisions name the protected scene snapshot that produced the
			// copied world transform. An unchanged scene can reuse the alternating
			// frame slot instead of resolving the same crowd object again.
			std::uint64_t sourceObjectSceneGeneration = 0U;
			std::uint64_t sourceObjectSpatialRevision = 0U;
			AnimationExecutionModeDecision executionDecision{};
			AnimationResolvedConsumerRequirements resolvedConsumers{};
			AnimationEvaluationCacheKey cacheKey{};
			AnimationEvaluationCacheRequirements cacheRequirements{};
			AnimationFallbackGenerationKey fallbackGenerations{};
			AnimationFallbackGenerationKey liveFallbackGenerations{};
			AnimationFallbackDecision fallbackDecision{};
			std::shared_ptr<const AnimationAuthorityRow> selectedPublication;
			std::optional<GpuAnimationLoweringInput> gpuLowering;
			// This record is the compact-path admission proof copied from protected
			// capture. Workers classify it without reopening scene/component storage.
			CompactGpuVisualInput compactGpuVisualInput{};
			AnimationProcessingClass processingClass = AnimationProcessingClass::Complete;
			// Preserve the checked request mask produced during preflight. Detailed
			// diagnostics must report the stages that will actually be dispatched,
			// rather than a hard-coded legacy bit pattern.
			std::uint32_t gpuStageMask = 0U;
			bool cacheEligible = false;
			bool cacheReused = false;
			bool visibleForVisualEvaluation = true;
			bool visualEvaluationSuspended = false;
			bool safePose = true;
			bool planned = false;
			bool evaluated = false;

			void ResetForCaptureReuse() noexcept {
				// Candidate storage alternates with the published frame-work vector. Clear
				// produced values without replacing input vectors so their allocations can
				// be reused when the same animator is captured again two frames later.
				// AnimationClipView::Open validates its retained immutable identity and
				// residency bitmap. Keeping it alive enables the exact-match fast path.
				// Binding fingerprints follow that same retained immutable storage and
				// are invalidated only when capture replaces part of the binding below.
				advance = {};
				rootMotionEnabled = false;
				physicalEvaluationDurationSeconds = 0.0;
				pose.reset();
				retainedVisualPose.reset();
				publication.reset();
				evaluationStatus = AnimationStatus::NotReady;
				eventStateCaptured = false;
				visualPoseContentIdLow = 0U;
				visualPoseContentIdHigh = 0U;
				retargetActive = false;
				sampledSourceSkeleton = false;
				retargetMetrics = {};
				retargetDeterministicHash = 0U;
				retargetEvaluationStatus = AnimationStatus::NotReady;
				controllerStateCaptured = false;
				pendingMotionQueries.clear();
				evaluatedMotionQueries.clear();
				pendingMotionQueriesV2.clear();
				evaluatedMotionQueriesV2.clear();
				restoredFromHistory = false;
				activeProceduralGoals.clear();
				proceduralResults.clear();
				proceduralStateObserved = false;
				ragdollPhysicsPose.reset();
				ragdollMotorDrives.clear();
				ragdollRecoveryBlendComplete = false;
				priorPublication.reset();
				// Source-object state is retained until CaptureFrameLocked compares its
				// scene and spatial revisions. That protected comparison either reuses
				// this exact value or clears it before any worker can observe the slot.
				executionDecision = {};
				resolvedConsumers = {};
				cacheKey = {};
				cacheRequirements = {};
				fallbackGenerations = {};
				liveFallbackGenerations = {};
				fallbackDecision = {};
				selectedPublication.reset();
				gpuLowering.reset();
				compactGpuVisualInput = {};
				compactCapture = {};
				compactCaptureOnly = false;
				processingClass = AnimationProcessingClass::Complete;
				gpuStageMask = 0U;
				cacheEligible = false;
				cacheReused = false;
				visibleForVisualEvaluation = true;
				visualEvaluationSuspended = false;
				safePose = true;
				planned = false;
				evaluated = false;
				runtimeStateCaptureCommitted = false;
			}
		};
		// Specialist consumers receive self-contained copied operations from the
		// worker seal. The protected finalizer may submit these sparse operations,
		// but it must never reopen FrameWorkItem or rediscover participants by
		// scanning the animator population after the authority root is visible.
		struct PreparedRagdollPublication {
			std::shared_ptr<RuntimeRecord> runtimeRecord;
			std::uint32_t motorDriveCount = 0U;
			std::optional<RagdollPhysicsRequest> motorRequest;
			std::optional<RagdollPhysicsRequest> deactivateRequest;
		};
		struct PreparedProceduralPublication {
			NativeAnimationAnimatorHandle animator{};
			std::vector<NativeAnimationProceduralResultSnapshot> results;
		};
		struct PreparedRetargetPublication {
			NativeAnimationRetargetSnapshot snapshot{};
		};
		struct PreparedAttachmentPublication {
			AnimatorInstanceHandle animator{};
			std::optional<AnimationBoneSocketSnapshot> snapshot;
		};
		struct PreparedAutomaticRootMotionPublication {
			AnimationRootMotionApplicationRequest request{};
		};
		// Compact GPU animators change only scalar playback/mode progress. Carry
		// those values as a typed post-publication commit instead of cloning the
		// animator's large stable binding, parameter, graph, and specialist state.
		struct PreparedCompactRuntimeCommit {
			// PublishedAuthorityCommit retains the complete expected runtime root until
			// this scalar batch is validated and applied. A raw pointer is therefore
			// stable here and avoids 4,000 shared-owner increments/decrements per frame.
			RuntimeRecord* runtimeRecord = nullptr;
			AnimatorInstanceHandle handle{};
			AnimatorPlayback playback{};
			AnimationGeneration poseGeneration = 0U;
			std::uint64_t stateGeneration = 0U;
			std::uint64_t executionModeGeneration = 0U;
			AnimationExecutionModeState executionModeState{};
		};
		// Worker sealing gathers every uncommon post-publication effect into this
		// self-contained packet. The protected boundary receives the packet only
		// after both immutable roots have been accepted, so it never revisits the
		// crowd-sized FrameWorkItem or captured-index collections to rediscover work.
		struct SealedSpecialistSidecarBatch {
			std::vector<PreparedRagdollPublication> ragdoll;
			std::vector<PreparedProceduralPublication> procedural;
			std::vector<PreparedRetargetPublication> retarget;
			std::vector<PreparedAttachmentPublication> attachments;
			std::vector<PreparedAutomaticRootMotionPublication> automaticRootMotion;
			std::vector<AnimationRootMotionApplicationRequest> explicitRootMotion;
			std::vector<PreparedCompactRuntimeCommit> compactRuntime;
			std::vector<std::size_t> evaluationCacheWorkIndices;
			std::vector<AnimatorInstanceHandle> rollbackHistoryAnimators;
		};
		// Compact workers intentionally receive indexed SoA storage instead of a
		// FrameWorkItem. The complete record remains protected-publication authority,
		// while tightly packed identity and packet columns are the only data a
		// visual-only GPU evaluator may read. This keeps crowd packets cache-friendly
		// and prevents dormant complete-evaluator members from entering worker cache.
		struct CompactGpuVisualWorkItems {
			std::vector<std::size_t> frameWorkIndices;
			std::vector<AnimatorInstanceHandle> animators;
			std::vector<std::uint64_t> sourceGenerations;
			std::vector<CompactGpuVisualInput> inputs;

			[[nodiscard]] std::size_t size() const noexcept {
				return inputs.size();
			}
			[[nodiscard]] bool empty() const noexcept {
				return inputs.empty();
			}
			void clear() noexcept {
				frameWorkIndices.clear();
				animators.clear();
				sourceGenerations.clear();
				inputs.clear();
			}
			void reserve(std::size_t capacity) {
				frameWorkIndices.reserve(capacity);
				animators.reserve(capacity);
				sourceGenerations.reserve(capacity);
				inputs.reserve(capacity);
			}
			void append(std::size_t frameWorkIndex, AnimatorInstanceHandle animator, std::uint64_t sourceGeneration,
						CompactGpuVisualInput input) {
				frameWorkIndices.push_back(frameWorkIndex);
				animators.push_back(animator);
				sourceGenerations.push_back(sourceGeneration);
				inputs.push_back(std::move(input));
			}
			[[nodiscard]] bool IsConsistent() const noexcept {
				return frameWorkIndices.size() == inputs.size() && animators.size() == inputs.size() &&
					   sourceGenerations.size() == inputs.size();
			}
		};
		// Completion columns mirror compact work by index. Every worker owns a
		// disjoint index stride, and protected publication validates all columns as
		// one atomic batch before it mutates a live runtime record.
		struct CompactGpuVisualCompletions {
			std::vector<std::size_t> compactItemIndices;
			std::vector<AnimatorInstanceHandle> animators;
			std::vector<std::uint64_t> sourceGenerations;
			std::vector<CompactGpuVisualResult> results;
			std::vector<AnimationStatus> statuses;

			[[nodiscard]] std::size_t size() const noexcept {
				return statuses.size();
			}
			void clear() noexcept {
				compactItemIndices.clear();
				animators.clear();
				sourceGenerations.clear();
				results.clear();
				statuses.clear();
			}
			void resize(std::size_t count) {
				compactItemIndices.resize(count);
				animators.resize(count);
				sourceGenerations.resize(count);
				results.resize(count);
				statuses.assign(count, AnimationStatus::NotReady);
			}
			[[nodiscard]] bool IsConsistent() const noexcept {
				return compactItemIndices.size() == statuses.size() && animators.size() == statuses.size() &&
					   sourceGenerations.size() == statuses.size() && results.size() == statuses.size();
			}
		};
		// Thousands of eventless direct-clip animators commonly share the exact same
		// immutable program, resident payload, execution policy, and asset generations.
		// Store those facts once per cohort instead of incrementing two shared pointers
		// and appending nine identical scalar columns for every crowd member. The frame
		// lookup below also reunites interleaved instances, which is important when a
		// crowd intentionally mixes clips or playback phases instead of arriving sorted
		// by animation asset.
		// Dynamic playback and authority generations remain per-instance below.
		struct CompactGpuVisualCohort {
			std::shared_ptr<const EvaluationProgram> program;
			std::shared_ptr<const AnimatorPlaybackBinding::GpuFastBinding> fastBinding;
			AnimationGpuExecutionInputs gpuExecution{};
			AnimationExecutionPolicy requestedPolicy = AnimationExecutionPolicy::Auto;
			AnimatorUpdatePolicy updatePolicy = AnimatorUpdatePolicy::EveryFrame;
			AnimationGeneration programGeneration = 0U;
			AnimationGeneration clipGeneration = 0U;
			AnimationGeneration skeletonGeneration = 0U;
			std::uint32_t jointCount = 0U;

			[[nodiscard]] bool Matches(const RuntimeRecord& record) const noexcept {
				const AnimationGpuExecutionInputs& candidate = record.binding.gpuExecution;
				return program == record.binding.evaluationProgram && fastBinding == record.binding.gpuFastBinding &&
					   gpuExecution.rendererAvailable == candidate.rendererAvailable &&
					   gpuExecution.animationCapabilityAvailable == candidate.animationCapabilityAvailable &&
					   gpuExecution.resourcesResident == candidate.resourcesResident &&
					   gpuExecution.evaluationHealthy == candidate.evaluationHealthy &&
					   gpuExecution.requiredBudget == candidate.requiredBudget &&
					   gpuExecution.availableBudget == candidate.availableBudget &&
					   requestedPolicy == record.binding.requestedExecutionPolicy && updatePolicy == record.binding.updatePolicy &&
					   programGeneration == record.binding.programGeneration && clipGeneration == record.binding.clipGeneration &&
					   skeletonGeneration == record.binding.skeletonGeneration && jointCount == record.binding.jointStableIds.size();
			}
		};
		// Compact planning publishes only the mode, reason, and next retained state.
		// The generic decision also owns consumer closure and transition records that
		// protected capture has already proven empty for this visual-only route.
		struct CompactExecutionDecision {
			AnimationEffectiveExecutionMode effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
			AnimationExecutionModeReason reason = AnimationExecutionModeReason::Uninitialized;
			AnimationExecutionModeState nextState{};
		};
		// Protected capture keeps simple GPU-visual animators out of
		// FrameWorkItem entirely. Parallel columns retain only the
		// scalar authority advances and immutable bindings that compact
		// planning, evaluation, and publication need. inputIndices
		// preserve the deterministic full-frame order when complete and
		// compact records are later validated as one atomic publication
		// batch.
		struct CompactGpuVisualCaptures {
			// The generic frame input also carries a controller asset handle. The
			// protected compact gate has already proven that no controller graph is
			// observed on this route, so retaining only publication identity avoids
			// copying unused asset metadata through every warmed crowd frame.
			struct InputIdentity {
				AnimatorInstanceHandle handle{};
				UUID ownerObjectId = EMPTY_UUID;
				AnimationGeneration componentGeneration = 0U;
				AnimationFixedTick tick = 0;
			};
			// The 64-bit value is only a lookup accelerator. Equal-hash
			// candidates still pass CompactGpuVisualCohort::Matches, so a
			// collision cannot merge animation programs, clips, skeleton
			// generations, or execution policies incorrectly.
			std::unordered_multimap<std::uint64_t, std::size_t> cohortLookup;
			std::vector<std::size_t> inputIndices;
			std::vector<InputIdentity> inputs;
			// The captured runtime root below owns these pointers for the
			// entire frame. Keeping the exact records beside compact
			// columns avoids a second 4,000-slot shared_ptr lookup during
			// authority sealing.
			std::vector<RuntimeRecord*> runtimeRecords;
			std::vector<AnimatorPlayback> playbacks;
			std::vector<AnimationExecutionModeState> previousExecutionModes;
			std::vector<std::size_t> cohortIndices;
			std::vector<CompactGpuVisualCohort> cohorts;
			std::vector<AnimationGeneration> candidatePoseGenerations;
			// Every row in one sealed candidate belongs to the same authority
			// generation. Retain that batch identity once instead of streaming an
			// identical 64-bit value through every compact owner.
			std::uint64_t authorityGeneration = 0U;
			std::vector<std::uint64_t> stateGenerations;
			std::vector<std::uint64_t> parameterGenerations;
			std::vector<std::uint64_t> executionModeGenerations;
			// Planning output remains in compact columns until it becomes
			// a compact work packet. No generic evaluator-owned state is
			// constructed for it.
			std::vector<AnimatorPlaybackAdvanceResult> advances;
			std::vector<CompactExecutionDecision> executionDecisions;
			std::vector<AnimationStatus> evaluationStatuses;
			std::vector<AnimationPlanRejectionReason> planningFailures;
			std::vector<AnimationProcessingClass> processingClasses;
			std::vector<std::uint32_t> stageMasks;
			// Warm frames need only the per-owner shader-visible lane. Program
			// and resident-payload ownership already lives once in the cohort;
			// a complete lowering object is reconstructed only for a cold plan.
			std::vector<GpuAnimationVisualInput> gpuPresentationVisuals;
			std::vector<CompactGpuVisualInput> compactInputs;

			[[nodiscard]] std::size_t size() const noexcept {
				return inputs.size();
			}
			[[nodiscard]] bool empty() const noexcept {
				return inputs.empty();
			}
			[[nodiscard]] static std::uint64_t CohortFingerprint(const RuntimeRecord& record) noexcept {
				std::uint64_t hash = 1469598103934665603ULL;
				const auto mix = [&hash](std::uint64_t value) noexcept {
					hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
				};
				const AnimationGpuExecutionInputs& gpu = record.binding.gpuExecution;
				mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record.binding.evaluationProgram.get())));
				mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record.binding.gpuFastBinding.get())));
				mix(gpu.rendererAvailable ? 1U : 0U);
				mix(gpu.animationCapabilityAvailable ? 1U : 0U);
				mix(gpu.resourcesResident ? 1U : 0U);
				mix(gpu.evaluationHealthy ? 1U : 0U);
				mix(std::bit_cast<std::uint64_t>(gpu.requiredBudget));
				mix(std::bit_cast<std::uint64_t>(gpu.availableBudget));
				mix(static_cast<std::uint64_t>(record.binding.requestedExecutionPolicy));
				mix(static_cast<std::uint64_t>(record.binding.updatePolicy));
				mix(record.binding.programGeneration);
				mix(record.binding.clipGeneration);
				mix(record.binding.skeletonGeneration);
				mix(record.binding.jointStableIds.size());
				return hash;
			}
			void clear() noexcept {
				cohortLookup.clear();
				inputIndices.clear();
				inputs.clear();
				runtimeRecords.clear();
				playbacks.clear();
				previousExecutionModes.clear();
				cohortIndices.clear();
				cohorts.clear();
				candidatePoseGenerations.clear();
				authorityGeneration = 0U;
				stateGenerations.clear();
				parameterGenerations.clear();
				executionModeGenerations.clear();
				advances.clear();
				executionDecisions.clear();
				evaluationStatuses.clear();
				planningFailures.clear();
				processingClasses.clear();
				stageMasks.clear();
				gpuPresentationVisuals.clear();
				compactInputs.clear();
			}
			void reserve(std::size_t capacity) {
				cohortLookup.reserve(capacity);
				inputIndices.reserve(capacity);
				inputs.reserve(capacity);
				runtimeRecords.reserve(capacity);
				playbacks.reserve(capacity);
				previousExecutionModes.reserve(capacity);
				cohortIndices.reserve(capacity);
				candidatePoseGenerations.reserve(capacity);
				stateGenerations.reserve(capacity);
				parameterGenerations.reserve(capacity);
				executionModeGenerations.reserve(capacity);
				advances.reserve(capacity);
				executionDecisions.reserve(capacity);
				evaluationStatuses.reserve(capacity);
				planningFailures.reserve(capacity);
				processingClasses.reserve(capacity);
				stageMasks.reserve(capacity);
				gpuPresentationVisuals.reserve(capacity);
				compactInputs.reserve(capacity);
			}
			void swap(CompactGpuVisualCaptures& other) noexcept {
				// Keep both frame buffers allocated. The candidate for
				// frame N+1 receives frame N-1's storage, so a stable crowd
				// performs no column reallocations.
				using std::swap;
				swap(cohortLookup, other.cohortLookup);
				swap(inputIndices, other.inputIndices);
				swap(inputs, other.inputs);
				swap(runtimeRecords, other.runtimeRecords);
				swap(playbacks, other.playbacks);
				swap(previousExecutionModes, other.previousExecutionModes);
				swap(cohortIndices, other.cohortIndices);
				swap(cohorts, other.cohorts);
				swap(candidatePoseGenerations, other.candidatePoseGenerations);
				swap(authorityGeneration, other.authorityGeneration);
				swap(stateGenerations, other.stateGenerations);
				swap(parameterGenerations, other.parameterGenerations);
				swap(executionModeGenerations, other.executionModeGenerations);
				swap(advances, other.advances);
				swap(executionDecisions, other.executionDecisions);
				swap(evaluationStatuses, other.evaluationStatuses);
				swap(planningFailures, other.planningFailures);
				swap(processingClasses, other.processingClasses);
				swap(stageMasks, other.stageMasks);
				swap(gpuPresentationVisuals, other.gpuPresentationVisuals);
				swap(compactInputs, other.compactInputs);
			}
			void append(std::size_t inputIndex, const AnimationFrameInput& input, RuntimeRecord& record,
						const CompactRuntimeState* currentState = nullptr) {
				inputIndices.push_back(inputIndex);
				inputs.push_back({input.handle, input.descriptor.ownerObjectId, input.descriptor.componentGeneration, input.tick});
				playbacks.push_back(currentState ? currentState->playback : record.playback);
				runtimeRecords.push_back(&record);
				previousExecutionModes.push_back(currentState ? currentState->executionModeState : record.executionModeState);
				std::size_t cohortIndex = cohorts.size();
				if(!cohorts.empty() && cohorts.back().Matches(record)) {
					// Sorted homogeneous crowds retain the former single
					// comparison fast path.
					cohortIndex = cohorts.size() - 1U;
				} else {
					const std::uint64_t fingerprint = CohortFingerprint(record);
					const auto [first, last] = cohortLookup.equal_range(fingerprint);
					for(auto candidate = first; candidate != last; ++candidate) {
						if(candidate->second < cohorts.size() && cohorts[candidate->second].Matches(record)) {
							cohortIndex = candidate->second;
							break;
						}
					}
					if(cohortIndex == cohorts.size()) {
						CompactGpuVisualCohort cohort;
						cohort.program = record.binding.evaluationProgram;
						cohort.fastBinding = record.binding.gpuFastBinding;
						cohort.gpuExecution = record.binding.gpuExecution;
						cohort.requestedPolicy = record.binding.requestedExecutionPolicy;
						cohort.updatePolicy = record.binding.updatePolicy;
						cohort.programGeneration = record.binding.programGeneration;
						cohort.clipGeneration = record.binding.clipGeneration;
						cohort.skeletonGeneration = record.binding.skeletonGeneration;
						cohort.jointCount = static_cast<std::uint32_t>(record.binding.jointStableIds.size());
						cohorts.push_back(std::move(cohort));
						cohortIndex = cohorts.size() - 1U;
						cohortLookup.emplace(fingerprint, cohortIndex);
					}
				}
				cohortIndices.push_back(cohortIndex);
				const AnimationGeneration currentPoseGeneration = currentState ? currentState->poseGeneration : record.poseGeneration;
				candidatePoseGenerations.push_back(currentPoseGeneration + 1U);
				stateGenerations.push_back(currentState ? currentState->stateGeneration : record.stateGeneration);
				parameterGenerations.push_back(record.parameterGeneration);
				executionModeGenerations.push_back(currentState ? currentState->executionModeGeneration : record.executionModeGeneration);
				advances.emplace_back();
				executionDecisions.emplace_back();
				evaluationStatuses.push_back(AnimationStatus::NotReady);
				planningFailures.push_back(AnimationPlanRejectionReason::None);
				processingClasses.push_back(AnimationProcessingClass::Complete);
				stageMasks.push_back(0U);
				gpuPresentationVisuals.emplace_back();
				compactInputs.emplace_back();
			}

			[[nodiscard]] const CompactGpuVisualCohort& cohort(std::size_t index) const noexcept {
				assert(index < cohortIndices.size() && cohortIndices[index] < cohorts.size());
				return cohorts[cohortIndices[index]];
			}
		};
		std::unique_ptr<AnimatorInstanceStore> instances = std::make_unique<AnimatorInstanceStore>();
		std::unique_ptr<AnimationCommandQueue> commands = std::make_unique<AnimationCommandQueue>();
		std::unique_ptr<ProceduralCommandStore> proceduralCommands = std::make_unique<ProceduralCommandStore>();
		std::unique_ptr<AdvancedAnimationCommandStore> advancedCommands = std::make_unique<AdvancedAnimationCommandStore>();
		AnimatorComponentBindingCoordinator componentBindings;
		// The coordinator publishes only the latest completed non-empty episode.
		// Atomics keep diagnostic readers independent from the coordinator thread
		// and avoid extending the runtime-ownership critical section.
		std::atomic<std::uint64_t> bindingEpisodeCount{0U};
		std::atomic<std::uint64_t> bindingReconciliationNanoseconds{0U};
		std::atomic<std::size_t> bindingReconciledCount{0U};
		std::atomic<std::size_t> bindingFailureCount{0U};
		// This is coordinator-thread domain state, not per-frame worker input.
		// Runtime applications keep the default enabled; the editor explicitly
		// switches it at protected Play Mode entry and exit boundaries.
		bool authoredComponentPlaybackEnabled = true;
		bool authoredComponentPlaybackAdvancing = true;
		bool headlessExecutionRequired = false;
		std::atomic<std::shared_ptr<RuntimeRecordRoot>> runtimeRecords{std::make_shared<RuntimeRecordRoot>()};
		std::atomic<std::shared_ptr<const CompactRuntimeStateRoot>> compactRuntimeStates{std::make_shared<CompactRuntimeStateRoot>()};
		// Managed/native producers append copied requests from any
		// scripting worker. Only PublishAnimationAuthority drains this
		// bounded queue and touches live scene/physics state,
		// preserving the worker-safe scripting declaration.
		std::vector<AnimationRootMotionApplicationRequest> pendingRootMotionApplications;
		// Every captured input resolves to exactly one compact or
		// complete slot. This explicit mapping preserves full-frame
		// deterministic order while compact crowds avoid constructing
		// heavyweight FrameWorkItem records.
		struct CapturedInputWorkIndex {
			std::size_t storageIndex = 0U;
			bool compact = false;
		};
		std::unique_ptr<AnimationGenerationLatch> lifecycleLatch = std::make_unique<AnimationGenerationLatch>();
		// Sealing owns all fallible authority construction.  The protected publisher
		// transfers this tiny receipt only after the immutable root has been accepted;
		// the follow-up finalizer can therefore never mutate runtime records for a
		// rejected, cancelled, or stale candidate.
		struct PublishedAuthorityCommitToken {
			std::size_t animatorCount = 0U;
			std::uint64_t frameEpoch = 0U;
			std::uint64_t authorityGeneration = 0U;
			std::uint64_t sessionGeneration = 0U;
			std::uint64_t sceneLifecycleGeneration = 0U;
			// Worker sealing prepares a complete copy-on-write evaluator root.  The
			// protected boundary compares the exact predecessor pointer before it swaps
			// this root, so a concurrent lifecycle change rejects the whole candidate
			// without exposing a partly updated crowd.
			std::shared_ptr<RuntimeRecordRoot> expectedRuntimeRoot;
			std::shared_ptr<RuntimeRecordRoot> nextRuntimeRoot;
			// Compact scalar state is exchanged beside the broad runtime root. The
			// predecessor identity prevents a late worker from replacing state authored
			// by a lifecycle or command mutation.
			std::shared_ptr<const CompactRuntimeStateRoot> expectedCompactRuntimeRoot;
			std::shared_ptr<const CompactRuntimeStateRoot> nextCompactRuntimeRoot;
			// This is sealed by the worker alongside nextRuntimeRoot. It is opaque to
			// ordinary consumers and contains only sparse copied operations.
			std::shared_ptr<SealedSpecialistSidecarBatch> specialistSidecars;
			bool ready = false;
		};
		std::atomic_uint64_t runtimeSessionEpoch = 1U;
		std::atomic_uint64_t frameDeclarationEpoch = 0U;
		std::uint64_t lastPublishedFrameDeclarationEpoch = 0U;
		// Authored automatic root-motion requests are a dedicated ordered producer.
		// The sequence survives animator reuse and is reset only with process lifetime.
		std::uint64_t automaticRootMotionSequence = 0U;
		// Only the scheduler coordinator reads or writes this transition marker. It
		// lets a steady static project skip locking merely to clear empty buffers.
		bool hadScheduledAnimatorMembership = false;
		// Managed/gameplay producers may submit typed commands concurrently with a
		// protected shutdown request. This guard covers only pointer lifetime; each
		// runtime type retains its own fine-grained data lock for normal queue/store
		// operations, and worker evaluation never takes this lifecycle mutex.
		mutable std::mutex runtimeOwnershipMutex;
		// Locomotion publishes complete copied mappings without entering this mutex;
		// Animation workers pin one immutable root for the entire evaluation phase.
		std::atomic<std::shared_ptr<const AnimationRootControllerBindingSnapshot>> rootControllerBindings;
		// Accepted ragdoll Activate is visible at submission, before any worker
		// evaluates root motion or the next locomotion movement publication.
		std::atomic<std::shared_ptr<const AnimationRagdollExclusionSnapshot>> ragdollExclusions;
		// The singleton starts usable for the existing engine bootstrap path. The
		// flag then prevents calls after Shutdown from recreating queue ownership;
		// only an explicit Initialize begins another runtime session.
		std::atomic_bool runtimeActive = true;
		// Scheduler ownership remains on the thread that first constructs the
		// process-wide singleton. Lifecycle calls may not silently transfer that
		// protected-boundary responsibility to another thread.
		const std::thread::id coordinatorThread = std::this_thread::get_id();
		// Retired alternating buffers keep capacity; pools are writable only after all retained readers release their roots.
		struct PersistentReuse {
			// A cooked clip generation is immutable. Cache its serialized deterministic
			// fingerprint once so a prefab crowd does not serialize and hash the same
			// complete clip independently for every runtime record during initial bind.
			std::map<AnimationAssetHandle, std::uint64_t> clipContentFingerprints;
			// Capture builds transactionally into this second owned vector.
			// Swapping on success preserves the previous allocation for the
			// next frame, avoiding a fresh FrameWorkItem allocation while a
			// failed capture stays invisible.
			std::vector<FrameWorkItem> candidateFrameWork;
			std::vector<CapturedInputWorkIndex> candidateInputWorkIndices;
			CompactGpuVisualCaptures candidateCompactGpuVisualCaptures;
			std::shared_ptr<RuntimeRecordRoot> candidateCompactRuntimeCaptureRoot;
			std::shared_ptr<const CompactRuntimeStateRoot> candidateCompactRuntimeStateCaptureRoot;
			// Presentation consumes immutable shared frames asynchronously.
			// Three retained slots let frame N reuse N-2 storage once all
			// readers release it, while a slow reader safely forces a
			// temporary fallback allocation instead of mutation.
			std::array<std::shared_ptr<GpuAnimationPresentationFrame>, 3U> gpuPresentationFramePool{};
			std::size_t gpuPresentationFramePoolCursor = 0U;
			// Published authority roots are immutable, but retired roots eventually
			// return to sole ownership here. Four slots cover the normal renderer and
			// presentation pipeline depth while preserving vector capacity for stable
			// crowds; a delayed reader simply makes the frame allocate a fallback root.
			std::array<std::vector<std::shared_ptr<AnimationAuthorityPage>>, 4U> authorityPagePool{};
			std::size_t authorityPagePoolCursor = 0U;
			std::array<std::shared_ptr<AnimationFrameSidecars>, 4U> authoritySidecarPool{};
			std::size_t authoritySidecarPoolCursor = 0U;
			AnimationMemoryController memoryController;
			AnimationHistory rollbackHistory;
			AnimationRollbackSnapshotCodec rollbackSnapshotCodec;
			AnimationEvaluationCache evaluationCache;
		};
		PersistentReuse persistentReuse;
		// Captured commands, consumer views and strong runtime roots remain owned until planning, evaluation and sealing finish.
		struct FrameInputs {
			std::vector<AnimationFrameInput> capturedFrameInputs;
			std::vector<AnimationCommand> capturedCommands;
			// A single resource-publication epoch protects all controller-qualified
			// commands at the scalar authority exchange. Capture reads it before the
			// per-command validation so even a reload during Capture changes the seal.
			std::uint64_t qualifiedControllerResourceRevision = 0U;
			// Each worker writes only entries targeting its disjoint
			// animator range. The protected publisher later turns these
			// staged values into observable receipts together with the pose
			// generation they affected.
			std::vector<AnimationStatus> capturedCommandStatuses;
			// Ordered workload phases retain the first failed command-frame boundary
			// until protected settlement logs it. Stable command-free frames do no IO.
			const char* commandFrameFailurePhase = nullptr;
			// Procedural requests use their own fixed ABI record because
			// the original gameplay command stride is pinned. They are
			// still drained and completed at the same protected frame
			// boundary as ordinary animation commands.
			std::vector<NativeAnimationProceduralGoalCommand> capturedProceduralCommands;
			std::vector<AnimationStatus> capturedProceduralCommandStatuses;
			// Capture retained procedural state under one store lock, then
			// merge these ordered flat ranges with the ordered animator
			// slots below.
			std::vector<ActiveProceduralGoalRange> capturedProceduralGoalRanges;
			std::vector<NativeAnimationProceduralGoalCommand> capturedProceduralGoals;
			std::vector<NativeAnimationRetargetRequest> capturedRetargetRequests;
			std::vector<NativeAnimationMotionMatchRequest> capturedMotionMatchRequests;
			std::vector<NativeAnimationMotionMatchQueryRequestV2> capturedMotionMatchQueriesV2;
			std::vector<NativeAnimationRagdollRequest> capturedRagdollRequests;
			std::vector<NativeAnimationDeterminismRequest> capturedDeterminismRequests;
			std::vector<DeterminismRequestCompletion> determinismRequestCompletions;
			std::vector<CapturedInputWorkIndex> capturedInputWorkIndices;
			// Protected capture uses workers only to write disjoint admission bytes.
			// The coordinator then assembles deterministic SoA columns in source order.
			std::vector<std::uint8_t> compactCaptureAdmissionProof;
			CompactGpuVisualCaptures compactGpuVisualCaptures;
			std::shared_ptr<RuntimeRecordRoot> compactRuntimeCaptureRoot;
			std::shared_ptr<const CompactRuntimeStateRoot> compactRuntimeStateCaptureRoot;
			AnimationLifecycleGenerationSnapshot frameLifecycleGenerations{};
			std::uint64_t frameSceneGeneration = 0U;
			// Consumer declarations are immutable for one captured frame. Remembering the
			// empty common case avoids thousands of contended atomic shared_ptr loads while still
			// making a declaration published mid-frame visible at the next boundary.
			bool frameHasConsumerDeclarations = false;
			AnimationConsumerRegistry::ReadView frameConsumerReadView;
			AnimationFixedTick frameTick = 0;
		};
		FrameInputs frameInputs;
		// Plans retain scratch storage and its budget lease; published poses share a reservation that survives frame reset.
		struct FrameStorage {
			std::vector<FrameWorkItem> frameWork;
			AnimationWorkScheduler workScheduler{MakeAnimationWorkSchedulerConfiguration()};
			AnimationWorkPlan workPlan;
			std::unique_ptr<AnimationWorkRangeLedger> workRangeLedger;
			std::vector<std::byte> frameScratch;
			AnimationMemoryLease frameScratchLease;
			std::shared_ptr<const AnimationMemoryLease> framePoseReservation;
		};
		FrameStorage frameStorage;
		// Disjoint workers write indexed completions; the single finalize gate publishes readiness before evaluation begins.
		struct FrameEvaluationState {
			AnimationWorkCancellationSource workCancellationSource;
			AnimationPlanRejectionReason workPlanRejectionReason = AnimationPlanRejectionReason::None;
			std::size_t workPlanRejectionItemIndex = std::numeric_limits<std::size_t>::max();
			std::vector<AnimationWorkItem> capturedWorkItems;
			std::vector<AnimationWorkCompletion> workCompletions;
			CompactGpuVisualWorkItems compactGpuVisualWorkItems;
			CompactGpuVisualCompletions compactGpuVisualCompletions;
			std::vector<std::size_t> deterministicPublicationOrder;
			AnimationCancellationSource evaluationCancellationSource;
			AnimationCancellationToken evaluationCancellation;
			std::atomic_size_t scheduledAnimatorCount = 0U;
			std::atomic_size_t plannedAnimatorCount = 0U;
			std::atomic_size_t evaluatedAnimatorCount = 0U;
			std::atomic_size_t publishedAnimatorCount = 0U;
			std::atomic_size_t cacheReusedAnimatorCount = 0U;
			// This counter tracks same-frame visual pose clones separately from the
			// cross-frame evaluation cache. Both avoid clip evaluation, but only the
			// latter retains the exact prior publication pointer.
			std::atomic_size_t sharedPoseAnimatorCount = 0U;
			std::atomic_size_t fallbackAnimatorCount = 0U;
			std::atomic_size_t compactGpuVisualCompletedCount = 0U;
			// Timings accumulate owned worker elapsed time in nanoseconds. They measure
			// real work even when sibling compact and complete branches overlap, instead
			// of attributing wall-clock queue delay to one processing class.
			std::atomic_uint64_t compactPlanningNanoseconds = 0U;
			std::atomic_uint64_t compactPreCopyCaptureCount = 0U;
			std::atomic_uint64_t completePlanningNanoseconds = 0U;
			std::atomic_uint64_t compactEvaluationNanoseconds = 0U;
			std::atomic_uint64_t completeEvaluationNanoseconds = 0U;
			std::atomic_bool frameCancellationRequested = false;
			bool deterministicPublicationValidated = false;
			// BuildPlan validates the immutable work plan once. Worker partitions and
			// protected publication consume that proof instead of rescanning all items.
			bool workPlanValidated = false;
			std::size_t availableWorkerCount = 0U;
			mutable std::mutex workPlanFinalizeMutex;
			std::condition_variable workPlanFinalizeCondition;
			WorkPlanFinalizeState workPlanFinalizeState = WorkPlanFinalizeState::Pending;
		};
		FrameEvaluationState evaluationState;
		// Live authority survives individual candidates and remains a coordinator-owned store.
		std::atomic_uint64_t authorityPublicationGeneration = 0U;
		// Consumers acquire this live immutable authority root rather than mutable evaluator records.
		AnimationAuthorityPublisher authorityPublisher;
		// Sealing owns fallible candidates; exact predecessor checks qualify exchange before follow-up consumers run.
		struct AuthorityState {
			SealedAnimationFrameResult sealedAuthorityCandidate{};
			bool sealedAuthorityCandidateReady = false;
			PublishedAuthorityCommitToken publishedAuthorityCommit{};
			std::optional<AnimationDiagnosticsSnapshot> preparedDiagnostics;
			std::optional<AnimationDetailedDiagnosticsSnapshot> preparedDetailedDiagnostics;
			std::shared_ptr<const GpuAnimationPresentationFrame> preparedPresentationFrame;
			// A frame made only of exact cache hits or compatible hold-last selections has
			// no new authority data. Keeping this explicit lets the protected boundary
			// retain the previous root rather than relabeling old sidecars as a new frame.
			bool retainsPublishedAuthorityRoot = false;
		};
		AuthorityState authorityState;
		Impl();
		[[nodiscard]] FrameWorkItem* CompleteWorkForInputIndex(std::size_t inputIndex) noexcept;
		[[nodiscard]] const FrameWorkItem* CompleteWorkForInputIndex(std::size_t inputIndex) const noexcept;
		void ResetFrameIntegrationStateLocked() noexcept;
		void RequestFrameCancellation(AnimationCancellationReason reason = AnimationCancellationReason::Explicit) noexcept;
		bool PollFrameCancellation() noexcept;
		AnimationStatus InvalidateLifecycleLocked(AnimationLifecycleBoundary boundary) noexcept;
		void RetireLifecyclePublicationStateLocked() noexcept;
		void ActivateRuntimeOwnershipLocked();
		[[nodiscard]] static const CompactRuntimeState* FindCompactRuntimeState(const std::shared_ptr<const CompactRuntimeStateRoot>& root,
																				AnimatorInstanceHandle handle) noexcept;
		[[nodiscard]] bool MaterializeCompactRuntimeStateLocked(AnimatorInstanceHandle handle, RuntimeRecord& record) noexcept;
		RuntimeRecord* FindRuntimeRecordLocked(AnimatorInstanceHandle handle) noexcept;
		const RuntimeRecord* FindRuntimeRecordLocked(AnimatorInstanceHandle handle) const noexcept;
		static AnimationNetworkFloat3Bits ToNetworkBits(const AnimationFloat3& value) noexcept;
		static AnimationNetworkQuaternionBits ToNetworkBits(const AnimationQuaternion& value) noexcept;
		static AnimationNetworkTransformBits ToNetworkBits(const AnimationTransform& value) noexcept;
		static std::uint64_t RuntimePoseChecksum(const PoseBuffer& pose) noexcept;
		static std::uint64_t RuntimeStateChecksum(const RuntimeRecord& record) noexcept;
		static AnimationDeterministicFingerprint BuildDeterministicFingerprint(const RuntimeRecord& record) noexcept;
		AnimationRollbackGenerationStamp BuildRollbackGenerations(const RuntimeRecord& record,
																  const AnimationAuthorityRow& published) const noexcept;
		static AnimationStatus EncodeParameter(const AnimationParameterSnapshot& source, AnimationParameterRollbackState& output) noexcept;
		static AnimationStatus DecodeParameter(const AnimationParameterRollbackState& source, AnimationParameterSnapshot& output) noexcept;
		AnimationStatus BuildRollbackSnapshotLocked(const RuntimeRecord& record, const AnimationAuthorityRow* published,
													AnimationRollbackSnapshot& output) const noexcept;
		static std::uint64_t RuntimePoseChecksum(const AnimationRollbackSnapshot& snapshot) noexcept;
		static std::uint64_t RuntimeStateChecksum(const AnimationRollbackSnapshot& snapshot) noexcept;
		AnimationStatus RestoreRollbackSnapshotLocked(RuntimeRecord& record, const AnimationRollbackSnapshot& snapshot) noexcept;
		void PrepareDeterminismRequestsLocked() noexcept;
		void CaptureRollbackHistoryLocked(std::span<const AnimatorInstanceHandle> rollbackHistoryAnimators) noexcept;
		// Child creation/submission/execution can throw. Scoped settlement joins
		// admitted readers first; the workload boundary reports the original error.
		bool CaptureFrameLocked(AnimationFixedTick captureTick, std::uint64_t candidateAuthorityGeneration);
		static std::uint64_t MixWorkFingerprint(std::uint64_t hash, std::uint64_t value) noexcept;
		static std::uint64_t BuildBindingOutputRequirementsFingerprint(const AnimatorPlaybackBinding& binding) noexcept;
		static std::uint64_t BuildBindingResourceLayoutFingerprint(const AnimatorPlaybackBinding& binding) noexcept;
		static AnimationCompatibilityFingerprint BuildFallbackCompatibilityFingerprint(const AnimatorPlaybackBinding& binding) noexcept;
		static bool TryAddMemoryBytes(std::size_t count, std::size_t elementBytes, std::size_t& total) noexcept;
		static bool TryEstimatePoseBytes(const FrameWorkItem& item, std::size_t& total) noexcept;
		static bool TryAdvanceGeneration(std::uint64_t current, bool changed, std::uint64_t& output) noexcept;
		void ResolveUnchangedCacheReuse(FrameWorkItem& item) noexcept;
		[[nodiscard]] static bool CanSuspendVisualEvaluation(const FrameWorkItem& item) noexcept;
		const EvaluationState* FindControllerState(const AnimatorPlaybackBinding& binding, std::uint64_t stateId) const noexcept;
		const AnimatorControllerClipBinding* FindControllerClipForState(const AnimatorPlaybackBinding& binding,
																		std::uint64_t stateId) const noexcept;
		AnimationStatus BeginControllerTransition(FrameWorkItem& item) noexcept;
		void PromoteCompletedControllerTransition(FrameWorkItem& item) noexcept;
		AnimationStatus SynchronizeControllerTransition(FrameWorkItem& item) noexcept;
		AnimationStatus SelectControllerClipForState(FrameWorkItem& item, std::uint64_t stateId) noexcept;
		AnimationStatus ResolveRequestedStateLeaf(const EvaluationProgram& program, std::uint64_t requestedStateId,
												  std::uint64_t& outputLeaf) const noexcept;
		std::uint64_t MakeCommandTransitionId(const AnimationCommand& command, std::uint64_t sourceStateId,
											  std::uint64_t destinationStateId) const noexcept;
		AnimationStatus ApplyGraphControlCommand(FrameWorkItem& item, const AnimationCommand& command) noexcept;
		AnimationStatus ApplyRetainedMotionSelection(FrameWorkItem& item, bool& changed) noexcept;
		// GPU visual bindings are long-lived asset records, while renderer, scene, and
		// playback identities belong to exactly one captured frame. Keep this stamp in
		// one helper so complete and compact lowering cannot disagree about a packet's
		// shader-visible 32-bit lanes or its exact 64-bit render identity.
		[[nodiscard]] bool StampGpuVisualFrameIdentity(GpuAnimationVisualInput& visual, AnimationFixedTick resolvedTick,
													   AnimationFixedTick frameTick) const noexcept;
		void PlanCompactCapture(FrameWorkItem& item, const AnimatorPlaybackTimeInput& timeInput) noexcept;
		void PlanCompactCapture(std::size_t compactIndex, const AnimatorPlaybackTimeInput& timeInput,
								const CompactGpuVisualCohort*& validatedCohort, std::uint32_t& validatedStageMask) noexcept;
		void PlanRange(std::size_t begin, std::size_t end, const AnimatorPlaybackTimeInput& timeInput) noexcept;
		bool FinalizeAnimationWorkPlan() noexcept;
		bool EnsureAnimationWorkPlanFinalized() noexcept;
		AnimationTransform ReferenceTransformForStableId(const FrameWorkItem& item, std::uint64_t stableId) const noexcept;
		void BuildModelTransforms(const FrameWorkItem& item, std::span<const AnimationTransform> local,
								  std::span<AnimationTransform> model) const noexcept;
		bool BuildSkeletonModelTransforms(const SkeletonAsset& skeleton, std::span<const AnimationTransform> local,
										  std::vector<AnimationTransform>& model) const;
		AnimationStatus ApplyRetargetStage(FrameWorkItem& item, const CpuClipEvaluationResult& sampled,
										   std::span<AnimationTransform> targetLocal, std::span<AnimationTransform> targetModel) const;
		static std::int16_t QuantizeMotionFeature(float value, float scale, float offset) noexcept;
		static float MotionPoseFeature(std::span<const AnimationTransform> modelPose, std::size_t featureIndex) noexcept;
		void EvaluateMotionMatchRequests(FrameWorkItem& item, std::span<const AnimationTransform> modelPose) const noexcept;
		void EvaluateMotionMatchQueriesV2(FrameWorkItem& item) const noexcept;
		AnimationStatus ApplyRetargetRootMotionPolicy(const FrameWorkItem& item, AnimationRootMotionDelta& delta) const noexcept;
		bool BuildLocalTransformsFromModel(const FrameWorkItem& item, std::span<const AnimationTransform> model,
										   std::span<AnimationTransform> local) const noexcept;
		ProceduralGenerationStamp MakeProceduralGeneration(const FrameWorkItem& item) const noexcept;
		std::uint32_t FindProceduralJointIndex(const FrameWorkItem& item, std::uint64_t stableJointId) const noexcept;
		AnimationStatus BuildProceduralNodes(const FrameWorkItem& item, const ProceduralGenerationStamp& generation,
											 std::vector<ProceduralGraphNodeInput>& nodes) const;
		NativeAnimationProceduralSolverKind NativeSolverForGoal(NativeAnimationProceduralGoalKind kind) const noexcept;
		AnimationStatus ApplyProceduralStage(FrameWorkItem& item, GraphPoseContribution& pose) const;
		AnimationStatus ApplyRagdollStage(FrameWorkItem& item, GraphPoseContribution& pose) const;
		void PrepareFallbackSelection(FrameWorkItem& item) noexcept;
		static AnimationFallbackGenerationKey BuildLiveFallbackGenerations(const RuntimeRecord& record, const FrameWorkItem& item,
																		   std::uint64_t sceneGeneration) noexcept;
		AnimationStatus BlendControllerTransitionSample(FrameWorkItem& item, CpuClipEvaluator& evaluator,
														CpuClipEvaluationResult& source) const noexcept;
		static std::pair<std::uint64_t, std::uint64_t> BuildVisualPoseContentIdentity(const PoseBuffer& pose) noexcept;
		void PublishEvaluatedCpuPose(FrameWorkItem& item) noexcept;
		void EvaluateItem(FrameWorkItem& item) noexcept;
		static bool CanShareCpuVisualPose(const FrameWorkItem& source, const FrameWorkItem& destination) noexcept;
		bool TryShareCpuVisualPose(const FrameWorkItem& source, FrameWorkItem& destination) noexcept;
		void EvaluatePartition(std::size_t partitionIndex, std::size_t partitionCount) noexcept;
		void EvaluateCompactGpuVisualPartition(std::size_t itemBegin, std::size_t itemEnd) noexcept;
		[[nodiscard]] bool CanRetainPublishedAuthorityRoot(const std::shared_ptr<const AnimationAuthorityFrame>& previous,
														   std::uint64_t session, std::uint64_t lifecycle) const noexcept;
		// A failed nested batch must reach the scheduler's frame failure boundary,
		// rather than terminate while unwinding an otherwise settled child group.
		[[nodiscard]] bool SealAuthorityFrameCandidate();
		// Runtime candidate preparation owns child work and propagates failures
		// only after those children stop accessing its captured candidate storage.
		[[nodiscard]] bool PrepareRuntimeRootCandidate(std::uint64_t authorityGeneration);
		bool CommitPublishedEvaluationCacheEntries(std::size_t animatorCount, std::uint64_t frameEpoch, std::uint64_t authorityGeneration,
												   std::uint64_t sessionGeneration) noexcept;
		bool ApplySealedSpecialistSidecars(PublishedAuthorityCommitToken& token) noexcept;
		AnimationDiagnosticsSnapshot BuildActiveDiagnosticsLocked(std::uint64_t publicationGeneration,
																  std::size_t detailedRecordCount = 0U) const noexcept;
		AnimationDetailedDiagnosticsSnapshot BuildActiveDetailedDiagnosticsLocked(std::uint64_t publicationGeneration) const;
		void PublishCommandCompletionsLocked(std::uint64_t publicationGeneration) noexcept;
		void PublishProceduralCommandCompletionsLocked(std::uint64_t publicationGeneration) noexcept;
		AnimationStatus BuildRagdollActivationPoseLocked(const RuntimeRecord& record, const NativeAnimationRagdollRequest& request,
														 std::uint64_t publicationGeneration, PhysicsPoseSnapshot& output,
														 RagdollGenerationStamp& generation) const noexcept;
		AnimationStatus BeginRagdollRecoveryLocked(RuntimeRecord& record, const NativeAnimationRagdollRequest& request,
												   bool& recoveryBlocked) noexcept;
		NativeAnimationRagdollSnapshot MakeRagdollSnapshot(const RuntimeRecord& record, NativeAnimationStatus status,
														   AnimationFixedTick observationTick, std::uint64_t publicationGeneration,
														   bool recoveryBlocked = false) const noexcept;
		bool HasRagdollExternalLeaseLocked(const RuntimeRecord& record) const noexcept;
		void SettleFailedFrameCommandsLocked(AnimationStatus status, const char* fallbackPhase) noexcept;
		void PublishCommittedOrdinaryCommandsLocked() noexcept;
		AnimationStatus AbortFailedRagdollAdmissionLocked(RuntimeRecord& record) noexcept;
		AnimationStatus RefreshExternalRagdollRecoveryLocked(RuntimeRecord& record) noexcept;
		void PublishRagdollExclusionForRecordLocked(const RuntimeRecord& record, std::uint64_t completedCorrelation) noexcept;
		void RejectDrainedFrameCommandsLocked() noexcept;
		void DropStalePendingRagdollExclusionLocked(AnimatorInstanceHandle animator, std::uint64_t correlationId) noexcept;
		void SynchronizeRagdollResultsLocked(std::uint64_t publicationGeneration) noexcept;
		void PublishAdvancedCommandCompletionsLocked(std::uint64_t publicationGeneration) noexcept;
		[[nodiscard]] std::shared_ptr<const GpuAnimationPresentationFrame>
		BuildGpuPresentationFrame(std::uint64_t sourceGeneration,
								  std::shared_ptr<const AnimationAuthorityFrame> sealedAuthority = {}) noexcept;
		void AssertCoordinatorThread() const;
		void EvaluateCompleteRootMotion(FrameWorkItem& item, CpuClipEvaluator& evaluator) noexcept;

		bool PrepareCompleteRuntimeRecords(std::uint64_t authorityGeneration, const std::shared_ptr<RuntimeRecordRoot>& expected,
										   const std::shared_ptr<RuntimeRecordRoot>& next,
										   const std::shared_ptr<CompactRuntimeStateRoot>& nextCompact,
										   const std::shared_ptr<SealedSpecialistSidecarBatch>& specialistSidecars,
										   std::uint64_t liveSceneGeneration);

		bool PrepareHomogeneousAuthorityRows(std::uint64_t candidateGeneration, std::uint64_t session, std::size_t pageCount,
											 const std::shared_ptr<const AnimationAuthorityFrame>& previous,
											 std::vector<std::shared_ptr<AnimationAuthorityPage>>& mutablePages,
											 std::vector<bool>& pageRetainsPrior, const std::shared_ptr<AnimationFrameSidecars>& sidecars,
											 bool& compactPagesWorkerValidated);

		bool PrepareMixedAuthorityRows(std::uint64_t candidateGeneration, std::uint64_t session, std::size_t pageCount,
									   const std::shared_ptr<const AnimationAuthorityFrame>& previous,
									   std::vector<std::shared_ptr<AnimationAuthorityPage>>& mutablePages,
									   std::vector<bool>& pageRetainsPrior, const std::shared_ptr<AnimationFrameSidecars>& sidecars,
									   bool& compactPagesWorkerValidated);

		bool PrepareOrdinaryAuthorityRows(std::uint64_t candidateGeneration, std::uint64_t session,
										  const std::shared_ptr<const AnimationAuthorityFrame>& previous,
										  std::vector<std::shared_ptr<AnimationAuthorityPage>>& mutablePages,
										  std::vector<bool>& pageRetainsPrior, const std::shared_ptr<AnimationFrameSidecars>& sidecars,
										  bool& compactPagesWorkerValidated);

		AnimatorPlaybackStateSnapshot PrepareCompletePlayback(FrameWorkItem& item, const AnimatorPlaybackTimeInput& timeInput) noexcept;

		std::shared_ptr<const AnimationAuthorityRow> FindPreEvaluationPublication(NativeAnimationAnimatorHandle animator);

		void PublishRetargetCompletionsLocked(std::uint64_t publicationGeneration) noexcept;

		void PublishMotionMatchCompletionsLocked(std::uint64_t publicationGeneration) noexcept;

		void PublishMotionMatchV2CompletionsLocked(std::uint64_t publicationGeneration) noexcept;

		void PublishRagdollCompletionsLocked(std::uint64_t publicationGeneration) noexcept;

		void PublishDeterminismCompletionsLocked(std::uint64_t publicationGeneration) noexcept;

		void CopyCompleteRuntimeInput(std::size_t inputIndex, const AnimationFrameInput& input, RuntimeRecord* record,
									  std::size_t& candidateCompleteCount, std::size_t& proceduralRangeIndex,
									  std::uint64_t frameSpatialRevision, std::uint64_t candidateAuthorityGeneration,
									  bool visibleForPresentation, bool rootMotionEnabled);

		bool CaptureRuntimeFrameInputs(std::uint64_t candidateAuthorityGeneration, std::uint64_t frameSpatialRevision,
									   const std::shared_ptr<const AnimationRootControllerBindingSnapshot>& rootBindings);

		bool CaptureQueuedFrameInputs(AnimationFixedTick captureTick);

		void ProveCompactCaptureRanges(const std::shared_ptr<RuntimeRecordRoot>& capturedRuntimeRoot,
									   const std::shared_ptr<const CompactRuntimeStateRoot>& capturedCompactRuntimeRoot,
									   bool compactFrameHasAdvancedRequests);

		void CaptureAnimationInputsStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
										 TerraEngine::Scheduling::JobGraphContext& context);

		void PlanAnimationEvaluationStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
										  const AnimatorPlaybackTimeInput& timeInput,
										  const TerraEngine::Scheduling::FrameWorkloadPartition& partition,
										  TerraEngine::Scheduling::JobGraphContext& context);

		void FinalizeAnimationWorkPlanStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
											TerraEngine::Scheduling::JobGraphContext& context);

		void EvaluateCompleteAnimationStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
											const TerraEngine::Scheduling::FrameWorkloadPartition& partition,
											TerraEngine::Scheduling::JobGraphContext& context);

		void EvaluateCompactAnimationStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
										   const TerraEngine::Scheduling::FrameWorkloadPartition& partition,
										   TerraEngine::Scheduling::JobGraphContext& context);

		void SealAnimationAuthorityStage(std::uint64_t runtimeSessionEpoch, std::uint64_t frameDeclarationEpoch,
										 TerraEngine::Scheduling::JobGraphContext& context);

		void PublishAnimationAuthorityStage(std::size_t animatorCount, std::uint64_t runtimeSessionEpoch,
											std::uint64_t frameDeclarationEpoch, TerraEngine::Scheduling::JobGraphContext& context);

		void CommitPublishedAnimationCacheStage(std::size_t animatorCount, std::uint64_t runtimeSessionEpoch,
												std::uint64_t frameDeclarationEpoch, TerraEngine::Scheduling::JobGraphContext& context);

		void FinalizePublishedAnimationStage(std::size_t animatorCount, std::uint64_t runtimeSessionEpoch,
											 std::uint64_t frameDeclarationEpoch, TerraEngine::Scheduling::JobGraphContext& context);
	};
} // namespace TerraEngine::Animation
