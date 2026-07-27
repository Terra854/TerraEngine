// RetargetProfileAsset.h - Durable source-to-target rig mapping contract.
//
// A profile contains authored intent only. Runtime generations are assigned by
// publication and deliberately excluded from serialization, while every source
// and target compatibility/reference-pose fingerprint is persisted so a hot
// reload cannot silently bind the mapping to a different rig.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Assets/AnimationAssetTypes.h"
#include "File Formats/TerraEngineAnimationFile.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint16_t kRetargetProfileAssetCurrentVersion = 1U;
	inline constexpr std::uint32_t kRetargetProfilePayloadMagic =
		MakeAnimationFourCC('R', 'T', 'G', 'T');
	inline constexpr std::size_t kRetargetProfileMaximumMappings = 4096U;
	inline constexpr std::size_t kRetargetProfileMaximumChains = 4096U;
	inline constexpr std::size_t kRetargetProfileMaximumTotalChainJoints = 65536U;
	inline constexpr std::size_t kRetargetProfileMaximumStringBytes = 4096U;
	inline constexpr std::size_t kRetargetProfileMaximumPayloadBytes = 64U * 1024U * 1024U;
	inline constexpr float kRetargetDefaultContactToleranceMeters = 0.020F;
	inline constexpr float kRetargetDefaultReachableEndEffectorToleranceMeters = 0.010F;

	// Semantic mappings resolve stable rig roles, while explicit mappings bind
	// authored stable joint IDs. Their numeric values are persisted and append-only.
	enum class RetargetMappingKind : std::uint8_t {
		Semantic = 0U,
		Explicit = 1U
	};

	enum class RetargetTranslationScaleMode : std::uint8_t {
		None = 0U,
		UniformHeight = 1U,
		PerChainLength = 2U,
		Authored = 3U
	};

	enum class RetargetRootMotionMode : std::uint8_t {
		PreserveDisplacement = 0U,
		ScaleByProportion = 1U,
		InPlace = 2U
	};

	enum class RetargetTrajectoryMode : std::uint8_t {
		PreserveFacingAndDisplacement = 0U,
		PreserveFacing = 1U,
		PreserveDisplacement = 2U,
		Ignore = 3U
	};

	enum class RetargetFallbackMode : std::uint8_t {
		ReferencePose = 0U,
		NearestMappedAncestor = 1U,
		DeclaredTargetChain = 2U,
		DropOptional = 3U
	};

	enum class RetargetQualityTier : std::uint8_t {
		Unrated = 0U,
		Reduced = 1U,
		Production = 2U,
		Verified = 3U
	};

	// parentMappingId describes the authored mapping hierarchy rather than either
	// skeleton hierarchy. Validation uses it to reject direct and indirect cycles.
	struct RetargetSemanticMapping {
		std::uint64_t mappingId = 0U;
		std::uint64_t parentMappingId = 0U;
		std::uint64_t basisId = 0U;
		std::uint64_t fallbackRuleId = 0U;
		std::uint64_t sourceJointId = 0U;
		std::uint64_t targetJointId = 0U;
		std::string sourceSemantic;
		std::string targetSemantic;
		bool required = true;

		auto operator<=>(const RetargetSemanticMapping&) const = default;
	};

	struct RetargetExplicitJointMapping {
		std::uint64_t mappingId = 0U;
		std::uint64_t parentMappingId = 0U;
		std::uint64_t basisId = 0U;
		std::uint64_t fallbackRuleId = 0U;
		std::uint64_t sourceJointId = 0U;
		std::uint64_t targetJointId = 0U;
		bool required = true;

		auto operator<=>(const RetargetExplicitJointMapping&) const = default;
	};

	// A chain records ordered root-to-effector joint IDs. Gaps are intentional:
	// different rigs may insert detail or twist joints between authored anchors.
	struct RetargetChainMapping {
		std::uint64_t chainId = 0U;
		std::string semantic;
		std::vector<std::uint64_t> sourceJointIds;
		std::vector<std::uint64_t> targetJointIds;
		RetargetTranslationScaleMode translationScaleMode =
			RetargetTranslationScaleMode::PerChainLength;
		float authoredTranslationScale = 1.0F;
		bool required = true;

		auto operator<=>(const RetargetChainMapping&) const = default;
	};

	// Bases convert a source rest-space delta into the target joint's authored
	// orientation. They must be finite normalized quaternions before publication.
	struct RetargetOrientationBasis {
		std::uint64_t basisId = 0U;
		std::uint64_t sourceJointId = 0U;
		std::uint64_t targetJointId = 0U;
		AnimationQuaternion sourceBasis{};
		AnimationQuaternion targetBasis{};

		auto operator<=>(const RetargetOrientationBasis&) const = default;
	};

	struct RetargetProportionRule {
		RetargetTranslationScaleMode translationScaleMode =
			RetargetTranslationScaleMode::UniformHeight;
		float sourceReferenceHeightMeters = 1.0F;
		float targetReferenceHeightMeters = 1.0F;
		float authoredTranslationScale = 1.0F;
		float minimumScale = 0.01F;
		float maximumScale = 100.0F;

		auto operator<=>(const RetargetProportionRule&) const = default;
	};

	struct RetargetRootRule {
		std::uint64_t sourceRootJointId = 0U;
		std::uint64_t targetRootJointId = 0U;
		RetargetRootMotionMode motionMode = RetargetRootMotionMode::ScaleByProportion;
		bool preserveFacing = true;
		bool preserveVerticalDisplacement = true;
		float displacementScale = 1.0F;
		float yawOffsetRadians = 0.0F;

		auto operator<=>(const RetargetRootRule&) const = default;
	};

	struct RetargetTrajectoryRule {
		RetargetTrajectoryMode mode = RetargetTrajectoryMode::PreserveFacingAndDisplacement;
		AnimationFloat3 sourceForwardAxis{0.0F, 0.0F, 1.0F};
		AnimationFloat3 targetForwardAxis{0.0F, 0.0F, 1.0F};
		float horizontalScale = 1.0F;
		float verticalScale = 1.0F;

		auto operator<=>(const RetargetTrajectoryRule&) const = default;
	};

	struct RetargetContactRule {
		std::uint64_t contactId = 0U;
		std::uint64_t sourceJointId = 0U;
		std::uint64_t targetJointId = 0U;
		std::uint64_t targetChainId = 0U;
		float toleranceMeters = kRetargetDefaultContactToleranceMeters;
		bool preservePosition = true;
		bool preserveOrientation = false;
		bool required = true;

		auto operator<=>(const RetargetContactRule&) const = default;
	};

	// targetWeights are paired with targetJointIds by index. Validation requires
	// a finite non-negative unit sum so distribution is deterministic.
	struct RetargetTwistRule {
		std::uint64_t twistId = 0U;
		std::uint64_t sourceJointId = 0U;
		std::uint64_t sourceChildJointId = 0U;
		AnimationFloat3 sourceTwistAxis{1.0F, 0.0F, 0.0F};
		std::vector<std::uint64_t> targetJointIds;
		std::vector<float> targetWeights;

		auto operator<=>(const RetargetTwistRule&) const = default;
	};

	// nextFallbackRuleId forms a deliberately explicit fallback chain. A zero ID
	// terminates it; validator cycle detection prevents unbounded runtime walks.
	struct RetargetFallbackRule {
		std::uint64_t fallbackRuleId = 0U;
		std::uint64_t sourceMappingId = 0U;
		std::uint64_t nextFallbackRuleId = 0U;
		RetargetFallbackMode mode = RetargetFallbackMode::ReferencePose;
		std::vector<std::uint64_t> targetJointIds;
		bool reportReducedFidelity = true;

		auto operator<=>(const RetargetFallbackRule&) const = default;
	};

	struct RetargetQualityMetadata {
		RetargetQualityTier tier = RetargetQualityTier::Unrated;
		float authoredCoverage = 0.0F;
		float expectedMeanPositionErrorMeters = 0.0F;
		float expectedMaximumPositionErrorMeters = 0.0F;
		float expectedMaximumRotationErrorRadians = 0.0F;
		float reachableEndEffectorToleranceMeters =
			kRetargetDefaultReachableEndEffectorToleranceMeters;
		float facingToleranceRadians = 0.00174532925F;
		float rootDisplacementToleranceMeters = 0.0005F;
		std::uint32_t timingToleranceTicks = 0U;
		std::uint64_t validationProfileHash = 0U;
		std::string reviewReason;

		auto operator<=>(const RetargetQualityMetadata&) const = default;
	};

	struct RetargetProfileAsset {
		AnimationGuid assetId{};
		// The publisher owns this session-local generation. Copying or serializing
		// authored data never aliases a prior runtime generation.
		AnimationGeneration runtimeGeneration = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		AnimationGuid sourceSkeletonAssetId{};
		AnimationGuid targetSkeletonAssetId{};
		AnimationCompatibilityFingerprint sourceSkeletonFingerprint = 0U;
		AnimationCompatibilityFingerprint targetSkeletonFingerprint = 0U;
		std::uint64_t sourceReferencePoseHash = 0U;
		std::uint64_t targetReferencePoseHash = 0U;
		std::vector<RetargetSemanticMapping> semanticMappings;
		std::vector<RetargetExplicitJointMapping> explicitMappings;
		std::vector<RetargetChainMapping> chainMappings;
		std::vector<RetargetOrientationBasis> orientationBases;
		RetargetProportionRule proportion{};
		RetargetRootRule root{};
		RetargetTrajectoryRule trajectory{};
		std::vector<RetargetContactRule> contacts;
		std::vector<RetargetTwistRule> twistRules;
		std::vector<RetargetFallbackRule> fallbackRules;
		RetargetQualityMetadata quality{};
		std::uint16_t formatVersion = kRetargetProfileAssetCurrentVersion;

		auto operator<=>(const RetargetProfileAsset&) const = default;
	};

	// Structural validation covers bounded allocation and durable value shape.
	// Cross-skeleton semantic/topology compatibility belongs to RetargetValidator.
	TERRAENGINE_API AnimationStatus ValidateRetargetProfileAssetStructure(
		const RetargetProfileAsset& asset) noexcept;
	TERRAENGINE_API AnimationStatus SerializeRetargetProfileAsset(
		const RetargetProfileAsset& asset,
		std::vector<std::uint8_t>& output);
	TERRAENGINE_API AnimationStatus DeserializeRetargetProfileAsset(
		std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		RetargetProfileAsset& output);
}
