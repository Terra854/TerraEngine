// AnimationClipCompiler.cpp - Deterministic segmented animation compression.

#include "AnimationClipCompiler.h"

#include "Animation/Codec/AnimationCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>

namespace TerraEngine::AssetPipeline {
	namespace {
		using namespace TerraEngine::Animation;

		constexpr float kSmallestThreeLimit = 0.7071067811865475244F;
		constexpr float kRadiansToDegrees = 57.295779513082320876F;

		bool IsCancellationRequested(const std::atomic_bool* requested) noexcept {
			return requested != nullptr && requested->load(std::memory_order_relaxed);
		}

		enum class SourceValueType : std::uint8_t {
		Scalar,
		Vector,
		Rotation
		};

		SourceValueType ValueTypeForKind(AnimationClipTrackKind kind) noexcept {
			if(kind == AnimationClipTrackKind::Rotation) {
				return SourceValueType::Rotation;
			}
			if(kind == AnimationClipTrackKind::Translation
				|| kind == AnimationClipTrackKind::Scale
				|| kind == AnimationClipTrackKind::RootTranslation) {
				return SourceValueType::Vector;
			}
			return SourceValueType::Scalar;
		}

		float Clamp01(float value) noexcept {
			return std::clamp(value, 0.0F, 1.0F);
		}

		AnimationQuaternion Normalize(AnimationQuaternion value) noexcept {
			const float lengthSquared = value.x * value.x + value.y * value.y
				+ value.z * value.z + value.w * value.w;
			if(!std::isfinite(lengthSquared) || lengthSquared <= std::numeric_limits<float>::epsilon()) {
				return {};
			}
			const float reciprocal = 1.0F / std::sqrt(lengthSquared);
			value.x *= reciprocal;
			value.y *= reciprocal;
			value.z *= reciprocal;
			value.w *= reciprocal;
			return value;
		}

		AnimationQuaternion InterpolateRotation(
			AnimationQuaternion left,
			AnimationQuaternion right,
			float factor) noexcept {
			const float dot = left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
			if(dot < 0.0F) {
				right.x = -right.x;
				right.y = -right.y;
				right.z = -right.z;
				right.w = -right.w;
			}
			return Normalize({
				left.x + (right.x - left.x) * factor,
				left.y + (right.y - left.y) * factor,
				left.z + (right.z - left.z) * factor,
				left.w + (right.w - left.w) * factor
			});
		}

		float InterpolationFactor(float factor, AnimationInterpolation interpolation) noexcept {
			factor = Clamp01(factor);
			return interpolation == AnimationInterpolation::Cubic
				? factor * factor * (3.0F - 2.0F * factor)
				: factor;
		}

		AnimationSourceKey InterpolateKeys(
			const AnimationSourceTrack& track,
			const AnimationSourceKey& left,
			const AnimationSourceKey& right,
			AnimationFixedTick tick) noexcept {
			AnimationSourceKey result;
			result.tick = tick;
			if(track.interpolation == AnimationInterpolation::Constant
				|| track.interpolation == AnimationInterpolation::Step
				|| right.tick <= left.tick) {
				result.vectorValue = left.vectorValue;
				result.rotationValue = left.rotationValue;
				result.scalarValue = left.scalarValue;
				return result;
			}
			const float factor = InterpolationFactor(
				static_cast<float>(tick - left.tick) / static_cast<float>(right.tick - left.tick),
				track.interpolation);
			result.vectorValue = {
				left.vectorValue.x + (right.vectorValue.x - left.vectorValue.x) * factor,
				left.vectorValue.y + (right.vectorValue.y - left.vectorValue.y) * factor,
				left.vectorValue.z + (right.vectorValue.z - left.vectorValue.z) * factor
			};
			result.rotationValue = InterpolateRotation(left.rotationValue, right.rotationValue, factor);
			result.scalarValue = left.scalarValue + (right.scalarValue - left.scalarValue) * factor;
			return result;
		}

		AnimationSourceKey SampleSourceTrack(
			const AnimationSourceTrack& track,
			AnimationFixedTick tick) noexcept {
			if(tick <= track.keys.front().tick) {
				AnimationSourceKey result = track.keys.front();
				result.tick = tick;
				return result;
			}
			if(tick >= track.keys.back().tick) {
				AnimationSourceKey result = track.keys.back();
				result.tick = tick;
				return result;
			}
			const auto right = std::upper_bound(track.keys.begin(), track.keys.end(), tick,
				[](AnimationFixedTick sampleTick, const AnimationSourceKey& key) {
					return sampleTick < key.tick;
				});
			return InterpolateKeys(track, *(right - 1), *right, tick);
		}

