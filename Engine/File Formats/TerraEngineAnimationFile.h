// TerraEngineAnimationFile.h - Versioned common envelope for cooked animation assets.
//
// Every animation asset family uses this small, bounded outer envelope. Concrete
// asset chunks may evolve independently, while readers reject incompatible major
// versions and skip unknown optional chunks without exposing partial data.

#pragma once

#include "Animation/AnimationTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	using AnimationContentHash = std::array<std::uint8_t, 32U>;
	constexpr std::uint32_t MakeAnimationFourCC(char first, char second, char third, char fourth) noexcept {
		return static_cast<std::uint32_t>(first)
			| (static_cast<std::uint32_t>(second) << 8U)
			| (static_cast<std::uint32_t>(third) << 16U)
			| (static_cast<std::uint32_t>(fourth) << 24U);
	}

	inline constexpr std::uint32_t kAnimationAssetMagic = MakeAnimationFourCC('T', 'E', 'A', 'N');
	inline constexpr std::uint16_t kAnimationAssetMajorVersion = 1U;
	inline constexpr std::uint16_t kAnimationAssetMinorVersion = 0U;
	inline constexpr std::uint32_t kAnimationLittleEndianMarker = 0x01020304U;
	inline constexpr std::uint32_t kAnimationChunkFlagRequired = 1U << 0U;
	inline constexpr std::uint32_t kAnimationChunkMetadata = MakeAnimationFourCC('M', 'E', 'T', 'A');
	inline constexpr std::uint32_t kAnimationChunkPayload = MakeAnimationFourCC('D', 'A', 'T', 'A');
	inline constexpr std::uint32_t kAnimationChunkStreamData = MakeAnimationFourCC('S', 'T', 'R', 'M');

	#pragma pack(push, 1)
	// The fixed prefix is copied as bytes, never reinterpreted from untrusted input.
	// Its explicit fields permit tools to validate every later range before allocating.
	struct AnimationAssetEnvelope {
		std::uint32_t magic = kAnimationAssetMagic;
		std::uint16_t majorVersion = kAnimationAssetMajorVersion;
		std::uint16_t minorVersion = kAnimationAssetMinorVersion;
		// Writers fill this with sizeof(AnimationAssetEnvelope) after construction.
		std::uint32_t headerSize = 0U;
		std::uint32_t endianMarker = kAnimationLittleEndianMarker;
		std::uint64_t payloadSize = 0U;
		AnimationContentHash contentHash{};
		AnimationGuid assetId{};
		std::uint64_t dependencyTableOffset = 0U;
		std::uint32_t dependencyCount = 0U;
		std::uint32_t chunkCount = 0U;
		std::uint64_t chunkTableOffset = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
	};

	// A dependency includes expected type and generation so a loader cannot bind a
	// compatible name to an incompatible replacement asset generation.
	struct AnimationDependencyEntry {
		AnimationGuid assetId{};
		std::uint32_t expectedAssetType = 0U;
		AnimationGeneration expectedGeneration = 0U;
	};

	// A chunk range is absolute from the start of the file. The required flag is
	// intentionally explicit so a newer optional chunk remains safely skippable.
	struct AnimationChunkEntry {
		std::uint32_t type = 0U;
		std::uint32_t flags = 0U;
		std::uint64_t offset = 0U;
		std::uint64_t size = 0U;
	};
	#pragma pack(pop)

	static_assert(sizeof(AnimationAssetEnvelope) == 104U, "Animation v1 envelope layout is append-only.");
	static_assert(sizeof(AnimationDependencyEntry) == 24U, "Animation v1 dependency layout is append-only.");
	static_assert(sizeof(AnimationChunkEntry) == 24U, "Animation v1 chunk layout is append-only.");

	constexpr bool CheckedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
		if(right > UINT64_MAX - left) {
			return false;
		}
		result = left + right;
		return true;
	}

	constexpr bool CheckedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t& result) noexcept {
		if(left != 0U && right > UINT64_MAX / left) {
			return false;
		}
		result = left * right;
		return true;
	}

	// ValidateRange performs all serialized arithmetic in 64-bit before any span
	// slicing or allocation. It is intentionally reusable by future chunk readers.
	TERRAENGINE_API AnimationStatus ValidateAnimationRange(
		std::uint64_t containerSize,
		std::uint64_t offset,
		std::uint64_t size) noexcept;

	TERRAENGINE_API AnimationContentHash ComputeAnimationContentHash(std::span<const std::uint8_t> bytes) noexcept;

	class TERRAENGINE_API AnimationAssetWriter {
	public:
		bool AddDependency(AnimationDependencyEntry dependency);
		bool AddChunk(std::uint32_t type, bool required, std::span<const std::uint8_t> bytes);
		AnimationStatus Finish(AnimationAssetEnvelope envelope, std::vector<std::uint8_t>& output) const;

	private:
		struct PendingChunk {
			std::uint32_t type = 0U;
			bool required = false;
			std::vector<std::uint8_t> bytes;
		};

		std::vector<AnimationDependencyEntry> dependencies_;
		std::vector<PendingChunk> chunks_;
	};

	class TERRAENGINE_API AnimationAssetReader {
	public:
		AnimationStatus Parse(std::span<const std::uint8_t> input);

		const AnimationAssetEnvelope& Envelope() const noexcept { return envelope_; }
		std::span<const AnimationDependencyEntry> Dependencies() const noexcept { return dependencies_; }
		std::span<const AnimationChunkEntry> Chunks() const noexcept { return chunks_; }
		std::span<const std::uint8_t> FindChunk(std::uint32_t type) const noexcept;

	private:
		AnimationAssetEnvelope envelope_{};
		std::vector<AnimationDependencyEntry> dependencies_;
		std::vector<AnimationChunkEntry> chunks_;
		std::vector<std::uint8_t> storage_;
	};
}
