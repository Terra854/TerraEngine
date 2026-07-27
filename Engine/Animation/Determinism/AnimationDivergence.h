// AnimationDivergence.h - First-difference localization for deterministic diagnostics.

#pragma once

#include "Animation/Determinism/AnimationNetworkPackets.h"

namespace TerraEngine::Animation {
	inline constexpr std::size_t kAnimationDivergenceDefaultSnapshots =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(
			AnimationLimit::DetailedDiagnosticHistoryFrames).operationalDefault);
	inline constexpr std::size_t kAnimationDivergenceMaximumSnapshots =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(
			AnimationLimit::DetailedDiagnosticHistoryFrames).hardMaximum);

	struct AnimationDivergenceConfiguration {
		std::size_t maximumSnapshots = kAnimationDivergenceDefaultSnapshots;
		auto operator<=>(const AnimationDivergenceConfiguration&) const = default;
	};

	struct AnimationDivergenceResult {
		AnimationStatus status = AnimationStatus::NotReady;
		AnimationDivergencePoint point{};
		std::size_t comparedSnapshots = 0U;
		std::size_t retentionLimit = 0U;
		auto operator<=>(const AnimationDivergenceResult&) const = default;
	};

	class TERRAENGINE_API AnimationDivergenceAnalyzer final {
	public:
		explicit AnimationDivergenceAnalyzer(
			AnimationDivergenceConfiguration configuration = {}) noexcept;

		[[nodiscard]] AnimationStatus Analyze(
			std::span<const AnimationRollbackSnapshot> expected,
			std::span<const AnimationRollbackSnapshot> actual,
			AnimationDivergenceResult& output) const noexcept;

		// Compatibility helper returns an empty point for invalid or over-limit input.
		[[nodiscard]] AnimationDivergencePoint FindFirstDifference(
			std::span<const AnimationRollbackSnapshot> expected,
			std::span<const AnimationRollbackSnapshot> actual) const noexcept;

	private:
		AnimationDivergenceConfiguration configuration_;
	};
}
