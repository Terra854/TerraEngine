// MotionMatchDiagnostics.h - Bounded copied query-result history.

#pragma once

#include "Animation/MotionMatching/MotionMatchSelector.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::size_t kMotionMatchDiagnosticExclusionDetailLimit = 32U;

	struct MotionMatchDiagnosticRecord {
		// Keep this three-field prefix stable for existing diagnostic producers.
		MotionMatchGpuGeneration generation{};
		AnimationFixedTick tick = 0;
		AnimationStatus status = AnimationStatus::NotReady;
		bool usedFallback = false;
		bool usedGpuCandidates = false;
		bool usedCpuFallback = false;
		bool candidateBudgetExhausted = false;
		MotionMatchSelectionSource selectionSource = MotionMatchSelectionSource::None;
		MotionMatchFallbackReason fallbackReason = MotionMatchFallbackReason::None;
		AnimationStatus gpuStatus = AnimationStatus::NotReady;
		AnimationGeneration databaseGeneration = 0U;
		std::uint32_t visitedSamples = 0U;
		std::uint32_t evaluatedSamples = 0U;
		std::uint32_t excludedSamples = 0U;
		std::array<std::uint32_t, 8U> exclusionReasonCounts{};
		std::uint32_t retainedExclusionDetails = 0U;
		std::array<MotionMatchExcludedCandidate,
			kMotionMatchDiagnosticExclusionDetailLimit> exclusionDetails{};
		// chosen includes selected sample/source clip/source tick plus total,
		// per-feature, per-block, and metadata component costs.
		MotionMatchCandidate chosen{};
	};

	struct MotionMatchDiagnosticsSnapshot {
		std::uint32_t schemaVersion = 2U;
		std::uint64_t evictedRecords = 0U;
		std::vector<MotionMatchDiagnosticRecord> records;
	};

	TERRAENGINE_API MotionMatchDiagnosticRecord MakeMotionMatchDiagnosticRecord(
		const MotionMatchGpuGeneration& generation, AnimationFixedTick tick,
		const MotionMatchSelection& selection) noexcept;

	class TERRAENGINE_API MotionMatchDiagnostics {
	public:
		[[nodiscard]] AnimationStatus Configure(std::size_t capacity) noexcept;
		[[nodiscard]] AnimationStatus Publish(const MotionMatchDiagnosticRecord& record) noexcept;
		[[nodiscard]] std::optional<MotionMatchDiagnosticsSnapshot> AcquireCopy() const noexcept;

	private:
		mutable std::mutex mutex_;
		std::size_t capacity_ = 0U;
		std::size_t nextWriteIndex_ = 0U;
		std::uint64_t evicted_ = 0U;
		std::vector<MotionMatchDiagnosticRecord> records_;
	};
}
