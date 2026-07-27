// ProceduralAnimationTests.cpp - Constraint, foot-query, and diagnostic coverage.

#include "TestHarness.h"

#include "Animation/Diagnostics/ProceduralDiagnostics.h"
#include "Animation/Graph/ProceduralNodeEvaluator.h"
#include "Animation/Procedural/AnimationConstraints.h"
#include "Animation/Procedural/FootPlacement.h"

#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		ProceduralGenerationStamp MakeGeneration(AnimationFixedTick tick = 60) {
			ProceduralGenerationStamp generation;
			generation.animator = {21U, 2U};
			generation.componentGeneration = 3U;
			generation.skeletonGeneration = 4U;
			generation.programGeneration = 5U;
			generation.poseGeneration = 6U;
			generation.publicationGeneration = 7U;
			generation.queryGeneration = 8U;
			generation.tick = tick;
			return generation;
		}

		AnimationTransform At(float x, float y, float z) {
			AnimationTransform transform;
			transform.translation = {x, y, z};
			return transform;
		}

		ProceduralGoal MakeGoal(
			const ProceduralGenerationStamp& generation,
			std::uint64_t id,
			AnimationFloat3 position,
			std::int32_t priority = 0,
			float weight = 1.0F) {
			ProceduralGoal goal;
			goal.generation = generation;
			goal.target.stableTargetId = 1000U + id;
			goal.goalId = id;
			goal.producerOrder = id;
			goal.priority = priority;
			goal.position = position;
			goal.weight = weight;
			goal.effectiveTick = generation.tick;
			return goal;
		}

		ProceduralQueryHit MakeHit(
			std::uint64_t queryId,
			std::uint64_t targetId,
			AnimationFloat3 point,
			AnimationFloat3 normal = {0.0F, 1.0F, 0.0F}) {
			ProceduralQueryHit hit;
			hit.queryId = queryId;
			hit.target.stableTargetId = targetId;
			hit.point = point;
			hit.normal = normal;
			hit.distance = 0.5F;
			return hit;
		}

		FootPlacementFootInput MakeFoot(std::uint64_t footId, std::uint64_t queryId, float x) {
			FootPlacementFootInput foot;
			foot.stableFootId = footId;
			foot.queryId = queryId;
			foot.animatedTransform = At(x, 0.0F, 0.0F);
			foot.hipPosition = {x, 1.0F, 0.0F};
			foot.maximumReachMeters = 1.5F;
			return foot;
		}

		GraphPoseContribution MakeGraphPose(float rootX = 0.0F) {
			GraphPoseContribution pose;
			pose.compatibilityFingerprint = 77U;
			pose.jointCount = 4U;
			pose.jointStableIds = {10U, 20U, 30U, 40U};
			pose.joints = {
				At(rootX, 0.0F, 0.0F),
				At(rootX + 1.0F, 0.0F, 0.0F),
				At(rootX + 2.0F, 0.0F, 0.0F),
				At(0.0F, 1.0F, 0.0F)};
			return pose;
		}

		AnimationMaskAsset MakeGraphMask(
			std::vector<AnimationMaskDenseJointWeight> weights) {
			AnimationMaskAsset mask;
			mask.assetId.value[0] = 1U;
			mask.skeletonAssetId.value[0] = 2U;
			mask.compatibilityFingerprint = 77U;
			mask.compiledJointWeights = std::move(weights);
			return mask;
		}

		ProceduralGraphNodeInput MakeCopyNode(
			std::uint64_t stableId,
			std::uint32_t layerOrder,
			std::uint32_t jointIndex,
			float targetX) {
			ProceduralCopyTransformNodeInput copy;
			copy.jointIndex = jointIndex;
			copy.targetTransform = At(targetX, 0.0F, 0.0F);
			copy.goalId = stableId;
			ProceduralGraphNodeInput node;
			node.stableId = stableId;
			node.layerOrder = layerOrder;
			node.payload = std::move(copy);
			return node;
		}
	}

	void RegisterProceduralAnimationTests(TestRegistry& registry)
	{
		registry.Add("Animation.Procedural.GoalsAreCopiedSortedAndRejectedAtomically", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const std::array goals{
				MakeGoal(generation, 3U, {3.0F, 0.0F, 0.0F}, 1),
				MakeGoal(generation, 2U, {2.0F, 0.0F, 0.0F}, 5),
				MakeGoal(generation, 1U, {1.0F, 0.0F, 0.0F}, 5)};
			ProceduralGoalSnapshot snapshot;
			context.Expect(BuildProceduralGoalSnapshot(generation, goals, snapshot) == AnimationStatus::Ok
				&& snapshot.goals[0].goalId == 1U
				&& snapshot.goals[1].goalId == 2U
				&& snapshot.goals[2].goalId == 3U,
				"Copied goals should sort by priority and stable producer order, not arrival order.");

			std::array invalid{goals[0], goals[0]};
			invalid[1].position.x = std::numeric_limits<float>::quiet_NaN();
			context.Expect(BuildProceduralGoalSnapshot(generation, invalid, snapshot)
				== AnimationStatus::InvalidArgument
				&& snapshot.goals.size() == goals.size()
				&& snapshot.goals.front().goalId == 1U,
				"A malformed producer batch must leave the prior complete snapshot unchanged.");
		});

		registry.Add("Animation.Procedural.AimLookAndMultiTargetConstraintsStayBounded", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const std::array goals{
				MakeGoal(generation, 1U, {1.0F, 0.0F, 0.0F}, 4, 0.25F),
				MakeGoal(generation, 2U, {1.0F, 0.0F, 0.0F}, 4, 0.75F),
				MakeGoal(generation, 3U, {-5.0F, 0.0F, 0.0F}, 1, 1.0F)};
			ProceduralGoalSnapshot snapshot;
			static_cast<void>(BuildProceduralGoalSnapshot(generation, goals, snapshot));

			WeightedProceduralTargetResult blended;
			context.Expect(BlendProceduralTargets(snapshot, generation, {}, blended) == AnimationStatus::Ok
				&& std::abs(blended.transform.translation.x - 1.0F) < 0.0001F
				&& blended.diagnostics.size() == goals.size()
				&& blended.diagnostics.back().finalContribution == 0.0F,
				"Only equal highest-priority targets should contribute to the normalized target.");

			AimConstraintRequest aim;
			aim.generation = generation;
			aim.expectedGeneration = generation;
			aim.goalSnapshot = &snapshot;
			aim.maximumAngleRadians = 3.14159265358979323846F;
			TransformConstraintResult aimed;
			context.Expect(SolveAimConstraint(aim, aimed) == AnimationStatus::Ok
				&& ProceduralMath::Dot(ProceduralMath::NormalizeOr(
					ProceduralMath::Rotate(aimed.transform.rotation, {0.0F, 0.0F, 1.0F}),
					{1.0F, 0.0F, 0.0F}), {1.0F, 0.0F, 0.0F}) > 0.999F,
				"Aim should honor weighted targets and align the selected local axis.");

			LookConstraintRequest look;
			look.aim = aim;
			look.maximumYawRadians = 0.10F;
			look.maximumPitchRadians = 0.10F;
			TransformConstraintResult looked;
			context.Expect(SolveLookConstraint(look, looked) == AnimationStatus::Ok
				&& looked.header.state == ProceduralSolverState::Clamped
				&& looked.diagnostic.clamped,
				"Look should expose yaw/pitch clamping instead of silently exceeding limits.");
		});

		registry.Add("Animation.Procedural.CopyJointAndTwistConstraintsRespectSpacesAndWeights", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			CopyTransformConstraintRequest copy;
			copy.generation = generation;
			copy.expectedGeneration = generation;
			copy.currentLocalTransform = At(0.0F, 0.0F, 0.0F);
			copy.parentWorldTransform = At(10.0F, 0.0F, 0.0F);
			copy.targetTransform = At(12.0F, 3.0F, 0.0F);
			copy.targetSpace = ProceduralCoordinateSpace::World;
			TransformConstraintResult copied;
			context.Expect(ApplyCopyTransformConstraint(copy, copied) == AnimationStatus::Ok
				&& copied.transform.translation == AnimationFloat3{2.0F, 3.0F, 0.0F},
				"A world-space copy should convert through the copied parent transform.");

			JointRotationConstraintRequest joint;
			joint.generation = generation;
			joint.expectedGeneration = generation;
			joint.candidateRotation = ProceduralMath::AxisAngle({1.0F, 0.0F, 0.0F}, 1.0F);
			joint.limit.enabled = true;
			joint.limit.maximumSwingRadians = 0.25F;
			joint.limit.maximumTwistRadians = 0.20F;
			JointRotationConstraintResult limited;
			context.Expect(ApplyJointRotationConstraint(joint, limited) == AnimationStatus::Ok
				&& limited.header.state == ProceduralSolverState::Clamped
				&& std::abs(limited.twistRadians) <= 0.2001F,
				"Joint constraints should clamp copied swing/twist values before publication.");

			const std::array transforms{At(0.0F, 0.0F, 0.0F), At(1.0F, 0.0F, 0.0F)};
			const std::array weights{1.0F, 3.0F};
			TwistDistributionRequest twist;
			twist.generation = generation;
			twist.expectedGeneration = generation;
			twist.transforms = transforms;
			twist.distributionWeights = weights;
			twist.twistRadians = 1.0F;
			TwistDistributionResult distributed;
			context.Expect(ApplyTwistDistribution(twist, distributed) == AnimationStatus::Ok
				&& distributed.diagnostics.size() == 2U
				&& std::abs(distributed.diagnostics[0].finalContribution - 0.25F) < 0.0001F
				&& std::abs(distributed.diagnostics[1].finalContribution - 0.75F) < 0.0001F,
				"Twist distribution should normalize copied chain weights deterministically.");
		});

		registry.Add("Animation.Procedural.FootPlacementLocksReleasesAndCompensatesPelvis", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const std::array hits{
				MakeHit(1U, 101U, {-0.25F, 0.20F, 0.0F}, {0.0F, 0.8F, 0.6F}),
				MakeHit(2U, 102U, {0.25F, 0.40F, 0.0F})};
			ProceduralQuerySnapshot queries;
			context.Expect(BuildProceduralQuerySnapshot(
				generation, generation.tick, hits, queries) == AnimationStatus::Ok,
				"Foot placement should consume a checked copied query snapshot.");
			std::array feet{MakeFoot(11U, 1U, -0.25F), MakeFoot(12U, 2U, 0.25F)};
			FootPlacementRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.querySnapshot = &queries;
			request.feet = feet;
			request.pelvisTransform = At(0.0F, 1.0F, 0.0F);
			request.maximumPelvisRaiseMeters = 0.25F;
			request.pelvisSmoothingSeconds = 0.0F;
			FootPlacementResult planted;
			context.Expect(SolveFootPlacement(request, planted) == AnimationStatus::Ok
				&& planted.feet.size() == feet.size()
				&& planted.feet[0].contact.phase == FootContactPhase::Locked
				&& planted.feet[1].contact.phase == FootContactPhase::Locked
				&& std::abs(planted.pelvisOffsetMeters - 0.25F) < 0.0001F,
				"Slope and step contacts should orient both feet and apply bounded pelvis compensation.");

			const std::array movedHits{
				MakeHit(1U, 101U, {-0.25F, 0.80F, 0.0F}),
				MakeHit(2U, 102U, {0.25F, 0.90F, 0.0F})};
			ProceduralQuerySnapshot movedQueries;
			static_cast<void>(BuildProceduralQuerySnapshot(
				generation, generation.tick, movedHits, movedQueries));
			feet[0].previousContact = planted.feet[0].contact;
			feet[1].previousContact = planted.feet[1].contact;
			request.querySnapshot = &movedQueries;
			request.feet = feet;
			FootPlacementResult retained;
			context.Expect(SolveFootPlacement(request, retained) == AnimationStatus::Ok
				&& retained.feet[0].contact.lockedTransform
					== planted.feet[0].contact.lockedTransform,
				"A matching target identity should retain the planted transform instead of sliding.");

			ProceduralQuerySnapshot emptyQueries;
			static_cast<void>(BuildProceduralQuerySnapshot(
				generation, generation.tick, {}, emptyQueries));
			request.querySnapshot = &emptyQueries;
			request.deltaSeconds = 0.25F;
			request.releaseSmoothingSeconds = 1.0F;
			FootPlacementResult releasing;
			context.Expect(SolveFootPlacement(request, releasing) == AnimationStatus::Ok
				&& releasing.feet[0].contact.phase == FootContactPhase::Releasing
				&& std::abs(releasing.feet[0].contact.contribution - 0.75F) < 0.0001F,
				"A disappearing contact should fade from its copied lock state over the release interval.");
		});

		registry.Add("Animation.Procedural.FootPlacementRejectsStaleQueriesWithoutPartialPose", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			ProceduralGenerationStamp staleGeneration = generation;
			++staleGeneration.queryGeneration;
			const std::array hits{MakeHit(1U, 101U, {0.0F, 0.0F, 0.0F})};
			ProceduralQuerySnapshot staleQueries;
			static_cast<void>(BuildProceduralQuerySnapshot(
				staleGeneration, staleGeneration.tick, hits, staleQueries));
			const std::array feet{MakeFoot(11U, 1U, 0.0F)};
			FootPlacementRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.querySnapshot = &staleQueries;
			request.feet = feet;
			FootPlacementResult result;
			context.Expect(SolveFootPlacement(request, result) == AnimationStatus::StaleHandle
				&& result.header.state == ProceduralSolverState::StaleInput
				&& result.feet.size() == 1U
				&& result.feet[0].transform == feet[0].animatedTransform
				&& result.feet[0].diagnostic.staleInput,
				"A stale physics/query generation should preserve the authored foot pose as one complete result.");
		});

		registry.Add("Animation.Procedural.DiagnosticsAreCompleteBoundedAndAtomic", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			ProceduralDiagnosticRecord first;
			first.requestedGeneration = generation;
			first.observedGeneration = generation;
			first.solver = ProceduralSolverKind::TwoBoneIk;
			first.state = ProceduralSolverState::Converged;
			first.status = AnimationStatus::Ok;
			first.goalId = 1U;
			first.targetErrorMeters = 0.001F;
			first.inputWeight = 1.0F;
			first.finalContribution = 1.0F;
			first.iterationCount = 1U;
			first.iterationLimit = 1U;
			first.converged = true;
			ProceduralDiagnosticRecord second = first;
			second.goalId = 2U;
			second.state = ProceduralSolverState::Clamped;
			second.clamped = true;
			ProceduralDiagnosticRecord third = first;
			third.goalId = 3U;
			third.state = ProceduralSolverState::Singular;
			third.singular = true;
			const std::array records{first, second, third};

			ProceduralDiagnosticsBuffer buffer;
			context.Expect(buffer.Configure(2U) == AnimationStatus::Ok
				&& buffer.Publish(generation, records) == AnimationStatus::Ok,
				"The diagnostic buffer should accept a complete validated solver batch.");
			const auto published = buffer.AcquireCopy();
			context.Expect(published && published->records.size() == 2U
				&& published->records[0].goalId == 2U
				&& published->records[1].goalId == 3U
				&& published->counters.evictedRecords == 1U,
				"Bounded history should retain the newest complete target, clamp, and singularity records.");

			ProceduralDiagnosticRecord invalid = first;
			invalid.finalContribution = std::numeric_limits<float>::quiet_NaN();
			context.Expect(buffer.Publish(generation, std::span{&invalid, 1U})
				== AnimationStatus::InvalidArgument,
				"A non-finite contribution should reject the entire diagnostics batch.");
			const auto afterRejection = buffer.AcquireCopy();
			context.Expect(afterRejection && afterRejection->records.size() == 2U
				&& afterRejection->records[0].goalId == 2U
				&& afterRejection->counters.rejectedBatches == 1U,
				"Rejected diagnostics must preserve prior records while exposing the rejection counter.");
		});

		registry.Add("Animation.Procedural.GraphUsesAuthoredOrderMasksAndCopiedDiagnostics", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const GraphPoseContribution pose = MakeGraphPose();
			const AnimationMaskAsset rootOnly = MakeGraphMask({{10U, 1.0F}});
			ProceduralGraphNodeInput later = MakeCopyNode(10U, 2U, 0U, 2.0F);
			ProceduralGraphNodeInput earlier = MakeCopyNode(20U, 1U, 0U, 4.0F);
			ProceduralGraphNodeInput masked = MakeCopyNode(30U, 3U, 1U, 99.0F);
			masked.mask = &rootOnly;
			const std::array nodes{later, masked, earlier};
			ProceduralGraphEvaluationRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.inputPose = &pose;
			request.nodes = nodes;
			ProceduralGraphEvaluationResult result;
			const AnimationStatus status = EvaluateProceduralGraphNodes(request, result);
			context.Expect(status == AnimationStatus::Ok
				&& result.orderedNodeIds == std::vector<std::uint64_t>{20U, 10U, 30U}
				&& std::abs(result.pose.joints[0].translation.x - 2.0F) < 0.0001F
				&& result.pose.joints[1] == pose.joints[1],
				"Procedural nodes should sort by authored layer/node keys and apply reusable joint masks.");
			context.Expect(result.diagnostics.size() == 3U
				&& result.diagnostics[0].goalId == 20U
				&& result.diagnostics[1].goalId == 10U
				&& result.diagnostics[2].goalId == 30U
				&& result.diagnostics[2].finalContribution == 0.0F,
				"Copied diagnostics should follow evaluation order and expose a mask-suppressed contribution.");
		});

		registry.Add("Animation.Procedural.GraphComposesAdditiveIkAndPreservesCpuSnapshots", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const GraphPoseContribution pose = MakeGraphPose(10.0F);
			const GraphPoseContribution reference = MakeGraphPose(1.0F);
			ProceduralGraphNodeInput additive = MakeCopyNode(41U, 0U, 3U, 3.0F);
			additive.layerMode = AnimationGraphLayerMode::Additive;
			additive.layerWeight = 0.5F;
			additive.additiveReference = &reference;

			ProceduralTwoBoneNodeInput ik;
			ik.rootJointIndex = 0U;
			ik.middleJointIndex = 1U;
			ik.endJointIndex = 2U;
			ik.targetPosition = {11.0F, 1.0F, 0.0F};
			ik.polePosition = {10.0F, 0.0F, 2.0F};
			ik.goalId = 42U;
			ik.toleranceMeters = 0.001F;
			ProceduralGraphNodeInput ikNode;
			ikNode.stableId = 42U;
			ikNode.layerOrder = 1U;
			ikNode.payload = ik;
			const std::array nodes{ikNode, additive};

			ProceduralGraphEvaluationRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.inputPose = &pose;
			request.nodes = nodes;
			request.authoritativeConsumer = true;
			request.recordingRequested = true;
			request.rollbackRequested = true;
			ProceduralGraphEvaluationResult result;
			context.Expect(EvaluateProceduralGraphNodes(request, result) == AnimationStatus::Ok
				&& result.cpuAuthoritative && result.recordingSnapshot && result.rollbackSnapshot
				&& result.orderedNodeIds == std::vector<std::uint64_t>{41U, 42U}
				&& std::abs(result.pose.joints[3].translation.x - 1.5F) < 0.001F
				&& std::abs(result.pose.joints[2].translation.x - 11.0F) < 0.001F
				&& std::abs(result.pose.joints[2].translation.y - 1.0F) < 0.001F,
				"Additive graph deltas should compose before IK and produce authoritative recording/rollback copies.");

			GraphPoseContribution restored = MakeGraphPose(99.0F);
			std::vector<ProceduralNodeStateSnapshot> restoredStates{{999U, 1.0F, {}}};
			context.Expect(RestoreProceduralPoseSnapshot(*result.rollbackSnapshot,
				generation, pose.compatibilityFingerprint, restored, restoredStates) == AnimationStatus::Ok
				&& restored == result.pose && restoredStates == result.nodeStates,
				"Rollback should atomically restore the copied procedural pose and state for the exact generation.");
		});

		registry.Add("Animation.Procedural.GraphRejectsVisualAuthorityAndStaleWorkAtomically", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const GraphPoseContribution pose = MakeGraphPose();
			const std::array nodes{MakeCopyNode(51U, 0U, 0U, 5.0F)};
			ProceduralGraphEvaluationRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.inputPose = &pose;
			request.nodes = nodes;
			request.authority = ProceduralGraphAuthority::VisualOnly;
			request.recordingRequested = true;
			ProceduralGraphEvaluationResult result;
			result.pose = MakeGraphPose(77.0F);
			const ProceduralGraphEvaluationResult before = result;
			context.Expect(EvaluateProceduralGraphNodes(request, result)
				== AnimationStatus::DeterminismViolation && result.pose == before.pose,
				"A visual-only pass must not satisfy recording or another authoritative consumer.");

			request.authority = ProceduralGraphAuthority::CpuAuthoritative;
			request.recordingRequested = false;
			++request.generation.poseGeneration;
			context.Expect(EvaluateProceduralGraphNodes(request, result) == AnimationStatus::StaleHandle
				&& result.pose == before.pose,
				"A stale graph generation should reject before replacing the caller's complete prior result.");
		});
	}
}
