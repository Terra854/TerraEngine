// AnimationClipView.h - Immutable resident clip view and checked sample addressing.
//
// Animation jobs never follow streaming state or mutable asset tables directly.
// A view captures one validated clip generation, an optional compatible reference
// skeleton, and the segment-residency snapshot visible to the current frame.

#pragma once

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Codec/AnimationCodec.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint32_t kInvalidAnimationClipSegment =
		std::numeric_limits<std::uint32_t>::max();

	// Sampling reports where a value came from so diagnostics can distinguish a
	// normal omitted channel from a temporarily unavailable streamed segment.
	enum class AnimationChannelSampleSource : std::uint8_t {
		Track = 0U,
		ReferencePose = 1U,
		DefaultValue = 2U
	};

	// resolvedTick is always in the closed [0, duration] interval for non-looping
	// playback and in [0, duration) for a positive-duration looping clip.
	struct AnimationResolvedClipTime {
		AnimationFixedTick requestedTick = 0;
		AnimationFixedTick resolvedTick = 0;
		std::int64_t loopIndex = 0;
		bool looped = false;
		bool clampedToStart = false;
		bool clampedToEnd = false;
		bool exactEndBoundary = false;

		auto operator<=>(const AnimationResolvedClipTime&) const = default;
	};

	struct AnimationClipSampleRequest {
		std::uint64_t stableTargetId = 0U;
		AnimationClipTrackKind kind = AnimationClipTrackKind::Translation;
		AnimationFixedTick tick = 0;
		bool loop = false;
	};

	struct AnimationClipChannelSample {
		AnimationDecodedValue value{};
		AnimationChannelSampleSource source = AnimationChannelSampleSource::DefaultValue;
		std::uint32_t segmentIndex = kInvalidAnimationClipSegment;
	};

	// Resolve time without consulting mutable playback state. AnimatorPlayback can
	// therefore use the exact same floor-division semantics for forward, reverse,
	// seek, and multi-loop catch-up operations.
	TERRAENGINE_API AnimationStatus ResolveAnimationClipTime(
		AnimationFixedTick durationTicks,
		AnimationFixedTick requestedTick,
		bool loop,
		AnimationResolvedClipTime& output) noexcept;

	class TERRAENGINE_API AnimationClipView {
	public:
		// An empty residency span means every segment in this immutable asset
		// generation is resident. Otherwise one checked 0/1 byte is required per
		// segment; the bytes are copied so worker jobs never borrow mutable state.
		// A non-null streamedPayloads set changes that empty-span convention: the
		// caller must provide one residency byte per segment and only published
		// payload bytes are eligible for decode.
		AnimationStatus Open(
			const AnimationClipAsset& asset,
			const SkeletonAsset* referenceSkeleton = nullptr,
			std::span<const std::uint8_t> residentSegments = {},
			AnimationGeneration streamedGeneration = 0U,
			std::shared_ptr<const AnimationClipSegmentPayloadSet> streamedPayloads = {}) noexcept;
		void Close() noexcept;

		AnimationStatus ResolveTime(
			AnimationFixedTick requestedTick,
			bool loop,
			AnimationResolvedClipTime& output) const noexcept;
		AnimationStatus Sample(
			const AnimationClipSampleRequest& request,
			AnimationClipChannelSample& output) const noexcept;
		AnimationStatus SampleResolved(
			std::uint64_t stableTargetId,
			AnimationClipTrackKind kind,
			const AnimationResolvedClipTime& time,
			AnimationClipChannelSample& output) const noexcept;

		[[nodiscard]] bool IsOpen() const noexcept { return asset_ != nullptr; }
		[[nodiscard]] bool IsSegmentResident(std::uint32_t segmentIndex) const noexcept;
		[[nodiscard]] const AnimationClipAsset* Asset() const noexcept { return asset_; }
		[[nodiscard]] const SkeletonAsset* ReferenceSkeleton() const noexcept { return referenceSkeleton_; }
		[[nodiscard]] std::span<const AnimationClipEvent> Events() const noexcept;

	private:
		AnimationStatus BuildFallback(
			std::uint64_t stableTargetId,
			AnimationClipTrackKind kind,
			AnimationClipChannelSample& output) const noexcept;
		std::uint32_t FindSegment(AnimationFixedTick resolvedTick) const noexcept;

		const AnimationClipAsset* asset_ = nullptr;
		const SkeletonAsset* referenceSkeleton_ = nullptr;
		AnimationCodecView codec_{};
		std::vector<std::uint8_t> residentSegments_;
		std::shared_ptr<const AnimationClipSegmentPayloadSet> streamedPayloads_;
	};
}