		float VectorError(AnimationFloat3 left, AnimationFloat3 right) noexcept {
			const float x = left.x - right.x;
			const float y = left.y - right.y;
			const float z = left.z - right.z;
			return std::sqrt(x * x + y * y + z * z);
		}

		float RotationErrorDegrees(AnimationQuaternion left, AnimationQuaternion right) noexcept {
			left = Normalize(left);
			right = Normalize(right);
			const float dot = std::abs(left.x * right.x + left.y * right.y
				+ left.z * right.z + left.w * right.w);
			return 2.0F * std::acos(std::clamp(dot, 0.0F, 1.0F)) * kRadiansToDegrees;
		}

		float ToleranceForTrack(
			const AnimationSourceTrack& track,
			const AnimationClipCompilerSettings& settings) noexcept {
			switch(track.kind) {
			case AnimationClipTrackKind::Translation:
			case AnimationClipTrackKind::RootTranslation:
				return settings.translationToleranceMeters;
			case AnimationClipTrackKind::Rotation:
				return settings.rotationToleranceDegrees;
			case AnimationClipTrackKind::Scale:
				return settings.scaleToleranceFraction;
			case AnimationClipTrackKind::MorphWeight:
				return settings.morphTolerance;
			case AnimationClipTrackKind::Curve:
			case AnimationClipTrackKind::RootYaw:
				return settings.curveTolerance;
			}
			return 0.0F;
		}

		float KeyError(
			const AnimationSourceTrack& track,
			const AnimationSourceKey& expected,
			const AnimationSourceKey& actual) noexcept {
			switch(ValueTypeForKind(track.kind)) {
			case SourceValueType::Vector: return VectorError(expected.vectorValue, actual.vectorValue);
			case SourceValueType::Rotation:
				return RotationErrorDegrees(expected.rotationValue, actual.rotationValue);
			case SourceValueType::Scalar: return std::abs(expected.scalarValue - actual.scalarValue);
			}
			return std::numeric_limits<float>::infinity();
		}

		bool SettingsAreValid(const AnimationClipCompilerSettings& settings) noexcept {
			for(const float tolerance : {
				settings.translationToleranceMeters,
				settings.rotationToleranceDegrees,
				settings.scaleToleranceFraction,
				settings.morphTolerance,
				settings.curveTolerance}) {
				if(!std::isfinite(tolerance) || tolerance < 0.0F) {
					return false;
				}
			}
			return true;
		}

		AnimationStatus ValidateAndNormalizeSourceTrack(
			AnimationSourceTrack& track,
			AnimationFixedTick durationTicks) noexcept {
			if(track.stableTargetId == 0U || track.keys.empty()) {
				return AnimationStatus::InvalidArgument;
			}
			AnimationFixedTick priorTick = -1;
			std::size_t retainedKeyCount = 0U;
			for(AnimationSourceKey key : track.keys) {
				if(key.tick < 0 || key.tick > durationTicks || key.tick < priorTick) {
					return AnimationStatus::InvalidArgument;
				}
				switch(ValueTypeForKind(track.kind)) {
				case SourceValueType::Vector:
					if(!IsFinite(key.vectorValue)) return AnimationStatus::InvalidArgument;
					break;
				case SourceValueType::Rotation:
					if(!IsFinite(key.rotationValue)) return AnimationStatus::InvalidArgument;
					key.rotationValue = Normalize(key.rotationValue);
					if(!IsNormalized(key.rotationValue)) return AnimationStatus::InvalidArgument;
					break;
				case SourceValueType::Scalar:
					if(!std::isfinite(key.scalarValue)) return AnimationStatus::InvalidArgument;
					break;
				}

				if(retainedKeyCount != 0U && key.tick == priorTick) {
					// Multiple source samples can quantize onto the same fixed tick. The
					// later-authored value deterministically owns that tick, matching the
					// importer rule and avoiding zero-width codec interpolation intervals.
					track.keys[retainedKeyCount - 1U] = key;
				} else {
					track.keys[retainedKeyCount++] = key;
				}
				priorTick = key.tick;
			}
			track.keys.resize(retainedKeyCount);
			return AnimationStatus::Ok;
		}

