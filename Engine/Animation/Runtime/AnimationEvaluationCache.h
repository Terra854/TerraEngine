// AnimationEvaluationCache.h - Generation-complete immutable evaluation reuse.
//
// Cache keys intentionally repeat every authority and subsystem generation that
// can change an animation result. Omitting one would permit a mixed-generation
// pose, morph packet, renderer publication, or deterministic observation.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	struct AnimationEvaluationCacheKey {
		AnimatorInstanceHandle animator;
		AnimationFixedTick evaluationTick = 0;
		std::uint64_t stateGeneration = 0U;
		std::uint64_t parameterGeneration = 0U;
		std::uint64_t consumerGeneration = 0U;
		std::uint64_t executionModeGeneration = 0U;
		std::uint64_t poseGeneration = 0U;
		std::uint64_t morphGeneration = 0U;
		std::uint64_t graphGeneration = 0U;
		std::uint64_t viewGeneration = 0U;
		std::uint64_t assetGeneration = 0U;
		std::uint64_t residencyGeneration = 0U;
		std::uint64_t materialGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::uint64_t physicsGeneration = 0U;
		std::uint64_t deterministicGeneration = 0U;

		auto operator<=>(const AnimationEvaluationCacheKey&) const = default;
	};

	// Tokens identify immutable caller-owned pose and visual publications. The
	// cache shares this small value record; it never exposes mutable scene storage.
	struct AnimationEvaluationCacheValue {
		std::uint64_t sourceFrameGeneration = 0U;
		std::uint64_t publicationGeneration = 0U;
		std::uint64_t poseToken = 0U;
		std::uint64_t visualPacketToken = 0U;
		std::uint64_t deterministicHash = 0U;
	};

	// Visibility is a visual scheduling hint, not authority. Any consumer below
	// can force evaluation and protected publication while an animator is hidden.
	struct AnimationEvaluationCacheRequirements {
		bool visible = true;
		bool timeAdvances = false;
		bool events = false;
		bool rootMotion = false;
		bool physics = false;
		bool recording = false;
		bool replication = false;
		bool rollback = false;
		bool deterministic = false;
		bool automation = false;
		bool detailedDiagnostics = false;

		[[nodiscard]] bool HasAuthoritativeWork() const noexcept;
	};

	enum class AnimationEvaluationCacheGenerationDomain : std::uint8_t {
		Pose = 0U,
		Morph = 1U,
		Graph = 2U,
		View = 3U,
		Asset = 4U,
		Residency = 5U,
		Material = 6U,
		Renderer = 7U,
		Physics = 8U,
		Deterministic = 9U
	};

	enum class AnimationEvaluationCacheDisposition : std::uint8_t {
		Reused = 0U,
		Miss = 1U,
		RequiredWork = 2U,
		InvalidKey = 3U
	};

	struct AnimationEvaluationCacheLookup {
		AnimationStatus status = AnimationStatus::NotReady;
		AnimationEvaluationCacheDisposition disposition = AnimationEvaluationCacheDisposition::Miss;
		std::shared_ptr<const AnimationEvaluationCacheValue> value;
		bool requiresEvaluation = false;
		bool requiresPublication = false;
		bool requiresUpload = false;
		bool requiresVisibilityInvalidation = false;
		bool requiresDetailedDiagnostics = false;
		std::size_t requestIndex = 0U;
	};

	struct AnimationEvaluationCacheLookupRequest {
		AnimationEvaluationCacheKey key;
		AnimationEvaluationCacheRequirements requirements;
	};

	struct AnimationEvaluationCacheBatchSummary {
		AnimationStatus status = AnimationStatus::Ok;
		std::size_t requestCount = 0U;
		std::size_t reusedCount = 0U;
		std::size_t requiredWorkCount = 0U;
		std::size_t missCount = 0U;
	};

	struct AnimationEvaluationCacheConfiguration {
		std::size_t maximumEntries = 4096U;
		std::size_t maximumBatchRequests = 65536U;
		// Watermarks are smaller than cached payloads and survive payload eviction.
		// This bound prevents unbounded metadata while retaining late-store defense
		// until AnimatorInstanceStore destruction explicitly invalidates a handle.
		std::size_t maximumAnimatorWatermarks = 65536U;
	};

	// The cache is internally synchronized for worker lookup/store traffic. A hit
	// returns a shared immutable value and does not allocate a replacement entry.
	class TERRAENGINE_API AnimationEvaluationCache {
	public:
		AnimationEvaluationCache();
		explicit AnimationEvaluationCache(AnimationEvaluationCacheConfiguration configuration);
		~AnimationEvaluationCache();

		AnimationEvaluationCache(const AnimationEvaluationCache&) = delete;
		AnimationEvaluationCache& operator=(const AnimationEvaluationCache&) = delete;
		AnimationEvaluationCache(AnimationEvaluationCache&&) noexcept;
		AnimationEvaluationCache& operator=(AnimationEvaluationCache&&) noexcept;

		[[nodiscard]] AnimationStatus Store(
			const AnimationEvaluationCacheKey& key,
			const AnimationEvaluationCacheValue& value);
		[[nodiscard]] AnimationEvaluationCacheLookup Lookup(
			const AnimationEvaluationCacheKey& key,
			const AnimationEvaluationCacheRequirements& requirements);
		[[nodiscard]] AnimationEvaluationCacheBatchSummary LookupBatch(
			std::span<const AnimationEvaluationCacheLookupRequest> requests,
			std::vector<AnimationEvaluationCacheLookup>& output);

		[[nodiscard]] std::size_t InvalidateGeneration(
			AnimationEvaluationCacheGenerationDomain domain,
			std::uint64_t generation);
		[[nodiscard]] std::size_t InvalidateAnimator(AnimatorInstanceHandle animator);
		void InvalidateAll();
		[[nodiscard]] std::size_t Size() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
