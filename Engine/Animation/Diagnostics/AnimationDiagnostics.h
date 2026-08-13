// AnimationDiagnostics.h - Bounded copied animation foundation diagnostics.
//
// Producers fill one fixed-size value snapshot at a protected publication
// boundary. Readers receive a copy, so editor, profiler, managed, and automation
// consumers never retain live animator storage or worker-owned scratch memory.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Runtime/AnimationFallback.h"
#include "Animation/Runtime/AnimationExecutionMode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::size_t kAnimationFallbackReasonCount =
		static_cast<std::size_t>(AnimationFallbackReason::InternalFailure) + 1U;
	inline constexpr std::string_view kAnimationFeatureIdentity = "animation";
	inline constexpr std::string_view kAnimationBaselineSourceCommit = "6d12e3a99d7c0d41899fc96fa18b02b61e5a0aa9";
	inline constexpr std::string_view kAnimationBaselineManifestSha256 = "cf4375934729e1afa745d9446faacfc1a7ba53a6d7db79b3d1e0e8164a5ced8f";
	inline constexpr std::string_view kAnimationBaselineEvidenceSha256 = "4020949bd881f94f0395853e947e4512fb587c7bdd1ce5592317e75c01b5fcb5";
	inline constexpr std::string_view kAnimationPerformanceWorkloadsSha256 = "a50c359f5dda932c0852eaa9a83c2e57cbbca3c60f038e975f7b67e0b17774f6";

	// Capture identity travels with every snapshot so a saved profiler result can
	// prove which immutable reference and threshold manifest it was compared to.
	// string_view is safe here because every value points at a program-lifetime
	// literal and keeps the summary record allocation-free and trivially copyable.
	struct AnimationBaselineIdentity {
		std::string_view feature = kAnimationFeatureIdentity;
		std::string_view sourceCommit = kAnimationBaselineSourceCommit;
		std::string_view manifestSha256 = kAnimationBaselineManifestSha256;
		std::string_view evidenceSha256 = kAnimationBaselineEvidenceSha256;
		std::string_view workloadsSha256 = kAnimationPerformanceWorkloadsSha256;
	};

	enum class AnimationDiagnosticsDetailLevel : std::uint8_t {
		Disabled = 0U,
		Summary = 1U,
		Detailed = 2U
	};

	// Foundation counters cover every subsystem that must remain inactive for a
	// static project. Later phases may append fields, but existing meanings and
	// units remain stable for profiler and automation consumers.
	struct AnimationDiagnosticsCounters {
		std::uint64_t activeAnimators = 0U;
		std::uint64_t evaluatedAnimators = 0U;
		std::uint64_t suspendedAnimators = 0U;
		std::uint64_t degradedAnimators = 0U;
		std::uint64_t schedulerMembershipChecks = 0U;
		std::uint64_t workerJobs = 0U;
		std::uint64_t transientAllocations = 0U;
		std::uint64_t poseAllocations = 0U;
		std::uint64_t residencyRequests = 0U;
		std::uint64_t gpuDispatches = 0U;
		std::uint64_t gpuUploadBytes = 0U;
		std::uint64_t gpuReadbacks = 0U;
		std::uint64_t gpuWaits = 0U;
		std::uint64_t physicsHandoffs = 0U;
		std::uint64_t managedCallbacks = 0U;
		std::uint64_t detailedRecords = 0U;
		std::uint64_t parameterQueueDepth = 0U;
		std::uint64_t commandQueueDepth = 0U;
		std::uint64_t eventQueueDepth = 0U;
		std::uint64_t rootMotionQueueDepth = 0U;
		std::uint64_t rejectedBatches = 0U;
		std::uint64_t staleWorkItems = 0U;
		// Mode counts and work volume distinguish real GPU offload from a silent
		// CPU fallback. They are counts, never percentages derived in the hot path.
		std::uint64_t gpuVisualAnimators = 0U;
		std::uint64_t hybridAuthoritativeAnimators = 0U;
		std::uint64_t cpuFallbackAnimators = 0U;
		std::uint64_t cpuSampledJoints = 0U;
		std::uint64_t gpuEvaluatedJoints = 0U;
		std::uint64_t gpuBatches = 0U;
		std::uint64_t gpuTransferBytes = 0U;
		std::uint64_t gpuReadbackBytes = 0U;
		std::uint64_t completePoseReadbackBytes = 0U;
		std::uint64_t boundedMetadataReadbackBytes = 0U;
		std::uint64_t residencyHits = 0U;
		std::uint64_t residencyMisses = 0U;
		std::uint64_t fallbackCount = 0U;
		std::uint64_t modeSwitchCount = 0U;
		// These fixed counters are published with the authority generation. They
		// avoid per-animator strings and make compact/complete accounting useful in
		// a recording without adding work while the profiler is idle.
		std::uint64_t compactEligibleAnimators = 0U;
		std::uint64_t compactProcessedAnimators = 0U;
		std::uint64_t completeCapturedAnimators = 0U;
		std::uint64_t completeProcessedAnimators = 0U;
		std::uint64_t compactToCompleteTransitions = 0U;
		std::uint64_t completeToCompactTransitions = 0U;
		std::uint64_t retainedTopologyHits = 0U;
		std::uint64_t retainedTopologyMisses = 0U;
		std::uint64_t compactPlanGroups = 0U;
		std::uint64_t compactDispatches = 0U;
		std::uint64_t rejectedCandidateCount = 0U;
		std::array<std::uint64_t, kAnimationFallbackReasonCount> fallbackReasonCounts{};
	};

	struct AnimationDiagnosticsTimings {
		double preparationMs = 0.0;
		double evaluationMs = 0.0;
		double publicationMs = 0.0;
		double presentationMs = 0.0;
		double cpuAuthorityMs = 0.0;
		double gpuPlanningMs = 0.0;
		double gpuExecutionMs = 0.0;
		double deformationMs = 0.0;
		double compactCaptureMs = 0.0;
		double compactEvaluationMs = 0.0;
		double compactPublicationMs = 0.0;
		double completeCaptureMs = 0.0;
		double completeEvaluationMs = 0.0;
		double completePublicationMs = 0.0;
		double topologyBuildMs = 0.0;
		double topologyRefreshMs = 0.0;
	};

	struct AnimationDiagnosticsSnapshot {
		std::uint32_t schemaVersion = 1U;
		std::uint64_t frameIndex = 0U;
		std::uint64_t candidateGeneration = 0U;
		std::uint64_t acceptedGeneration = 0U;
		std::uint64_t rejectedGeneration = 0U;
		std::uint64_t topologyGeneration = 0U;
		std::uint64_t presentationGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::uint64_t sceneGeneration = 0U;
		AnimationFixedTick fixedTick = 0;
		AnimationDiagnosticsDetailLevel detailLevel = AnimationDiagnosticsDetailLevel::Disabled;
		bool dormant = true;
		AnimationBaselineIdentity baseline;
		AnimationDiagnosticsCounters counters;
		AnimationDiagnosticsTimings timings;
	};

	// Pose residency is a copied diagnostic label, not a renderer resource handle.
	// The append-only values can be serialized by profiler/editor automation.
	enum class AnimationPoseResidencyState : std::uint8_t {
		Unavailable = 0U,
		CpuOnly = 1U,
		UploadPending = 2U,
		GpuResident = 3U,
		Retiring = 4U,
		Evicted = 5U
	};

	// Detailed records are opt-in and copied only at the protected diagnostics
	// boundary. They expose every FunctionalContract decision/work field without retaining a
	// live animator, renderer allocation, or producer-owned string.
	struct AnimationAnimatorDiagnosticRecord {
		std::uint64_t stableInstanceId = 0U;
		std::uint32_t instanceSlot = 0U;
		AnimationGeneration instanceGeneration = 0U;
		AnimationExecutionPolicy requestedPolicy = AnimationExecutionPolicy::Auto;
		AnimationEffectiveExecutionMode effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationExecutionModeReason selectionReason = AnimationExecutionModeReason::Uninitialized;
		AnimationExecutionModeReason fallbackReason = AnimationExecutionModeReason::Uninitialized;
		AnimationPoseResidencyState poseResidency = AnimationPoseResidencyState::Unavailable;
		std::uint32_t cpuAuthoritativeJointCount = 0U;
		std::uint32_t gpuStageMask = 0U;
		std::uint32_t batchOrdinal = 0U;
		std::uint64_t gpuEvaluatedElements = 0U;
		std::uint64_t transferBytes = 0U;
		std::uint64_t readbackBytes = 0U;
		std::uint64_t completePoseReadbackBytes = 0U;
		std::uint32_t readbackCount = 0U;
		std::uint32_t synchronizationWaitCount = 0U;
		std::uint32_t modeSwitchCount = 0U;
		std::uint32_t reserved = 0U;
	};

	struct AnimationModeSwitchDiagnosticRecord {
		std::uint64_t stableInstanceId = 0U;
		AnimationFixedTick fixedTick = 0;
		AnimationEffectiveExecutionMode from = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationEffectiveExecutionMode to = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationExecutionModeReason reason = AnimationExecutionModeReason::Uninitialized;
		std::uint64_t continuityFingerprint = 0U;
	};

	struct AnimationDetailedDiagnosticsSnapshot {
		std::uint32_t schemaVersion = 1U;
		std::uint64_t frameIndex = 0U;
		bool collapsed = true;
		std::vector<AnimationAnimatorDiagnosticRecord> animators;
		std::vector<AnimationModeSwitchDiagnosticRecord> modeSwitchHistory;
	};

	// A detailed lease represents one independent editor or automation consumer.
	// The global publisher counts leases rather than a Boolean so closing one UI
	// window cannot disable a concurrent MCP request. The move-only lifetime also
	// makes the final release deterministic: it clears retained detail rows before
	// another caller can mistake them for current opt-in evidence.
	class AnimationDetailedDiagnosticsRequest final {
	public:
		AnimationDetailedDiagnosticsRequest() noexcept = default;
		AnimationDetailedDiagnosticsRequest(const AnimationDetailedDiagnosticsRequest&) = delete;
		AnimationDetailedDiagnosticsRequest& operator=(const AnimationDetailedDiagnosticsRequest&) = delete;
		AnimationDetailedDiagnosticsRequest(AnimationDetailedDiagnosticsRequest&& other) noexcept;
		AnimationDetailedDiagnosticsRequest& operator=(AnimationDetailedDiagnosticsRequest&& other) noexcept;
		~AnimationDetailedDiagnosticsRequest();

		[[nodiscard]] bool IsActive() const noexcept { return active_; }
		void Reset() noexcept;

	private:
		explicit AnimationDetailedDiagnosticsRequest(bool active) noexcept : active_(active) {}
		bool active_ = false;

		friend TERRAENGINE_API AnimationDetailedDiagnosticsRequest
			AcquireAnimationDetailedDiagnosticsRequest();
	};

	// The dormant constructor deliberately reports the sole bounded membership
	// check and leaves every allocation/work/consumer counter at zero.
	[[nodiscard]] TERRAENGINE_API AnimationDiagnosticsSnapshot MakeDormantAnimationDiagnostics(
		std::uint64_t frameIndex,
		AnimationFixedTick fixedTick,
		std::uint64_t schedulerMembershipChecks = 1U) noexcept;

	// Validation occurs before publication. A contradictory dormant snapshot or
	// an over-limit detailed count is rejected without replacing the last valid
	// complete value, matching the engine's transactional publication rules.
	[[nodiscard]] TERRAENGINE_API bool ValidateAnimationDiagnosticsSnapshot(
		const AnimationDiagnosticsSnapshot& snapshot) noexcept;
	[[nodiscard]] TERRAENGINE_API bool PublishAnimationDiagnosticsSnapshot(
		const AnimationDiagnosticsSnapshot& snapshot);
	[[nodiscard]] TERRAENGINE_API std::optional<AnimationDiagnosticsSnapshot> AcquireAnimationDiagnosticsSnapshot();
	TERRAENGINE_API void ClearPublishedAnimationDiagnostics();

	// Detailed publication is separately opt-in so a collapsed profiler/editor
	// window never pays for per-animator vectors. Rejection retains the prior copy.
	[[nodiscard]] TERRAENGINE_API bool ValidateAnimationDetailedDiagnosticsSnapshot(
		const AnimationDetailedDiagnosticsSnapshot& snapshot) noexcept;
	[[nodiscard]] TERRAENGINE_API bool PublishAnimationDetailedDiagnosticsSnapshot(
		const AnimationDetailedDiagnosticsSnapshot& snapshot);
	[[nodiscard]] TERRAENGINE_API std::optional<AnimationDetailedDiagnosticsSnapshot>
		AcquireAnimationDetailedDiagnosticsSnapshot();
	// Production code checks this at its protected animation boundary before it
	// allocates per-animator records. Summary diagnostics intentionally do not use
	// this switch and therefore remain available at their usual low cost.
	[[nodiscard]] TERRAENGINE_API AnimationDetailedDiagnosticsRequest
		AcquireAnimationDetailedDiagnosticsRequest();
	[[nodiscard]] TERRAENGINE_API bool IsAnimationDetailedDiagnosticsRequested() noexcept;
	TERRAENGINE_API void ClearPublishedAnimationDetailedDiagnostics();
}
