// SkeletonAsset.h - Checked, deterministic cooked skeleton representation.
//
// Skeleton display labels are intentionally separate from semantic identifiers:
// artists may reuse a friendly label, while runtime references must remain stable
// through source-file reordering and reimport.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Assets/AnimationAssetTypes.h"
#include "File Formats/TerraEngineAnimationFile.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint16_t kSkeletonAssetLegacyVersion = 0U;
	inline constexpr std::uint16_t kSkeletonAssetCurrentVersion = 1U;
	inline constexpr std::uint32_t kSkeletonAssetMagic = MakeAnimationFourCC('S', 'K', 'E', 'L');

	// Source axes are persisted for diagnostics and reproducible import conversion.
	// Their fixed values are append-only once emitted by an asset producer.
	enum class SkeletonSourceAxis : std::uint8_t {
		PositiveX = 0U,
		PositiveY = 1U,
		PositiveZ = 2U,
		NegativeX = 3U,
		NegativeY = 4U,
		NegativeZ = 5U
	};

	struct SkeletonSourceMetadata {
		float unitScaleMeters = 1.0F;
		SkeletonSourceAxis upAxis = SkeletonSourceAxis::PositiveY;
		SkeletonSourceAxis forwardAxis = SkeletonSourceAxis::PositiveZ;
		std::string sourcePackagePath;

		auto operator<=>(const SkeletonSourceMetadata&) const = default;
	};

	// stableId and semanticRole are source-independent reference keys. displayName
	// is only an editor-facing label and is explicitly allowed to collide.
	struct SkeletonJoint {
		std::uint64_t stableId = 0U;
		std::int32_t parentIndex = -1;
		AnimationTransform localBindTransform{};
		bool deformsMesh = false;
		std::string displayName;
		std::string normalizedSourcePath;
		std::string semanticRole;

		auto operator<=>(const SkeletonJoint&) const = default;
	};

	struct SkeletonSocket {
		std::uint64_t stableId = 0U;
		std::uint64_t jointStableId = 0U;
		AnimationTransform localTransform{};
		std::string displayName;
		std::string semanticRole;

		auto operator<=>(const SkeletonSocket&) const = default;
	};

	struct SkeletonAsset {
		AnimationGuid assetId{};
		// ResourceManager assigns this session-local value when it publishes an
		// immutable cooked payload. It is deliberately not serialized: a recook
		// with the same durable GUID must still invalidate old runtime snapshots.
		AnimationGeneration runtimeGeneration = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint32_t rootJointIndex = 0U;
		std::uint64_t referencePoseHash = 0U;
		SkeletonSourceMetadata source{};
		std::vector<SkeletonJoint> joints;
		std::vector<AnimationMatrix4x4> inverseBindMatrices;
		std::vector<SkeletonSocket> sockets;
		std::uint16_t formatVersion = kSkeletonAssetCurrentVersion;

		auto operator<=>(const SkeletonAsset&) const = default;
	};

	#pragma pack(push, 1)
	// This header uses offsets instead of pointer-sized fields so every table can
	// be range-checked before its byte span is sliced or its records allocated.
	struct SkeletonAssetFileHeaderV1 {
		std::uint32_t magic = kSkeletonAssetMagic;
		std::uint16_t formatVersion = kSkeletonAssetCurrentVersion;
		// Writers set this to sizeof(SkeletonAssetFileHeaderV1) after the complete
		// record exists; a zero default avoids relying on an incomplete type here.
		std::uint16_t headerSize = 0U;
		AnimationGuid assetId{};
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint64_t referencePoseHash = 0U;
		std::uint32_t rootJointIndex = 0U;
		std::uint32_t jointCount = 0U;
		std::uint32_t inverseBindMatrixCount = 0U;
		std::uint32_t socketCount = 0U;
		float unitScaleMeters = 1.0F;
		SkeletonSourceAxis upAxis = SkeletonSourceAxis::PositiveY;
		SkeletonSourceAxis forwardAxis = SkeletonSourceAxis::PositiveZ;
		std::uint16_t reserved = 0U;
		std::uint64_t sourcePackagePathOffset = 0U;
		std::uint64_t sourcePackagePathSize = 0U;
		std::uint64_t jointTableOffset = 0U;
		std::uint64_t inverseBindMatrixTableOffset = 0U;
		std::uint64_t socketTableOffset = 0U;
	};
	#pragma pack(pop)

	// Writers stage all bytes before assigning output. Readers likewise stage a
	// complete asset, so a malformed file cannot replace a last-known-good asset.
	TERRAENGINE_API AnimationStatus SerializeSkeletonAsset(
		const SkeletonAsset& asset,
		std::vector<std::uint8_t>& output);
	TERRAENGINE_API AnimationStatus DeserializeSkeletonAsset(
		std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		SkeletonAsset& output);
}
