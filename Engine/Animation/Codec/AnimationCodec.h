// AnimationCodec.h - Checked CPU decoder for the shared segmented clip bit contract.
//
// Each track range is independently addressable. Runtime CPU sampling, validation,
// and the later GPU decoder therefore consume the same immutable byte layout.

#pragma once

#include "Animation/Assets/AnimationClipAsset.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint32_t kAnimationCodecTrackMagic = MakeAnimationFourCC('A', 'C', 'T', '1');
	inline constexpr std::uint16_t kAnimationCodecVersion = 1U;
	inline constexpr std::uint8_t kAnimationCodecConstantTrack = 1U << 0U;
	inline constexpr std::uint32_t kAnimationCodecScalarBits = 16U;
	inline constexpr std::uint32_t kAnimationCodecVectorBits = 48U;
	inline constexpr std::uint32_t kAnimationCodecQuaternionBits = 50U;

	enum class AnimationCodecValueEncoding : std::uint8_t {
		ScalarUnorm16 = 0U,
		Vector3Unorm16 = 1U,
		QuaternionSmallestThree16 = 2U
	};

	#pragma pack(push, 1)
	// The header keeps all decode parameters beside one track. Constant tracks
	// need no following sample arrays, while animated tracks append time offsets
	// followed by fixed-stride encoded values.
	struct AnimationCodecTrackHeaderV1 {
		std::uint32_t magic = kAnimationCodecTrackMagic;
		std::uint16_t version = kAnimationCodecVersion;
		AnimationCodecValueEncoding encoding = AnimationCodecValueEncoding::ScalarUnorm16;
		std::uint8_t flags = 0U;
		std::uint32_t byteSize = 0U;
		std::uint32_t keyCount = 0U;
		AnimationFixedTick segmentStartTick = 0;
		AnimationFixedTick segmentEndTick = 0;
		AnimationFloat3 rangeMinimum{};
		AnimationFloat3 rangeExtent{};
		AnimationFloat3 constantVector{};
		AnimationQuaternion constantRotation{};
		float constantScalar = 0.0F;
	};

	struct AnimationCodecVectorKeyV1 {
		std::uint16_t x = 0U;
		std::uint16_t y = 0U;
		std::uint16_t z = 0U;
	};

	struct AnimationCodecQuaternionKeyV1 {
		std::int16_t component0 = 0;
		std::int16_t component1 = 0;
		std::int16_t component2 = 0;
		std::uint8_t largestComponent = 0U;
		std::uint8_t reserved = 0U;
	};
	#pragma pack(pop)

	struct AnimationDecodedValue {
		AnimationFloat3 vectorValue{};
		AnimationQuaternion rotationValue{};
		float scalarValue = 0.0F;
	};

	// One payload owns the exact immutable bytes published for a cooked segment.
	// streamOffset remains the clip-local offset used by track metadata; decoding
	// translates it to a checked offset inside bytes instead of consulting the
	// mutable/full-resident AnimationClipAsset::streamData cache.
	struct AnimationClipSegmentPayload {
		std::uint32_t segmentIndex = 0U;
		std::uint64_t streamOffset = 0U;
		std::uint64_t streamSize = 0U;
		std::uint64_t contentFingerprint = 0U;
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
	};

	// A set is one protected streaming publication for exactly one asset
	// generation. Holding the set pins every resident segment through worker
	// evaluation even when a later boundary publishes, cancels, or evicts it.
	struct AnimationClipSegmentPayloadSet {
		AnimationGuid assetId{};
		AnimationGeneration generation = 0U;
		std::uint64_t publicationSequence = 0U;
		std::vector<AnimationClipSegmentPayload> segments;

		[[nodiscard]] const AnimationClipSegmentPayload* Find(
			std::uint32_t segmentIndex) const noexcept;
	};

	// A view borrows an immutable clip generation. Open validates every addressable
	// resident byte range up front so Decode never follows corrupt offsets.
	class TERRAENGINE_API AnimationCodecView {
	public:
		AnimationStatus Open(const AnimationClipAsset& asset) noexcept;
		// A non-null payload set makes streamed bytes authoritative. Missing
		// segments remain NotReady; the decoder never falls through to streamData.
		AnimationStatus Open(
			const AnimationClipAsset& asset,
			AnimationGeneration expectedGeneration,
			std::shared_ptr<const AnimationClipSegmentPayloadSet> streamedPayloads) noexcept;
		AnimationStatus Decode(
			std::uint64_t stableTargetId,
			AnimationClipTrackKind kind,
			AnimationFixedTick tick,
			bool loop,
			AnimationDecodedValue& output) const noexcept;

		bool IsOpen() const noexcept { return asset_ != nullptr; }

	private:
		const AnimationClipAsset* asset_ = nullptr;
		std::shared_ptr<const AnimationClipSegmentPayloadSet> streamedPayloads_;
	};
}
