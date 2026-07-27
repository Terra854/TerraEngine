// ChainIkTests.cpp - Bounded FABRIK, conflict, and singular-chain coverage.

#include "TestHarness.h"

#include "Animation/Procedural/ChainIk.h"

#include <array>
#include <cmath>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		ProceduralGenerationStamp MakeGeneration() {
			ProceduralGenerationStamp generation;
			generation.animator = {11U, 2U};
			generation.componentGeneration = 3U;
			generation.skeletonGeneration = 4U;
			generation.programGeneration = 5U;
			generation.poseGeneration = 6U;
			generation.publicationGeneration = 7U;
			generation.queryGeneration = 8U;
			generation.tick = 240;
			return generation;
		}

		AnimationTransform At(float x, float y, float z) {
			AnimationTransform transform;
			transform.translation = {x, y, z};
			return transform;
		}

		std::vector<ChainIkJoint> MakeChain(bool mirrored = false) {
			const float sign = mirrored ? -1.0F : 1.0F;
			return {
				{100U, At(0.0F, 0.0F, 0.0F), {}},
				{101U, At(sign, 0.0F, 0.0F), {}},
				{102U, At(sign * 2.0F, 0.0F, 0.0F), {}}};
		}

		ProceduralGoal MakeGoal(
			const ProceduralGenerationStamp& generation,
			std::uint64_t id,
			AnimationFloat3 position,
			std::int32_t priority,
			float weight = 1.0F) {
			ProceduralGoal goal;
			goal.generation = generation;
			goal.target.stableTargetId = id + 1000U;
			goal.goalId = id;
			goal.producerOrder = id;
			goal.priority = priority;
			goal.position = position;
			goal.weight = weight;
			goal.effectiveTick = generation.tick;
			return goal;
		}

		bool FiniteChain(const ChainIkResult& result) {
			for(const ChainIkJoint& joint : result.joints) {
				if(!IsFinite(joint.transform) || !IsNormalized(joint.transform.rotation, 0.01F)) {
					return false;
				}
			}
			return std::isfinite(result.targetErrorMeters);
		}
	}

	void RegisterChainIkTests(TestRegistry& registry)
	{
		registry.Add("Animation.Procedural.ChainConvergesWithinBoundedIterations", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const std::array goals{MakeGoal(generation, 1U, {1.0F, 1.0F, 0.0F}, 3)};
			ProceduralGoalSnapshot snapshot;
			context.Expect(BuildProceduralGoalSnapshot(generation, goals, snapshot) == AnimationStatus::Ok,
				"The chain fixture should build a copied deterministic goal snapshot.");
			const std::vector joints = MakeChain();
			ChainIkRequest request{generation, generation, joints, &snapshot, 16U, 0.001F};
			ChainIkResult result;
			context.Expect(SolveChainIk(request, result) == AnimationStatus::Ok
				&& result.iterationCount <= request.maximumIterations
				&& result.targetErrorMeters <= request.toleranceMeters
				&& FiniteChain(result),
				"A reachable chain should converge without exceeding its caller-supplied iteration budget.");
		});

		registry.Add("Animation.Procedural.ChainConflictsUsePriorityThenStableWeights", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const std::array goals{
				MakeGoal(generation, 1U, {2.0F, 0.0F, 0.0F}, 1),
				MakeGoal(generation, 2U, {0.0F, 2.0F, 0.0F}, 5),
				MakeGoal(generation, 3U, {2.0F, 0.0F, 0.0F}, 5)};
			ProceduralGoalSnapshot snapshot;
			context.Expect(BuildProceduralGoalSnapshot(generation, goals, snapshot) == AnimationStatus::Ok,
				"Conflicting goals should be normalized into deterministic priority order.");
			const std::vector joints = MakeChain();
			ChainIkRequest request{generation, generation, joints, &snapshot, 32U, 0.001F};
			ChainIkResult result;
			context.Expect(SolveChainIk(request, result) == AnimationStatus::Ok
				&& std::abs(result.resolvedTarget.x - 1.0F) < 0.0001F
				&& std::abs(result.resolvedTarget.y - 1.0F) < 0.0001F
				&& result.diagnostics.size() == goals.size()
				&& result.diagnostics.back().finalContribution == 0.0F,
				"Equal highest-priority goals should blend stably while lower priorities contribute zero.");
		});

		registry.Add("Animation.Procedural.ChainZeroLengthUnreachableAndLimitsStayFinite", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const std::array goals{MakeGoal(generation, 1U, {8.0F, 0.0F, 0.0F}, 1)};
			ProceduralGoalSnapshot snapshot;
			static_cast<void>(BuildProceduralGoalSnapshot(generation, goals, snapshot));
			std::vector joints = MakeChain();
			joints[1].transform = joints[0].transform;
			joints[1].limit.enabled = true;
			joints[1].limit.maximumBendRadians = 0.25F;
			ChainIkRequest request{generation, generation, joints, &snapshot, 8U, 0.001F};
			ChainIkResult result;
			context.Expect(SolveChainIk(request, result) == AnimationStatus::Ok
				&& result.encounteredZeroLength
				&& result.header.state == ProceduralSolverState::Singular
				&& FiniteChain(result),
				"Zero-length and unreachable segments should select a bounded finite singular fallback.");

			ChainIkResult sentinel;
			sentinel.targetErrorMeters = 123.0F;
			request.maximumIterations = kChainIkIterationHardMaximum + 1U;
			context.Expect(SolveChainIk(request, sentinel) == AnimationStatus::HardLimitExceeded
				&& sentinel.targetErrorMeters == 123.0F,
				"An iteration request above 128 must reject before replacing the prior complete result.");
		});

		registry.Add("Animation.Procedural.MirroredChainIsBitStableAcrossTenThousandRuns", [](TestContext& context) {
			const ProceduralGenerationStamp generation = MakeGeneration();
			const std::array goals{MakeGoal(generation, 7U, {-1.0F, 1.0F, 0.0F}, 2)};
			ProceduralGoalSnapshot snapshot;
			static_cast<void>(BuildProceduralGoalSnapshot(generation, goals, snapshot));
			const std::vector joints = MakeChain(true);
			ChainIkRequest request{generation, generation, joints, &snapshot, 32U, 0.001F};
			ChainIkResult reference;
			context.Expect(SolveChainIk(request, reference) == AnimationStatus::Ok,
				"The mirrored repeat fixture should produce a reference result.");
			bool stable = true;
			for(std::size_t iteration = 0U; iteration < 10000U; ++iteration) {
				ChainIkResult repeated;
				stable = stable && SolveChainIk(request, repeated) == AnimationStatus::Ok
					&& repeated.joints.size() == reference.joints.size()
					&& repeated.header.state == reference.header.state;
				for(std::size_t joint = 0U; stable && joint < repeated.joints.size(); ++joint) {
					stable = repeated.joints[joint].transform == reference.joints[joint].transform;
				}
			}
			context.Expect(stable && FiniteChain(reference),
				"Mirrored input should remain finite and bit-identical across ten thousand evaluations.");
		});
	}
}
