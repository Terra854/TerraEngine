// AnimationMaskAsset.h - Durable reusable selectors and compiled dense weights.

#pragma once

#include "Animation/AnimationTypes.h"
#include "File Formats/TerraEngineAnimationFile.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint16_t kAnimationMaskAssetCurrentVersion = 1U;
	inline constexpr std::uint32_t kAnimationMaskPayloadMagic = MakeAnimationFourCC('M', 'A', 'S', 'K');

	// Selectors are durable authored intent. The cooker resolves them into the
	// dense tables below so worker evaluation never walks live skeleton data.
	struct AnimationMaskJointSelector {
		std::uint64_t stableJointId = 0U;
		float weight = 0.0F;
		bool includeDescendants = false;
		auto operator<=>(const AnimationMaskJointSelector &) const = default;
	};
	struct AnimationMaskBranchSelector {
		std::uint64_t rootJointId = 0U;
		float weight = 0.0F;
		bool includeDescendants = true;
		auto operator<=>(const AnimationMaskBranchSelector &) const = default;
	};
	struct AnimationMaskNamedSelector {
		std::uint64_t stableId = 0U;
		bool include = true;
		auto operator<=>(const AnimationMaskNamedSelector &) const = default;
	};
	struct AnimationMaskDenseJointWeight {
		std::uint64_t stableJointId = 0U;
		float weight = 0.0F;
		auto operator<=>(const AnimationMaskDenseJointWeight &) const = default;
	};
	struct AnimationMaskAsset {
		AnimationGuid assetId{};
		AnimationGuid skeletonAssetId{};
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint16_t formatVersion = kAnimationMaskAssetCurrentVersion;
		std::vector<AnimationMaskJointSelector> jointSelectors;
		std::vector<AnimationMaskBranchSelector> branchSelectors;
		std::vector<AnimationMaskNamedSelector> morphSelectors;
		std::vector<AnimationMaskNamedSelector> curveSelectors;
		float rootMotionWeight = 0.0F;
		std::vector<AnimationMaskDenseJointWeight> compiledJointWeights;
		std::vector<std::uint64_t> compiledMorphIds;
		std::vector<std::uint64_t> compiledCurveIds;
		auto operator<=>(const AnimationMaskAsset&) const = default;
	};
	TERRAENGINE_API AnimationStatus ValidateAnimationMaskAsset(const AnimationMaskAsset& asset) noexcept;
	TERRAENGINE_API AnimationStatus SerializeAnimationMaskAsset(const AnimationMaskAsset& asset, std::vector<std::uint8_t>& output);
	TERRAENGINE_API AnimationStatus DeserializeAnimationMaskAsset(std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint, AnimationMaskAsset& output);
}
