// RetargetProfileAssetTests.cpp - Durable profile format and identity coverage.

#include "TestHarness.h"

#include "Animation/Assets/RetargetProfileAsset.h"

#include <algorithm>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid Guid(std::uint8_t seed) {
			AnimationGuid value;
			for(std::size_t index = 0U; index < value.value.size(); ++index) {
				value.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return value;
		}

		RetargetProfileAsset MakeProfile(bool reverseCollections = false) {
			RetargetProfileAsset profile;
			profile.assetId = Guid(1U);
			profile.compatibilityFingerprint = 0x1122334455667788ULL;
			profile.sourceSkeletonAssetId = Guid(20U);
			profile.targetSkeletonAssetId = Guid(40U);
			profile.sourceSkeletonFingerprint = 0x1111U;
			profile.targetSkeletonFingerprint = 0x2222U;
			profile.sourceReferencePoseHash = 0x3333U;
			profile.targetReferencePoseHash = 0x4444U;
			profile.semanticMappings = {
				{1U, 0U, 10U, 0U, 100U, 200U, "Root", "Root", true},
				{2U, 1U, 11U, 0U, 101U, 201U, "Hips", "Hips", true}};
			profile.explicitMappings = {
				{4U, 2U, 0U, 0U, 103U, 203U, true},
				{3U, 2U, 0U, 50U, 102U, 202U, false}};
			profile.chainMappings = {
				{20U, "LeftLeg", {101U, 103U}, {201U, 203U},
					RetargetTranslationScaleMode::PerChainLength, 1.0F, true},
				{21U, "Spine", {100U, 101U}, {200U, 201U},
					RetargetTranslationScaleMode::UniformHeight, 1.0F, true}};
			profile.orientationBases = {
				{11U, 101U, 201U, {}, {}}, {10U, 100U, 200U, {}, {}}};
			profile.proportion.sourceReferenceHeightMeters = 1.8F;
			profile.proportion.targetReferenceHeightMeters = 2.1F;
			profile.root = {100U, 200U, RetargetRootMotionMode::ScaleByProportion,
				true, true, 1.0F, 0.0F};
			profile.contacts = {{30U, 103U, 203U, 20U,
				kRetargetDefaultContactToleranceMeters, true, true, true}};
			profile.twistRules = {{40U, 101U, 103U, {1.0F, 0.0F, 0.0F},
				{202U, 203U}, {0.25F, 0.75F}}};
			profile.fallbackRules = {{50U, 3U, 0U,
				RetargetFallbackMode::DeclaredTargetChain, {201U, 203U}, true}};
			profile.quality.tier = RetargetQualityTier::Verified;
			profile.quality.authoredCoverage = 0.95F;
			profile.quality.expectedMeanPositionErrorMeters = 0.003F;
			profile.quality.expectedMaximumPositionErrorMeters = 0.010F;
			profile.quality.expectedMaximumRotationErrorRadians = 0.01F;
			profile.quality.validationProfileHash = 0xabcdefU;
			profile.quality.reviewReason = "Synthetic humanoid regression profile";
			if(reverseCollections) {
				std::reverse(profile.semanticMappings.begin(), profile.semanticMappings.end());
				std::reverse(profile.explicitMappings.begin(), profile.explicitMappings.end());
				std::reverse(profile.chainMappings.begin(), profile.chainMappings.end());
				std::reverse(profile.orientationBases.begin(), profile.orientationBases.end());
			}
			return profile;
		}
	}

	void RegisterRetargetProfileAssetTests(TestRegistry& registry) {
		registry.Add("Animation.Retarget.AssetRoundTripsDeterministically",
			[](TestContext& context) {
				std::vector<std::uint8_t> forward;
				std::vector<std::uint8_t> reversed;
				AnimationAssetReader reader;
				context.Expect(SerializeRetargetProfileAsset(MakeProfile(false), forward)
						== AnimationStatus::Ok
					&& SerializeRetargetProfileAsset(MakeProfile(true), reversed)
						== AnimationStatus::Ok
					&& forward == reversed && reader.Parse(forward) == AnimationStatus::Ok
					&& reader.Dependencies().size() == 2U,
					"Stable IDs, not authored insertion order, must define retarget profile bytes.");

				RetargetProfileAsset restored;
				RetargetProfileAsset expected = MakeProfile(false);
				std::sort(expected.semanticMappings.begin(), expected.semanticMappings.end());
				std::sort(expected.explicitMappings.begin(), expected.explicitMappings.end());
				std::sort(expected.chainMappings.begin(), expected.chainMappings.end());
				std::sort(expected.orientationBases.begin(), expected.orientationBases.end());
				context.Expect(DeserializeRetargetProfileAsset(forward,
						expected.compatibilityFingerprint, restored) == AnimationStatus::Ok
					&& restored == expected
					&& restored.sourceSkeletonFingerprint == 0x1111U
					&& restored.targetSkeletonFingerprint == 0x2222U
					&& restored.sourceReferencePoseHash == 0x3333U
					&& restored.targetReferencePoseHash == 0x4444U,
					"Every mapping rule, rig fingerprint, basis, tolerance, and quality field must round-trip.");
			});

		registry.Add("Animation.Retarget.AssetIdentitySurvivesMoveRenameAndReimport",
			[](TestContext& context) {
				RetargetProfileAsset first = MakeProfile(false);
				first.runtimeGeneration = 7U;
				std::vector<std::uint8_t> bytes;
				context.Expect(SerializeRetargetProfileAsset(first, bytes) == AnimationStatus::Ok,
					"The initial mapping asset should serialize without its session generation.");
				RetargetProfileAsset moved;
				context.Expect(DeserializeRetargetProfileAsset(bytes,
						first.compatibilityFingerprint, moved) == AnimationStatus::Ok
					&& moved.assetId == first.assetId && moved.runtimeGeneration == 0U,
					"Path-independent GUID identity must survive a move/rename while runtime generation remains session-local.");

				RetargetProfileAsset reimported = moved;
				reimported.quality.reviewReason = "Reimported from a renamed authoring file";
				std::vector<std::uint8_t> reimportedBytes;
				RetargetProfileAsset reloaded;
				context.Expect(SerializeRetargetProfileAsset(reimported, reimportedBytes)
						== AnimationStatus::Ok
					&& DeserializeRetargetProfileAsset(reimportedBytes,
						reimported.compatibilityFingerprint, reloaded) == AnimationStatus::Ok
					&& reloaded.assetId == first.assetId
					&& reloaded.quality.reviewReason == reimported.quality.reviewReason,
					"Compatible reimport may change authored metadata without churning the durable asset identity.");
			});

		registry.Add("Animation.Retarget.AssetRejectsCorruptionCompatibilityAndBoundsAtomically",
			[](TestContext& context) {
				const RetargetProfileAsset profile = MakeProfile(false);
				std::vector<std::uint8_t> bytes;
				static_cast<void>(SerializeRetargetProfileAsset(profile, bytes));
				RetargetProfileAsset unchanged = profile;
				unchanged.assetId = Guid(90U);
				context.Expect(DeserializeRetargetProfileAsset(bytes, 0x9999U, unchanged)
						== AnimationStatus::IncompatibleAsset
					&& unchanged.assetId == Guid(90U),
					"A compatibility mismatch must leave the caller's complete last-good profile untouched.");

				std::vector<std::uint8_t> corrupt = bytes;
				corrupt.back() ^= 0x5aU;
				context.Expect(DeserializeRetargetProfileAsset(corrupt,
						profile.compatibilityFingerprint, unchanged)
						== AnimationStatus::ContentHashMismatch
					&& unchanged.assetId == Guid(90U),
					"Corrupt hot-reload bytes must fail at the checked envelope and preserve prior output.");

				RetargetProfileAsset oversized = profile;
				oversized.explicitMappings.resize(kRetargetProfileMaximumMappings + 1U);
				std::vector<std::uint8_t> untouched{1U, 2U, 3U};
				context.Expect(SerializeRetargetProfileAsset(oversized, untouched)
						== AnimationStatus::HardLimitExceeded
					&& untouched == std::vector<std::uint8_t>({1U, 2U, 3U}),
					"Oversized authored tables must reject before payload allocation or output replacement.");

				RetargetProfileAsset unreviewedTightening = profile;
				unreviewedTightening.quality.reviewReason.clear();
				unreviewedTightening.contacts.front().toleranceMeters = 0.010F;
				unreviewedTightening.quality.reachableEndEffectorToleranceMeters = 0.005F;
				context.Expect(ValidateRetargetProfileAssetStructure(unreviewedTightening)
						== AnimationStatus::InvalidArgument,
					"A profile may tighten the frozen 20-mm contact or 10-mm reachable-IK contract only with an explicit review reason.");
			});
	}
}
