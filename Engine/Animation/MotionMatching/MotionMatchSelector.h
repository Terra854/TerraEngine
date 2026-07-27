// MotionMatchSelector.h - CPU-authoritative selection from asynchronous top-K.

#pragma once

#include "Animation/MotionMatching/MotionMatchGpu.h"

namespace TerraEngine::Animation {
	enum class MotionMatchSelectionSource : std::uint8_t {
		None = 0U,
		GpuTopKCpuAuthoritative = 1U,
		CpuFullSearch = 2U
	};

	struct MotionMatchSelection {
		AnimationStatus status = AnimationStatus::NotReady;
		bool usedGpuCandidates = false;
		bool usedCpuFallback = false;
		MotionMatchCandidate chosen{};
		MotionMatchCpuResult authoritativeScores{};
		MotionMatchSelectionSource source = MotionMatchSelectionSource::None;
		MotionMatchFallbackReason fallbackReason = MotionMatchFallbackReason::None;
		AnimationStatus gpuStatus = AnimationStatus::NotReady;
		std::uint32_t gpuCandidateCount = 0U;
	};

	// GPU scoring narrows a large resident database to a bounded ID set. CPU then
	// revalidates every hard constraint, recomputes integer costs, and applies the
	// stable tie-break. Missing/stale/invalid GPU data falls back to the same full
	// CPU scorer used by deterministic and headless execution.
	TERRAENGINE_API AnimationStatus SelectMotionMatchAuthoritatively(
		const MotionDatabaseAsset& database, const MotionMatchQuery& query,
		const MotionMatchGpuReadback* gpuReadback, MotionMatchSelection& output) noexcept;
}
