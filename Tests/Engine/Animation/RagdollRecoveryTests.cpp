// RagdollRecoveryTests.cpp - Matching, alignment, moving-support, and stale gates.

#include "TestHarness.h"

#include "Animation/Ragdoll/RagdollRecovery.h"
#include "Animation/Ragdoll/RagdollState.h"

#include <cmath>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		AnimationGuid Guid(std::uint8_t seed) {
			AnimationGuid value;
			value.value.front() = seed;
			value.value.back() = static_cast<std::uint8_t>(seed ^ 0xa5U);
			return value;
		}

		RagdollGenerationStamp Generation() {
			return {1U, 2U, 3U, 4U, 5};
		}

		PhysicsPoseSnapshot Pose() {
			return {Generation(), {{1U, 10U, {}}}, true};
		}

		RagdollRecoveryRecord Candidate(std::uint64_t id, RagdollRecoveryPosture posture) {
			RagdollRecoveryRecord candidate;
			candidate.candidateId = id;
			candidate.clipAssetId = Guid(static_cast<std::uint8_t>(id));
			candidate.clipGeneration = 7U;
			candidate.clipSkeletonFingerprint = 0x2222U;
			candidate.requiredReferencePoseHash = 0x3333U;
			candidate.rootJointStableId = 10U;
			candidate.posture = posture;
			candidate.rootAlignmentOffset = {1.0F, 0.0F, 0.0F};
			candidate.maximumPoseErrorMeters = 0.5F;
			candidate.maximumLinearSpeed = 2.0F;
			candidate.maximumAngularSpeed = 3.0F;
			candidate.blendDurationSeconds = 0.5F;
			candidate.clearanceRadiusMeters = 0.25F;
			candidate.clearanceHeightMeters = 1.5F;
			return candidate;
		}

		RagdollProfileAsset Profile() {
			RagdollProfileAsset profile;
			profile.assetId = Guid(1U);
			profile.runtimeGeneration = Generation().profileGeneration;
			profile.skeletonAssetId = Guid(2U);
			profile.skeletonFingerprint = 0x2222U;
			profile.referencePoseHash = 0x3333U;
			profile.physicsProviderFingerprint = 0x4a4f4c54U;
			profile.rootBodyId = 1U;
			profile.allowedCollisionLayerMask = 0x1U;
			RagdollBodyRecord body;
			body.bodyId = 1U;
			body.stableJointId = 10U;
			body.collisionMask = 0x1U;
			RagdollColliderRecord collider;
			collider.colliderId = 11U;
			collider.shape = RagdollColliderShape::Sphere;
			collider.dimensions = {0.25F, 0.25F, 0.25F};
			body.colliders.push_back(collider);
			profile.bodies.push_back(body);
			profile.recovery = {
				Candidate(20U, RagdollRecoveryPosture::FaceUp),
				Candidate(10U, RagdollRecoveryPosture::FaceUp),
				Candidate(30U, RagdollRecoveryPosture::FaceDown)};
			return profile;
		}

		RagdollRecoveryRequest Request() {
			RagdollRecoveryRequest request;
			request.observedPosture = RagdollRecoveryPosture::FaceUp;
			request.poseErrorMeters = 0.1F;
			request.rootPosition = {2.0F, 0.0F, 3.0F};
			request.rootLinearSpeed = 0.5F;
			request.rootAngularSpeed = 0.25F;
			request.clearance = {Generation().worldGeneration, Generation().fixedStep,
				request.rootPosition, 0.5F, 2.0F, true};
			return request;
		}

		bool Close(float left, float right) {
			return std::abs(left - right) <= 0.0001F;
		}
	}

	void RegisterRagdollRecoveryTests(TestRegistry& registry) {
		registry.Add("Animation.Ragdoll.RecoveryReportsBlockedSpace", [](TestContext& context) {
			RagdollRecoveryRequest request = Request();
			request.clearance.clear = false;
			const auto result = SelectRagdollRecovery(Profile(), Pose(), Generation(), request);
			context.Expect(result.blocked && result.status == AnimationStatus::Rejected
				&& result.blockReason == RagdollRecoveryBlockReason::SpaceBlocked,
				"A complete blocked clearance result must never teleport recovery through geometry.");
		});

		registry.Add("Animation.Ragdoll.RecoveryBuildsPoseMatchedRequestFromCopiedState", [](TestContext& context) {
			PhysicsPoseSnapshot physics = Pose();
			physics.bodies.front().modelPose.translation = {2.0F, 0.0F, 3.0F};
			physics.bodies.front().linearVelocity = {0.5F, 0.0F, 0.0F};
			physics.bodies.front().angularVelocity = {0.0F, 0.25F, 0.0F};
			RagdollRecoveryObservation observation;
			observation.completeAnimationPose = Pose();
			observation.completeAnimationPose.bodies.front().modelPose.translation = {
				2.1F, 0.0F, 3.0F};
			observation.clearanceConfirmed = true;
			RagdollRecoveryRequest request;
			context.Expect(BuildRagdollRecoveryRequest(
				Profile(), physics, Generation(), observation, request) == AnimationStatus::Ok
				&& Close(request.poseErrorMeters, 0.1F)
				&& Close(request.rootLinearSpeed, 0.5F)
				&& request.clearance.worldGeneration == Generation().worldGeneration
				&& request.clearance.fixedStep == Generation().fixedStep,
				"Recovery matching must use one generation-compatible physics and animation observation.");
			const auto selected = SelectRagdollRecovery(Profile(), physics, Generation(), request);
			context.Expect(selected.status == AnimationStatus::Ok && selected.candidateId == 10U,
				"The copied production request must flow directly into deterministic get-up selection.");
		});

		registry.Add("Animation.Ragdoll.RecoverySelectionAndReplayTieBreakAreStable", [](TestContext& context) {
			const RagdollRecoveryRequest request = Request();
			const auto first = SelectRagdollRecovery(Profile(), Pose(), Generation(), request);
			const auto replay = SelectRagdollRecovery(Profile(), Pose(), Generation(), request);
			context.Expect(first.status == AnimationStatus::Ok && first.candidateId == 10U
				&& replay.candidateId == first.candidateId
				&& replay.transitionStartTick == Generation().fixedStep,
				"Equal pose matches must use stable candidate identity for rollback and replay.");
		});

		registry.Add("Animation.Ragdoll.MovingSupportAlignmentCarriesVelocityIntoHandoff", [](TestContext& context) {
			RagdollRecoveryRequest request = Request();
			request.requestedCandidateId = 20U;
			request.onMovingSupport = true;
			request.supportGeneration = 9U;
			request.supportVelocity = {2.0F, 0.0F, 0.0F};
			const auto result = SelectRagdollRecovery(Profile(), Pose(), Generation(), request);
			context.Expect(result.status == AnimationStatus::Ok && result.candidateId == 20U
				&& result.inheritedSupportVelocity == request.supportVelocity
				&& Close(result.alignedRootPosition.x, 4.0F),
				"Root alignment must include authored offset plus moving-support displacement over the blend handoff.");
		});

		registry.Add("Animation.Ragdoll.StaleClearanceAndIneligibleVelocityRejectWithoutCandidate", [](TestContext& context) {
			RagdollRecoveryRequest stale = Request();
			stale.clearance.fixedStep -= 1;
			const auto staleResult = SelectRagdollRecovery(Profile(), Pose(), Generation(), stale);
			context.Expect(staleResult.status == AnimationStatus::StaleHandle
				&& staleResult.candidateId == 0U
				&& staleResult.blockReason == RagdollRecoveryBlockReason::StaleClearance,
				"A clearance query from another physics step must reject before candidate publication.");
			RagdollRecoveryRequest fast = Request();
			fast.rootLinearSpeed = 20.0F;
			const auto fastResult = SelectRagdollRecovery(Profile(), Pose(), Generation(), fast);
			context.Expect(fastResult.status == AnimationStatus::NotReady
				&& fastResult.blockReason == RagdollRecoveryBlockReason::VelocityExceeded,
				"Recovery must remain pending until the copied body velocity enters the candidate envelope.");
		});

		registry.Add("Animation.Ragdoll.StateCarriesIntegratedRollbackRecordingAndNetworkValues", [](TestContext& context) {
			RagdollState state;
			RagdollStateSnapshot creating;
			creating.lifecycle = RagdollLifecycleState::Creating;
			creating.generation = Generation();
			creating.eventSequence = 4U;
			context.Expect(state.Publish(creating) == AnimationStatus::Ok,
				"The bounded lifecycle must begin with one generation-stamped creating record.");

			RagdollStateSnapshot active;
			active.lifecycle = RagdollLifecycleState::Active;
			active.generation = Generation();
			active.physicsPose = Pose();
			active.blendWeights = {0.75F};
			active.eventSequence = 5U;
			active.recordingSequence = 6U;
			active.motionMatchCandidateId = 7U;
			active.motionDatabaseGeneration = 8U;
			active.retargetGeneration = 9U;
			active.accumulatedRootTranslation = {1.0F, 0.0F, 2.0F};
			active.accumulatedRootYawRadians = 0.25F;
			active.contactMask = 0x3U;
			active.recoveryCandidateId = 13U;
			active.recoveryTransitionTick = 14;
			active.rollbackGeneration = 10U;
			active.replaySequence = 11U;
			active.networkSequence = 12U;
			context.Expect(state.Publish(active) == AnimationStatus::Ok
				&& state.Current().recordingSequence == 6U
				&& state.Current().deterministicHash != 0U,
				"Active state must preserve every recording, retarget, motion, root, rollback, replay, and network field.");
			const std::uint64_t authoritativeHash = state.Current().deterministicHash;
			RagdollStateSnapshot predicted = active;
			predicted.predicted = true;
			context.Expect(state.Publish(predicted) == AnimationStatus::Ok
				&& state.Current().deterministicHash != authoritativeHash,
				"Prediction and recovery-handoff identity must participate in the deterministic ragdoll hash.");

			std::vector<AnimationRagdollRollbackState> records;
			context.Expect(state.CaptureRollback(99U, records) == AnimationStatus::Ok
				&& records.size() == 1U && records.front().eventSequence == active.eventSequence,
				"Rollback capture must emit bounded transport-neutral body values and the one-shot event watermark.");
			RagdollState restored;
			context.Expect(restored.RestoreRollback(records, Generation()) == AnimationStatus::Ok
				&& restored.Current().physicsPose.bodies == active.physicsPose.bodies
				&& restored.Current().blendWeights == active.blendWeights,
				"A compatible rollback record must reproduce the complete authoritative body and blend state.");
			records.front().bodyGeneration += 1U;
			const std::uint64_t lastGoodHash = restored.Current().deterministicHash;
			context.Expect(restored.RestoreRollback(records, Generation()) == AnimationStatus::Ok
				&& restored.Current().deterministicHash != lastGoodHash,
				"A newer body-slot generation is valid when the whole copied rollback packet names it consistently.");
			records.front().worldGeneration += 1U;
			const std::uint64_t currentHash = restored.Current().deterministicHash;
			context.Expect(restored.RestoreRollback(records, Generation()) == AnimationStatus::StaleHandle
				&& restored.Current().deterministicHash == currentHash,
				"A stale world generation must reject atomically without replacing the restored last-good state.");
		});
	}
}
