// AnimationBlendTests.cpp - CPU reference blending and crossfade regressions.

#include "TestHarness.h"

#include "Animation/Evaluation/PoseBlend.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationTransform MakeTransform(
			float translationX,
			AnimationQuaternion rotation = {},
			float uniformScale = 1.0F) {
			AnimationTransform transform;
			transform.translation.x = translationX;
			transform.rotation = rotation;
			transform.scale = {uniformScale, uniformScale, uniformScale};
			return transform;
		}

		bool Near(float left, float right, float tolerance = 0.0001F) {
			return std::abs(left - right) <= tolerance;
		}

		bool FiniteNormalized(const AnimationQuaternion& rotation) {
			return IsFinite(rotation) && IsNormalized(rotation, 0.0001F);
		}

		PoseCrossfadeRequest FixedRequest(AnimationFixedTick durationTicks) {
			PoseCrossfadeRequest request;
			request.durationMode = PoseCrossfadeDurationMode::FixedTicks;
			request.durationTicks = durationTicks;
			request.sourceDurationTicks = 100;
			request.destinationDurationTicks = 200;
			return request;
		}
	}

	void RegisterAnimationBlendTests(TestRegistry& registry) {
		registry.Add("Animation.Blend.NormalizesWeightsAndStableOrders", [](TestContext& context) {
			const std::vector<AnimationTransform> fallback{MakeTransform(-10.0F)};
			const std::vector<AnimationTransform> first{MakeTransform(0.0F)};
			const std::vector<AnimationTransform> second{MakeTransform(10.0F)};
			const std::vector<AnimationTransform> third{MakeTransform(20.0F)};
			const WeightedLocalPose forward[] = {
				{first, 1.0F, 10U}, {second, 2.0F, 20U}, {third, 1.0F, 30U}
			};
			const WeightedLocalPose shuffled[] = {
				{third, 1.0F, 30U}, {first, 1.0F, 10U}, {second, 2.0F, 20U}
			};
			std::vector<AnimationTransform> forwardOutput;
			std::vector<AnimationTransform> shuffledOutput;
			context.Expect(BlendLocalPoses(forward, fallback, forwardOutput) == AnimationStatus::Ok
				&& BlendLocalPoses(shuffled, fallback, shuffledOutput) == AnimationStatus::Ok,
				"Compatible poses should blend after stable weight normalization.");
			context.Expect(forwardOutput == shuffledOutput && Near(forwardOutput[0].translation.x, 10.0F),
				"Stable-order accumulation must produce the same normalized pose for shuffled worker results.");
		});

		registry.Add("Animation.Blend.ZeroWeightUsesExplicitFallback", [](TestContext& context) {
			const std::vector<AnimationTransform> fallback{MakeTransform(7.0F)};
			const std::vector<AnimationTransform> ignored{MakeTransform(100.0F)};
			const WeightedLocalPose inputs[] = {{ignored, 0.0F, 1U}};
			std::vector<AnimationTransform> output{MakeTransform(-1.0F)};
			context.Expect(BlendLocalPoses(inputs, fallback, output) == AnimationStatus::Ok,
				"An effective total weight of zero is a supported blend case.");
			context.Expect(output == fallback,
				"Zero total weight must use the caller's compatible reference/default pose without a bind-pose flash.");
		});

		registry.Add("Animation.Blend.QuaternionHemisphereStaysFinite", [](TestContext& context) {
			// +170 and -170 degrees about Y are only 20 degrees apart through the
			// shortest hemisphere; a component lerp without sign correction flashes
			// through identity instead of reaching the 180-degree midpoint.
			const AnimationQuaternion positive170{0.0F, 0.9961947F, 0.0F, 0.0871557F};
			const AnimationQuaternion negative170{0.0F, -0.9961947F, 0.0F, 0.0871557F};
			const std::vector<AnimationTransform> fallback{MakeTransform(0.0F)};
			const std::vector<AnimationTransform> left{MakeTransform(0.0F, positive170)};
			const std::vector<AnimationTransform> right{MakeTransform(0.0F, negative170)};
			const WeightedLocalPose inputs[] = {{left, 1.0F, 0U}, {right, 1.0F, 1U}};
			std::vector<AnimationTransform> output;
			context.Expect(BlendLocalPoses(inputs, fallback, output) == AnimationStatus::Ok,
				"Shortest-hemisphere rotation blending should succeed.");
			context.Expect(FiniteNormalized(output[0].rotation)
				&& std::abs(output[0].rotation.y) > 0.999F
				&& std::abs(output[0].rotation.w) < 0.001F,
				"A crossfade rotation must remain normalized and continuous through the 180-degree midpoint.");
		});

		registry.Add("Animation.Blend.TimedOffsetsAndCurves", [](TestContext& context) {
			PoseCrossfadeRequest request = FixedRequest(10);
			request.curve = PoseCrossfadeCurve::EaseInQuadratic;
			request.initialNormalizedPhase = 0.20F;
			request.sourceOffsetNormalized = 0.05F;
			request.destinationOffsetNormalized = 0.30F;
			PoseCrossfadeState state;
			context.Expect(state.Begin(request) == AnimationStatus::Ok
				&& state.Advance(5) == AnimationStatus::Ok,
				"A finite fixed-tick crossfade should advance deterministically.");
			PoseCrossfadeSample sample;
			context.Expect(state.Sample(sample) == AnimationStatus::Ok,
				"Crossfade inspection should return a copied timing record.");
			context.Expect(Near(sample.linearProgress, 0.5F)
				&& Near(sample.destinationWeight, 0.25F)
				&& Near(sample.sourceNormalizedTime, 0.30F)
				&& Near(sample.destinationNormalizedTime, 0.525F),
				"Offsets, independent clip clocks, and the authored transition curve must all affect the sample.");
		});

		registry.Add("Animation.Blend.NormalizedDurationAndSyncContinuity", [](TestContext& context) {
			PoseCrossfadeRequest request;
			request.durationMode = PoseCrossfadeDurationMode::NormalizedSource;
			request.normalizedDuration = 0.25F;
			request.sourceDurationTicks = 40;
			request.destinationDurationTicks = 80;
			request.syncMode = PoseCrossfadeSyncMode::PreserveNormalizedPhase;
			request.initialNormalizedPhase = 0.25F;
			request.sourceOffsetNormalized = 0.10F;
			request.destinationOffsetNormalized = -0.05F;
			PoseCrossfadeState state;
			context.Expect(state.Begin(request) == AnimationStatus::Ok
				&& state.ResolvedDurationTicks() == 10
				&& state.Advance(4) == AnimationStatus::Ok,
				"A normalized transition duration should resolve against the source clip once at Begin.");
			PoseCrossfadeSample sample;
			state.Sample(sample);
			context.Expect(Near(sample.synchronizationPhase, 0.35F)
				&& Near(sample.sourceNormalizedTime, 0.45F)
				&& Near(sample.destinationNormalizedTime, 0.30F),
				"Synchronized clips of unequal lengths must retain one phase while preserving their authored offsets.");
		});

		registry.Add("Animation.Blend.FixedTickSamplingAndNonLoopingEndAreExact", [](TestContext& context) {
			PoseCrossfadeRequest exactTick = FixedRequest(100);
			PoseCrossfadeState state;
			context.Expect(state.Begin(exactTick) == AnimationStatus::Ok
				&& state.Advance(29) == AnimationStatus::Ok,
				"A crossfade should advance using its integer fixed-tick clock.");
			PoseCrossfadeSample sample;
			context.Expect(state.Sample(sample) == AnimationStatus::Ok
				&& sample.sourceSampleTick == 29,
				"Converting 29/100 progress back to a sample must not floor to tick 28 through float round-off.");

			PoseCrossfadeRequest nonLooping = FixedRequest(100);
			nonLooping.sourceLoop = false;
			nonLooping.destinationLoop = false;
			nonLooping.destinationDurationTicks = 100;
			context.Expect(state.Begin(nonLooping) == AnimationStatus::Ok
				&& state.Advance(100) == AnimationStatus::Ok,
				"A non-looping transition should reach its exact completion boundary.");
			context.Expect(state.Sample(sample) == AnimationStatus::Ok
				&& sample.complete
				&& sample.sourceNormalizedTime == 1.0F
				&& sample.destinationNormalizedTime == 1.0F
				&& sample.sourceSampleTick == 100
				&& sample.destinationSampleTick == 100,
				"Non-looping clips must hold their closed end sample instead of wrapping to tick zero.");

			PoseCrossfadeRequest overlong = FixedRequest(1);
			overlong.sourceDurationTicks = static_cast<AnimationFixedTick>(
				GetAnimationRuntimeLimit(AnimationLimit::ClipDurationSeconds).hardMaximum
					* kAnimationFixedTicksPerSecond) + 1;
			PoseCrossfadeState rejected;
			context.Expect(rejected.Begin(overlong) == AnimationStatus::HardLimitExceeded
				&& !rejected.IsInitialized(),
				"Crossfade timing must reject a clip duration beyond the shared twenty-four-hour limit atomically.");
		});

		registry.Add("Animation.Blend.InterruptionRestartsFromCurrentPose", [](TestContext& context) {
			const std::vector<AnimationTransform> fallback{MakeTransform(0.0F)};
			const std::vector<AnimationTransform> first{MakeTransform(0.0F)};
			const std::vector<AnimationTransform> second{MakeTransform(10.0F)};
			const std::vector<AnimationTransform> third{MakeTransform(20.0F)};
			PoseCrossfadeState state;
			PoseCrossfadeRequest firstRequest = FixedRequest(10);
			context.Expect(state.Begin(firstRequest) == AnimationStatus::Ok
				&& state.Advance(4) == AnimationStatus::Ok,
				"The first transition should reach its deterministic interruption point.");

			PoseCrossfadeRequest interruption = FixedRequest(10);
			interruption.syncMode = PoseCrossfadeSyncMode::PreserveNormalizedPhase;
			interruption.interruptionPolicy = PoseCrossfadeInterruptionPolicy::RestartFromCurrentPose;
			context.Expect(state.Interrupt(interruption, first, second, fallback) == AnimationStatus::Ok
				&& state.UsesCapturedSourcePose(),
				"Explicit restart interruption should capture the complete current output pose.");
			std::vector<AnimationTransform> output;
			context.Expect(state.Blend(second, third, fallback, output) == AnimationStatus::Ok
				&& Near(output[0].translation.x, 4.0F),
				"Interruption progress zero must equal the prior visible pose, not either clip or the fallback pose.");
			state.Advance(5);
			context.Expect(state.Blend(second, third, fallback, output) == AnimationStatus::Ok
				&& Near(output[0].translation.x, 12.0F),
				"The replacement transition should blend from the captured pose to its new destination.");
		});

		registry.Add("Animation.Blend.RejectedInterruptionAndInvalidDataAreAtomic", [](TestContext& context) {
			const std::vector<AnimationTransform> fallback{MakeTransform(0.0F)};
			const std::vector<AnimationTransform> source{MakeTransform(0.0F)};
			const std::vector<AnimationTransform> destination{MakeTransform(10.0F)};
			PoseCrossfadeState state;
			PoseCrossfadeRequest original = FixedRequest(10);
			state.Begin(original);
			state.Advance(3);
			PoseCrossfadeSample before;
			state.Sample(before);

			PoseCrossfadeRequest rejected = FixedRequest(5);
			rejected.interruptionPolicy = PoseCrossfadeInterruptionPolicy::RejectWhileActive;
			context.Expect(state.Interrupt(rejected, source, destination, fallback) == AnimationStatus::Rejected,
				"RejectWhileActive must report the rejected command explicitly.");
			PoseCrossfadeSample afterRejected;
			state.Sample(afterRejected);
			context.Expect(afterRejected.elapsedTicks == before.elapsedTicks
				&& afterRejected.durationTicks == before.durationTicks,
				"A rejected interruption must not partially replace current transition timing.");

			std::vector<AnimationTransform> badPose = destination;
			badPose[0].translation.x = std::numeric_limits<float>::quiet_NaN();
			std::vector<AnimationTransform> output{MakeTransform(77.0F)};
			const std::vector<AnimationTransform> preserved = output;
			const WeightedLocalPose invalidInputs[] = {{source, 1.0F, 0U}, {badPose, 1.0F, 1U}};
			context.Expect(BlendLocalPoses(invalidInputs, fallback, output) == AnimationStatus::InvalidArgument
				&& output == preserved,
				"Non-finite pose input must be rejected before replacing the caller's prior complete output.");

			PoseCrossfadeRequest invalidRequest = FixedRequest(5);
			invalidRequest.destinationOffsetNormalized = std::numeric_limits<float>::infinity();
			context.Expect(state.Interrupt(invalidRequest, source, destination, fallback)
				== AnimationStatus::InvalidArgument,
				"Non-finite transition parameters must be rejected before publication.");
			PoseCrossfadeSample afterInvalid;
			state.Sample(afterInvalid);
			context.Expect(afterInvalid.elapsedTicks == before.elapsedTicks
				&& afterInvalid.durationTicks == before.durationTicks,
				"A malformed interruption must preserve the last-known-good transition state.");
		});
	}
}