		bool ReduceTrackKeys(
			AnimationSourceTrack& track,
			const AnimationClipCompilerSettings& settings,
			const std::atomic_bool* cancellationRequested) {
			if(track.keys.size() < 2U) {
				return !IsCancellationRequested(cancellationRequested);
			}
			if(track.interpolation == AnimationInterpolation::Constant) {
				track.keys.resize(1U);
				return !IsCancellationRequested(cancellationRequested);
			}
			if(track.interpolation == AnimationInterpolation::Step) {
				std::vector<AnimationSourceKey> retained;
				retained.reserve(track.keys.size());
				retained.push_back(track.keys.front());
				for(std::size_t index = 1U; index < track.keys.size(); ++index) {
					if(IsCancellationRequested(cancellationRequested)) {
						return false;
					}
					if(KeyError(track, retained.back(), track.keys[index]) > 0.0F
						|| index + 1U == track.keys.size()) {
						retained.push_back(track.keys[index]);
					}
				}
				track.keys = std::move(retained);
				return true;
			}

			// Repeated passes remove a key only when its neighbors reproduce it within
			// the class-specific authored tolerance. This keeps reduction deterministic.
			bool removed = true;
			while(removed && track.keys.size() > 2U) {
				if(IsCancellationRequested(cancellationRequested)) {
					return false;
				}
				removed = false;
				for(std::size_t index = 1U; index + 1U < track.keys.size(); ++index) {
					const AnimationSourceKey predicted = InterpolateKeys(
						track, track.keys[index - 1U], track.keys[index + 1U], track.keys[index].tick);
					if(KeyError(track, track.keys[index], predicted) <= ToleranceForTrack(track, settings)) {
						track.keys.erase(track.keys.begin() + static_cast<std::ptrdiff_t>(index));
						removed = true;
						break;
					}
				}
			}
			return true;
		}

		template<typename Value>
		bool AppendValue(std::vector<std::uint8_t>& bytes, const Value& value) {
			if(bytes.size() > std::numeric_limits<std::size_t>::max() - sizeof(Value)) {
				return false;
			}
			const std::size_t offset = bytes.size();
			bytes.resize(offset + sizeof(Value));
			std::memcpy(bytes.data() + offset, &value, sizeof(Value));
			return true;
		}

		bool AlignTrackHeader(std::vector<std::uint8_t>& bytes) {
			// GPU decoding uses ByteAddressBuffer.Load, whose addresses are
			// 32-bit-word based. Padding between independent tracks keeps every
			// header and its following time table legal for both CPU and GPU reads.
			constexpr std::size_t alignment = alignof(std::uint32_t);
			const std::size_t padding =
				(alignment - (bytes.size() % alignment)) % alignment;
			if(padding > std::numeric_limits<std::size_t>::max() - bytes.size()) {
				return false;
			}
			bytes.resize(bytes.size() + padding, 0U);
			return true;
		}

		std::uint16_t QuantizeUnorm16(float value, float minimum, float extent) noexcept {
			if(extent <= std::numeric_limits<float>::epsilon()) {
				return 0U;
			}
			return static_cast<std::uint16_t>(std::lround(Clamp01((value - minimum) / extent) * 65535.0F));
		}

		AnimationCodecQuaternionKeyV1 EncodeSmallestThree(AnimationQuaternion rotation) noexcept {
			rotation = Normalize(rotation);
			float components[4] = {rotation.x, rotation.y, rotation.z, rotation.w};
			std::uint8_t largest = 0U;
			for(std::uint8_t index = 1U; index < 4U; ++index) {
				if(std::abs(components[index]) > std::abs(components[largest])) {
					largest = index;
				}
			}
			if(components[largest] < 0.0F) {
				for(float& component : components) component = -component;
			}
			std::int16_t stored[3]{};
			std::size_t storedIndex = 0U;
			for(std::uint8_t index = 0U; index < 4U; ++index) {
				if(index == largest) continue;
				const float normalized = std::clamp(components[index] / kSmallestThreeLimit, -1.0F, 1.0F);
				stored[storedIndex++] = static_cast<std::int16_t>(std::lround(normalized * 32767.0F));
			}
			return {stored[0], stored[1], stored[2], largest, 0U};
		}

		bool TrackIsConstant(
			const AnimationSourceTrack& track,
			std::span<const AnimationSourceKey> keys,
			const AnimationClipCompilerSettings& settings) noexcept {
			for(std::size_t index = 1U; index < keys.size(); ++index) {
				if(KeyError(track, keys.front(), keys[index]) > ToleranceForTrack(track, settings)) {
					return false;
				}
			}
			return true;
		}

