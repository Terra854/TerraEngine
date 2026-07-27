// MotionMatchCpu.h - Bounded deterministic reference scoring for motion search.

#pragma once

#include "Animation/Assets/MotionDatabaseAsset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::size_t kMotionMatchMaximumDiagnosticExclusions = 256U;

	enum class MotionMatchExclusionReason : std::uint32_t {
		None = 0U,
		MissingRequiredTags = 1U << 0U,
		ContainsExcludedTags = 1U << 1U,
		MissingRequiredContacts = 1U << 2U,
		PhaseOutsideWindow = 1U << 3U,
		ExplicitSample = 1U << 4U,
		DatabaseExclusionRegion = 1U << 5U,
		MissingRequiredTransitionFlags = 1U << 6U,
		ContainsExcludedTransitionFlags = 1U << 7U
	};

	constexpr MotionMatchExclusionReason operator|(
		MotionMatchExclusionReason left, MotionMatchExclusionReason right) noexcept {
		return static_cast<MotionMatchExclusionReason>(
			static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}

	constexpr MotionMatchExclusionReason& operator|=(
		MotionMatchExclusionReason& left, MotionMatchExclusionReason right) noexcept {
		left = left | right;
		return left;
	}

	constexpr bool HasMotionMatchExclusionReason(MotionMatchExclusionReason value,
		MotionMatchExclusionReason flag) noexcept {
		return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0U;
	}

	enum class MotionMatchFallbackReason : std::uint8_t {
		None = 0U,
		EmptyDatabase = 1U,
		NoEligibleCandidates = 2U,
		CandidateBudgetExhausted = 3U,
		GpuUnavailable = 4U,
		GpuNotReady = 5U,
		GpuCancelled = 6U,
		StaleGpuResult = 7U,
		GpuBackendFailure = 8U,
		InvalidGpuCandidates = 9U
	};

	struct MotionMatchQuery {
		AnimationAssetHandle database{};
		std::uint64_t expectedSchemaFingerprint = 0U;
		MotionDatabaseQuantizedFeatureVector features{};
		std::array<std::uint16_t, kMotionDatabaseFeatureCount> featureWeights{};
		std::uint64_t requiredTags = 0U;
		std::uint64_t excludedTags = 0U;
		std::uint32_t requiredContacts = 0U;
		std::uint16_t preferredPhase = 0U;
		std::uint16_t maximumPhaseDistance = UINT16_MAX;
		std::uint32_t preferredContinuityGroup = 0U;
		std::uint32_t continuityPenalty = 0U;
		std::uint64_t lastSampleId = 0U;
		std::uint64_t lastSourceClipId = 0U;
		std::uint32_t sameClipPenalty = 0U;
		std::size_t maximumCandidates = kMotionDatabaseTopKDefault;
		std::span<const std::uint64_t> excludedSampleIds{};

		// These fields complete transition/exclusion semantics while defaulting to
		// the behavior expected by older callers that initialize only the prefix.
		std::uint16_t requiredTransitionFlags = 0U;
		std::uint16_t excludedTransitionFlags = 0U;
		std::uint16_t preferredTransitionFlags = 0U;
		std::uint16_t reserved = 0U;
		std::uint32_t excludedRegionFlags = 0U;
		std::uint64_t preferredTags = 0U;
		std::uint32_t preferredContacts = 0U;
		std::uint32_t phaseWeight = 1U;
		std::uint32_t missingPreferredTagPenalty = 0U;
		std::uint32_t contactMismatchPenalty = 0U;
		std::uint32_t transitionMismatchPenalty = 0U;
		std::uint32_t continuityBreakPenalty = 0U;
		std::size_t maximumSamplesToVisit = kMotionDatabaseMaximumSamples;
		std::size_t maximumDiagnosticExclusions = 32U;
	};

	struct MotionMatchScoreBreakdown {
		std::array<std::uint64_t, kMotionDatabaseFeatureCount> perFeatureCosts{};
		std::array<std::uint64_t, kMotionDatabaseFeatureBlockCount> featureBlockCosts{};
		std::uint64_t featureCost = 0U;
		std::uint64_t phaseCost = 0U;
		std::uint64_t tagCost = 0U;
		std::uint64_t contactCost = 0U;
		std::uint64_t continuityCost = 0U;
		std::uint64_t transitionCost = 0U;
		std::uint64_t totalCost = 0U;

		[[nodiscard]] std::uint64_t FeatureBlockCost(
			MotionDatabaseFeatureSemantic semantic) const noexcept {
			const std::size_t index = static_cast<std::size_t>(semantic);
			return index < featureBlockCosts.size() ? featureBlockCosts[index] : 0U;
		}

		auto operator<=>(const MotionMatchScoreBreakdown&) const = default;
	};

	struct MotionMatchCandidate {
		std::uint64_t stableSampleId = 0U;
		std::uint64_t sourceClipId = 0U;
		AnimationFixedTick sourceTick = 0;
		MotionMatchScoreBreakdown score{};

		auto operator<=>(const MotionMatchCandidate&) const = default;
	};

	struct MotionMatchExcludedCandidate {
		std::uint64_t stableSampleId = 0U;
		MotionMatchExclusionReason reasons = MotionMatchExclusionReason::None;

		auto operator<=>(const MotionMatchExcludedCandidate&) const = default;
	};

	struct MotionMatchCpuResult {
		AnimationStatus status = AnimationStatus::NotReady;
		bool usedFallback = false;
		bool candidateBudgetExhausted = false;
		MotionMatchFallbackReason fallbackReason = MotionMatchFallbackReason::None;
		std::uint32_t visitedSamples = 0U;
		std::uint32_t evaluatedSamples = 0U;
		std::uint32_t excludedSamples = 0U;
		std::array<std::uint32_t, 8U> exclusionReasonCounts{};
		std::vector<MotionMatchExcludedCandidate> exclusionDetails;
		std::vector<MotionMatchCandidate> candidates;
	};

	TERRAENGINE_API MotionMatchExclusionReason ComputeMotionMatchExclusionReasons(
		const MotionDatabaseSample& sample, const MotionMatchQuery& query) noexcept;
	TERRAENGINE_API AnimationStatus ScoreMotionMatchCpu(
		const MotionDatabaseAsset& database,
		const MotionMatchQuery& query,
		MotionMatchCpuResult& output) noexcept;
	// The GPU path supplies at most top-K IDs. CPU revalidates and re-scores this
	// bounded set before selection, retaining authority without a full database scan.
	TERRAENGINE_API AnimationStatus ScoreMotionMatchCpuSubset(
		const MotionDatabaseAsset& database,
		const MotionMatchQuery& query,
		std::span<const std::uint64_t> candidateIds,
		MotionMatchCpuResult& output) noexcept;
	TERRAENGINE_API bool IsMotionMatchCandidateBetter(
		const MotionMatchCandidate& left,
		const MotionMatchCandidate& right) noexcept;
	TERRAENGINE_API std::string_view ToString(MotionMatchFallbackReason reason) noexcept;
}
