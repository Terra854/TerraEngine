// RagdollProfileAssetTests.cpp - Versioned asset and cross-skeleton validation.

#include "TestHarness.h"

#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Ragdoll/RagdollProfileValidator.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		AnimationGuid Guid(std::uint8_t seed) {
			AnimationGuid guid;
			guid.value[0U] = seed;
			guid.value[15U] = static_cast<std::uint8_t>(seed ^ 0xa5U);
			return guid;
		}

		SkeletonJoint Joint(
			std::uint64_t stableId,
			std::int32_t parentIndex,
			bool deformsMesh,
			const char* path) {
			SkeletonJoint joint;
			joint.stableId = stableId;
			joint.parentIndex = parentIndex;
			joint.deformsMesh = deformsMesh;
			joint.displayName = path;
			joint.normalizedSourcePath = path;
			return joint;
		}

		SkeletonAsset MakeSkeleton() {
			SkeletonAsset skeleton;
			skeleton.assetId = Guid(2U);
			skeleton.compatibilityFingerprint = 0x2222U;
			skeleton.referencePoseHash = 0x3333U;
			skeleton.rootJointIndex = 0U;
			skeleton.joints = {
				Joint(10U, -1, true, "root"),
				Joint(20U, 0, true, "root/spine"),
				Joint(30U, 1, true, "root/spine/head"),
				Joint(40U, 1, false, "root/spine/arm")
			};
			skeleton.inverseBindMatrices.resize(skeleton.joints.size());
			return skeleton;
		}

		RagdollColliderRecord PrimitiveCollider(std::uint64_t colliderId) {
			RagdollColliderRecord collider;
			collider.colliderId = colliderId;
			collider.shape = RagdollColliderShape::Capsule;
			collider.dimensions = {0.15F, 0.3F, 0.15F};
			return collider;
		}

		RagdollColliderRecord InlineHullCollider(std::uint64_t colliderId) {
			RagdollColliderRecord collider;
			collider.colliderId = colliderId;
			collider.shape = RagdollColliderShape::ConvexHull;
			collider.convexHullSource = RagdollConvexHullSource::InlineVertices;
			collider.convexHullVertices = {
				{0.0F, 0.0F, 0.0F},
				{0.0F, 0.0F, 1.0F},
				{0.0F, 1.0F, 0.0F},
				{1.0F, 0.0F, 0.0F}
			};
			return collider;
		}

		RagdollColliderRecord ImmutableHullCollider(std::uint64_t colliderId) {
			RagdollColliderRecord collider;
			collider.colliderId = colliderId;
			collider.shape = RagdollColliderShape::ConvexHull;
			collider.convexHullSource = RagdollConvexHullSource::ImmutableCookedAsset;
			collider.immutableShape.assetId = Guid(90U);
			collider.immutableShape.expectedGeneration = 7U;
			collider.immutableShape.compatibilityFingerprint = 0x9090U;
			return collider;
		}

		RagdollBodyRecord Body(
			std::uint64_t bodyId,
			std::uint64_t jointId,
			std::uint32_t layer,
			float mass,
			RagdollColliderRecord collider) {
			RagdollBodyRecord body;
			body.bodyId = bodyId;
			body.stableJointId = jointId;
			body.jointToBodyOffset = {0.0F, 0.1F, 0.0F};
			body.colliders.push_back(std::move(collider));
			body.massKilograms = mass;
			body.inertiaMode = RagdollInertiaMode::ExplicitDiagonal;
			body.inertiaDiagonal = {1.0F, 1.5F, 2.0F};
			body.collisionLayer = layer;
			body.collisionMask = 0x3U;
			return body;
		}

		RagdollConstraintRecord Constraint(
			std::uint64_t constraintId,
			std::uint64_t parentBodyId,
			std::uint64_t childBodyId) {
			RagdollConstraintRecord constraint;
			constraint.constraintId = constraintId;
			constraint.parentBodyId = parentBodyId;
			constraint.childBodyId = childBodyId;
			RagdollAxisLimit& angularLimit = constraint.limits[
				static_cast<std::size_t>(RagdollConstraintAxis::AngularX)];
			angularLimit.mode = RagdollLimitMode::Limited;
			angularLimit.minimum = -1.0F;
			angularLimit.maximum = 1.0F;
			RagdollAxisMotor& motor = constraint.motors[
				static_cast<std::size_t>(RagdollConstraintAxis::AngularX)];
			motor.mode = RagdollMotorMode::Position;
			motor.stiffness = 20.0F;
			motor.damping = 4.0F;
			motor.maximumForce = 500.0F;
			return constraint;
		}

		RagdollRecoveryRecord Recovery(const SkeletonAsset& skeleton) {
			RagdollRecoveryRecord recovery;
			recovery.candidateId = 700U;
			recovery.clipAssetId = Guid(70U);
			recovery.requiredContactMask = 0x1U;
			recovery.clipGeneration = 3U;
			recovery.clipSkeletonFingerprint = skeleton.compatibilityFingerprint;
			recovery.requiredReferencePoseHash = skeleton.referencePoseHash;
			recovery.rootJointStableId = 10U;
			recovery.contactJointStableIds = {20U, 30U};
			recovery.posture = RagdollRecoveryPosture::FaceUp;
			return recovery;
		}

		RagdollProfileAsset MakeProfile(const SkeletonAsset& skeleton) {
			RagdollProfileAsset profile;
			profile.assetId = Guid(1U);
			profile.skeletonAssetId = skeleton.assetId;
			profile.skeletonFingerprint = skeleton.compatibilityFingerprint;
			profile.referencePoseHash = skeleton.referencePoseHash;
			profile.physicsProviderFingerprint = 0x4a4f4c54U;
			profile.rootBodyId = 100U;
			profile.coveragePolicy = RagdollCoveragePolicy::RequiredJointSet;
			profile.requiredBodyJointStableIds = {10U, 20U, 30U};
			profile.expectedTotalMassKilograms = 30.0F;
			profile.expectedMassToleranceFraction = 0.01F;
			profile.minimumBodyMassFraction = 0.1F;
			profile.maximumBodyMassFraction = 0.6F;
			profile.allowedCollisionLayerMask = 0x3U;
			profile.bodies = {
				Body(100U, 10U, 0U, 10.0F, PrimitiveCollider(1000U)),
				Body(200U, 20U, 1U, 10.0F, InlineHullCollider(2000U)),
				Body(300U, 30U, 1U, 10.0F, ImmutableHullCollider(3000U))
			};
			profile.constraints = {
				Constraint(400U, 100U, 200U),
				Constraint(500U, 200U, 300U)
			};
			profile.blends = {
				{10U, 1.0F, RagdollBoundaryPolicy::KinematicAnchor},
				{20U, 0.8F, RagdollBoundaryPolicy::MotorToAnimation},
				{30U, 1.0F, RagdollBoundaryPolicy::MatchAnimation}
			};
			profile.recovery.push_back(Recovery(skeleton));
			return profile;
		}

		std::vector<RagdollCollisionLayerRule> CollisionLayers() {
			return {
				{0U, 0x3U, true},
				{1U, 0x3U, true}
			};
		}

		RagdollProfileValidationReport Validate(
			const RagdollProfileAsset& profile,
			const SkeletonAsset& skeleton) {
			const std::vector<RagdollCollisionLayerRule> layers = CollisionLayers();
			RagdollProfileValidationOptions options;
			options.collisionLayers = layers;
			return ValidateRagdollProfile(profile, skeleton, options);
		}

		bool HasCode(
			const RagdollProfileValidationReport& report,
			RagdollProfileDiagnosticCode code) {
			return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
				[code](const RagdollProfileDiagnostic& diagnostic) {
					return diagnostic.code == code;
				});
		}

		std::vector<std::tuple<RagdollProfileDiagnosticCode, std::uint64_t,
			std::uint64_t, std::uint64_t, std::uint64_t>> DiagnosticIdentity(
			const RagdollProfileValidationReport& report) {
			std::vector<std::tuple<RagdollProfileDiagnosticCode, std::uint64_t,
				std::uint64_t, std::uint64_t, std::uint64_t>> result;
			for(const RagdollProfileDiagnostic& diagnostic : report.diagnostics) {
				result.emplace_back(diagnostic.code, diagnostic.bodyId,
					diagnostic.constraintId, diagnostic.stableJointId,
					diagnostic.recoveryCandidateId);
			}
			return result;
		}

		std::vector<std::uint8_t> RebuildWithPayload(
			std::span<const std::uint8_t> original,
			std::span<const std::uint8_t> replacementPayload) {
			AnimationAssetReader reader;
			if(reader.Parse(original) != AnimationStatus::Ok) {
				return {};
			}
			AnimationAssetWriter writer;
			for(const AnimationDependencyEntry& dependency : reader.Dependencies()) {
				if(!writer.AddDependency(dependency)) {
					return {};
				}
			}
			if(!writer.AddChunk(kAnimationChunkPayload, true, replacementPayload)) {
				return {};
			}
			AnimationAssetEnvelope envelope = reader.Envelope();
			std::vector<std::uint8_t> rebuilt;
			return writer.Finish(envelope, rebuilt) == AnimationStatus::Ok
				? rebuilt : std::vector<std::uint8_t>{};
		}
	}

	void RegisterRagdollProfileAssetTests(TestRegistry& registry) {
		static_assert(noexcept(ValidateRagdollProfileAssetStructure(
			std::declval<const RagdollProfileAsset&>())));
		static_assert(noexcept(SerializeRagdollProfileAsset(
			std::declval<const RagdollProfileAsset&>(),
			std::declval<std::vector<std::uint8_t>&>())));
		static_assert(noexcept(DeserializeRagdollProfileAsset(
			std::declval<std::span<const std::uint8_t>>(), 0U,
			std::declval<RagdollProfileAsset&>())));
		static_assert(noexcept(ValidateRagdollProfile(
			std::declval<const RagdollProfileAsset&>(),
			std::declval<const SkeletonAsset&>(),
			std::declval<RagdollProfileValidationOptions>())));

		registry.Add("Animation.Ragdoll.ProfileRoundTripUsesCommonEnvelope",
			[](TestContext& context) {
				const SkeletonAsset skeleton = MakeSkeleton();
				const RagdollProfileAsset profile = MakeProfile(skeleton);
				std::vector<std::uint8_t> bytes;
				context.Expect(SerializeRagdollProfileAsset(profile, bytes)
					== AnimationStatus::Ok,
					"A complete ragdoll profile should serialize.");
				AnimationAssetReader envelope;
				context.Expect(envelope.Parse(bytes) == AnimationStatus::Ok,
					"The ragdoll file must use the shared checked animation envelope.");
				context.Expect(envelope.Dependencies().size() == 3U,
					"Skeleton, recovery clip, and immutable collider must be dependencies.");
				RagdollProfileAsset decoded;
				context.Expect(DeserializeRagdollProfileAsset(bytes,
					skeleton.compatibilityFingerprint, decoded) == AnimationStatus::Ok,
					"A valid envelope and payload should deserialize.");
				context.Expect(decoded == profile,
					"All body, shape, mass, constraint, blend, and recovery data must round trip.");

				RagdollProfileAsset reordered = profile;
				std::reverse(reordered.bodies.begin(), reordered.bodies.end());
				std::reverse(reordered.constraints.begin(), reordered.constraints.end());
				std::reverse(reordered.blends.begin(), reordered.blends.end());
				std::reverse(reordered.requiredBodyJointStableIds.begin(),
					reordered.requiredBodyJointStableIds.end());
				std::reverse(reordered.recovery.front().contactJointStableIds.begin(),
					reordered.recovery.front().contactJointStableIds.end());
				std::vector<std::uint8_t> reorderedBytes;
				context.Expect(SerializeRagdollProfileAsset(reordered, reorderedBytes)
					== AnimationStatus::Ok && reorderedBytes == bytes,
					"Canonical ordering should make equivalent authoring data byte deterministic.");
			});

		registry.Add("Animation.Ragdoll.ProfileRejectsVersionsAndCorruption",
			[](TestContext& context) {
				const SkeletonAsset skeleton = MakeSkeleton();
				std::vector<std::uint8_t> bytes;
				context.Expect(SerializeRagdollProfileAsset(MakeProfile(skeleton), bytes)
					== AnimationStatus::Ok, "The test profile should serialize.");

				std::vector<std::uint8_t> unsupportedEnvelope = bytes;
				AnimationAssetEnvelope envelope{};
				std::memcpy(&envelope, unsupportedEnvelope.data(), sizeof(envelope));
				++envelope.majorVersion;
				std::memcpy(unsupportedEnvelope.data(), &envelope, sizeof(envelope));
				RagdollProfileAsset output;
				context.Expect(DeserializeRagdollProfileAsset(unsupportedEnvelope, 0U, output)
					== AnimationStatus::UnsupportedMajorVersion,
					"An unknown common-envelope major version must fail closed.");

				AnimationAssetReader reader;
				context.Expect(reader.Parse(bytes) == AnimationStatus::Ok,
					"The source envelope should parse before payload mutation.");
				AnimationAssetWriter missingDependencyWriter;
				for(std::size_t index = 0U; index + 1U < reader.Dependencies().size(); ++index) {
					missingDependencyWriter.AddDependency(reader.Dependencies()[index]);
				}
				missingDependencyWriter.AddChunk(kAnimationChunkPayload, true,
					reader.FindChunk(kAnimationChunkPayload));
				std::vector<std::uint8_t> missingDependency;
				context.Expect(missingDependencyWriter.Finish(reader.Envelope(),
					missingDependency) == AnimationStatus::Ok
					&& DeserializeRagdollProfileAsset(missingDependency, 0U, output)
						== AnimationStatus::InvalidEnvelope,
					"A profile may not omit a skeleton, clip, or immutable-shape dependency.");
				std::vector<std::uint8_t> payload(reader.FindChunk(kAnimationChunkPayload).begin(),
					reader.FindChunk(kAnimationChunkPayload).end());
				payload[sizeof(std::uint32_t)] = 2U;
				const std::vector<std::uint8_t> unsupportedPayload =
					RebuildWithPayload(bytes, payload);
				context.Expect(DeserializeRagdollProfileAsset(unsupportedPayload, 0U, output)
					== AnimationStatus::UnsupportedMajorVersion,
					"An unknown ragdoll payload version must fail closed.");

				std::vector<std::uint8_t> corrupt = bytes;
				corrupt.back() ^= 0x5aU;
				context.Expect(DeserializeRagdollProfileAsset(corrupt, 0U, output)
					== AnimationStatus::ContentHashMismatch,
					"Payload corruption must be detected by the common content hash.");
				std::vector<std::uint8_t> truncated = bytes;
				truncated.pop_back();
				context.Expect(DeserializeRagdollProfileAsset(truncated, 0U, output)
					!= AnimationStatus::Ok,
					"A truncated common envelope must never publish a profile.");
				// Restore the supported profile version before isolating a deep
				// truncation failure from the version-rejection case above.
				payload[sizeof(std::uint32_t)] =
					static_cast<std::uint8_t>(kRagdollProfileAssetCurrentVersion);
				payload.pop_back();
				const std::vector<std::uint8_t> deepTruncation =
					RebuildWithPayload(bytes, payload);
				context.Expect(DeserializeRagdollProfileAsset(deepTruncation, 0U, output)
					== AnimationStatus::TruncatedInput,
					"A hash-valid truncated typed payload must be rejected before publication.");
				context.Expect(DeserializeRagdollProfileAsset(bytes, 0xdeadU, output)
					== AnimationStatus::IncompatibleAsset,
					"A caller-provided skeleton fingerprint must be enforced.");
			});

		registry.Add("Animation.Ragdoll.ProfileBoundsAndLastGoodAreAtomic",
			[](TestContext& context) {
				const SkeletonAsset skeleton = MakeSkeleton();
				RagdollProfileAsset invalidHull = MakeProfile(skeleton);
				invalidHull.bodies[1U].colliders.front().convexHullVertices.pop_back();
				std::vector<std::uint8_t> unchanged{0xaaU, 0xbbU};
				context.Expect(SerializeRagdollProfileAsset(invalidHull, unchanged)
					== AnimationStatus::InvalidArgument
					&& unchanged == std::vector<std::uint8_t>({0xaaU, 0xbbU}),
					"An inline convex hull needs a finite three-dimensional point set.");
				RagdollProfileAsset oversized = MakeProfile(skeleton);
				oversized.bodies.resize(kRagdollProfileMaximumBodies + 1U);
				context.Expect(SerializeRagdollProfileAsset(oversized, unchanged)
					== AnimationStatus::HardLimitExceeded,
					"Body counts beyond the frozen bound must reject before allocation/cook.");
				context.Expect(unchanged == std::vector<std::uint8_t>({0xaaU, 0xbbU}),
					"A failed writer must preserve the caller's last-good bytes.");

				const RagdollProfileAsset lastGood = MakeProfile(skeleton);
				RagdollProfileAsset output = lastGood;
				std::vector<std::uint8_t> validBytes;
				context.Expect(SerializeRagdollProfileAsset(lastGood, validBytes)
					== AnimationStatus::Ok, "The last-good fixture should serialize.");
				std::vector<std::uint8_t> excessiveTables = validBytes;
				AnimationAssetEnvelope excessiveEnvelope{};
				std::memcpy(&excessiveEnvelope, excessiveTables.data(),
					sizeof(excessiveEnvelope));
				excessiveEnvelope.dependencyCount =
					static_cast<std::uint32_t>(kRagdollProfileMaximumColliders
						+ kRagdollProfileMaximumRecoveryRecords + 2U);
				std::memcpy(excessiveTables.data(), &excessiveEnvelope,
					sizeof(excessiveEnvelope));
				context.Expect(DeserializeRagdollProfileAsset(excessiveTables, 0U, output)
					== AnimationStatus::HardLimitExceeded && output == lastGood,
					"Asset-specific envelope table caps must reject before reader allocation.");
				validBytes.back() ^= 0xffU;
				context.Expect(DeserializeRagdollProfileAsset(validBytes, 0U, output)
					!= AnimationStatus::Ok && output == lastGood,
					"A failed staged read must leave the prior live asset unchanged.");
			});

		registry.Add("Animation.Ragdoll.ProfileValidatesSkeletonCoverageAndTopology",
			[](TestContext& context) {
				const SkeletonAsset skeleton = MakeSkeleton();
				const RagdollProfileAsset valid = MakeProfile(skeleton);
				const RagdollProfileValidationReport validReport = Validate(valid, skeleton);
				context.Expect(validReport.status == AnimationStatus::Ok
					&& validReport.rootBodyIds == std::vector<std::uint64_t>({100U}),
					"A complete skeleton-aligned tree should validate.");

				RagdollProfileAsset missingJoint = valid;
				missingJoint.bodies.back().stableJointId = 999U;
				const RagdollProfileValidationReport missingReport =
					Validate(missingJoint, skeleton);
				context.Expect(HasCode(missingReport,
					RagdollProfileDiagnosticCode::UnknownBodyJoint)
					&& HasCode(missingReport,
						RagdollProfileDiagnosticCode::MissingRequiredBody),
					"Unknown mappings and coverage gaps need separate actionable diagnostics.");

				RagdollProfileAsset wrongOrder = valid;
				wrongOrder.constraints.front() = Constraint(400U, 200U, 100U);
				const RagdollProfileValidationReport orderReport =
					Validate(wrongOrder, skeleton);
				context.Expect(HasCode(orderReport,
					RagdollProfileDiagnosticCode::ConstraintSkeletonOrder),
					"Constraint direction must follow actual skeleton ancestry.");

				RagdollProfileAsset cyclic = valid;
				cyclic.constraints.push_back(Constraint(600U, 300U, 100U));
				const RagdollProfileValidationReport cycleReport = Validate(cyclic, skeleton);
				context.Expect(HasCode(cycleReport,
					RagdollProfileDiagnosticCode::ConstraintCycle),
					"A cycle must be found even when every endpoint is otherwise valid.");
			});

		registry.Add("Animation.Ragdoll.ProfileValidatesCollisionMassMotorsAndRecovery",
			[](TestContext& context) {
				const SkeletonAsset skeleton = MakeSkeleton();
				RagdollProfileAsset invalid = MakeProfile(skeleton);
				invalid.bodies.front().collisionMask = 0x1U;
				invalid.bodies.front().massKilograms = 100.0F;
				invalid.expectedTotalMassKilograms = 30.0F;
				RagdollAxisMotor& motor = invalid.constraints.front().motors[
					static_cast<std::size_t>(RagdollConstraintAxis::AngularX)];
				motor.maximumForce = 0.0F;
				invalid.recovery.front().requiredReferencePoseHash = 0xbeefU;
				invalid.recovery.front().clipSkeletonFingerprint = 0xcafeU;
				invalid.recovery.front().contactJointStableIds.push_back(999U);
				const RagdollProfileValidationReport report = Validate(invalid, skeleton);
				context.Expect(HasCode(report,
					RagdollProfileDiagnosticCode::AsymmetricCollisionFilter),
					"Asymmetric body masks must be rejected deterministically.");
				context.Expect(HasCode(report,
					RagdollProfileDiagnosticCode::TotalMassOutsideExpectedRange)
					&& HasCode(report,
						RagdollProfileDiagnosticCode::BodyMassDistribution),
					"Total mass and per-body distribution require independent evidence.");
				context.Expect(HasCode(report,
					RagdollProfileDiagnosticCode::MotorMaximumForceMissing),
					"An enabled motor must declare a bounded positive maximum force.");
				context.Expect(HasCode(report,
					RagdollProfileDiagnosticCode::RecoveryClipIncompatible)
					&& HasCode(report,
					RagdollProfileDiagnosticCode::RecoveryReferencePoseMismatch)
					&& HasCode(report,
						RagdollProfileDiagnosticCode::RecoveryContactJointMissing),
					"Recovery reference-pose and contact mappings must be checked separately.");
			});

		registry.Add("Animation.Ragdoll.ProfileDuplicatesHaveDeterministicDiagnostics",
			[](TestContext& context) {
				const SkeletonAsset skeleton = MakeSkeleton();
				RagdollProfileAsset first = MakeProfile(skeleton);
				first.bodies.push_back(first.bodies.front());
				first.constraints.push_back(first.constraints.front());
				first.blends.push_back(first.blends.front());
				first.recovery.push_back(first.recovery.front());
				RagdollProfileAsset second = first;
				std::reverse(second.bodies.begin(), second.bodies.end());
				std::reverse(second.constraints.begin(), second.constraints.end());
				std::reverse(second.blends.begin(), second.blends.end());
				std::reverse(second.recovery.begin(), second.recovery.end());
				const RagdollProfileValidationReport firstReport = Validate(first, skeleton);
				const RagdollProfileValidationReport secondReport = Validate(second, skeleton);
				context.Expect(HasCode(firstReport,
					RagdollProfileDiagnosticCode::DuplicateBodyId)
					&& HasCode(firstReport,
						RagdollProfileDiagnosticCode::DuplicateColliderId)
					&& HasCode(firstReport,
						RagdollProfileDiagnosticCode::DuplicateConstraintId)
					&& HasCode(firstReport,
						RagdollProfileDiagnosticCode::DuplicateBlendJoint)
					&& HasCode(firstReport,
						RagdollProfileDiagnosticCode::DuplicateRecoveryCandidate),
					"Every stable identity family must reject duplicates.");
				context.Expect(DiagnosticIdentity(firstReport)
					== DiagnosticIdentity(secondReport),
					"Diagnostic code and subject ordering must not depend on authoring array order.");
			});
	}
}