		AnimationStatus EncodeSegmentTrack(
			const AnimationSourceTrack& sourceTrack,
			std::span<const AnimationSourceKey> keys,
			const AnimationClipCompilerSettings& settings,
			std::uint32_t segmentIndex,
			AnimationFixedTick segmentStart,
			AnimationFixedTick segmentEnd,
			AnimationClipAsset& clip) {
			AnimationCodecTrackHeaderV1 header;
			header.segmentStartTick = segmentStart;
			header.segmentEndTick = segmentEnd;
			header.keyCount = static_cast<std::uint32_t>(keys.size());
			const SourceValueType valueType = ValueTypeForKind(sourceTrack.kind);
			header.encoding = valueType == SourceValueType::Vector
				? AnimationCodecValueEncoding::Vector3Unorm16
				: valueType == SourceValueType::Rotation
					? AnimationCodecValueEncoding::QuaternionSmallestThree16
					: AnimationCodecValueEncoding::ScalarUnorm16;

			const bool constant = TrackIsConstant(sourceTrack, keys, settings);
			if(constant) {
				header.flags = kAnimationCodecConstantTrack;
				header.keyCount = 1U;
				header.byteSize = sizeof(header);
				header.constantVector = keys.front().vectorValue;
				header.constantRotation = Normalize(keys.front().rotationValue);
				header.constantScalar = keys.front().scalarValue;
			} else {
				AnimationFloat3 minimum{
					std::numeric_limits<float>::max(),
					std::numeric_limits<float>::max(),
					std::numeric_limits<float>::max()};
				AnimationFloat3 maximum{
					std::numeric_limits<float>::lowest(),
					std::numeric_limits<float>::lowest(),
					std::numeric_limits<float>::lowest()};
				if(valueType == SourceValueType::Vector) {
					for(const AnimationSourceKey& key : keys) {
						minimum.x = std::min(minimum.x, key.vectorValue.x);
						minimum.y = std::min(minimum.y, key.vectorValue.y);
						minimum.z = std::min(minimum.z, key.vectorValue.z);
						maximum.x = std::max(maximum.x, key.vectorValue.x);
						maximum.y = std::max(maximum.y, key.vectorValue.y);
						maximum.z = std::max(maximum.z, key.vectorValue.z);
					}
				} else if(valueType == SourceValueType::Scalar) {
					minimum = {};
					maximum = {};
					minimum.x = std::numeric_limits<float>::max();
					maximum.x = std::numeric_limits<float>::lowest();
					for(const AnimationSourceKey& key : keys) {
						minimum.x = std::min(minimum.x, key.scalarValue);
						maximum.x = std::max(maximum.x, key.scalarValue);
					}
				} else {
					minimum = {};
					maximum = {};
				}
				header.rangeMinimum = minimum;
				header.rangeExtent = {
					maximum.x - minimum.x,
					maximum.y - minimum.y,
					maximum.z - minimum.z};
				const std::uint64_t valueStride = valueType == SourceValueType::Vector
					? sizeof(AnimationCodecVectorKeyV1)
					: valueType == SourceValueType::Rotation
						? sizeof(AnimationCodecQuaternionKeyV1)
						: sizeof(std::uint16_t);
				std::uint64_t byteSize = sizeof(header);
				std::uint64_t timeBytes = 0U;
				std::uint64_t valueBytes = 0U;
				if(!CheckedMultiply(keys.size(), sizeof(std::uint32_t), timeBytes)
					|| !CheckedMultiply(keys.size(), valueStride, valueBytes)
					|| !CheckedAdd(byteSize, timeBytes, byteSize)
					|| !CheckedAdd(byteSize, valueBytes, byteSize)
					|| byteSize > std::numeric_limits<std::uint32_t>::max()) {
					return AnimationStatus::RangeOverflow;
				}
				header.byteSize = static_cast<std::uint32_t>(byteSize);
			}

			AnimationClipTrack track;
			track.stableTargetId = sourceTrack.stableTargetId;
			track.kind = sourceTrack.kind;
			track.interpolation = sourceTrack.interpolation;
			track.segmentIndex = segmentIndex;
			if(!AlignTrackHeader(clip.streamData)) {
				return AnimationStatus::RangeOverflow;
			}
			track.sampleOffset = clip.streamData.size();
			track.sampleCount = header.keyCount;
			track.encodedValueBits = valueType == SourceValueType::Vector
				? kAnimationCodecVectorBits
				: valueType == SourceValueType::Rotation
					? kAnimationCodecQuaternionBits
					: kAnimationCodecScalarBits;
			if(!AppendValue(clip.streamData, header)) {
				return AnimationStatus::RangeOverflow;
			}
			if(!constant) {
				for(const AnimationSourceKey& key : keys) {
					const std::uint32_t tickOffset = static_cast<std::uint32_t>(key.tick - segmentStart);
					if(!AppendValue(clip.streamData, tickOffset)) return AnimationStatus::RangeOverflow;
				}
				for(const AnimationSourceKey& key : keys) {
					if(valueType == SourceValueType::Vector) {
						const AnimationCodecVectorKeyV1 encoded{
							QuantizeUnorm16(key.vectorValue.x, header.rangeMinimum.x, header.rangeExtent.x),
							QuantizeUnorm16(key.vectorValue.y, header.rangeMinimum.y, header.rangeExtent.y),
							QuantizeUnorm16(key.vectorValue.z, header.rangeMinimum.z, header.rangeExtent.z)};
						if(!AppendValue(clip.streamData, encoded)) return AnimationStatus::RangeOverflow;
					} else if(valueType == SourceValueType::Rotation) {
						const AnimationCodecQuaternionKeyV1 encoded = EncodeSmallestThree(key.rotationValue);
						if(!AppendValue(clip.streamData, encoded)) return AnimationStatus::RangeOverflow;
					} else {
						const std::uint16_t encoded = QuantizeUnorm16(
							key.scalarValue, header.rangeMinimum.x, header.rangeExtent.x);
						if(!AppendValue(clip.streamData, encoded)) return AnimationStatus::RangeOverflow;
					}
				}
			}
			clip.tracks.push_back(track);
			return AnimationStatus::Ok;
		}

