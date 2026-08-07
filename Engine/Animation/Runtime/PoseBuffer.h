// PoseBuffer.h - Worker-owned animation output storage with immutable publication.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Assets/AnimationAssetTypes.h"
#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Animation {
	// Scratch is intentionally bounded even though it is never published. The
	// limit prevents malformed programs from turning one worker packet into an
	// unbounded allocation request.
	inline constexpr std::size_t kAnimationPoseScratchHardMaximumBytes = 64U * 1024U * 1024U;

	enum class AnimationPlaybackDirection : std::int8_t {
		Reverse = -1,
		Stationary = 0,
		Forward = 1
	};

	// Named float channels cover both morph weights and gameplay-readable custom
	// curves. stableId, rather than a display string, is the runtime lookup key.
	struct AnimationNamedFloatValue {
		std::uint64_t stableId = 0U;
		float value = 0.0F;

		auto operator<=>(const AnimationNamedFloatValue&) const = default;
	};

	// The complete identity is retained across mode changes and rollback. A
	// direction is metadata, not part of identity, so replaying the same authored
	// occurrence cannot accidentally expose a second one-shot result.
	struct AnimationEventIdentity {
		AnimatorInstanceHandle animator;
		std::uint64_t stateInstanceId = 0U;
		AnimationGuid clipAssetId{};
		std::uint64_t eventId = 0U;
		AnimationFixedTick sourceTick = 0;
		std::int64_t loopCount = 0;

		auto operator<=>(const AnimationEventIdentity&) const = default;
	};

	struct AnimationEventRecord {
		AnimationEventIdentity identity;
		AnimationEventPayloadType payloadType = AnimationEventPayloadType::None;
		std::string payload;
		std::uint64_t sourceStateId = 0U;
		std::uint32_t layerIndex = 0U;
		AnimationPlaybackDirection direction = AnimationPlaybackDirection::Stationary;
		std::uint64_t correlationId = 0U;
		std::uint64_t producerOrder = 0U;
		bool historical = false;

		auto operator<=>(const AnimationEventRecord&) const = default;
	};

	// Root motion is an immutable request/result input at this phase. Later
	// physics and scene integration consumes this copied value through typed
	// queues; worker evaluation never mutates a live transform or Jolt body.
	struct AnimationRootMotionDelta {
		AnimatorInstanceHandle animator;
		AnimationFixedTick sourceBeginTick = 0;
		AnimationFixedTick sourceEndTick = 0;
		AnimationFloat3 localTranslation{};
		AnimationFloat3 worldTranslation{};
		float localYawRadians = 0.0F;
		float worldYawRadians = 0.0F;
		std::int64_t accumulatedCycles = 0;
		std::uint64_t sourceStateId = 0U;
		std::uint32_t layerIndex = 0U;
		AnimationGeneration generation = 0U;
		AnimatorRootMotionPolicy policy = AnimatorRootMotionPolicy::ExtractOnly;

		auto operator<=>(const AnimationRootMotionDelta&) const = default;
	};

	// Every published pose records all generations needed to reject stale worker
	// output at the protected Animation publication boundary.
	struct AnimationPoseGeneration {
		AnimatorInstanceHandle animator;
		AnimationGeneration poseGeneration = 0U;
		AnimationAssetHandle skeleton;
		AnimationGeneration programGeneration = 0U;
		AnimationFixedTick tick = 0;

		auto operator<=>(const AnimationPoseGeneration&) const = default;
	};

	struct PoseBufferLayout {
		std::size_t jointCount = 0U;
		std::size_t morphCount = 0U;
		std::size_t curveCount = 0U;
		std::size_t eventCapacity = 0U;
		std::size_t scratchBytes = 0U;

		auto operator<=>(const PoseBufferLayout&) const = default;
	};

	// Visual-only crowd followers retain their own animator generation while
	// sharing this sealed transform/channel payload with an exact-state leader.
	// Events, root motion, and write scratch remain per PoseBuffer because they
	// can carry instance authority and therefore must never be aliased.
	struct PoseBufferVisualPayload {
		std::vector<AnimationTransform> localTransforms;
		std::vector<AnimationTransform> modelTransforms;
		std::vector<AnimationNamedFloatValue> morphValues;
		std::vector<AnimationNamedFloatValue> curveValues;
	};

	// A PoseBuffer has a short worker-owned write phase followed by a permanent
	// sealed phase. Public readers receive only const spans, and every mutating
	// accessor returns an empty span once Seal succeeds.
	class TERRAENGINE_API PoseBuffer {
	public:
		AnimationStatus BeginWrite(PoseBufferLayout layout, AnimationPoseGeneration generation);
		std::span<AnimationTransform> MutableLocalTransforms() noexcept;
		std::span<AnimationTransform> MutableModelTransforms() noexcept;
		std::span<AnimationNamedFloatValue> MutableMorphValues() noexcept;
		std::span<AnimationNamedFloatValue> MutableCurveValues() noexcept;
		std::span<std::byte> MutableScratch() noexcept;
		AnimationStatus AppendEvent(AnimationEventRecord event);
		AnimationStatus SetRootMotion(AnimationRootMotionDelta rootMotion);
		AnimationStatus Seal() noexcept;
		// Creates a sealed visual-only follower without copying local/model/palette
		// inputs. The caller must first prove that no events or root motion are
		// present and supplies the follower's distinct publication generation.
		[[nodiscard]] AnimationStatus ShareSealedVisualPayloadFrom(
			const PoseBuffer& source,
			AnimationPoseGeneration generation) noexcept;

		[[nodiscard]] bool IsSealed() const noexcept { return sealed_; }
		[[nodiscard]] AnimationStatus ValidateForPublication() const noexcept;
		[[nodiscard]] const PoseBufferLayout& Layout() const noexcept { return layout_; }
		[[nodiscard]] const AnimationPoseGeneration& Generation() const noexcept { return generation_; }
		[[nodiscard]] std::span<const AnimationTransform> LocalTransforms() const noexcept;
		[[nodiscard]] std::span<const AnimationTransform> ModelTransforms() const noexcept;
		[[nodiscard]] std::span<const AnimationNamedFloatValue> MorphValues() const noexcept;
		[[nodiscard]] std::span<const AnimationNamedFloatValue> CurveValues() const noexcept;
		[[nodiscard]] std::span<const AnimationEventRecord> Events() const noexcept { return events_; }
		[[nodiscard]] const std::optional<AnimationRootMotionDelta>& RootMotionDelta() const noexcept { return rootMotion_; }

	private:
		PoseBufferLayout layout_{};
		AnimationPoseGeneration generation_{};
		std::shared_ptr<PoseBufferVisualPayload> visualPayload_;
		std::vector<AnimationEventRecord> events_;
		std::vector<std::byte> scratch_;
		std::optional<AnimationRootMotionDelta> rootMotion_;
		bool sealed_ = false;
	};
}
