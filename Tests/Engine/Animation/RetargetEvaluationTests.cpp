// RetargetEvaluationTests.cpp - Deterministic cross-rig pose and constraint coverage.

#include "TestHarness.h"

#include "Animation/Graph/RetargetNodeEvaluator.h"
#include "Animation/Procedural/ProceduralTypes.h"
#include "Animation/Retargeting/RetargetEvaluator.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string_view>
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

		SkeletonJoint Joint(
			std::uint64_t stableId,
			std::int32_t parentIndex,
			AnimationFloat3 translation,
			std::string_view semantic) {
			SkeletonJoint joint;
			joint.stableId = stableId;
			joint.parentIndex = parentIndex;
			joint.localBindTransform.translation = translation;
			joint.deformsMesh = true;
			joint.displayName = std::string(semantic);
			joint.normalizedSourcePath = std::string(semantic);
			joint.semanticRole = std::string(semantic);
			return joint;
		}

		SkeletonAsset MakeSourceSkeleton() {
			SkeletonAsset skeleton;
			skeleton.assetId = Guid(20U);
			skeleton.runtimeGeneration = 3U;
			skeleton.compatibilityFingerprint = 0x1111U;
			skeleton.referencePoseHash = 0x3333U;
			skeleton.joints = {
				Joint(1U, -1, {0.0F, 0.0F, 0.0F}, "Root"),
				Joint(2U, 0, {0.0F, 1.0F, 0.0F}, "Hips"),
				Joint(3U, 1, {0.0F, -0.5F, 0.0F}, "LeftLeg"),
				Joint(4U, 2, {0.0F, -0.5F, 0.0F}, "LeftFoot"),
				Joint(5U, 1, {0.5F, 0.3F, 0.0F}, "LeftArm"),
				Joint(6U, 4, {0.5F, 0.0F, 0.0F}, "LeftHand"),
				Joint(7U, 1, {0.0F, 0.0F, -0.4F}, "OptionalTail")};
			return skeleton;
		}

		SkeletonAsset MakeTargetSkeleton() {
			SkeletonAsset skeleton;
			skeleton.assetId = Guid(40U);
			skeleton.runtimeGeneration = 4U;
			skeleton.compatibilityFingerprint = 0x2222U;
			skeleton.referencePoseHash = 0x4444U;
			skeleton.joints = {
				Joint(101U, -1, {0.0F, 0.0F, 0.0F}, "Root"),
				Joint(102U, 0, {0.0F, 1.5F, 0.0F}, "Hips"),
				Joint(103U, 1, {0.0F, -0.75F, 0.0F}, "LeftLeg"),
				Joint(104U, 2, {0.0F, -0.25F, 0.0F}, "LegDetail"),
				Joint(105U, 3, {0.0F, -0.5F, 0.0F}, "LeftFoot"),
				Joint(106U, 1, {0.0F, 0.5F, 0.0F}, "LeftArm"),
				Joint(107U, 5, {0.0F, 0.25F, 0.0F}, "ArmTwist"),
				Joint(108U, 6, {0.0F, 0.25F, 0.0F}, "LeftHand")};
			return skeleton;
		}

		RetargetProfileAsset MakeProfile(
			const SkeletonAsset& source,
			const SkeletonAsset& target) {
			RetargetProfileAsset profile;
			profile.assetId = Guid(1U);
			profile.compatibilityFingerprint = 0x7788U;
			profile.sourceSkeletonAssetId = source.assetId;
			profile.targetSkeletonAssetId = target.assetId;
			profile.sourceSkeletonFingerprint = source.compatibilityFingerprint;
			profile.targetSkeletonFingerprint = target.compatibilityFingerprint;
			profile.sourceReferencePoseHash = source.referencePoseHash;
			profile.targetReferencePoseHash = target.referencePoseHash;
			profile.semanticMappings = {
				{1U, 0U, 0U, 0U, 1U, 101U, "Root", "Root", true},
				{2U, 1U, 0U, 0U, 2U, 102U, "Hips", "Hips", true},
				{3U, 2U, 0U, 0U, 3U, 103U, "LeftLeg", "LeftLeg", true},
				{4U, 3U, 0U, 0U, 4U, 105U, "LeftFoot", "LeftFoot", true},
				{5U, 2U, 10U, 0U, 5U, 106U, "LeftArm", "LeftArm", true},
				{6U, 5U, 0U, 0U, 6U, 108U, "LeftHand", "LeftHand", true}};
			profile.explicitMappings = {{7U, 2U, 0U, 50U, 7U, 999U, false}};
			profile.chainMappings = {
				{10U, "LeftLeg", {2U, 3U, 4U}, {102U, 103U, 105U},
					RetargetTranslationScaleMode::PerChainLength, 1.0F, true},
				{11U, "LeftArm", {5U, 6U}, {106U, 108U},
					RetargetTranslationScaleMode::PerChainLength, 1.0F, true}};
			// This ninety-degree authored basis covers different arm rest orientation.
			profile.orientationBases = {{10U, 5U, 106U, {},
				{0.0F, 0.0F, 0.70710677F, 0.70710677F}}};
			profile.proportion.sourceReferenceHeightMeters = 1.0F;
			profile.proportion.targetReferenceHeightMeters = 1.5F;
			profile.root = {1U, 101U, RetargetRootMotionMode::ScaleByProportion,
				true, true, 1.0F, 0.0F};
			profile.contacts = {{20U, 4U, 105U, 10U,
				kRetargetDefaultContactToleranceMeters, true, true, true}};
			profile.twistRules = {{30U, 5U, 6U, {1.0F, 0.0F, 0.0F},
				{107U}, {1.0F}}};
			profile.fallbackRules = {{50U, 7U, 0U,
				RetargetFallbackMode::ReferencePose, {108U}, true}};
			profile.quality.tier = RetargetQualityTier::Verified;
			profile.quality.authoredCoverage = 0.9F;
			profile.quality.expectedMeanPositionErrorMeters = 0.003F;
			profile.quality.expectedMaximumPositionErrorMeters = 0.010F;
			profile.quality.expectedMaximumRotationErrorRadians = 0.01F;
			profile.quality.validationProfileHash = 0xabcddcbaU;
			profile.quality.reviewReason = "Synthetic humanoid retarget evaluation";
			return profile;
		}

		std::vector<AnimationTransform> BindLocals(const SkeletonAsset& skeleton) {
			std::vector<AnimationTransform> result;
			result.reserve(skeleton.joints.size());
			for(const SkeletonJoint& joint : skeleton.joints) {
				result.push_back(joint.localBindTransform);
			}
			return result;
		}

		std::vector<AnimationTransform> BuildModels(
			const SkeletonAsset& skeleton,
			const std::vector<AnimationTransform>& local) {
			std::vector<AnimationTransform> model(local.size());
			for(std::size_t index = 0U; index < local.size(); ++index) {
				const std::int32_t parent = skeleton.joints[index].parentIndex;
				model[index] = parent < 0
					? local[index]
					: ProceduralMath::Combine(model[static_cast<std::size_t>(parent)],
						local[index]);
			}
			return model;
		}

		struct EvaluationFixture {
			RetargetEvaluationRequest request{};
			std::shared_ptr<const RetargetCompiledProfile> profile;
			std::shared_ptr<const SkeletonAsset> sourceSkeleton;
			std::shared_ptr<const SkeletonAsset> targetSkeleton;
			std::shared_ptr<const RetargetPoseSnapshot> sourcePose;
			std::shared_ptr<const RetargetPoseSnapshot> targetReferencePose;
			std::shared_ptr<const RetargetConstraintSnapshot> constraints;
		};

		EvaluationFixture MakeEvaluationFixture() {
			EvaluationFixture fixture;
			SkeletonAsset source = MakeSourceSkeleton();
			SkeletonAsset target = MakeTargetSkeleton();
			RetargetProfileAsset profile = MakeProfile(source, target);
			RetargetValidationResult validation;
			RetargetCompiledProfile compiled;
			static_cast<void>(CompileRetargetProfile(profile, source, target,
				validation, compiled));
			compiled.profile.runtimeGeneration = 5U;

			RetargetPoseSnapshot sourcePose;
			sourcePose.skeleton = {source.assetId, source.runtimeGeneration};
			sourcePose.poseGeneration = 6U;
			sourcePose.tick = 120;
			sourcePose.localTransforms = BindLocals(source);
			// Motion exercises root displacement/facing, non-uniform proportions,
			// hierarchy transfer, arm rest bases, scale, twist, and optional fallback.
			sourcePose.localTransforms[0].translation.x = 1.0F;
			sourcePose.localTransforms[0].rotation = ProceduralMath::AxisAngle(
				{0.0F, 1.0F, 0.0F}, 0.5235987756F);
			sourcePose.localTransforms[2].rotation = ProceduralMath::AxisAngle(
				{0.0F, 0.0F, 1.0F}, 0.3490658504F);
			sourcePose.localTransforms[4].rotation = ProceduralMath::AxisAngle(
				{1.0F, 0.0F, 0.0F}, 1.0471975512F);
			sourcePose.localTransforms[4].scale = {1.2F, 1.0F, 1.0F};
			sourcePose.localTransforms[6].rotation = ProceduralMath::AxisAngle(
				{0.0F, 1.0F, 0.0F}, 0.2617993878F);
			sourcePose.modelTransforms = BuildModels(source,
				sourcePose.localTransforms);

			RetargetPoseSnapshot targetReference;
			targetReference.skeleton = {target.assetId, target.runtimeGeneration};
			targetReference.poseGeneration = 7U;
			targetReference.tick = sourcePose.tick;
			targetReference.localTransforms = BindLocals(target);
			targetReference.modelTransforms = BuildModels(target,
				targetReference.localTransforms);

			RetargetGenerationStamp generation;
			generation.profile = {profile.assetId, 5U};
			generation.sourceSkeleton = sourcePose.skeleton;
			generation.targetSkeleton = targetReference.skeleton;
			generation.sourcePoseGeneration = sourcePose.poseGeneration;
			generation.targetReferencePoseGeneration = targetReference.poseGeneration;
			generation.publicationGeneration = 8U;
			generation.tick = sourcePose.tick;

			RetargetConstraintSnapshot constraints;
			constraints.generation = generation;
			constraints.sourceTick = sourcePose.tick;
			constraints.normalizedTime = 0.375F;
			// Hips are at y=1.5 and the leg reach is 1.25 m, so this goal is
			// exactly reachable and provides a strict error-bound regression.
			constraints.contacts = {{20U, {1.5F, 0.25F, 0.0F},
				{0.0F, 1.0F, 0.0F}, true, true, true}};
			constraints.endEffectors = {{10U, 105U,
				{1.5F, 0.25F, 0.0F}, true}};

			fixture.profile = std::make_shared<const RetargetCompiledProfile>(
				std::move(compiled));
			fixture.sourceSkeleton = std::make_shared<const SkeletonAsset>(
				std::move(source));
			fixture.targetSkeleton = std::make_shared<const SkeletonAsset>(
				std::move(target));
			fixture.sourcePose = std::make_shared<const RetargetPoseSnapshot>(
				std::move(sourcePose));
			fixture.targetReferencePose = std::make_shared<const RetargetPoseSnapshot>(
				std::move(targetReference));
			fixture.constraints = std::make_shared<const RetargetConstraintSnapshot>(
				std::move(constraints));
			fixture.request.profile = fixture.profile;
			fixture.request.sourceSkeleton = fixture.sourceSkeleton;
			fixture.request.targetSkeleton = fixture.targetSkeleton;
			fixture.request.sourcePose = fixture.sourcePose;
			fixture.request.targetReferencePose = fixture.targetReferencePose;
			fixture.request.constraints = fixture.constraints;
			fixture.request.generation = generation;
			fixture.request.expectedGeneration = generation;
			fixture.request.normalizedTime = 0.375F;
			return fixture;
		}

		bool EveryTransformFinite(const RetargetEvaluationResult& result) {
			for(const AnimationTransform& transform : result.localTransforms) {
				if(!IsFinite(transform) || !IsNormalized(transform.rotation, 0.01F)) {
					return false;
				}
			}
			for(const AnimationTransform& transform : result.modelTransforms) {
				if(!IsFinite(transform) || !IsNormalized(transform.rotation, 0.01F)) {
					return false;
				}
			}
			return true;
		}
	}

	void RegisterRetargetEvaluationTests(TestRegistry& registry) {
		registry.Add("Animation.Retarget.EvaluationPreservesPoseTimingRootContactsAndTwist",
			[](TestContext& context) {
				const EvaluationFixture fixture = MakeEvaluationFixture();
				RetargetEvaluationResult result;
				const AnimationStatus status = EvaluateRetargetPose(fixture.request, result);
				context.Expect(status == AnimationStatus::Ok && result.status == AnimationStatus::Ok
					&& result.finite && EveryTransformFinite(result)
					&& result.localTransforms.size() == fixture.targetSkeleton->joints.size()
					&& result.modelTransforms.size() == fixture.targetSkeleton->joints.size(),
					"A valid cross-rig worker packet must produce complete finite local and model target poses.");
				context.Expect(result.sourceTick == 120 && result.normalizedTime == 0.375F
					&& result.metrics.constraints.timingPreserved,
					"Retargeting and constraints must copy the source fixed tick and normalized time exactly.");
				context.Expect(std::abs(result.modelTransforms[0].translation.x - 1.5F) < 0.0001F
					&& result.metrics.appliedTranslationScale >= 1.0F
					&& result.metrics.constraints.rootDisplacementPreserved
					&& result.metrics.constraints.facingPreserved,
					"Root displacement and facing should transfer through the declared proportion and trajectory policies.");
				context.Expect(result.metrics.constraints.appliedContactCount == 1U
					&& result.metrics.constraints.appliedEndEffectorCount == 1U
					&& result.metrics.constraints.maximumContactErrorMeters
						<= kRetargetDefaultContactToleranceMeters
					&& result.metrics.constraints.maximumEndEffectorErrorMeters
						<= kRetargetDefaultReachableEndEffectorToleranceMeters
					&& result.metrics.constraints.contactsPreserved
					&& result.metrics.constraints.reachableEndEffectorsPreserved,
					"Reachable contact and IK goals must remain inside the explicit 20-mm and 10-mm contracts.");
				const AnimationQuaternion& twist = result.localTransforms[6].rotation;
				context.Expect(result.metrics.evaluatedMappingCount == 6U
					&& result.metrics.twistJointCount == 1U
					&& std::abs(twist.x) + std::abs(twist.y) + std::abs(twist.z) > 0.001F
					&& result.metrics.fallbackCount >= 1U && result.reducedFidelity,
					"Mapped hierarchy detail, authored rest bases, twist distribution, and optional fallback must all have visible quantitative outcomes.");
			});

		registry.Add("Animation.Retarget.EvaluationIsBitStableAcrossTenThousandRuns",
			[](TestContext& context) {
				const EvaluationFixture fixture = MakeEvaluationFixture();
				RetargetEvaluationResult baseline;
				bool deterministic = EvaluateRetargetPose(fixture.request, baseline)
					== AnimationStatus::Ok;
				// This is a repeatability count, not a wall-clock performance assertion.
				// Exact values catch ordering or mutable-input races without flaky timing.
				for(std::size_t iteration = 0U; iteration < 10000U && deterministic;
					++iteration) {
					RetargetEvaluationResult repeated;
					deterministic = EvaluateRetargetPose(fixture.request, repeated)
						== AnimationStatus::Ok
						&& repeated.deterministicHash == baseline.deterministicHash
						&& repeated.localTransforms == baseline.localTransforms
						&& repeated.modelTransforms == baseline.modelTransforms
						&& repeated.metrics == baseline.metrics;
				}
				context.Expect(deterministic && baseline.deterministicHash != 0U,
					"Ten thousand identical immutable packets must produce bit-stable poses, metrics, and hashes without a timing threshold.");
			});

		registry.Add("Animation.Retarget.EvaluationRejectsStaleMalformedAndUnreachablePacketsSafely",
			[](TestContext& context) {
				const EvaluationFixture fixture = MakeEvaluationFixture();
				RetargetEvaluationRequest stale = fixture.request;
				++stale.expectedGeneration.publicationGeneration;
				RetargetEvaluationResult staleResult;
				context.Expect(EvaluateRetargetPose(stale, staleResult)
						== AnimationStatus::StaleHandle
					&& staleResult.status == AnimationStatus::StaleHandle
					&& staleResult.finite
					&& staleResult.localTransforms
						== fixture.targetReferencePose->localTransforms,
					"A stale worker result must be rejected with a complete finite target-reference fallback.");

				auto malformedPose = std::make_shared<RetargetPoseSnapshot>(*fixture.sourcePose);
				malformedPose->localTransforms[2].translation.x =
					std::numeric_limits<float>::quiet_NaN();
				RetargetEvaluationRequest malformed = fixture.request;
				malformed.sourcePose = malformedPose;
				RetargetEvaluationResult malformedResult;
				context.Expect(EvaluateRetargetPose(malformed, malformedResult)
						== AnimationStatus::InvalidArgument
					&& malformedResult.finite
					&& EveryTransformFinite(malformedResult),
					"Non-finite source input must never leak a partial or non-finite target pose.");

				auto mismatchedTiming =
					std::make_shared<RetargetConstraintSnapshot>(*fixture.constraints);
				mismatchedTiming->normalizedTime = 0.5F;
				RetargetEvaluationRequest badTiming = fixture.request;
				badTiming.constraints = mismatchedTiming;
				RetargetEvaluationResult badTimingResult;
				context.Expect(EvaluateRetargetPose(badTiming, badTimingResult)
						== AnimationStatus::InvalidArgument
					&& badTimingResult.finite,
					"A copied constraint snapshot from another animation time must fail before constraint mutation.");

				auto unreachable =
					std::make_shared<RetargetConstraintSnapshot>(*fixture.constraints);
				unreachable->contacts.front().positionModel = {100.0F, 100.0F, 100.0F};
				unreachable->endEffectors.front().positionModel = {100.0F, 100.0F, 100.0F};
				RetargetEvaluationRequest unreachableRequest = fixture.request;
				unreachableRequest.constraints = unreachable;
				RetargetEvaluationResult unreachableResult;
				context.Expect(EvaluateRetargetPose(unreachableRequest, unreachableResult)
						== AnimationStatus::Ok
					&& unreachableResult.finite && unreachableResult.reducedFidelity
					&& unreachableResult.metrics.constraints.fallbackCount >= 2U
					&& !unreachableResult.metrics.constraints.contactsPreserved
					&& !unreachableResult.metrics.constraints.reachableEndEffectorsPreserved,
					"Unreachable copied goals should degrade through bounded fallback while keeping the target pose finite.");
			});

		registry.Add("Animation.Retarget.GraphCompositionPreservesChannelsAndHybridClosure",
			[](TestContext& context) {
				const EvaluationFixture fixture = MakeEvaluationFixture();
				GraphPoseContribution base;
				base.compatibilityFingerprint = fixture.targetSkeleton->compatibilityFingerprint;
				base.jointCount = static_cast<std::uint32_t>(fixture.targetSkeleton->joints.size());
				for(const SkeletonJoint& joint : fixture.targetSkeleton->joints) {
					base.jointStableIds.push_back(joint.stableId);
				}
				base.joints = fixture.targetReferencePose->localTransforms;
				base.morphs = {{7001U, 0.75F}};
				base.curves = {{8001U, 0.25F}};
				AnimationRootMotionDelta root;
				root.sourceBeginTick = 119;
				root.sourceEndTick = 120;
				root.localTranslation = {0.1F, 0.0F, 0.0F};
				root.worldTranslation = root.localTranslation;
				base.rootMotion = root;

				const std::array<std::uint64_t, 1U> requiredJoints{108U};
				RetargetGraphNodeRequest request;
				request.evaluation = fixture.request;
				request.targetBasePose = &base;
				request.hybridConsumerJointIds = requiredJoints;
				request.motionSelectionApplied = true;
				request.proceduralFollows = true;
				request.ragdollFollows = true;
				request.recordingRequested = true;
				request.rollbackRequested = true;
				RetargetGraphNodeResult result;
				context.Expect(EvaluateRetargetGraphNode(request, result) == AnimationStatus::Ok
					&& result.pose.morphs == base.morphs && result.pose.curves == base.curves
					&& result.pose.rootMotion == base.rootMotion
					&& result.recordingPose == result.pose && result.rollbackPose == result.pose,
					"Retarget graph composition must preserve morph, curve, and root channels and copy the final pose for recording and rollback.");
				const std::vector<std::uint32_t> expectedClosure{0U, 1U, 5U, 6U, 7U};
				const std::vector<RetargetCompositionStage> expectedStages{
					RetargetCompositionStage::MotionSelection,
					RetargetCompositionStage::Retarget,
					RetargetCompositionStage::Procedural,
					RetargetCompositionStage::Ragdoll,
					RetargetCompositionStage::Recording};
				context.Expect(result.hybridJointClosure == expectedClosure
					&& result.orderedStages == expectedStages,
					"Hybrid consumers must receive the requested joint plus every target ancestor in the fixed advanced-node order.");
			});
	}
}
