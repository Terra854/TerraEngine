// RagdollBlendTests.cpp - Full/partial, boundary, timing, and parity regressions.

#include "TestHarness.h"

#include "Animation/Ragdoll/RagdollBlend.h"

#include <array>
#include <cmath>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		RagdollGenerationStamp Generation() {
			return {1U, 2U, 3U, 4U, 5};
		}

		PhysicsPoseSnapshot PhysicsPose() {
			PhysicsPoseSnapshot pose;
			pose.generation = Generation();
			pose.completeCurrentStep = true;
			for(std::uint64_t index = 0U; index < 3U; ++index) {
				PhysicsPoseBodySnapshot body;
				body.bodyId = index + 1U;
				body.stableJointId = 10U + index * 10U;
				body.modelPose.translation = {static_cast<float>(index + 1U) * 4.0F, 0.0F, 0.0F};
				pose.bodies.push_back(body);
			}
			return pose;
		}

		bool Close(float left, float right) {
			return std::abs(left - right) <= 0.0001F;
		}
	}

	void RegisterRagdollBlendTests(TestRegistry& registry) {
		registry.Add("Animation.Ragdoll.PartialBlendKeepsKinematicBoundary", [](TestContext& context) {
			const std::array<std::uint64_t, 1U> ids{10U};
			const std::array<AnimationTransform, 1U> animation{};
			const PhysicsPoseSnapshot physics = PhysicsPose();
			RagdollBlendInput boundary{10U, 1.0F, true, RagdollBoundaryPolicy::KinematicAnchor};
			RagdollBlendResult output;
			context.Expect(BlendRagdollPose(ids, animation, physics, Generation(),
				std::span<const RagdollBlendInput>(&boundary, 1U), 1.0F, output) == AnimationStatus::Ok,
				"A valid partial blend must publish one complete copied result.");
			context.Expect(output.effectiveWeights[0U] == 0.0F
				&& output.modelPose[0U] == animation[0U],
				"A kinematic animated/simulated boundary must remain exactly animation-driven.");
		});

		registry.Add("Animation.Ragdoll.BranchLayerMaskAndTimeFactorsComposeDeterministically", [](TestContext& context) {
			const std::array<std::uint64_t, 3U> ids{10U, 20U, 30U};
			const std::array<std::int32_t, 3U> parents{-1, 0, 1};
			const std::array<AnimationTransform, 3U> animation{};
			std::array<RagdollBlendInput, 3U> inputs{};
			inputs[0U].stableJointId = 10U;
			inputs[0U].branchWeight = 0.5F;
			inputs[0U].includeDescendants = true;
			inputs[1U].stableJointId = 20U;
			inputs[1U].layerWeight = 0.5F;
			inputs[1U].maskWeight = 0.5F;
			inputs[1U].timeStartWeight = 0.0F;
			inputs[1U].timeTargetWeight = 1.0F;
			inputs[1U].elapsedSeconds = 0.5F;
			inputs[1U].durationSeconds = 1.0F;
			inputs[2U].stableJointId = 30U;
			inputs[2U].weight = 0.8F;
			RagdollBlendResult output;
			context.Expect(BlendRagdollPose(ids, parents, animation, PhysicsPose(), Generation(),
				inputs, 0.5F, output) == AnimationStatus::Ok,
				"Branch, layer, mask, and time inputs must lower into one deterministic reference pass.");
			context.Expect(Close(output.effectiveWeights[0U], 0.25F)
				&& Close(output.effectiveWeights[1U], 0.03125F)
				&& Close(output.effectiveWeights[2U], 0.2F),
				"Each independent authored weight must contribute exactly once in the documented order.");
			context.Expect(Close(output.modelPose[0U].translation.x, 1.0F)
				&& Close(output.modelPose[1U].translation.x, 0.25F)
				&& Close(output.modelPose[2U].translation.x, 2.4F),
				"The CPU reference pose must match the same factors used by the visual GPU stage.");
		});

		registry.Add("Animation.Ragdoll.MatchBoundaryIsContinuousAndMotorTargetsAnimation", [](TestContext& context) {
			const std::array<std::uint64_t, 2U> ids{10U, 20U};
			const std::array<std::int32_t, 2U> parents{-1, 0};
			const std::array<AnimationTransform, 2U> animation{};
			std::array<RagdollBlendInput, 2U> inputs{};
			inputs[0U].stableJointId = 10U;
			inputs[0U].weight = 0.25F;
			inputs[1U].stableJointId = 20U;
			inputs[1U].weight = 1.0F;
			inputs[1U].isBoundary = true;
			inputs[1U].boundaryPolicy = RagdollBoundaryPolicy::MatchAnimation;
			RagdollBlendResult output;
			context.Expect(BlendRagdollPose(ids, parents, animation, PhysicsPose(), Generation(),
				inputs, 1.0F, output) == AnimationStatus::Ok
				&& Close(output.effectiveWeights[1U], output.effectiveWeights[0U]),
				"A MatchAnimation boundary must not jump to a stronger simulated weight than its parent.");

			inputs[1U].boundaryPolicy = RagdollBoundaryPolicy::MotorToAnimation;
			inputs[1U].motorStrength = 80.0F;
			context.Expect(BlendRagdollPose(ids, parents, animation, PhysicsPose(), Generation(),
				inputs, 1.0F, output) == AnimationStatus::Ok
				&& Close(output.effectiveMotorStrengths[1U], 0.0F),
				"A fully simulated motor boundary has no residual animation correction.");
			inputs[1U].weight = 0.25F;
			context.Expect(BlendRagdollPose(ids, parents, animation, PhysicsPose(), Generation(),
				inputs, 1.0F, output) == AnimationStatus::Ok
				&& Close(output.effectiveMotorStrengths[1U], 60.0F),
				"Motor strength must scale with the remaining animation-authoritative fraction.");
		});

		registry.Add("Animation.Ragdoll.StaleOrIncompleteSnapshotPreservesLastGoodOutput", [](TestContext& context) {
			const std::array<std::uint64_t, 1U> ids{10U};
			const std::array<AnimationTransform, 1U> animation{};
			RagdollBlendResult output;
			output.effectiveWeights = {0.125F};
			PhysicsPoseSnapshot stale = PhysicsPose();
			stale.completeCurrentStep = false;
			context.Expect(BlendRagdollPose(ids, animation, stale, Generation(), {}, 1.0F, output)
				== AnimationStatus::StaleHandle && output.effectiveWeights == std::vector<float>{0.125F},
				"A stale current-step snapshot must reject before replacing any part of the last visual pose.");
		});
	}
}
