// AnimationCodecTests.cpp - Engine-owned segmented clip codec contracts.

#include "TestHarness.h"

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Codec/AnimationCodec.h"
#include "AnimationClipCompiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		AnimationGuid MakeCodecGuid(std::uint8_t seed) {
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

		AnimationClipCompileInput MakeCodecInput() {
			AnimationClipCompileInput input;
			input.assetId = MakeCodecGuid(20U);
			input.skeletonAssetId = MakeCodecGuid(60U);
			input.compatibilityFingerprint = 0x8899aabbccddeeffULL;
			input.durationTicks = 120;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 30;

			AnimationSourceTrack translation;
			translation.stableTargetId = 100U;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			translation.keys = {
				VectorKey(0, {0.0F, 0.0F, 0.0F}),
				VectorKey(30, {0.25F, 0.5F, -0.25F}),
				VectorKey(60, {0.5F, 1.0F, -0.5F}),
				VectorKey(90, {0.75F, 1.5F, -0.75F}),
				VectorKey(120, {1.0F, 2.0F, -1.0F})
			};

			AnimationSourceTrack rotation;
			rotation.stableTargetId = 100U;
			rotation.kind = AnimationClipTrackKind::Rotation;
			rotation.interpolation = AnimationInterpolation::Linear;
			rotation.keys = {
				RotationKey(0, {0.0F, 0.0F, 0.0F, 1.0F}),
				RotationKey(120, {0.0F, 0.70710677F, 0.0F, 0.70710677F})
			};

			AnimationSourceTrack morph;
			morph.stableTargetId = 700U;
			morph.kind = AnimationClipTrackKind::MorphWeight;
			morph.interpolation = AnimationInterpolation::Cubic;
			morph.keys = {ScalarKey(0, 0.0F), ScalarKey(60, 1.0F), ScalarKey(120, 0.0F)};

			AnimationSourceTrack curve;
			curve.stableTargetId = 600U;
			curve.kind = AnimationClipTrackKind::Curve;
			curve.interpolation = AnimationInterpolation::Step;
			curve.keys = {ScalarKey(0, 0.0F), ScalarKey(60, 2.0F), ScalarKey(120, 4.0F)};

			AnimationSourceTrack rootTranslation = translation;
			rootTranslation.stableTargetId = 1U;
			rootTranslation.kind = AnimationClipTrackKind::RootTranslation;
			AnimationSourceTrack rootYaw;
			rootYaw.stableTargetId = 1U;
			rootYaw.kind = AnimationClipTrackKind::RootYaw;
			rootYaw.interpolation = AnimationInterpolation::Linear;
			rootYaw.keys = {ScalarKey(0, 0.0F), ScalarKey(120, 0.5F)};

			input.tracks = {translation, rotation, morph, curve, rootTranslation, rootYaw};
			input.namedChannels = {{600U, "Speed"}, {700U, "Smile"}};
			input.events = {
				{1000U, 15, AnimationEventPayloadType::String, "LeftFoot", true, true},
				{2000U, 75, AnimationEventPayloadType::String, "RightFoot", true, true}
			};
			return input;
		}

		float Length(AnimationFloat3 value) {
			return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
		}
	}

	void RegisterAnimationCodecTests(TestRegistry& registry) {
		registry.Add("Animation.Codec.QuantizedRandomSeekStaysWithinAuthoredError", [](TestContext& context) {
			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			const AnimationStatus compileStatus = CompileAnimationClip(MakeCodecInput(), {}, clip, report);
			const std::string compileMessage = "A valid source clip should compile into independently seekable segments. Status: "
				+ std::string(ToString(compileStatus)) + "; translation="
				+ std::to_string(report.measuredTranslationErrorMeters) + ", rotation="
				+ std::to_string(report.measuredRotationErrorDegrees) + ", scale="
				+ std::to_string(report.measuredScaleErrorFraction) + ", morph="
				+ std::to_string(report.measuredMorphError) + ", curve="
				+ std::to_string(report.measuredCurveError) + "; " + report.diagnostic;
			context.Expect(compileStatus == AnimationStatus::Ok, compileMessage);
			context.Expect(report.measuredTranslationErrorMeters <= 0.0005F
				&& report.measuredRotationErrorDegrees <= 0.1F,
				"Offline validation must reject codec output outside the frozen authoring tolerances.");
			context.Expect(report.segmentCount == 4U,
				"A 120-tick clip with 30-tick segments should produce four independent ranges.");

			AnimationCodecView view;
			context.Expect(view.Open(clip) == AnimationStatus::Ok,
				"The CPU reference decoder should validate every segment before sampling.");
			for(const AnimationFixedTick tick : {0, 1, 17, 29, 30, 47, 60, 89, 90, 119, 120}) {
				AnimationDecodedValue value;
				context.Expect(view.Decode(100U, AnimationClipTrackKind::Translation, tick, false, value)
					== AnimationStatus::Ok, "Random seeks must not depend on decoding an earlier segment.");
				const float ratio = static_cast<float>(tick) / 120.0F;
				const AnimationFloat3 expected{ratio, ratio * 2.0F, -ratio};
				context.Expect(Length({value.vectorValue.x - expected.x, value.vectorValue.y - expected.y,
					value.vectorValue.z - expected.z}) <= 0.001F,
					"Decoded translation should stay within the reviewed quantization error.");
			}
		});

		registry.Add("Animation.Codec.LoopSeamAndCpuDecodeAreReproducible", [](TestContext& context) {
			const AnimationClipCompileInput input = MakeCodecInput();
			AnimationClipAsset first;
			AnimationClipAsset second;
			AnimationClipCompileReport firstReport;
			AnimationClipCompileReport secondReport;
			context.Expect(CompileAnimationClip(input, {}, first, firstReport) == AnimationStatus::Ok
				&& CompileAnimationClip(input, {}, second, secondReport) == AnimationStatus::Ok,
				"Repeated compiler runs should both succeed.");
			std::vector<std::uint8_t> firstBytes;
			std::vector<std::uint8_t> secondBytes;
			SerializeAnimationClipAsset(first, firstBytes);
			SerializeAnimationClipAsset(second, secondBytes);
			context.Expect(firstBytes == secondBytes,
				"Identical source keys and settings must produce byte-identical cooked clips.");

			AnimationCodecView view;
			view.Open(first);
			AnimationDecodedValue atStart;
			AnimationDecodedValue atLoop;
			context.Expect(view.Decode(100U, AnimationClipTrackKind::Translation, 0, true, atStart)
				== AnimationStatus::Ok
				&& view.Decode(100U, AnimationClipTrackKind::Translation, first.durationTicks, true, atLoop)
				== AnimationStatus::Ok,
				"Loop seam samples should be available through the same checked decoder.");
			context.Expect(atStart.vectorValue == atLoop.vectorValue,
				"A looped sample at exact duration must resolve to the exact start pose.");
		});

		registry.Add("Animation.Codec.GpuTrackHeadersRemainDwordAligned", [](TestContext& context) {
			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(MakeCodecInput(), {}, clip, report)
					== AnimationStatus::Ok,
				"The mixed-channel fixture must compile before checking its GPU-readable layout.");
			// ByteAddressBuffer.Load reads 32-bit words. Every header and its
			// following time table must therefore begin at a four-byte boundary,
			// even when the preceding packed value stream has a two-byte stride.
			const bool allTrackHeadersAligned = std::all_of(
				clip.tracks.begin(), clip.tracks.end(),
				[](const AnimationClipTrack& track) {
					return track.sampleOffset % alignof(std::uint32_t) == 0U;
				});
			context.Expect(allTrackHeadersAligned,
				"Cooked track headers must be dword aligned so the GPU decoder never performs an unaligned raw-buffer load.");
		});

		registry.Add("Animation.Codec.CumulativeReductionErrorFallsBackToAuthoredKeys", [](TestContext& context) {
			AnimationClipCompileInput input;
			input.assetId = MakeCodecGuid(31U);
			input.skeletonAssetId = MakeCodecGuid(32U);
			input.compatibilityFingerprint = 0xAABBCCDDU;
			input.durationTicks = 4;
			input.segmentDurationTicks = 4;
			AnimationSourceTrack morph;
			morph.stableTargetId = 900U;
			morph.kind = AnimationClipTrackKind::MorphWeight;
			morph.interpolation = AnimationInterpolation::Linear;
			// Each early removal is locally within 0.0005, but the final chord would
			// move tick one by 0.000667. This is the smallest cumulative-reduction
			// shape that reproduced the acquired-model import rejection.
			for(const auto [tick, value] : std::array<std::pair<AnimationFixedTick, float>, 5U>{
				std::pair{0, 0.0F}, std::pair{1, -0.001F}, std::pair{2, -0.001F},
				std::pair{3, -0.001F}, std::pair{4, 0.0F}}) {
				AnimationSourceKey key;
				key.tick = tick;
				key.scalarValue = value;
				morph.keys.push_back(key);
			}
			input.tracks.push_back(std::move(morph));

			AnimationClipAsset first;
			AnimationClipCompileReport firstReport;
			AnimationClipAsset second;
			AnimationClipCompileReport secondReport;
			context.Expect(CompileAnimationClip(input, {}, first, firstReport) == AnimationStatus::Ok
				&& CompileAnimationClip(input, {}, second, secondReport) == AnimationStatus::Ok,
				"Unsafe optional reduction should retry with authored keys instead of rejecting a valid source clip.");
			context.Expect(firstReport.reductionFallbackUsed && firstReport.removedKeyCount == 0U
				&& firstReport.measuredMorphError <= AnimationClipCompilerSettings{}.morphTolerance,
				"Fallback evidence must report that no authored key was discarded and the decoded result stayed within tolerance.");
			AnimationCodecView view;
			AnimationDecodedValue decoded;
			context.Expect(view.Open(first) == AnimationStatus::Ok
				&& view.Decode(900U, AnimationClipTrackKind::MorphWeight, 1, false, decoded) == AnimationStatus::Ok
				&& std::abs(decoded.scalarValue + 0.001F) <= AnimationClipCompilerSettings{}.morphTolerance,
				"The fallback clip must preserve the previously invalidated authored sample.");
			std::vector<std::uint8_t> firstBytes;
			std::vector<std::uint8_t> secondBytes;
			SerializeAnimationClipAsset(first, firstBytes);
			SerializeAnimationClipAsset(second, secondBytes);
			context.Expect(firstBytes == secondBytes && secondReport.reductionFallbackUsed,
				"The authored-key retry must remain byte-identical across repeated compiler runs.");
		});

		registry.Add("Animation.Codec.AuthoredKeyFallbackStillRejectsIntrinsicCodecError", [](TestContext& context) {
			AnimationClipCompileInput input;
			input.assetId = MakeCodecGuid(41U);
			input.skeletonAssetId = MakeCodecGuid(42U);
			input.compatibilityFingerprint = 0x10203040U;
			input.durationTicks = 4;
			input.segmentDurationTicks = 4;
			AnimationSourceTrack morph;
			morph.stableTargetId = 901U;
			morph.kind = AnimationClipTrackKind::MorphWeight;
			morph.keys = {
				{0, {}, {}, 0.0F}, {1, {}, {}, -0.001F}, {2, {}, {}, -0.001F},
				{3, {}, {}, -0.001F}, {4, {}, {}, 0.0F}};
			AnimationSourceTrack curve;
			curve.stableTargetId = 902U;
			curve.kind = AnimationClipTrackKind::Curve;
			curve.keys = {
				{0, {}, {}, 0.0F}, {1, {}, {}, 0.123456F}, {4, {}, {}, 1.0F}};
			input.tracks = {std::move(morph), std::move(curve)};
			AnimationClipCompilerSettings settings;
			settings.curveTolerance = 0.0F;
			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, settings, clip, report) == AnimationStatus::DeterminismViolation
				&& report.diagnostic.find("authored-key fallback also failed") != std::string::npos,
				"Retrying unsafe reduction must not accept quantized bytes that intrinsically exceed a zero authored tolerance.");
		});

		registry.Add("Animation.Codec.PreservesRootMotionEventsCurvesAndMorphs", [](TestContext& context) {
			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(MakeCodecInput(), {}, clip, report) == AnimationStatus::Ok,
				"The multi-channel fixture should compile.");
			context.Expect(clip.rootMotion.enabled && clip.events.size() == 2U
				&& clip.namedChannels.size() == 2U,
				"Root-motion metadata, ordered events, and collision-safe curve/morph names must survive compilation.");

			AnimationCodecView view;
			view.Open(clip);
			AnimationDecodedValue morph;
			AnimationDecodedValue curveBefore;
			AnimationDecodedValue curveAfter;
			AnimationDecodedValue root;
			context.Expect(view.Decode(700U, AnimationClipTrackKind::MorphWeight, 60, false, morph)
				== AnimationStatus::Ok && std::abs(morph.scalarValue - 1.0F) <= 0.0005F,
				"Morph weights must decode through the same segmented representation.");
			context.Expect(view.Decode(600U, AnimationClipTrackKind::Curve, 59, false, curveBefore)
				== AnimationStatus::Ok
				&& view.Decode(600U, AnimationClipTrackKind::Curve, 60, false, curveAfter)
				== AnimationStatus::Ok
				&& curveBefore.scalarValue == 0.0F && curveAfter.scalarValue == 2.0F,
				"Step curves must preserve their discontinuity exactly at an authored key.");
			context.Expect(view.Decode(1U, AnimationClipTrackKind::RootTranslation, 120, false, root)
				== AnimationStatus::Ok && std::abs(root.vectorValue.x - 1.0F) <= 0.001F,
				"Root-motion channel decode must preserve the accumulated cycle delta.");
		});

		registry.Add("Animation.Codec.CorruptSegmentRangeFailsBeforeDecode", [](TestContext& context) {
			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			CompileAnimationClip(MakeCodecInput(), {}, clip, report);
			clip.streamData.resize(clip.streamData.size() / 2U);
			AnimationCodecView view;
			const AnimationStatus status = view.Open(clip);
			context.Expect(status == AnimationStatus::RangeOutOfBounds
				|| status == AnimationStatus::TruncatedInput,
				"A corrupt segment must fail range validation before any decoder reads it.");
		});
	}
}
