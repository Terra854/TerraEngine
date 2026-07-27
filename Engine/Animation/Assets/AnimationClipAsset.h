// AnimationClipAsset.h - Source-independent segmented animation clip contract.
//
// Runtime and GPU evaluators consume the same immutable tables. The public
// records describe identity and checked byte ranges only; codec-specific bits
// remain opaque until AnimationCodec validates and decodes a resident segment.

#pragma once

#include "Animation/Assets/AnimationAssetTypes.h"
#include "Animation/AnimationTypes.h"
#include "File Formats/TerraEngineAnimationFile.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint16_t kAnimationClipAssetLegacyVersion = 0U;
	inline constexpr std::uint16_t kAnimationClipAssetCurrentVersion = 1U;
	inline constexpr std::uint32_t kAnimationClipPayloadMagic = MakeAnimationFourCC('C', 'L', 'I', 'P');

	enum class AnimationClipTrackKind : std::uint8_t {
		Translation = 0U,
		Rotation = 1U,
		Scale = 2U,
		MorphWeight = 3U,
		Curve = 4U,
		RootTranslation = 5U,
		RootYaw = 6U
	};

	enum class AnimationInterpolation : std::uint8_t {
		Constant = 0U,
		Step = 1U,
		Linear = 2U,
		Cubic = 3U
	};

	enum class AnimationEventPayloadType : std::uint8_t {
		None = 0U,
		String = 1U,
		Integer = 2U,
		Float = 3U,
		Boolean = 4U
	};

	struct AnimationClipSegment {
		AnimationFixedTick startTick = 0;
		AnimationFixedTick endTick = 0;
		std::uint64_t streamOffset = 0U;
		std::uint64_t streamSize = 0U;
		std::uint32_t firstTrack = 0U;
		std::uint32_t trackCount = 0U;

		auto operator<=>(const AnimationClipSegment&) const = default;
	};

	struct AnimationClipTrack {
		std::uint64_t stableTargetId = 0U;
		AnimationClipTrackKind kind = AnimationClipTrackKind::Translation;
		AnimationInterpolation interpolation = AnimationInterpolation::Constant;
		std::uint32_t segmentIndex = 0U;
		std::uint64_t sampleOffset = 0U;
		std::uint32_t sampleCount = 0U;
		std::uint32_t encodedValueBits = 0U;

		auto operator<=>(const AnimationClipTrack&) const = default;
	};

	struct AnimationClipNamedChannel {
		std::uint64_t stableId = 0U;
		std::string name;

		auto operator<=>(const AnimationClipNamedChannel&) const = default;
	};

	struct AnimationClipEvent {
		std::uint64_t stableEventId = 0U;
		AnimationFixedTick tick = 0;
		AnimationEventPayloadType payloadType = AnimationEventPayloadType::None;
		std::string payload;
		bool fireForward = true;
		bool fireReverse = true;

		auto operator<=>(const AnimationClipEvent&) const = default;
	};

	struct AnimationClipRootMotion {
		bool enabled = false;
		std::uint32_t translationTrackIndex = 0U;
		std::uint32_t yawTrackIndex = 0U;
		AnimationFloat3 accumulatedCycleTranslation{};
		float accumulatedCycleYawRadians = 0.0F;

		auto operator<=>(const AnimationClipRootMotion&) const = default;
	};

	struct AnimationClipCompressionMetadata {
		std::uint32_t decoderVersion = 0U;
		float translationErrorMeters = 0.0F;
		float rotationErrorDegrees = 0.0F;
		float scaleErrorFraction = 0.0F;
		float morphError = 0.0F;

		auto operator<=>(const AnimationClipCompressionMetadata&) const = default;
	};

	// Recording metadata is an optional, append-only META chunk. Ordinary imported
	// clips omit it, so adding editor provenance does not change their runtime
	// payload, decode path, or format version.
	enum class AnimationClipRecordingClock : std::uint8_t {
		FixedSimulation = 0U,
		ScaledGameTime = 1U,
		UnscaledGameTime = 2U,
		EditorPreview = 3U,
		Manual = 4U
	};

	enum class AnimationClipRecordedChannelKind : std::uint8_t {
		BoneTranslation = 0U,
		BoneRotation = 1U,
		BoneScale = 2U,
		MorphWeight = 3U,
		Curve = 4U,
		Event = 5U,
		RootTranslation = 6U,
		RootYaw = 7U
	};

	struct AnimationClipRecordedChannel {
		// channelId identifies this selected recorder lane. targetId identifies the
		// bone, morph, curve, or event target used by the cooked runtime tables.
		std::uint64_t stableChannelId = 0U;
		std::uint64_t stableTargetId = 0U;
		std::string name;
		AnimationClipRecordedChannelKind kind = AnimationClipRecordedChannelKind::Curve;
		AnimationInterpolation interpolation = AnimationInterpolation::Linear;

		auto operator<=>(const AnimationClipRecordedChannel&) const = default;
	};

	struct AnimationClipRecordingReductionSettings {
		float translationToleranceMeters = 0.0F;
		float rotationToleranceDegrees = 0.0F;
		float scaleToleranceFraction = 0.0F;
		float morphTolerance = 0.0F;
		float curveTolerance = 0.0F;

		auto operator<=>(const AnimationClipRecordingReductionSettings&) const = default;
	};

	struct AnimationClipRecordingProvenance {
		AnimationGuid sourceAnimatorId{};
		// Controller identity is optional because a live source may be direct-clip.
		AnimationGuid sourceControllerAssetId{};
		std::string sourceSceneIdentity;
		std::string sourceSessionIdentity;
		AnimationFixedTick sourceFirstTick = 0;
		AnimationFixedTick sourceLastTick = 0;
		std::uint32_t sourceSampleRate = 0U;
		AnimationClipRecordingClock clock = AnimationClipRecordingClock::FixedSimulation;
		std::uint64_t capturedSampleCount = 0U;
		std::uint64_t capturedEventCount = 0U;
		std::uint64_t sourceKeyCount = 0U;
		std::uint64_t retainedKeyCount = 0U;
		std::uint64_t removedKeyCount = 0U;
		AnimationClipRecordingReductionSettings reduction{};
		std::vector<AnimationClipRecordedChannel> channels;

		auto operator<=>(const AnimationClipRecordingProvenance&) const = default;
	};

	struct AnimationClipAsset {
		AnimationGuid assetId{};
		// This is a ResourceManager publication generation, not authoring data.
		// Keeping it out of the cooked file lets a same-GUID hot reload retire
		// worker snapshots without changing durable asset identity.
		AnimationGeneration runtimeGeneration = 0U;
		AnimationGuid skeletonAssetId{};
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint16_t formatVersion = kAnimationClipAssetCurrentVersion;
		AnimationFixedTick durationTicks = 0;
		std::uint32_t sampleRate = kAnimationFixedTicksPerSecond;
		std::vector<AnimationClipSegment> segments;
		std::vector<AnimationClipTrack> tracks;
		std::vector<AnimationClipNamedChannel> namedChannels;
		std::vector<AnimationClipEvent> events;
		AnimationClipRootMotion rootMotion{};
		AnimationClipCompressionMetadata compression{};
		std::optional<AnimationClipRecordingProvenance> recordingProvenance;
		std::vector<std::uint8_t> streamData;

		auto operator<=>(const AnimationClipAsset&) const = default;
	};

	#pragma pack(push, 1)
	// These disk records are public so binary-layout tests and shader-layout
	// checks can pin the contract before content is produced at scale.
	struct AnimationClipAssetFileHeaderV1 {
		std::uint32_t magic = kAnimationClipPayloadMagic;
		std::uint16_t formatVersion = kAnimationClipAssetCurrentVersion;
		std::uint16_t headerSize = 0U;
		AnimationGuid skeletonAssetId{};
		AnimationFixedTick durationTicks = 0;
		std::uint32_t sampleRate = 0U;
		std::uint32_t segmentCount = 0U;
		std::uint32_t trackCount = 0U;
		std::uint32_t namedChannelCount = 0U;
		std::uint32_t eventCount = 0U;
		std::uint64_t segmentTableOffset = 0U;
		std::uint64_t trackTableOffset = 0U;
		std::uint64_t namedChannelTableOffset = 0U;
		std::uint64_t eventTableOffset = 0U;
		std::uint64_t stringTableOffset = 0U;
		std::uint64_t stringTableSize = 0U;
		std::uint64_t streamDataOffset = 0U;
		std::uint64_t streamDataSize = 0U;
		std::uint32_t rootMotionFlags = 0U;
		std::uint32_t rootTranslationTrackIndex = 0U;
		std::uint32_t rootYawTrackIndex = 0U;
		AnimationFloat3 accumulatedCycleTranslation{};
		float accumulatedCycleYawRadians = 0.0F;
		AnimationClipCompressionMetadata compression{};
	};
	#pragma pack(pop)

	TERRAENGINE_API AnimationStatus ValidateAnimationClipAsset(const AnimationClipAsset& asset) noexcept;
	TERRAENGINE_API AnimationStatus SerializeAnimationClipAsset(
		const AnimationClipAsset& asset,
		std::vector<std::uint8_t>& output);
	TERRAENGINE_API AnimationStatus DeserializeAnimationClipAsset(
		std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		AnimationClipAsset& output);
}
