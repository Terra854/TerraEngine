// MotionDatabaseAsset.h - Immutable, quantized motion-search data.
//
// The cooked database uses one fixed 48-lane schema. Named blocks make those
// lanes understandable to tools while fixed-width CPU/GPU records let the
// reference scorer and renderer consume the same integer values.

#pragma once

#include "Animation/AnimationTypes.h"
#include "File Formats/TerraEngineAnimationFile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint16_t kMotionDatabaseAssetCurrentVersion = 1U;
	inline constexpr std::uint32_t kMotionDatabasePayloadMagic =
		MakeAnimationFourCC('M', 'M', 'D', 'B');
	inline constexpr std::uint32_t kMotionDatabaseExtensionMagic =
		MakeAnimationFourCC('M', 'M', 'X', '1');
	inline constexpr std::uint16_t kMotionDatabaseExtensionVersion = 1U;
	inline constexpr std::size_t kMotionDatabaseFeatureCount = 48U;
	inline constexpr std::size_t kMotionDatabaseFeatureBlockCount = 9U;
	inline constexpr std::size_t kMotionDatabaseTopKDefault = 32U;
	inline constexpr std::size_t kMotionDatabaseMaximumTopK = 256U;
	inline constexpr std::size_t kMotionDatabaseMaximumSamples =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(
			AnimationLimit::MotionDatabaseSamples).hardMaximum);
	inline constexpr std::size_t kMotionDatabaseSamplesPerBlockDefault = 4096U;
	inline constexpr std::size_t kMotionDatabaseMaximumResidentBlocks = 4096U;
	inline constexpr std::size_t kMotionDatabaseMaximumPayloadBytes = 512U * 1024U * 1024U;
	inline constexpr std::uint32_t kMotionDatabaseCpuSampleStrideBytes = 160U;
	inline constexpr std::uint32_t kMotionDatabaseGpuSampleStrideBytes = 256U;

	// These numeric values are shared with MotionMatchScore.comp.hlsl. Append new
	// semantics only by creating a new database schema version and parity tests.
	enum class MotionDatabaseFeatureSemantic : std::uint8_t {
		Trajectory = 0U,
		Pose = 1U,
		Velocity = 2U,
		Facing = 3U,
		Contact = 4U,
		Phase = 5U,
		Tag = 6U,
		Transition = 7U,
		Continuity = 8U
	};

	inline constexpr std::uint16_t kMotionFeatureBlockCpuResident = 1U << 0U;
	inline constexpr std::uint16_t kMotionFeatureBlockGpuResident = 1U << 1U;
	inline constexpr std::uint16_t kMotionFeatureBlockFilterMetadata = 1U << 2U;
	inline constexpr std::uint32_t kMotionStorageCpuResident = 1U << 0U;
	inline constexpr std::uint32_t kMotionStorageGpuResident = 1U << 1U;
	inline constexpr std::uint32_t kMotionStorageStreamableBlocks = 1U << 2U;

	struct MotionDatabaseFeatureBlock {
		MotionDatabaseFeatureSemantic semantic = MotionDatabaseFeatureSemantic::Trajectory;
		std::uint8_t reserved = 0U;
		std::uint16_t firstFeature = 0U;
		std::uint16_t featureCount = 0U;
		std::uint16_t flags = kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident;

		auto operator<=>(const MotionDatabaseFeatureBlock&) const = default;
	};

	// Three trajectory horizons use x/z position plus x/z facing. Six pose points
	// and three velocity points then leave explicit facing/contact/phase/tag/
	// transition/continuity lanes. The ranges exactly cover [0, 48).
	inline constexpr std::array<MotionDatabaseFeatureBlock, kMotionDatabaseFeatureBlockCount>
		kMotionDatabaseDefaultFeatureBlocks{{
			{MotionDatabaseFeatureSemantic::Trajectory, 0U, 0U, 12U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident},
			{MotionDatabaseFeatureSemantic::Pose, 0U, 12U, 18U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident},
			{MotionDatabaseFeatureSemantic::Velocity, 0U, 30U, 9U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident},
			{MotionDatabaseFeatureSemantic::Facing, 0U, 39U, 3U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident},
			{MotionDatabaseFeatureSemantic::Contact, 0U, 42U, 2U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident
					| kMotionFeatureBlockFilterMetadata},
			{MotionDatabaseFeatureSemantic::Phase, 0U, 44U, 1U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident
					| kMotionFeatureBlockFilterMetadata},
			{MotionDatabaseFeatureSemantic::Tag, 0U, 45U, 1U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident
					| kMotionFeatureBlockFilterMetadata},
			{MotionDatabaseFeatureSemantic::Transition, 0U, 46U, 1U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident
					| kMotionFeatureBlockFilterMetadata},
			{MotionDatabaseFeatureSemantic::Continuity, 0U, 47U, 1U,
				kMotionFeatureBlockCpuResident | kMotionFeatureBlockGpuResident
					| kMotionFeatureBlockFilterMetadata}
		}};

	struct MotionDatabaseStorageLayout {
		std::uint32_t schemaVersion = 1U;
		std::uint32_t samplesPerBlock = static_cast<std::uint32_t>(
			kMotionDatabaseSamplesPerBlockDefault);
		std::uint32_t cpuSampleStrideBytes = kMotionDatabaseCpuSampleStrideBytes;
		std::uint32_t gpuSampleStrideBytes = kMotionDatabaseGpuSampleStrideBytes;
		std::uint32_t maximumTopK = static_cast<std::uint32_t>(kMotionDatabaseMaximumTopK);
		std::uint32_t flags = kMotionStorageCpuResident | kMotionStorageGpuResident
			| kMotionStorageStreamableBlocks;

		auto operator<=>(const MotionDatabaseStorageLayout&) const = default;
	};

	// A block describes both the compact CPU bytes and expanded GPU upload bytes.
	// Offsets are relative to their respective resident sample buffers, not files.
	struct MotionDatabaseResidentBlock {
		std::uint32_t blockIndex = 0U;
		std::uint32_t firstSample = 0U;
		std::uint32_t sampleCount = 0U;
		std::uint32_t flags = kMotionStorageCpuResident | kMotionStorageGpuResident;
		// The CPU byte range is relative to both the conceptual resident CPU
		// buffer and the optional contiguous STRM chunk. A loader adds the checked
		// STRM chunk file offset before asking the shared IO path for this block.
		std::uint64_t cpuByteOffset = 0U;
		std::uint64_t cpuByteSize = 0U;
		std::uint64_t gpuByteOffset = 0U;
		std::uint64_t gpuByteSize = 0U;
		std::uint64_t contentFingerprint = 0U;

		auto operator<=>(const MotionDatabaseResidentBlock&) const = default;
	};

	// Values are normalized by the compiler and quantized before publication.
	// int16 avoids platform float comparison differences during authoritative
	// selection while retaining sufficient resolution for trajectory features.
	struct MotionDatabaseQuantizedFeatureVector {
		std::array<std::int16_t, kMotionDatabaseFeatureCount> values{};

		auto operator<=>(const MotionDatabaseQuantizedFeatureVector&) const = default;
	};

	// The field order intentionally forms the compact 160-byte CPU resident
	// record. Serialization still writes fields explicitly so padding can never
	// become authored data.
	struct MotionDatabaseSample {
		std::uint64_t stableSampleId = 0U;
		std::uint64_t sourceClipId = 0U;
		AnimationFixedTick sourceTick = 0;
		MotionDatabaseQuantizedFeatureVector features{};
		std::uint64_t tagMask = 0U;
		std::uint32_t contactMask = 0U;
		std::uint16_t phaseQuantized = 0U;
		std::uint16_t transitionFlags = 0U;
		std::uint32_t continuityGroup = 0U;
		std::uint32_t exclusionFlags = 0U;
		std::uint64_t continuityPredecessorId = 0U;
		std::uint64_t continuitySuccessorId = 0U;

		auto operator<=>(const MotionDatabaseSample&) const = default;
	};

	// HLSL expands int16 features to int32 storage lanes. This mirror freezes the
	// exact 256-byte upload stride checked by shader-layout tests and backends.
	struct MotionDatabaseGpuSample {
		std::uint64_t stableSampleId = 0U;
		std::uint64_t sourceClipId = 0U;
		AnimationFixedTick sourceTick = 0;
		std::array<std::int32_t, kMotionDatabaseFeatureCount> features{};
		std::uint64_t tagMask = 0U;
		std::uint32_t contactMask = 0U;
		std::uint16_t phaseQuantized = 0U;
		std::uint16_t transitionFlags = 0U;
		std::uint32_t continuityGroup = 0U;
		std::uint32_t exclusionFlags = 0U;
		std::uint64_t continuityPredecessorId = 0U;
		std::uint64_t continuitySuccessorId = 0U;

		auto operator<=>(const MotionDatabaseGpuSample&) const = default;
	};

	static_assert(std::is_standard_layout_v<MotionDatabaseSample>);
	static_assert(sizeof(MotionDatabaseSample) == kMotionDatabaseCpuSampleStrideBytes,
		"Motion database CPU layout is an append-only 160-byte contract.");
	static_assert(sizeof(MotionDatabaseGpuSample) == kMotionDatabaseGpuSampleStrideBytes,
		"Motion database GPU layout is an append-only 256-byte contract.");

	struct MotionDatabaseAsset {
		AnimationGuid assetId{};
		AnimationGeneration runtimeGeneration = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint64_t sourceFingerprint = 0U;
		std::uint64_t featureSchemaFingerprint = 0U;
		std::uint16_t formatVersion = kMotionDatabaseAssetCurrentVersion;
		std::uint16_t featureCount = static_cast<std::uint16_t>(kMotionDatabaseFeatureCount);
		std::array<float, kMotionDatabaseFeatureCount> quantizationScales{};
		std::array<float, kMotionDatabaseFeatureCount> quantizationOffsets{};
		std::array<MotionDatabaseFeatureBlock, kMotionDatabaseFeatureBlockCount> featureBlocks =
			kMotionDatabaseDefaultFeatureBlocks;
		MotionDatabaseStorageLayout storageLayout{};
		std::vector<MotionDatabaseResidentBlock> residentBlocks;
		std::vector<MotionDatabaseSample> samples;

		auto operator<=>(const MotionDatabaseAsset&) const = default;
	};

	// Resolve returns this small immutable description instead of exposing the
	// envelope reader. The absolute range is safe to pass directly to bounded
	// loose-file or package IO, while the DATA fingerprint remains the authority
	// used when the returned bytes are decoded.
	struct MotionDatabaseStreamBlockRange {
		std::uint32_t blockIndex = 0U;
		std::uint32_t firstSample = 0U;
		std::uint32_t sampleCount = 0U;
		std::uint64_t absoluteByteOffset = 0U;
		std::uint64_t byteSize = 0U;
		// This is the DATA sample fingerprint consumed by checked decode. It is
		// intentionally not the streaming coordinator's independent raw-byte hash.
		std::uint64_t contentFingerprint = 0U;

		auto operator<=>(const MotionDatabaseStreamBlockRange&) const = default;
	};

	TERRAENGINE_API std::string_view ToString(MotionDatabaseFeatureSemantic semantic) noexcept;
	TERRAENGINE_API const MotionDatabaseFeatureBlock* FindMotionDatabaseFeatureBlock(
		const MotionDatabaseAsset& asset, MotionDatabaseFeatureSemantic semantic) noexcept;
	TERRAENGINE_API MotionDatabaseGpuSample ExpandMotionDatabaseSampleForGpu(
		const MotionDatabaseSample& sample) noexcept;
	// Rebuilding operates on an unpublished staged asset. Runtime databases remain
	// immutable, while compilers and serializers can deterministically derive the
	// complete block table before their protected publication boundary.
	TERRAENGINE_API AnimationStatus RebuildMotionDatabaseStorageMetadata(
		MotionDatabaseAsset& asset) noexcept;
	TERRAENGINE_API std::uint64_t ComputeMotionDatabaseSchemaFingerprint(
		const MotionDatabaseAsset& asset) noexcept;
	TERRAENGINE_API std::uint64_t ComputeMotionDatabaseContentFingerprint(
		const MotionDatabaseAsset& asset) noexcept;
	TERRAENGINE_API AnimationStatus ValidateMotionDatabaseAsset(
		const MotionDatabaseAsset& asset) noexcept;
	// Encode and decode operate on exactly one DATA-declared block. They stage
	// their vector outputs and publish only after size, fingerprint, and sample
	// parity checks pass, so a failed stream read cannot replace last-good data.
	TERRAENGINE_API AnimationStatus EncodeMotionDatabaseStreamBlock(
		const MotionDatabaseAsset& authoritativeAsset,
		std::uint32_t blockIndex,
		std::vector<std::uint8_t>& output);
	TERRAENGINE_API AnimationStatus DecodeMotionDatabaseStreamBlock(
		const MotionDatabaseAsset& authoritativeAsset,
		std::uint32_t blockIndex,
		std::span<const std::uint8_t> encodedBlock,
		std::vector<MotionDatabaseSample>& output);
	// STRM is optional for legacy/v1 files. When present, every byte must describe
	// the same samples and block fingerprints as authoritative DATA.
	TERRAENGINE_API AnimationStatus ValidateMotionDatabaseStreamChunk(
		const MotionDatabaseAsset& authoritativeAsset,
		std::span<const std::uint8_t> streamChunk) noexcept;
	// Unsupported means the otherwise-valid legacy asset has no optional STRM
	// chunk. All other failures leave output untouched.
	TERRAENGINE_API AnimationStatus ResolveMotionDatabaseStreamBlockRange(
		std::span<const std::uint8_t> serializedAsset,
		std::uint32_t blockIndex,
		MotionDatabaseStreamBlockRange& output) noexcept;
	TERRAENGINE_API AnimationStatus SerializeMotionDatabaseAsset(
		const MotionDatabaseAsset& asset, std::vector<std::uint8_t>& output);
	TERRAENGINE_API AnimationStatus DeserializeMotionDatabaseAsset(
		std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		MotionDatabaseAsset& output);
}
