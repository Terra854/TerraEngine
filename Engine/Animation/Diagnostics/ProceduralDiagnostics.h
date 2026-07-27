// ProceduralDiagnostics.h - Bounded copied IK/procedural diagnostic history.

#pragma once

#include "Animation/Procedural/ProceduralTypes.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::size_t kProceduralDiagnosticRecordHardMaximum =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(
			AnimationLimit::DetailedDiagnosticHistoryFrames).hardMaximum);

	struct ProceduralDiagnosticsCounters {
		std::size_t size = 0U;
		std::size_t capacity = 0U;
		std::size_t highWaterRecords = 0U;
		std::uint64_t publishedBatches = 0U;
		std::uint64_t publishedRecords = 0U;
		std::uint64_t evictedRecords = 0U;
		std::uint64_t rejectedBatches = 0U;
	};

	struct ProceduralDiagnosticsSnapshot {
		std::uint32_t schemaVersion = 1U;
		ProceduralGenerationStamp generation;
		ProceduralDiagnosticsCounters counters;
		std::vector<ProceduralDiagnosticRecord> records;
	};

	[[nodiscard]] TERRAENGINE_API bool ValidateProceduralDiagnosticRecord(
		const ProceduralDiagnosticRecord& record) noexcept;

	// Configure allocates both buffers once. Publication composes into the spare
	// pre-reserved vector and swaps atomically, so a protected boundary cannot show
	// only the first records of a solver batch.
	class TERRAENGINE_API ProceduralDiagnosticsBuffer {
	public:
		[[nodiscard]] AnimationStatus Configure(std::size_t capacity) noexcept;
		[[nodiscard]] AnimationStatus Publish(
			const ProceduralGenerationStamp& generation,
			std::span<const ProceduralDiagnosticRecord> records) noexcept;
		[[nodiscard]] std::optional<ProceduralDiagnosticsSnapshot> AcquireCopy() const noexcept;
		void Clear() noexcept;

	private:
		mutable std::mutex mutex_;
		std::vector<ProceduralDiagnosticRecord> records_;
		std::vector<ProceduralDiagnosticRecord> staging_;
		ProceduralGenerationStamp generation_;
		ProceduralDiagnosticsCounters counters_;
		bool configured_ = false;
	};
}
