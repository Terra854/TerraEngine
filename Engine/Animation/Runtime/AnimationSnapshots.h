// AnimationSnapshots.h - Copied immutable gameplay/render animation state.

#pragma once

#include "Animation/Runtime/AnimationCommandQueue.h"
#include "Animation/Runtime/PoseBuffer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace TerraEngine::Animation {
	struct AnimationLayerSnapshot {
		std::uint32_t layerIndex = 0U;
		std::uint64_t stateId = 0U;
		float weight = 0.0F;
		AnimationFixedTick stateTick = 0;

		auto operator<=>(const AnimationLayerSnapshot&) const = default;
	};

	struct AnimationParameterSnapshot {
		std::uint32_t parameterId = 0U;
		AnimationParameterType type = AnimationParameterType::Float;
		AnimationParameterValue value;

		auto operator<=>(const AnimationParameterSnapshot&) const = default;
	};

	struct AnimationPlaybackSnapshot {
		std::uint64_t currentStateId = 0U;
		std::uint64_t previousStateId = 0U;
		float transitionProgress = 0.0F;
		float normalizedTime = 0.0F;
		std::int64_t loopCount = 0;
		float effectiveSpeed = 0.0F;
		AnimationStatus latestStatus = AnimationStatus::NotReady;
		std::vector<AnimationLayerSnapshot> activeLayers;
		std::vector<AnimationParameterSnapshot> parameters;

		auto operator<=>(const AnimationPlaybackSnapshot&) const = default;
	};

	struct AnimationSnapshot {
		AnimatorInstanceHandle animator;
		AnimationGeneration publicationGeneration = 0U;
		AnimationFixedTick tick = 0;
		AnimationPlaybackSnapshot playback;
		std::shared_ptr<const PoseBuffer> pose;
	};

	// A slot is read lock-free by render/gameplay consumers. Publication is a
	// complete pointer swap after all generation and finite-value checks pass.
	class TERRAENGINE_API AnimationSnapshotPublisher {
	public:
		AnimationStatus Publish(AnimationSnapshot snapshot, const AnimatorInstanceStore& store);
		[[nodiscard]] std::shared_ptr<const AnimationSnapshot> Load() const noexcept;
		void Clear() noexcept;

	private:
		std::atomic<std::shared_ptr<const AnimationSnapshot>> latest_{};
	};
}
