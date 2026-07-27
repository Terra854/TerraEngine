// AnimationClipCompiler.h - Source-neutral offline clip compression seam.
//
// glTF, FBX, recording, and future importers normalize their authored keys into
// these types before the deterministic engine-owned codec is produced.

#pragma once

#include "Animation/Assets/AnimationClipAsset.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::AssetPipeline {
	struct AnimationSourceKey {
		Animation::AnimationFixedTick tick = 0;
		Animation::AnimationFloat3 vectorValue{};
		Animation::AnimationQuaternion rotationValue{};
		float scalarValue = 0.0F;
	};

	struct AnimationSourceTrack {
		std::uint64_t stableTargetId = 0U;
		Animation::AnimationClipTrackKind kind = Animation::AnimationClipTrackKind::Translation;
		Animation::AnimationInterpolation interpolation = Animation::AnimationInterpolation::Linear;
		// Keys are authored in nondecreasing fixed-tick order. If multiple keys
		// share a tick, the later entry owns that tick after deterministic cooking.
		std::vector<AnimationSourceKey> keys;
	};

	struct AnimationClipCompileInput {
		Animation::AnimationGuid assetId{};
		Animation::AnimationGuid skeletonAssetId{};
		Animation::AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		Animation::AnimationFixedTick durationTicks = 0;
		std::uint32_t sampleRate = Animation::kAnimationFixedTicksPerSecond;
		Animation::AnimationFixedTick segmentDurationTicks = 30;
		std::vector<AnimationSourceTrack> tracks;
		std::vector<Animation::AnimationClipNamedChannel> namedChannels;
		std::vector<Animation::AnimationClipEvent> events;
	};

	struct AnimationClipCompilerSettings {
		float translationToleranceMeters = 0.0005F;
		float rotationToleranceDegrees = 0.1F;
		float scaleToleranceFraction = 0.001F;
		float morphTolerance = 0.0005F;
		float curveTolerance = 0.0005F;
		// Key reduction is normally enabled. The compiler turns it off for one
		// deterministic retry when cumulative greedy-removal error is discovered
		// by the complete decoded-output validation pass.
		bool enableKeyReduction = true;
	};

	struct AnimationClipCompileReport {
		std::uint32_t segmentCount = 0U;
		std::uint64_t sourceKeyCount = 0U;
		std::uint64_t retainedKeyCount = 0U;
		std::uint64_t removedKeyCount = 0U;
		float measuredTranslationErrorMeters = 0.0F;
		float measuredRotationErrorDegrees = 0.0F;
		float measuredScaleErrorFraction = 0.0F;
		float measuredMorphError = 0.0F;
		float measuredCurveError = 0.0F;
		bool reductionFallbackUsed = false;
		std::string diagnostic;
	};

	Animation::AnimationStatus CompileAnimationClip(
		const AnimationClipCompileInput& input,
		const AnimationClipCompilerSettings& settings,
		Animation::AnimationClipAsset& output,
		AnimationClipCompileReport& report,
		// The pointer is additive and optional so ordinary import cooking keeps its
		// existing behavior. Long editor recordings pass a durable atomic token that
		// is checked between normalization, reduction, segment, and validation work.
		const std::atomic_bool* cancellationRequested = nullptr);
}