		void AccumulateMeasuredError(
			AnimationClipTrackKind kind,
			const AnimationSourceKey& expected,
			const AnimationDecodedValue& decoded,
			AnimationClipCompileReport& report) noexcept {
			switch(kind) {
			case AnimationClipTrackKind::Translation:
			case AnimationClipTrackKind::RootTranslation:
				report.measuredTranslationErrorMeters = std::max(
					report.measuredTranslationErrorMeters, VectorError(expected.vectorValue, decoded.vectorValue));
				break;
			case AnimationClipTrackKind::Rotation:
				report.measuredRotationErrorDegrees = std::max(
					report.measuredRotationErrorDegrees,
					RotationErrorDegrees(expected.rotationValue, decoded.rotationValue));
				break;
			case AnimationClipTrackKind::Scale:
				report.measuredScaleErrorFraction = std::max(
					report.measuredScaleErrorFraction, VectorError(expected.vectorValue, decoded.vectorValue));
				break;
			case AnimationClipTrackKind::MorphWeight:
				report.measuredMorphError = std::max(
					report.measuredMorphError, std::abs(expected.scalarValue - decoded.scalarValue));
				break;
			case AnimationClipTrackKind::Curve:
			case AnimationClipTrackKind::RootYaw:
				report.measuredCurveError = std::max(
					report.measuredCurveError, std::abs(expected.scalarValue - decoded.scalarValue));
				break;
			}
		}

