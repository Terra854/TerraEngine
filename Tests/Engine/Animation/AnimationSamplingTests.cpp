// AnimationSamplingTests.cpp - CPU reference playback and clip-view semantics.

#include "TestHarness.h"

#include "Animation/Evaluation/CpuClipEvaluator.h"
#include "Animation/Runtime/AnimationClipView.h"
#include "Animation/Streaming/AnimationStreaming.h"
#include "AnimationClipCompiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		constexpr std::uint64_t kAnimatedJoint = 100U;
		constexpr std::uint64_t kReferenceOnlyJoint = 101U;
		constexpr std::uint64_t kMorph = 700U;
		constexpr std::uint64_t kCurve = 600U;
		constexpr std::uint64_t kRoot = 1U;
		constexpr AnimationCompatibilityFingerprint kFingerprint = 0x1234567890abcdefULL;

		AnimationGuid MakeGuid(std::uint8_t seed) {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		AnimationSourceKey VectorKey(AnimationFixedTick tick, AnimationFloat3 value) {
			AnimationSourceKey key;
			key.tick = tick;
			key.vectorValue = value;
			return key;
		}

		AnimationSourceKey RotationKey(AnimationFixedTick tick, AnimationQuaternion value) {
			AnimationSourceKey key;
			key.tick = tick;
			key.rotationValue = value;
			return key;
		}

		AnimationSourceKey ScalarKey(AnimationFixedTick tick, float value) {
			AnimationSourceKey key;
			key.tick = tick;
			key.scalarValue = value;
			return key;
		}

		AnimationMatrix4x4 IdentityMatrix() {
			AnimationMatrix4x4 matrix;
			matrix.values[0U] = 1.0F;
			matrix.values[5U] = 1.0F;
			matrix.values[10U] = 1.0F;
			matrix.values[15U] = 1.0F;
			return matrix;
		}

		SkeletonAsset MakeSkeleton() {
			SkeletonAsset skeleton;
			skeleton.assetId = MakeGuid(40U);
			skeleton.compatibilityFingerprint = kFingerprint;
			skeleton.rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = kAnimatedJoint;
			root.parentIndex = -1;
			root.localBindTransform.translation = {4.0F, 5.0F, 6.0F};
			root.localBindTransform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			root.localBindTransform.scale = {2.0F, 2.0F, 2.0F};
			root.normalizedSourcePath = "/root";
			SkeletonJoint child;
			child.stableId = kReferenceOnlyJoint;
			child.parentIndex = 0;
			child.localBindTransform.translation = {7.0F, 8.0F, 9.0F};
			child.localBindTransform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			child.localBindTransform.scale = {3.0F, 4.0F, 5.0F};
			child.normalizedSourcePath = "/root/reference_only";
			skeleton.joints = {root, child};
			skeleton.inverseBindMatrices = {IdentityMatrix(), IdentityMatrix()};
			return skeleton;
		}

		AnimationClipCompileInput MakeSamplingInput() {
			AnimationClipCompileInput input;
			input.assetId = MakeGuid(10U);
			input.skeletonAssetId = MakeGuid(40U);
			input.compatibilityFingerprint = kFingerprint;
			input.durationTicks = 12;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 6;

			AnimationSourceTrack translation;
			translation.stableTargetId = kAnimatedJoint;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			translation.keys = {
				VectorKey(0, {0.0F, 0.0F, 0.0F}),
				VectorKey(12, {12.0F, 0.0F, 0.0F})};

			AnimationSourceTrack rotation;
			rotation.stableTargetId = kAnimatedJoint;
			rotation.kind = AnimationClipTrackKind::Rotation;
			rotation.interpolation = AnimationInterpolation::Linear;
			rotation.keys = {
				RotationKey(0, {0.0F, 0.0F, 0.0F, 1.0F}),
				RotationKey(12, {0.0F, 0.70710677F, 0.0F, 0.70710677F})};

			AnimationSourceTrack scale;
			scale.stableTargetId = kAnimatedJoint;
			scale.kind = AnimationClipTrackKind::Scale;
			scale.interpolation = AnimationInterpolation::Step;
			scale.keys = {
				VectorKey(0, {1.0F, 1.0F, 1.0F}),
				VectorKey(6, {3.0F, 3.0F, 3.0F}),
				VectorKey(12, {5.0F, 5.0F, 5.0F})};

			AnimationSourceTrack morph;
			morph.stableTargetId = kMorph;
			morph.kind = AnimationClipTrackKind::MorphWeight;
			morph.interpolation = AnimationInterpolation::Linear;
			morph.keys = {ScalarKey(0, 0.0F), ScalarKey(12, 1.0F)};

			AnimationSourceTrack curve;
			curve.stableTargetId = kCurve;
			curve.kind = AnimationClipTrackKind::Curve;
			curve.interpolation = AnimationInterpolation::Cubic;
			curve.keys = {ScalarKey(0, 0.0F), ScalarKey(12, 1.0F)};

			AnimationSourceTrack rootTranslation = translation;
			rootTranslation.stableTargetId = kRoot;
			rootTranslation.kind = AnimationClipTrackKind::RootTranslation;
			AnimationSourceTrack rootYaw;
			rootYaw.stableTargetId = kRoot;
			rootYaw.kind = AnimationClipTrackKind::RootYaw;
			rootYaw.interpolation = AnimationInterpolation::Linear;
			rootYaw.keys = {ScalarKey(0, 0.0F), ScalarKey(12, 1.0F)};

			input.tracks = {
				translation, rotation, scale, morph, curve, rootTranslation, rootYaw};
			input.namedChannels = {{kCurve, "Speed"}, {kMorph, "Smile"}};
			input.events = {
				{900U, 3, AnimationEventPayloadType::String, "Quarter", true, true},
				{901U, 9, AnimationEventPayloadType::String, "ThreeQuarter", true, true}};
			return input;
		}

		AnimationClipAsset CompileSamplingClip(TestContext& context) {
			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			const AnimationStatus status = CompileAnimationClip(MakeSamplingInput(), {}, clip, report);
			context.Expect(status == AnimationStatus::Ok,
				"The sampling fixture must compile into two independently seekable segments.");
			return clip;
		}

		CpuClipEvaluationRequest MakeRequest(
			AnimationFixedTick tick,
			bool loop,
			std::span<const std::uint64_t> joints,
			std::span<const std::uint64_t> morphs,
			std::span<const std::uint64_t> curves) {
			CpuClipEvaluationRequest request;
			request.tick = tick;
			request.loop = loop;
			request.jointStableIds = joints;
			request.morphStableIds = morphs;
			request.curveStableIds = curves;
			return request;
		}

		bool Near(float left, float right, float tolerance = 0.001F) {
			return std::abs(left - right) <= tolerance;
		}
	}

	void RegisterAnimationSamplingTests(TestRegistry& registry) {
		registry.Add("Animation.Sampling.InterpolationAndAllPointChannels", [](TestContext& context) {
			const SkeletonAsset skeleton = MakeSkeleton();
			const AnimationClipAsset clip = CompileSamplingClip(context);
			AnimationClipView view;
			context.Expect(view.Open(clip, &skeleton) == AnimationStatus::Ok,
				"A compatible clip and reference skeleton should open as one immutable view.");

			const std::array<std::uint64_t, 1U> joints{kAnimatedJoint};
			const std::array<std::uint64_t, 1U> morphs{kMorph};
			const std::array<std::uint64_t, 1U> curves{kCurve};
			CpuClipEvaluationResult result;
			CpuClipEvaluator evaluator;
			context.Expect(evaluator.Evaluate(
				view, MakeRequest(3, false, joints, morphs, curves), result) == AnimationStatus::Ok,
				"Linear, smooth-cubic, rotation, morph, curve, event, and root channels should sample together.");
			context.Expect(result.joints.size() == 1U && Near(result.joints[0U].localTransform.translation.x, 3.0F),
				"Linear translation should evaluate at the requested fixed tick.");
			context.Expect(result.joints[0U].localTransform.scale == AnimationFloat3{1.0F, 1.0F, 1.0F},
				"Step interpolation should hold the left value until its next exact key.");
			context.Expect(IsNormalized(result.joints[0U].localTransform.rotation),
				"Shortest-path rotation interpolation must remain finite and normalized.");
			context.Expect(result.morphs.size() == 1U && Near(result.morphs[0U].value, 0.25F),
				"Morph channels should use their authored linear interpolation.");
			context.Expect(result.curves.size() == 1U && Near(result.curves[0U].value, 0.15625F, 0.001F),
				"Cubic channels should use the deterministic smooth interpolation contract.");
			context.Expect(result.rootMotion.enabled && Near(result.rootMotion.translation.x, 3.0F)
				&& Near(result.rootMotion.yawRadians, 0.25F),
				"The CPU reference sampler should expose root translation and yaw point values.");
			context.Expect(result.pointEvents.size() == 1U
				&& result.pointEvents[0U].event->stableEventId == 900U,
				"Point event sampling should return deterministic metadata at the exact tick.");

			CpuClipEvaluationResult atStepBoundary;
			context.Expect(evaluator.Evaluate(
				view, MakeRequest(6, false, joints, {}, {}), atStepBoundary) == AnimationStatus::Ok
				&& atStepBoundary.joints[0U].localTransform.scale == AnimationFloat3{3.0F, 3.0F, 3.0F},
				"An exact step boundary should choose the key authored at that boundary.");
		});

		registry.Add("Animation.Sampling.ClipViewRecognizesAnUnchangedResidentGeneration", [](TestContext& context) {
			const SkeletonAsset skeleton = MakeSkeleton();
			const AnimationClipAsset clip = CompileSamplingClip(context);
			AnimationClipView view;
			context.Expect(view.Open(clip, &skeleton) == AnimationStatus::Ok,
				"The initial checked clip view should open successfully.");
			context.Expect(view.Matches(clip, &skeleton, {}, 0U, {}),
				"A fully resident unchanged clip should match its retained checked view.");
			context.Expect(view.Open(clip, &skeleton) == AnimationStatus::Ok && view.IsOpen(),
				"Opening the exact same immutable generation should remain a safe no-op.");
			const std::array<std::uint8_t, 2U> changedResidency{1U, 0U};
			context.Expect(!view.Matches(clip, &skeleton, changedResidency, 0U, {}),
				"A changed residency bitmap must invalidate the capture fast path.");
		});

		registry.Add("Animation.Sampling.ReferenceDefaultAndUnavailableFallback", [](TestContext& context) {
			const SkeletonAsset skeleton = MakeSkeleton();
			const AnimationClipAsset clip = CompileSamplingClip(context);
			const std::array<std::uint8_t, 2U> residency{1U, 0U};
			AnimationClipView view;
			context.Expect(view.Open(clip, &skeleton, residency) == AnimationStatus::Ok,
				"A residency snapshot with one byte per segment should be accepted.");

			const std::array<std::uint64_t, 1U> joints{kReferenceOnlyJoint};
			const std::array<std::uint64_t, 1U> morphs{701U};
			const std::array<std::uint64_t, 1U> curves{601U};
			CpuClipEvaluator evaluator;
			CpuClipEvaluationResult residentFallback;
			context.Expect(evaluator.Evaluate(
				view, MakeRequest(2, false, joints, morphs, curves), residentFallback) == AnimationStatus::Ok,
				"Omitted resident channels should sample their documented fallbacks successfully.");
			context.Expect(residentFallback.joints[0U].localTransform.translation
					== skeleton.joints[1U].localBindTransform.translation
				&& residentFallback.joints[0U].localTransform.scale
					== skeleton.joints[1U].localBindTransform.scale,
				"Missing joint channels should use the compatible skeleton reference pose.");
			context.Expect(residentFallback.morphs[0U].value == 0.0F
				&& residentFallback.curves[0U].value == 0.0F,
				"Missing morph and curve channels should use the explicit zero default.");

			AnimationClipChannelSample nonResidentReference;
			AnimationClipChannelSample nonResidentDefault;
			context.Expect(view.Sample({
				kReferenceOnlyJoint, AnimationClipTrackKind::Translation, 8, false},
				nonResidentReference) == AnimationStatus::Ok
				&& view.Sample({701U, AnimationClipTrackKind::MorphWeight, 8, false},
					nonResidentDefault) == AnimationStatus::Ok,
				"A missing channel must not inherit NotReady from an unrelated non-resident segment.");
			context.Expect(nonResidentReference.source == AnimationChannelSampleSource::ReferencePose
				&& nonResidentReference.value.vectorValue
					== skeleton.joints[1U].localBindTransform.translation
				&& nonResidentDefault.source == AnimationChannelSampleSource::DefaultValue
				&& nonResidentDefault.value.scalarValue == 0.0F,
				"Reference/default fallback remains immediately available without reading streamed bytes.");

			const std::array<std::uint64_t, 1U> animatedJoint{kAnimatedJoint};
			CpuClipEvaluationResult unavailable;
			context.Expect(evaluator.Evaluate(
				view, MakeRequest(8, false, animatedJoint, morphs, curves), unavailable)
					== AnimationStatus::NotReady,
				"A non-resident segment should be reported without reading its bytes.");
			context.Expect(unavailable.joints[0U].localTransform.translation
					== skeleton.joints[0U].localBindTransform.translation
				&& unavailable.unavailableChannelCount > 0U,
				"Unavailable segment output should remain a complete safe reference pose with diagnostics.");
		});

		registry.Add("Animation.Sampling.StreamedViewPinsExactPublishedSegmentBytes", [](TestContext& context) {
			const SkeletonAsset skeleton = MakeSkeleton();
			AnimationClipAsset clip = CompileSamplingClip(context);
			clip.runtimeGeneration = 7U;
			const AnimationClipSegment& segment = clip.segments.front();
			const auto first = clip.streamData.begin()
				+ static_cast<std::ptrdiff_t>(segment.streamOffset);
			const auto last = first + static_cast<std::ptrdiff_t>(segment.streamSize);
			auto residentBytes = std::make_shared<std::vector<std::uint8_t>>(first, last);

			AnimationClipSegmentPayloadSet stagedPayloads;
			stagedPayloads.assetId = clip.assetId;
			stagedPayloads.generation = clip.runtimeGeneration;
			stagedPayloads.publicationSequence = 11U;
			stagedPayloads.segments.push_back({
				0U,
				segment.streamOffset,
				segment.streamSize,
				ComputeAnimationStreamContentFingerprint(*residentBytes),
				residentBytes});
			auto payloads = std::make_shared<const AnimationClipSegmentPayloadSet>(
				std::move(stagedPayloads));
			const std::array<std::uint8_t, 2U> firstOnly{1U, 0U};

			AnimationClipView streamedView;
			context.Expect(streamedView.Open(
				clip, &skeleton, firstOnly, clip.runtimeGeneration, payloads)
					== AnimationStatus::Ok,
				"A streamed view should accept an exact immutable segment publication.");
			std::fill(first, last, 0U);
			AnimationClipChannelSample streamedSample;
			context.Expect(streamedView.Sample({
				kAnimatedJoint, AnimationClipTrackKind::Translation, 3, false}, streamedSample)
					== AnimationStatus::Ok
				&& streamedSample.source == AnimationChannelSampleSource::Track
				&& Near(streamedSample.value.vectorValue.x, 3.0F),
				"Sampling must use the pinned publication even after the clip cache bytes change.");

			AnimationClipSegmentPayloadSet missingPayloads;
			missingPayloads.assetId = clip.assetId;
			missingPayloads.generation = clip.runtimeGeneration;
			missingPayloads.publicationSequence = 12U;
			const std::array<std::uint8_t, 2U> noneResident{0U, 0U};
			AnimationClipView missingView;
			context.Expect(missingView.Open(
				clip,
				&skeleton,
				noneResident,
				clip.runtimeGeneration,
				std::make_shared<const AnimationClipSegmentPayloadSet>(
					std::move(missingPayloads))) == AnimationStatus::Ok,
				"A publication with no resident segments should remain a valid fallback view.");
			AnimationClipChannelSample missingSample;
			context.Expect(missingView.Sample({
				kAnimatedJoint, AnimationClipTrackKind::Translation, 3, false}, missingSample)
					== AnimationStatus::NotReady
				&& missingSample.source == AnimationChannelSampleSource::ReferencePose
				&& missingSample.value.vectorValue
					== skeleton.joints.front().localBindTransform.translation,
				"A missing streamed segment must expose only the safe reference fallback.");

			AnimationClipChannelSample promotedSample;
			AnimationClipView unopenedView;
			context.Expect(missingView.CopyStreamingStateFrom(streamedView)
					== AnimationStatus::Ok
				&& missingView.Matches(
					clip, &skeleton, firstOnly, clip.runtimeGeneration, payloads)
				&& missingView.Sample({
					kAnimatedJoint, AnimationClipTrackKind::Translation, 3, false},
					promotedSample) == AnimationStatus::Ok
				&& Near(promotedSample.value.vectorValue.x, 3.0F)
				&& unopenedView.CopyStreamingStateFrom(streamedView)
					== AnimationStatus::InvalidArgument,
				"A validated streaming view should promote an identical open clip in place while rejecting an unbound destination.");

			AnimationClipAsset replacementClip = clip;
			++replacementClip.runtimeGeneration;
			AnimationClipSegmentPayloadSet replacementPayloadSet = *payloads;
			replacementPayloadSet.generation = replacementClip.runtimeGeneration;
			replacementPayloadSet.publicationSequence = 13U;
			auto replacementPayloads =
				std::make_shared<const AnimationClipSegmentPayloadSet>(
					std::move(replacementPayloadSet));
			AnimationClipView replacementView;
			context.Expect(replacementView.Open(
				replacementClip,
				&skeleton,
				firstOnly,
				replacementClip.runtimeGeneration,
				replacementPayloads) == AnimationStatus::Ok,
				"A hot-reloaded clip publication must be validated before adoption.");
			context.Expect(missingView.CopyStreamingStateFrom(replacementView)
					== AnimationStatus::InvalidArgument
				&& missingView.AdoptValidatedPublicationFrom(replacementView)
					== AnimationStatus::Ok
				&& missingView.Matches(
					replacementClip,
					&skeleton,
					firstOnly,
					replacementClip.runtimeGeneration,
					replacementPayloads)
				&& unopenedView.AdoptValidatedPublicationFrom(replacementView)
					== AnimationStatus::Ok,
				"A checked publication replacement must support both an old generation and a previously unavailable destination without weakening same-generation identity checks.");

			AnimationClipSegmentPayloadSet corruptPayloads = *payloads;
			auto corruptBytes =
				std::make_shared<std::vector<std::uint8_t>>(*residentBytes);
			corruptBytes->front() ^= 0xffU;
			corruptPayloads.segments.front().bytes = std::move(corruptBytes);
			AnimationClipView corruptView;
			context.Expect(corruptView.Open(
				clip,
				&skeleton,
				firstOnly,
				clip.runtimeGeneration,
				std::make_shared<const AnimationClipSegmentPayloadSet>(
					std::move(corruptPayloads))) == AnimationStatus::ContentHashMismatch,
				"A same-generation payload with corrupt bytes must be rejected before decode.");
		});

		registry.Add("Animation.Sampling.BoundariesReverseSeekRepeatedAndMultiLoop", [](TestContext& context) {
			const SkeletonAsset skeleton = MakeSkeleton();
			const AnimationClipAsset clip = CompileSamplingClip(context);
			AnimationClipView view;
			view.Open(clip, &skeleton);
			const std::array<std::uint64_t, 1U> joints{kAnimatedJoint};
			CpuClipEvaluator evaluator;

			CpuClipEvaluationResult end;
			CpuClipEvaluationResult seam;
			context.Expect(evaluator.Evaluate(view, MakeRequest(12, false, joints, {}, {}), end)
					== AnimationStatus::Ok
				&& evaluator.Evaluate(view, MakeRequest(12, true, joints, {}, {}), seam)
					== AnimationStatus::Ok,
				"Exact end and loop-seam boundaries should both be valid point samples.");
			context.Expect(Near(end.joints[0U].localTransform.translation.x, 12.0F)
				&& Near(seam.joints[0U].localTransform.translation.x, 0.0F)
				&& seam.time.loopIndex == 1 && seam.time.exactEndBoundary,
				"Non-looping end selects the final key while looping end selects loop one start.");

			CpuClipEvaluationResult reverse;
			CpuClipEvaluationResult multiLoop;
			context.Expect(evaluator.Evaluate(view, MakeRequest(-1, true, joints, {}, {}), reverse)
					== AnimationStatus::Ok
				&& evaluator.Evaluate(view, MakeRequest(37, true, joints, {}, {}), multiLoop)
					== AnimationStatus::Ok,
				"Negative reverse seeks and positive multi-loop catch-up should use floor-division time semantics.");
			context.Expect(reverse.time.resolvedTick == 11 && reverse.time.loopIndex == -1
				&& Near(reverse.joints[0U].localTransform.translation.x, 11.0F)
				&& multiLoop.time.resolvedTick == 1 && multiLoop.time.loopIndex == 3
				&& Near(multiLoop.joints[0U].localTransform.translation.x, 1.0F),
				"Resolved ticks and signed loop counts must not depend on playback direction or prior samples.");

			CpuClipEvaluationResult randomLater;
			CpuClipEvaluationResult randomEarlier;
			CpuClipEvaluationResult repeated;
			evaluator.Evaluate(view, MakeRequest(10, false, joints, {}, {}), randomLater);
			evaluator.Evaluate(view, MakeRequest(2, false, joints, {}, {}), randomEarlier);
			evaluator.Evaluate(view, MakeRequest(2, false, joints, {}, {}), repeated);
			context.Expect(Near(randomEarlier.joints[0U].localTransform.translation.x, 2.0F)
				&& randomEarlier.joints[0U].localTransform == repeated.joints[0U].localTransform,
				"Random backward seek and repeated-time sampling must not inherit a mutable decoder cursor.");
		});

		registry.Add("Animation.Sampling.RepeatedAuthoredTimestampUsesLaterValue", [](TestContext& context) {
			AnimationClipCompileInput input = MakeSamplingInput();
			AnimationSourceTrack& translation = input.tracks[0U];
			translation.keys = {
				VectorKey(0, {0.0F, 0.0F, 0.0F}),
				VectorKey(6, {3.0F, 0.0F, 0.0F}),
				VectorKey(6, {9.0F, 0.0F, 0.0F}),
				VectorKey(12, {12.0F, 0.0F, 0.0F})};

			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, clip, report) == AnimationStatus::Ok,
				"Repeated authored timestamps should normalize deterministically instead of rejecting the clip.");
			context.Expect(report.sourceKeyCount >= report.retainedKeyCount
				&& report.removedKeyCount >= 1U,
				"The compile report should account for the superseded repeated-time key.");

			const SkeletonAsset skeleton = MakeSkeleton();
			AnimationClipView view;
			context.Expect(view.Open(clip, &skeleton) == AnimationStatus::Ok,
				"A clip normalized from repeated timestamps must remain codec-valid.");
			AnimationClipChannelSample sample;
			context.Expect(view.Sample({
				kAnimatedJoint, AnimationClipTrackKind::Translation, 6, false}, sample)
					== AnimationStatus::Ok
				&& sample.source == AnimationChannelSampleSource::Track
				&& Near(sample.value.vectorValue.x, 9.0F),
				"The later authored value must own an exact repeated timestamp deterministically.");
		});

		registry.Add("Animation.Sampling.ZeroDurationIsStableReferencePoint", [](TestContext& context) {
			const SkeletonAsset skeleton = MakeSkeleton();
			AnimationClipAsset clip;
			clip.assetId = MakeGuid(10U);
			clip.skeletonAssetId = skeleton.assetId;
			clip.compatibilityFingerprint = kFingerprint;
			clip.durationTicks = 0;
			clip.sampleRate = 60U;
			clip.compression.decoderVersion = 1U;
			AnimationClipView view;
			context.Expect(view.Open(clip, &skeleton) == AnimationStatus::Ok,
				"A zero-duration clip with no animated channels should remain a valid reference point.");

			const std::array<std::uint64_t, 1U> joints{kAnimatedJoint};
			CpuClipEvaluationResult result;
			CpuClipEvaluator evaluator;
			context.Expect(evaluator.Evaluate(view, MakeRequest(
				std::numeric_limits<AnimationFixedTick>::max(), true, joints, {}, {}), result)
					== AnimationStatus::Ok,
				"Zero-duration looping must not divide by zero or create an unbounded loop count.");
			context.Expect(result.time.resolvedTick == 0 && result.time.loopIndex == 0
				&& result.time.exactEndBoundary
				&& result.joints[0U].localTransform == skeleton.joints[0U].localBindTransform,
				"Every zero-duration seek should resolve to the compatible reference pose at tick zero.");
		});

		registry.Add("Animation.Sampling.CheckedInputsPreservePriorOutput", [](TestContext& context) {
			SkeletonAsset skeleton = MakeSkeleton();
			const AnimationClipAsset clip = CompileSamplingClip(context);
			AnimationClipView view;
			const std::array<std::uint8_t, 1U> wrongResidency{1U};
			context.Expect(view.Open(clip, &skeleton, wrongResidency) == AnimationStatus::InvalidArgument,
				"Residency arrays must match the immutable clip segment count exactly.");
			skeleton.compatibilityFingerprint ^= 1U;
			context.Expect(view.Open(clip, &skeleton) == AnimationStatus::IncompatibleAsset,
				"A reference pose from another skeleton generation must be rejected before sampling.");

			skeleton = MakeSkeleton();
			context.Expect(view.Open(clip, &skeleton) == AnimationStatus::Ok,
				"The compatible view should still open after rejected candidates.");
			const std::array<std::uint64_t, 2U> duplicateJoints{kAnimatedJoint, kAnimatedJoint};
			CpuClipEvaluationResult output;
			output.fallbackChannelCount = 77U;
			CpuClipEvaluator evaluator;
			context.Expect(evaluator.Evaluate(
				view, MakeRequest(2, false, duplicateJoints, {}, {}), output)
					== AnimationStatus::InvalidArgument,
				"Duplicate target identities should be rejected before any output is written.");
			context.Expect(output.fallbackChannelCount == 77U && output.joints.empty(),
				"Rejected evaluation must preserve the caller's prior complete output atomically.");

			std::vector<std::uint64_t> tooManyJoints(4097U, 1U);
			context.Expect(evaluator.Evaluate(
				view, MakeRequest(2, false, tooManyJoints, {}, {}), output)
					== AnimationStatus::HardLimitExceeded,
				"Sampling requests must enforce the frozen version-one skeleton hard limit before allocation.");
			AnimationResolvedClipTime unchanged;
			unchanged.requestedTick = 55;
			context.Expect(ResolveAnimationClipTime(-1, 0, false, unchanged)
					== AnimationStatus::InvalidArgument && unchanged.requestedTick == 55,
				"Invalid negative duration input should not partially replace the prior time result.");
		});

		registry.Add("Animation.Sampling.ClipDurationHardLimitIsTwentyFourHours", [](TestContext& context) {
			const AnimationFixedTick maximumDuration = static_cast<AnimationFixedTick>(
				GetAnimationRuntimeLimit(AnimationLimit::ClipDurationSeconds).hardMaximum
					* kAnimationFixedTicksPerSecond);
			AnimationClipAsset boundary;
			boundary.assetId = MakeGuid(10U);
			boundary.skeletonAssetId = MakeGuid(40U);
			boundary.compatibilityFingerprint = kFingerprint;
			boundary.durationTicks = maximumDuration;
			boundary.sampleRate = 60U;
			boundary.compression.decoderVersion = 1U;
			context.Expect(ValidateAnimationClipAsset(boundary) == AnimationStatus::Ok,
				"The exact frozen twenty-four-hour clip duration should remain valid.");
			boundary.durationTicks = maximumDuration + 1;
			context.Expect(ValidateAnimationClipAsset(boundary) == AnimationStatus::HardLimitExceeded,
				"A serialized clip even one fixed tick beyond twenty-four hours must fail closed.");

			AnimationClipCompileInput input = MakeSamplingInput();
			input.durationTicks = maximumDuration + 1;
			AnimationClipAsset output;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, output, report)
					== AnimationStatus::HardLimitExceeded
				&& report.diagnostic.find("duration") != std::string::npos,
				"The offline compiler should reject an overlong clip before segment expansion allocates work.");
		});

		registry.Add("Animation.Sampling.SegmentCountDoesNotReuseChannelLimit", [](TestContext& context) {
			AnimationClipAsset clip;
			clip.assetId = MakeGuid(10U);
			clip.skeletonAssetId = MakeGuid(40U);
			clip.compatibilityFingerprint = kFingerprint;
			clip.durationTicks = 65537;
			clip.sampleRate = 60U;
			clip.compression.decoderVersion = 1U;
			clip.segments.resize(65537U);
			for(std::size_t index = 0U; index < clip.segments.size(); ++index) {
				// Empty one-tick segments are legal metadata and contain no channels.
				clip.segments[index].startTick = static_cast<AnimationFixedTick>(index);
				clip.segments[index].endTick = static_cast<AnimationFixedTick>(index + 1U);
			}
			AnimationClipView view;
			context.Expect(view.Open(clip) == AnimationStatus::Ok,
				"Segment residency must not misuse the independent clip-channel hard limit.");
		});
	}
}
