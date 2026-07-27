// TwoBoneIkTests.cpp - Reach, limit, weighting, and stability coverage.

#include "TestHarness.h"

#include "Animation/Procedural/TwoBoneIk.h"

#include <cmath>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		ProceduralGenerationStamp MakeGeneration() {
			ProceduralGenerationStamp generation;
			generation.animator = {3U, 4U};
			generation.componentGeneration = 5U;
			generation.skeletonGeneration = 6U;
			generation.programGeneration = 7U;
			generation.poseGeneration = 8U;
			generation.publicationGeneration = 9U;
			generation.queryGeneration = 10U;
			generation.tick = 120;
			return generation;
		}

		AnimationTransform At(float x, float y, float z) {
			AnimationTransform transform;
			transform.translation = {x, y, z};
			return transform;
		}

		TwoBoneIkRequest MakeRequest() {
			TwoBoneIkRequest request;
			request.generation = MakeGeneration();
			request.expectedGeneration = request.generation;
			request.rootTransform = At(0.0F, 0.0F, 0.0F);
			request.middleTransform = At(1.0F, 0.0F, 0.0F);
			request.endTransform = At(2.0F, 0.0F, 0.0F);
			request.targetPosition = {1.0F, 1.0F, 0.0F};
			request.polePosition = {0.0F, 0.0F, 2.0F};
			request.goalId = 44U;
			request.producerOrder = 12U;
			request.priority = 2;
			request.toleranceMeters = 0.001F;
			return request;
		}

		bool FiniteResult(const TwoBoneIkResult& result) {
			return IsFinite(result.rootTransform) && IsFinite(result.middleTransform)
				&& IsFinite(result.endTransform) && std::isfinite(result.targetErrorMeters)
				&& IsFinite(result.diagnostic);
		}
	}

	void RegisterTwoBoneIkTests(TestRegistry& registry)
	{
		registry.Add("Animation.Procedural.TwoBoneReachUsesTargetAndPole", [](TestContext& context) {
			const TwoBoneIkRequest request = MakeRequest();
			TwoBoneIkResult result;
			const AnimationStatus status = SolveTwoBoneIk(request, result);
			context.Expect(status == AnimationStatus::Ok && FiniteResult(result)
				&& result.targetErrorMeters <= request.toleranceMeters
				&& result.middleTransform.translation.z > 0.0F
				&& result.diagnostic.converged,
				"A reachable target should be reached through the authored pole side with finite diagnostics.");
		});

		registry.Add("Animation.Procedural.TwoBoneStretchAndUnreachableStayBounded", [](TestContext& context) {
			TwoBoneIkRequest request = MakeRequest();
			request.targetPosition = {3.0F, 0.0F, 0.0F};
			request.polePosition = {0.0F, 1.0F, 0.0F};
			TwoBoneIkResult bounded;
			context.Expect(SolveTwoBoneIk(request, bounded) == AnimationStatus::Ok
				&& bounded.header.state == ProceduralSolverState::Unreachable
				&& std::abs(bounded.endTransform.translation.x - 2.0F) < 0.0001F
				&& FiniteResult(bounded),
				"An unreachable target without stretch should stop at the finite chain reach.");

			request.allowStretch = true;
			request.maximumStretchScale = 1.5F;
			TwoBoneIkResult stretched;
			context.Expect(SolveTwoBoneIk(request, stretched) == AnimationStatus::Ok
				&& std::abs(stretched.stretchScale - 1.5F) < 0.0001F
				&& stretched.targetErrorMeters <= request.toleranceMeters
				&& FiniteResult(stretched),
				"Explicit bounded stretch should reach a target at the configured maximum scale.");
		});

		registry.Add("Animation.Procedural.TwoBoneLimitsTwistAndWeightAreExplicit", [](TestContext& context) {
			TwoBoneIkRequest request = MakeRequest();
			request.targetPosition = {0.25F, 0.25F, 0.0F};
			request.middleLimit.enabled = true;
			request.middleLimit.minimumBendRadians = 0.10F;
			request.middleLimit.maximumBendRadians = 0.35F;
			request.middleLimit.maximumTwistRadians = 0.15F;
			request.twistRadians = 1.0F;
			TwoBoneIkResult limited;
			context.Expect(SolveTwoBoneIk(request, limited) == AnimationStatus::Ok
				&& limited.diagnostic.clamped
				&& limited.bendRadians <= 0.3501F
				&& FiniteResult(limited),
				"Bend and twist limits should clamp rather than publish an invalid transform.");

			request = MakeRequest();
			request.weight = 0.5F;
			TwoBoneIkResult weighted;
			context.Expect(SolveTwoBoneIk(request, weighted) == AnimationStatus::Ok
				&& std::abs(weighted.endTransform.translation.x - 1.5F) < 0.0001F
				&& std::abs(weighted.endTransform.translation.y - 0.5F) < 0.0001F
				&& std::abs(weighted.diagnostic.finalContribution - 0.5F) < 0.0001F,
				"A partial IK weight should blend from the authored end transform exactly once.");

			request.weight = 0.0F;
			TwoBoneIkResult disabled;
			context.Expect(SolveTwoBoneIk(request, disabled) == AnimationStatus::Ok
				&& disabled.header.state == ProceduralSolverState::NoContribution
				&& disabled.endTransform == request.endTransform,
				"A zero-weight goal should preserve the authored pose and report no contribution.");
		});

		registry.Add("Animation.Procedural.TwoBoneSingularAndRepeatedEvaluationRemainFinite", [](TestContext& context) {
			TwoBoneIkRequest singularRequest = MakeRequest();
			singularRequest.middleTransform = singularRequest.rootTransform;
			TwoBoneIkResult singular;
			context.Expect(SolveTwoBoneIk(singularRequest, singular) == AnimationStatus::Ok
				&& singular.header.state == ProceduralSolverState::Singular
				&& FiniteResult(singular),
				"A zero-length segment should preserve a finite pose and report a singular result.");

			const TwoBoneIkRequest request = MakeRequest();
			TwoBoneIkResult reference;
			context.Expect(SolveTwoBoneIk(request, reference) == AnimationStatus::Ok,
				"The repeat fixture should produce its reference result.");
			bool identical = true;
			for(std::size_t iteration = 0U; iteration < 10000U; ++iteration) {
				TwoBoneIkResult repeated;
				identical = identical && SolveTwoBoneIk(request, repeated) == AnimationStatus::Ok
					&& repeated.rootTransform == reference.rootTransform
					&& repeated.middleTransform == reference.middleTransform
					&& repeated.endTransform == reference.endTransform
					&& repeated.header.state == reference.header.state;
			}
			context.Expect(identical,
				"Ten thousand identical two-bone inputs should publish bit-identical finite transforms.");
		});
	}
}
