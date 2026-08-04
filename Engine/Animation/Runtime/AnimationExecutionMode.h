// AnimationExecutionMode.h - Deterministic requested-to-effective mode policy.
//
// This module deliberately contains no renderer, scene, or wall-clock access.
// AnimationSystem can therefore resolve disjoint animator ranges on workers from
// immutable frame inputs and merge the value results in deterministic order.

#pragma once

#include "Animation/AnimationTypes.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace TerraEngine::Animation {
	// Effective values are append-only because copied diagnostics and future ABI
	// records retain the numeric identity even though authored content stores the
	// separate AnimationExecutionPolicy request.
	enum class AnimationEffectiveExecutionMode : std::uint8_t {
		GpuVisual = 0U,
		HybridAuthoritative = 1U,
		CpuDeterministicFallback = 2U
	};

	// Reasons are append-only diagnostic identities. The resolver uses an explicit
	// fail-safe branch order instead of relying on these numeric values, so future
	// reasons can be appended without changing captured diagnostics.
	enum class AnimationExecutionModeReason : std::uint8_t {
		Uninitialized = 0U,
		InvalidConsumerClosure = 1U,
		DeterministicRequired = 2U,
		RollbackRequired = 3U,
		HeadlessRequired = 4U,
		ValidationRequired = 5U,
		RendererUnavailable = 6U,
		GpuCapabilityUnavailable = 7U,
		GpuResourcesUnavailable = 8U,
		GpuEvaluationFailure = 9U,
		GpuBudgetUnavailable = 10U,
		ForceCpuRequested = 11U,
		CompletePoseRequired = 12U,
		ForceGpuUnsafe = 13U,
		ForceHybridMissingSubset = 14U,
		ForceHybridRequested = 15U,
		AuthoritativeSubsetRequired = 16U,
		ForceGpuRequested = 17U,
		GpuVisualEligible = 18U,
		PromotionHysteresis = 19U,
		MinimumResidence = 20U,
		InvalidRequestedPolicy = 21U,
		InvalidPreviousState = 22U
	};

	// The frozen skeleton hard limit also bounds temporary ancestor bookkeeping.
	// The expression reads the single source-of-truth v1 table rather than copying
	// the numeric 4,096 limit into a second policy definition.
	inline constexpr std::size_t kMaximumAnimationExecutionJoints = static_cast<std::size_t>(
		GetAnimationRuntimeLimit(AnimationLimit::SkeletonJoints).hardMaximum);
	inline constexpr std::size_t kMaximumAnimationExecutionModeBatchSize = 65536U;

	// Producers declare only the CPU-authoritative data they actually consume.
	// Spans are borrowed for the duration of one resolver call; the returned
	// closure owns a sorted copy and can safely outlive the frame-input vectors.
	struct AnimationConsumerRequirements {
		std::span<const std::uint32_t> requestedJoints;
		std::span<const std::int32_t> jointParents;
		bool curves = false;
		bool morphs = false;
		bool events = false;
		bool rootMotion = false;
		bool contacts = false;
		bool proceduralResults = false;
		bool physics = false;
		bool attachments = false;
		bool networking = false;
		bool rollback = false;
		bool replay = false;
		bool recording = false;
		// Automation can request authoritative observations outside normal gameplay
		// visibility, so it participates in mode safety just like other consumers.
		bool automation = false;
		bool diagnostics = false;
		bool completePose = false;
	};

	// The closure contains only bounded CPU work. completePose is retained as a
	// separate flag because it forces the complete CPU evaluator instead of being
	// misrepresented as an acceptable Hybrid subset.
	struct AnimationConsumerClosure {
		AnimationStatus status = AnimationStatus::Ok;
		std::vector<std::uint32_t> joints;
		bool curves = false;
		bool morphs = false;
		bool events = false;
		bool rootMotion = false;
		bool contacts = false;
		bool proceduralResults = false;
		bool physics = false;
		bool attachments = false;
		bool networking = false;
		bool rollback = false;
		bool replay = false;
		bool recording = false;
		bool automation = false;
		bool diagnostics = false;
		bool completePose = false;
		std::uint64_t fingerprint = 0U;

		[[nodiscard]] bool HasAuthoritativeSubset() const noexcept;
		auto operator<=>(const AnimationConsumerClosure&) const = default;
	};

	// GPU eligibility is captured as immutable typed facts. Budget units are
	// provider-neutral; they may represent bytes, arena pages, or a normalized
	// project budget as long as required and available use the same finite unit.
	struct AnimationGpuExecutionInputs {
		bool rendererAvailable = true;
		bool animationCapabilityAvailable = true;
		bool resourcesResident = true;
		bool evaluationHealthy = true;
		double requiredBudget = 0.0;
		double availableBudget = std::numeric_limits<double>::max();
	};

	// Promotions wait for both gates. Downgrades that add CPU authority or enter
	// fallback ignore these values because delaying them could violate correctness.
	struct AnimationExecutionModeStability {
		AnimationFixedTick promotionHysteresisTicks = 2;
		AnimationFixedTick minimumResidenceTicks = 4;
	};

	// This opaque value record carries every continuity identity owned by other
	// animation modules. The resolver never interprets or modifies these tokens;
	// it copies the current record atomically into a mode transition. These fields
	// explicitly cover controller parameters, synchronization phase, normalized
	// controller time, and the fixed tick at which evaluators may switch safely.
	struct AnimationModeContinuityToken {
		AnimationFixedTick transitionBoundaryTick = 0;
		AnimationFixedTick stateTimeTick = 0;
		float normalizedControllerTime = 0.0F;
		std::uint64_t controllerState = 0U;
		std::uint64_t parameterSnapshot = 0U;
		std::uint64_t transition = 0U;
		std::uint64_t synchronizationState = 0U;
		AnimationFixedTick synchronizationPhaseTick = 0;
		std::uint64_t eventWatermark = 0U;
		std::uint64_t rootMotion = 0U;
		std::uint64_t motionMatch = 0U;
		std::uint64_t ragdollBlend = 0U;
		std::uint64_t pose = 0U;

		auto operator<=>(const AnimationModeContinuityToken&) const = default;
	};

	struct AnimationExecutionModeRequest {
		AnimationFixedTick fixedTick = 0;
		AnimationExecutionPolicy requestedPolicy = AnimationExecutionPolicy::Auto;
		bool deterministicRequired = false;
		bool rollbackOrResimulation = false;
		bool headless = false;
		bool validationReference = false;
		AnimationConsumerRequirements consumers;
		AnimationGpuExecutionInputs gpu;
		AnimationExecutionModeStability stability;
		AnimationModeContinuityToken continuity;
	};

	// Callers persist this small value beside an animator instance. Candidate
	// fingerprinting resets hysteresis when the requested subset changes even if
	// its broad target mode remains Hybrid.
	struct AnimationExecutionModeState {
		bool initialized = false;
		AnimationEffectiveExecutionMode effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationExecutionModeReason effectiveReason = AnimationExecutionModeReason::Uninitialized;
		AnimationFixedTick modeEnteredTick = 0;
		bool promotionPending = false;
		AnimationEffectiveExecutionMode promotionMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationExecutionModeReason promotionReason = AnimationExecutionModeReason::Uninitialized;
		AnimationFixedTick promotionSinceTick = 0;
		std::uint64_t promotionFingerprint = 0U;
		AnimationModeContinuityToken continuity;

		auto operator<=>(const AnimationExecutionModeState&) const = default;
	};

	struct AnimationExecutionModeTransition {
		bool occurred = false;
		AnimationEffectiveExecutionMode from = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationEffectiveExecutionMode to = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationModeContinuityToken continuity;

		auto operator<=>(const AnimationExecutionModeTransition&) const = default;
	};

	struct AnimationExecutionModeDecision {
		AnimationEffectiveExecutionMode effectiveMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationExecutionModeReason reason = AnimationExecutionModeReason::Uninitialized;
		AnimationEffectiveExecutionMode eligibleMode = AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		AnimationExecutionModeReason eligibilityReason = AnimationExecutionModeReason::Uninitialized;
		AnimationConsumerClosure consumerClosure;
		bool modeChanged = false;
		AnimationExecutionModeTransition transition;
		AnimationExecutionModeState nextState;

		auto operator<=>(const AnimationExecutionModeDecision&) const = default;
	};

	struct AnimationExecutionModeBatchRequest {
		AnimationExecutionModeRequest request;
		AnimationExecutionModeState previousState;
	};

	struct AnimationExecutionModeBatchResult {
		AnimationStatus status = AnimationStatus::Ok;
		std::size_t resolvedCount = 0U;
	};

	[[nodiscard]] TERRAENGINE_API std::string_view ToString(AnimationEffectiveExecutionMode mode) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(AnimationExecutionModeReason reason) noexcept;

	// Root-motion policy describes what to do with a delta, but only an authored
	// track, interrupted transition, destination transition, or explicit consumer
	// can produce authoritative work. Keeping this decision pure lets import/runtime
	// callers share the same rule without consulting live scene or renderer state.
	[[nodiscard]] TERRAENGINE_API bool RequiresCpuRootMotionAuthority(
		AnimatorRootMotionPolicy policy,
		bool currentClipHasRootMotion,
		bool interruptedTransitionHasRootMotion,
		bool destinationTransitionHasRootMotion,
		bool explicitlyConsumed) noexcept;

	// All returned state depends only on the request and previousState. In
	// particular, this function does not consult clocks, globals, or provider state.
	[[nodiscard]] TERRAENGINE_API AnimationExecutionModeDecision ResolveAnimationExecutionMode(
		const AnimationExecutionModeRequest& request,
		const AnimationExecutionModeState& previousState);

	// A zero-length span is the dormant fast path: stale outputs are cleared while
	// vector capacity is retained, and no per-animator resolution or allocation is
	// performed. Larger batches remain explicitly bounded before reserve/allocation.
	[[nodiscard]] TERRAENGINE_API AnimationExecutionModeBatchResult ResolveAnimationExecutionModes(
		std::span<const AnimationExecutionModeBatchRequest> requests,
		std::vector<AnimationExecutionModeDecision>& output);
}