		AnimationStatus ValidateDecodedErrors(
			const std::vector<AnimationSourceTrack>& sourceTracks,
			const AnimationClipCompilerSettings& settings,
			AnimationClipAsset& clip,
			AnimationClipCompileReport& report,
			const std::atomic_bool* cancellationRequested) {
			AnimationCodecView view;
			if(const AnimationStatus status = view.Open(clip); status != AnimationStatus::Ok) {
				return status;
			}
			for(const AnimationSourceTrack& track : sourceTracks) {
				if(IsCancellationRequested(cancellationRequested)) {
					return AnimationStatus::Cancelled;
				}
				std::set<AnimationFixedTick> validationTicks;
				for(const AnimationSourceKey& key : track.keys) validationTicks.insert(key.tick);
				for(const AnimationClipSegment& segment : clip.segments) {
					validationTicks.insert(segment.startTick);
					validationTicks.insert(segment.endTick);
					validationTicks.insert(segment.startTick + (segment.endTick - segment.startTick) / 2);
				}
				// Dense short-clip validation catches interpolation and loop-boundary errors;
				// long clips use the bounded authored/boundary set above.
				if(clip.durationTicks <= 4096) {
					for(AnimationFixedTick tick = 0; tick <= clip.durationTicks; ++tick) validationTicks.insert(tick);
				}
				for(const AnimationFixedTick tick : validationTicks) {
					if(IsCancellationRequested(cancellationRequested)) {
						return AnimationStatus::Cancelled;
					}
					const AnimationSourceKey expected = SampleSourceTrack(track, tick);
					AnimationDecodedValue decoded;
					if(const AnimationStatus status = view.Decode(
						track.stableTargetId, track.kind, tick, false, decoded);
						status != AnimationStatus::Ok) {
						return status;
					}
					AccumulateMeasuredError(track.kind, expected, decoded, report);
				}
			}
			clip.compression.translationErrorMeters = report.measuredTranslationErrorMeters;
			clip.compression.rotationErrorDegrees = report.measuredRotationErrorDegrees;
			clip.compression.scaleErrorFraction = report.measuredScaleErrorFraction;
			clip.compression.morphError = report.measuredMorphError;
			if(report.measuredTranslationErrorMeters > settings.translationToleranceMeters
				|| report.measuredRotationErrorDegrees > settings.rotationToleranceDegrees
				|| report.measuredScaleErrorFraction > settings.scaleToleranceFraction
				|| report.measuredMorphError > settings.morphTolerance
				|| report.measuredCurveError > settings.curveTolerance) {
				return AnimationStatus::DeterminismViolation;
			}
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus CompileAnimationClip(
		const AnimationClipCompileInput& input,
		const AnimationClipCompilerSettings& settings,
		AnimationClipAsset& output,
		AnimationClipCompileReport& report,
		const std::atomic_bool* cancellationRequested) {
		AnimationClipCompileReport stagedReport;
		auto fail = [&](AnimationStatus status, std::string diagnostic) {
			stagedReport.diagnostic = std::move(diagnostic);
			report = stagedReport;
			return status;
		};
		if(IsCancellationRequested(cancellationRequested)) {
			return fail(AnimationStatus::Cancelled, "Clip compilation was cancelled before validation.");
		}
		if(!input.assetId.IsValid() || !input.skeletonAssetId.IsValid()
			|| input.compatibilityFingerprint == 0U || input.durationTicks <= 0
			|| input.sampleRate == 0U || input.segmentDurationTicks <= 0
			|| static_cast<std::uint64_t>(input.segmentDurationTicks) > std::numeric_limits<std::uint32_t>::max()
			|| input.tracks.empty() || !SettingsAreValid(settings)) {
			return fail(AnimationStatus::InvalidArgument, "Clip identity, timing, tracks, or tolerances are invalid.");
		}
		const std::uint64_t maximumDurationTicks =
			GetAnimationRuntimeLimit(AnimationLimit::ClipDurationSeconds).hardMaximum
				* static_cast<std::uint64_t>(kAnimationFixedTicksPerSecond);
		if(static_cast<std::uint64_t>(input.durationTicks) > maximumDurationTicks) {
			return fail(AnimationStatus::HardLimitExceeded,
				"Clip duration exceeds the frozen twenty-four-hour hard limit.");
		}
		if(ValidateAnimationHardLimit(AnimationLimit::ClipChannels, input.tracks.size()) != AnimationStatus::Ok
			|| ValidateAnimationHardLimit(AnimationLimit::ClipAuthoredEvents, input.events.size()) != AnimationStatus::Ok) {
			return fail(AnimationStatus::HardLimitExceeded, "Clip channels or authored events exceed the frozen hard limit.");
		}

		std::vector<AnimationSourceTrack> originalTracks = input.tracks;
		std::set<std::pair<AnimationClipTrackKind, std::uint64_t>> uniqueTracks;
		for(AnimationSourceTrack& track : originalTracks) {
			if(IsCancellationRequested(cancellationRequested)) {
				return fail(AnimationStatus::Cancelled, "Clip compilation was cancelled while validating tracks.");
			}
			if(!CheckedAdd(stagedReport.sourceKeyCount,
				static_cast<std::uint64_t>(track.keys.size()), stagedReport.sourceKeyCount)) {
				return fail(AnimationStatus::RangeOverflow, "Source key count overflowed its checked range.");
			}
			if(!uniqueTracks.emplace(track.kind, track.stableTargetId).second) {
				return fail(AnimationStatus::InvalidArgument, "A clip contains duplicate stable target and channel pairs.");
			}
			if(const AnimationStatus status = ValidateAndNormalizeSourceTrack(track, input.durationTicks);
				status != AnimationStatus::Ok) {
				return fail(status, "A source track contains invalid, non-finite, descending, or out-of-range keys.");
			}
		}
		if(ValidateAnimationHardLimit(AnimationLimit::ClipKeys, stagedReport.sourceKeyCount)
			!= AnimationStatus::Ok) {
			return fail(AnimationStatus::HardLimitExceeded, "Source key count exceeds the frozen hard limit.");
		}

		std::vector<AnimationSourceTrack> reducedTracks = originalTracks;
		for(AnimationSourceTrack& track : reducedTracks) {
			if(settings.enableKeyReduction && !ReduceTrackKeys(track, settings, cancellationRequested)) {
				return fail(AnimationStatus::Cancelled, "Clip compilation was cancelled during key reduction.");
			}
			stagedReport.retainedKeyCount += track.keys.size();
		}
		stagedReport.removedKeyCount = stagedReport.sourceKeyCount - stagedReport.retainedKeyCount;
		std::sort(reducedTracks.begin(), reducedTracks.end(), [](const auto& left, const auto& right) {
			return std::tie(left.kind, left.stableTargetId) < std::tie(right.kind, right.stableTargetId);
		});

		const std::uint64_t segmentCount64 = (
			static_cast<std::uint64_t>(input.durationTicks) + static_cast<std::uint64_t>(input.segmentDurationTicks) - 1U)
			/ static_cast<std::uint64_t>(input.segmentDurationTicks);
		if(segmentCount64 == 0U || segmentCount64 > std::numeric_limits<std::uint32_t>::max()
			|| segmentCount64 * reducedTracks.size()
				> GetAnimationRuntimeLimit(AnimationLimit::ClipChannels).hardMaximum) {
			return fail(AnimationStatus::HardLimitExceeded, "Segment expansion exceeds the frozen clip-channel hard limit.");
		}
		stagedReport.segmentCount = static_cast<std::uint32_t>(segmentCount64);

		AnimationClipAsset staged;
		staged.assetId = input.assetId;
		staged.skeletonAssetId = input.skeletonAssetId;
		staged.compatibilityFingerprint = input.compatibilityFingerprint;
		staged.durationTicks = input.durationTicks;
		staged.sampleRate = input.sampleRate;
		staged.namedChannels = input.namedChannels;
		staged.events = input.events;
		staged.compression.decoderVersion = kAnimationCodecVersion;
		std::sort(staged.namedChannels.begin(), staged.namedChannels.end(), [](const auto& left, const auto& right) {
			return std::tie(left.stableId, left.name) < std::tie(right.stableId, right.name);
		});
		std::sort(staged.events.begin(), staged.events.end(), [](const auto& left, const auto& right) {
			return std::tie(left.tick, left.stableEventId) < std::tie(right.tick, right.stableEventId);
		});

		for(std::uint32_t segmentIndex = 0U; segmentIndex < stagedReport.segmentCount; ++segmentIndex) {
			if(IsCancellationRequested(cancellationRequested)) {
				return fail(AnimationStatus::Cancelled, "Clip compilation was cancelled while encoding segments.");
			}
			const AnimationFixedTick segmentStart = static_cast<AnimationFixedTick>(segmentIndex)
				* input.segmentDurationTicks;
			const AnimationFixedTick segmentEnd = std::min(
				input.durationTicks, segmentStart + input.segmentDurationTicks);
			AnimationClipSegment segment;
			segment.startTick = segmentStart;
			segment.endTick = segmentEnd;
			segment.streamOffset = staged.streamData.size();
			segment.firstTrack = static_cast<std::uint32_t>(staged.tracks.size());
			for(const AnimationSourceTrack& sourceTrack : reducedTracks) {
				if(IsCancellationRequested(cancellationRequested)) {
					return fail(AnimationStatus::Cancelled, "Clip compilation was cancelled while encoding tracks.");
				}
				std::vector<AnimationSourceKey> segmentKeys;
				segmentKeys.push_back(SampleSourceTrack(sourceTrack, segmentStart));
				if(sourceTrack.interpolation == AnimationInterpolation::Cubic
					|| sourceTrack.kind == AnimationClipTrackKind::Rotation) {
					// Cubic source data has no portable tangent record at this normalized
					// seam, and normalized quaternion lerp is not associative when a source
					// interval is split. Fixed-tick samples preserve both authored results
					// exactly while keeping each segment independently addressable.
					for(AnimationFixedTick tick = segmentStart + 1; tick < segmentEnd; ++tick) {
						segmentKeys.push_back(SampleSourceTrack(sourceTrack, tick));
					}
				} else {
					for(const AnimationSourceKey& key : sourceTrack.keys) {
						if(key.tick > segmentStart && key.tick < segmentEnd) segmentKeys.push_back(key);
					}
				}
				if(segmentEnd != segmentStart) segmentKeys.push_back(SampleSourceTrack(sourceTrack, segmentEnd));
				std::sort(segmentKeys.begin(), segmentKeys.end(), [](const auto& left, const auto& right) {
					return left.tick < right.tick;
				});
				segmentKeys.erase(std::unique(segmentKeys.begin(), segmentKeys.end(), [](const auto& left, const auto& right) {
					return left.tick == right.tick;
				}), segmentKeys.end());
				if(const AnimationStatus status = EncodeSegmentTrack(
					sourceTrack, segmentKeys, settings, segmentIndex, segmentStart, segmentEnd, staged);
					status != AnimationStatus::Ok) {
					return fail(status, "A segment could not be encoded within checked byte ranges.");
				}
			}
			segment.trackCount = static_cast<std::uint32_t>(staged.tracks.size()) - segment.firstTrack;
			segment.streamSize = staged.streamData.size() - segment.streamOffset;
			staged.segments.push_back(segment);
		}

		const AnimationSourceTrack* rootTranslation = nullptr;
		const AnimationSourceTrack* rootYaw = nullptr;
		for(const AnimationSourceTrack& track : originalTracks) {
			if(track.kind == AnimationClipTrackKind::RootTranslation) rootTranslation = &track;
			if(track.kind == AnimationClipTrackKind::RootYaw) rootYaw = &track;
		}
		if(rootTranslation != nullptr && rootYaw != nullptr) {
			staged.rootMotion.enabled = true;
			for(std::uint32_t index = 0U; index < staged.tracks.size(); ++index) {
				if(staged.tracks[index].segmentIndex != 0U) continue;
				if(staged.tracks[index].kind == AnimationClipTrackKind::RootTranslation) {
					staged.rootMotion.translationTrackIndex = index;
				}
				if(staged.tracks[index].kind == AnimationClipTrackKind::RootYaw) {
					staged.rootMotion.yawTrackIndex = index;
				}
			}
			const AnimationSourceKey translationStart = SampleSourceTrack(*rootTranslation, 0);
			const AnimationSourceKey translationEnd = SampleSourceTrack(*rootTranslation, input.durationTicks);
			staged.rootMotion.accumulatedCycleTranslation = {
				translationEnd.vectorValue.x - translationStart.vectorValue.x,
				translationEnd.vectorValue.y - translationStart.vectorValue.y,
				translationEnd.vectorValue.z - translationStart.vectorValue.z};
			const AnimationSourceKey yawStart = SampleSourceTrack(*rootYaw, 0);
			const AnimationSourceKey yawEnd = SampleSourceTrack(*rootYaw, input.durationTicks);
			staged.rootMotion.accumulatedCycleYawRadians = yawEnd.scalarValue - yawStart.scalarValue;
		}

		if(const AnimationStatus status = ValidateAnimationClipAsset(staged); status != AnimationStatus::Ok) {
			return fail(status, "The staged clip failed its public asset invariants.");
		}
		if(const AnimationStatus status = ValidateDecodedErrors(
			originalTracks, settings, staged, stagedReport, cancellationRequested);
			status != AnimationStatus::Ok) {
			if(status == AnimationStatus::DeterminismViolation
				&& settings.enableKeyReduction && stagedReport.removedKeyCount > 0U) {
				// Greedy local removals can compose into a larger error after later
				// neighbors are removed. The exhaustive decoded comparison above is the
				// authority: retry once with every normalized authored key so a valid
				// source is not rejected merely because optional reduction was unsafe.
				AnimationClipCompilerSettings fallbackSettings = settings;
				fallbackSettings.enableKeyReduction = false;
				AnimationClipAsset fallbackClip;
				AnimationClipCompileReport fallbackReport;
				const AnimationStatus fallbackStatus = CompileAnimationClip(
					input, fallbackSettings, fallbackClip, fallbackReport, cancellationRequested);
				if(fallbackStatus == AnimationStatus::Ok) {
					fallbackReport.reductionFallbackUsed = true;
					fallbackReport.diagnostic = "Compiled deterministic segmented animation clip after preserving authored keys because reduction exceeded tolerance.";
					output = std::move(fallbackClip);
					report = std::move(fallbackReport);
					return AnimationStatus::Ok;
				}
				return fail(fallbackStatus,
					"Reduced codec output exceeded tolerance and the authored-key fallback also failed: "
						+ fallbackReport.diagnostic);
			}
			return fail(status, "Decoded codec output exceeded a configured authored error tolerance.");
		}
		if(const AnimationStatus status = ValidateAnimationClipAsset(staged); status != AnimationStatus::Ok) {
			return fail(status, "Measured error metadata made the staged clip invalid.");
		}

		stagedReport.diagnostic = "Compiled deterministic segmented animation clip.";
		output = std::move(staged);
		report = std::move(stagedReport);
		return AnimationStatus::Ok;
	}
}
