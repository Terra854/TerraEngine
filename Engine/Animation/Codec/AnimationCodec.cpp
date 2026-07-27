// AnimationCodec.cpp - Range-checked random-access animation decompression.

#include "Animation/Codec/AnimationCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace TerraEngine::Animation {
	namespace {
		constexpr float kSmallestThreeLimit = 0.7071067811865475244F;
		constexpr std::uint64_t kStreamFingerprintOffsetBasis = 1469598103934665603ULL;
		constexpr std::uint64_t kStreamFingerprintPrime = 1099511628211ULL;

		std::uint64_t ComputeSegmentFingerprint(
			std::span<const std::uint8_t> bytes) noexcept {
			// Match AnimationStreaming's stable byte fingerprint without making the
			// low-level codec depend on the streaming coordinator implementation.
			std::uint64_t hash = kStreamFingerprintOffsetBasis;
			for(const std::uint8_t value : bytes) {
				hash ^= value;
				hash *= kStreamFingerprintPrime;
			}
			return hash;
		}

		AnimationCodecValueEncoding EncodingForKind(AnimationClipTrackKind kind) noexcept {
			switch(kind) {
			case AnimationClipTrackKind::Rotation:
				return AnimationCodecValueEncoding::QuaternionSmallestThree16;
			case AnimationClipTrackKind::Translation:
			case AnimationClipTrackKind::Scale:
			case AnimationClipTrackKind::RootTranslation:
				return AnimationCodecValueEncoding::Vector3Unorm16;
			case AnimationClipTrackKind::MorphWeight:
			case AnimationClipTrackKind::Curve:
			case AnimationClipTrackKind::RootYaw:
				return AnimationCodecValueEncoding::ScalarUnorm16;
			}
			return AnimationCodecValueEncoding::ScalarUnorm16;
		}

		std::uint32_t EncodedBitsForKind(AnimationClipTrackKind kind) noexcept {
			switch(EncodingForKind(kind)) {
			case AnimationCodecValueEncoding::ScalarUnorm16: return kAnimationCodecScalarBits;
			case AnimationCodecValueEncoding::Vector3Unorm16: return kAnimationCodecVectorBits;
			case AnimationCodecValueEncoding::QuaternionSmallestThree16: return kAnimationCodecQuaternionBits;
			}
			return 0U;
		}

		std::uint64_t EncodedStride(AnimationCodecValueEncoding encoding) noexcept {
			switch(encoding) {
			case AnimationCodecValueEncoding::ScalarUnorm16: return sizeof(std::uint16_t);
			case AnimationCodecValueEncoding::Vector3Unorm16: return sizeof(AnimationCodecVectorKeyV1);
			case AnimationCodecValueEncoding::QuaternionSmallestThree16:
				return sizeof(AnimationCodecQuaternionKeyV1);
			}
			return 0U;
		}

		template<typename Value>
		bool ReadValue(std::span<const std::uint8_t> bytes, std::uint64_t offset, Value& output) noexcept {
			if(ValidateAnimationRange(bytes.size(), offset, sizeof(Value)) != AnimationStatus::Ok) {
				return false;
			}
			std::memcpy(&output, bytes.data() + static_cast<std::size_t>(offset), sizeof(Value));
			return true;
		}

		AnimationStatus ExpectedTrackByteSize(
			const AnimationCodecTrackHeaderV1& header,
			std::uint64_t& byteSize) noexcept {
			byteSize = sizeof(header);
			if((header.flags & kAnimationCodecConstantTrack) != 0U) {
				return header.keyCount == 1U ? AnimationStatus::Ok : AnimationStatus::InvalidChunk;
			}
			if(header.keyCount < 2U) {
				return AnimationStatus::InvalidChunk;
			}
			std::uint64_t timeBytes = 0U;
			std::uint64_t valueBytes = 0U;
			if(!CheckedMultiply(header.keyCount, sizeof(std::uint32_t), timeBytes)
				|| !CheckedMultiply(header.keyCount, EncodedStride(header.encoding), valueBytes)
				|| !CheckedAdd(byteSize, timeBytes, byteSize)
				|| !CheckedAdd(byteSize, valueBytes, byteSize)) {
				return AnimationStatus::RangeOverflow;
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus ReadAndValidateHeader(
			const AnimationClipAsset& asset,
			const AnimationClipTrack& track,
			std::span<const std::uint8_t> bytes,
			std::uint64_t baseStreamOffset,
			std::uint64_t& localTrackOffset,
			AnimationCodecTrackHeaderV1& header) noexcept {
			const AnimationClipSegment& segment = asset.segments[track.segmentIndex];
			if(track.sampleOffset < segment.streamOffset
				|| track.sampleOffset < baseStreamOffset) {
				return AnimationStatus::RangeOutOfBounds;
			}
			localTrackOffset = track.sampleOffset - baseStreamOffset;
			if(!ReadValue(bytes, localTrackOffset, header)) {
				return AnimationStatus::TruncatedInput;
			}
			if(header.magic != kAnimationCodecTrackMagic || header.version != kAnimationCodecVersion
				|| header.encoding != EncodingForKind(track.kind)
				|| (header.flags & ~kAnimationCodecConstantTrack) != 0U
				|| header.keyCount != track.sampleCount
				|| header.segmentStartTick != segment.startTick
				|| header.segmentEndTick != segment.endTick
				|| header.segmentEndTick < header.segmentStartTick
				|| track.encodedValueBits != EncodedBitsForKind(track.kind)
				|| !IsFinite(header.rangeMinimum) || !IsFinite(header.rangeExtent)
				|| header.rangeExtent.x < 0.0F || header.rangeExtent.y < 0.0F || header.rangeExtent.z < 0.0F) {
				return AnimationStatus::InvalidChunk;
			}
			std::uint64_t expectedSize = 0U;
			if(const AnimationStatus status = ExpectedTrackByteSize(header, expectedSize);
				status != AnimationStatus::Ok) {
				return status;
			}
			if(header.byteSize != expectedSize
				|| ValidateAnimationRange(bytes.size(), localTrackOffset, expectedSize)
					!= AnimationStatus::Ok) {
				return AnimationStatus::RangeOutOfBounds;
			}
			std::uint64_t trackEnd = 0U;
			std::uint64_t segmentEnd = 0U;
			if(!CheckedAdd(track.sampleOffset, expectedSize, trackEnd)
				|| !CheckedAdd(segment.streamOffset, segment.streamSize, segmentEnd)) {
				return AnimationStatus::RangeOverflow;
			}
			if(trackEnd > segmentEnd) {
				return AnimationStatus::RangeOutOfBounds;
			}

			if((header.flags & kAnimationCodecConstantTrack) != 0U) {
				if(header.encoding == AnimationCodecValueEncoding::QuaternionSmallestThree16) {
					return IsNormalized(header.constantRotation)
						? AnimationStatus::Ok : AnimationStatus::InvalidChunk;
				}
				if(header.encoding == AnimationCodecValueEncoding::Vector3Unorm16) {
					return IsFinite(header.constantVector)
						? AnimationStatus::Ok : AnimationStatus::InvalidChunk;
				}
				return std::isfinite(header.constantScalar)
					? AnimationStatus::Ok : AnimationStatus::InvalidChunk;
			}

			const std::uint64_t timeOffset = localTrackOffset + sizeof(header);
			std::uint32_t priorOffset = 0U;
			const std::uint64_t segmentDuration = static_cast<std::uint64_t>(
				header.segmentEndTick - header.segmentStartTick);
			for(std::uint32_t keyIndex = 0U; keyIndex < header.keyCount; ++keyIndex) {
				std::uint32_t tickOffset = 0U;
				if(!ReadValue(bytes, timeOffset + static_cast<std::uint64_t>(keyIndex) * sizeof(tickOffset), tickOffset)
					|| tickOffset > segmentDuration
					|| (keyIndex != 0U && tickOffset <= priorOffset)) {
					return AnimationStatus::InvalidChunk;
				}
				priorOffset = tickOffset;
			}
			return AnimationStatus::Ok;
		}

		AnimationQuaternion Normalize(AnimationQuaternion value) noexcept {
			const float lengthSquared = value.x * value.x + value.y * value.y
				+ value.z * value.z + value.w * value.w;
			if(lengthSquared <= std::numeric_limits<float>::epsilon() || !std::isfinite(lengthSquared)) {
				return {};
			}
			const float reciprocalLength = 1.0F / std::sqrt(lengthSquared);
			value.x *= reciprocalLength;
			value.y *= reciprocalLength;
			value.z *= reciprocalLength;
			value.w *= reciprocalLength;
			return value;
		}

		AnimationQuaternion DecodeSmallestThree(const AnimationCodecQuaternionKeyV1& encoded) noexcept {
			float stored[3] = {
				static_cast<float>(encoded.component0) * (kSmallestThreeLimit / 32767.0F),
				static_cast<float>(encoded.component1) * (kSmallestThreeLimit / 32767.0F),
				static_cast<float>(encoded.component2) * (kSmallestThreeLimit / 32767.0F)
			};
			float components[4]{};
			std::size_t storedIndex = 0U;
			float storedLengthSquared = 0.0F;
			for(std::size_t component = 0U; component < 4U; ++component) {
				if(component == encoded.largestComponent) {
					continue;
				}
				components[component] = stored[storedIndex++];
				storedLengthSquared += components[component] * components[component];
			}
			components[encoded.largestComponent] = std::sqrt(std::max(0.0F, 1.0F - storedLengthSquared));
			return Normalize({components[0], components[1], components[2], components[3]});
		}

		AnimationStatus DecodeKey(
			std::span<const std::uint8_t> bytes,
			std::uint64_t localTrackOffset,
			const AnimationCodecTrackHeaderV1& header,
			std::uint32_t keyIndex,
			AnimationDecodedValue& output) noexcept {
			if((header.flags & kAnimationCodecConstantTrack) != 0U) {
				output.vectorValue = header.constantVector;
				output.rotationValue = header.constantRotation;
				output.scalarValue = header.constantScalar;
				return AnimationStatus::Ok;
			}
			const std::uint64_t valuesOffset = localTrackOffset + sizeof(header)
				+ static_cast<std::uint64_t>(header.keyCount) * sizeof(std::uint32_t);
			switch(header.encoding) {
			case AnimationCodecValueEncoding::ScalarUnorm16: {
				std::uint16_t encoded = 0U;
				if(!ReadValue(bytes, valuesOffset + static_cast<std::uint64_t>(keyIndex) * sizeof(encoded), encoded)) {
					return AnimationStatus::TruncatedInput;
				}
				output.scalarValue = header.rangeMinimum.x
					+ header.rangeExtent.x * (static_cast<float>(encoded) / 65535.0F);
				return AnimationStatus::Ok;
			}
			case AnimationCodecValueEncoding::Vector3Unorm16: {
				AnimationCodecVectorKeyV1 encoded{};
				if(!ReadValue(bytes, valuesOffset + static_cast<std::uint64_t>(keyIndex) * sizeof(encoded), encoded)) {
					return AnimationStatus::TruncatedInput;
				}
				output.vectorValue = {
					header.rangeMinimum.x + header.rangeExtent.x * (static_cast<float>(encoded.x) / 65535.0F),
					header.rangeMinimum.y + header.rangeExtent.y * (static_cast<float>(encoded.y) / 65535.0F),
					header.rangeMinimum.z + header.rangeExtent.z * (static_cast<float>(encoded.z) / 65535.0F)
				};
				return AnimationStatus::Ok;
			}
			case AnimationCodecValueEncoding::QuaternionSmallestThree16: {
				AnimationCodecQuaternionKeyV1 encoded{};
				if(!ReadValue(bytes, valuesOffset + static_cast<std::uint64_t>(keyIndex) * sizeof(encoded), encoded)
					|| encoded.largestComponent > 3U) {
					return AnimationStatus::InvalidChunk;
				}
				output.rotationValue = DecodeSmallestThree(encoded);
				return AnimationStatus::Ok;
			}
			}
			return AnimationStatus::InvalidChunk;
		}

		float SmoothFactor(float factor, AnimationInterpolation interpolation) noexcept {
			factor = std::clamp(factor, 0.0F, 1.0F);
			return interpolation == AnimationInterpolation::Cubic
				? factor * factor * (3.0F - 2.0F * factor)
				: factor;
		}
	}

	const AnimationClipSegmentPayload* AnimationClipSegmentPayloadSet::Find(
		std::uint32_t segmentIndex) const noexcept {
		const auto found = std::lower_bound(
			segments.begin(), segments.end(), segmentIndex,
			[](const AnimationClipSegmentPayload& payload, std::uint32_t index) {
				return payload.segmentIndex < index;
			});
		return found != segments.end() && found->segmentIndex == segmentIndex
			? &*found : nullptr;
	}

	AnimationStatus AnimationCodecView::Open(const AnimationClipAsset& asset) noexcept {
		return Open(asset, 0U, {});
	}

	AnimationStatus AnimationCodecView::Open(
		const AnimationClipAsset& asset,
		AnimationGeneration expectedGeneration,
		std::shared_ptr<const AnimationClipSegmentPayloadSet> streamedPayloads) noexcept {
		asset_ = nullptr;
		streamedPayloads_.reset();
		if(const AnimationStatus status = ValidateAnimationClipAsset(asset);
			status != AnimationStatus::Ok) {
			return status;
		}

		if(streamedPayloads) {
			if(expectedGeneration == 0U || streamedPayloads->generation == 0U) {
				return AnimationStatus::InvalidArgument;
			}
			if(streamedPayloads->assetId != asset.assetId) {
				return AnimationStatus::IncompatibleAsset;
			}
			if(streamedPayloads->generation != expectedGeneration
				|| asset.runtimeGeneration != expectedGeneration) {
				return AnimationStatus::StaleHandle;
			}

			std::uint32_t priorSegmentIndex = 0U;
			bool hasPriorSegment = false;
			for(const AnimationClipSegmentPayload& payload : streamedPayloads->segments) {
				if(payload.segmentIndex >= asset.segments.size()
					|| (hasPriorSegment && payload.segmentIndex <= priorSegmentIndex)
					|| !payload.bytes) {
					return AnimationStatus::InvalidChunk;
				}
				const AnimationClipSegment& segment = asset.segments[payload.segmentIndex];
				if(payload.streamOffset != segment.streamOffset
					|| payload.streamSize != segment.streamSize
					|| payload.streamSize != payload.bytes->size()) {
					return AnimationStatus::RangeOutOfBounds;
				}
				if(ComputeSegmentFingerprint(*payload.bytes)
					!= payload.contentFingerprint) {
					return AnimationStatus::ContentHashMismatch;
				}
				priorSegmentIndex = payload.segmentIndex;
				hasPriorSegment = true;
			}
		}

		for(const AnimationClipTrack& track : asset.tracks) {
			std::span<const std::uint8_t> bytes(asset.streamData);
			std::uint64_t baseStreamOffset = 0U;
			if(streamedPayloads) {
				const AnimationClipSegmentPayload* payload =
					streamedPayloads->Find(track.segmentIndex);
				// Missing segments are a normal streaming state. Decode reports NotReady
				// if a caller later asks for a channel owned by that segment.
				if(payload == nullptr) continue;
				bytes = std::span<const std::uint8_t>(*payload->bytes);
				baseStreamOffset = payload->streamOffset;
			}
			AnimationCodecTrackHeaderV1 header{};
			std::uint64_t localTrackOffset = 0U;
			if(const AnimationStatus status = ReadAndValidateHeader(
				asset, track, bytes, baseStreamOffset, localTrackOffset, header);
				status != AnimationStatus::Ok) {
				return status;
			}
		}
		asset_ = &asset;
		streamedPayloads_ = std::move(streamedPayloads);
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationCodecView::Decode(
		std::uint64_t stableTargetId,
		AnimationClipTrackKind kind,
		AnimationFixedTick tick,
		bool loop,
		AnimationDecodedValue& output) const noexcept {
		if(asset_ == nullptr || stableTargetId == 0U || asset_->segments.empty()) {
			return AnimationStatus::NotReady;
		}
		AnimationFixedTick sampleTick = tick;
		if(loop) {
			if(asset_->durationTicks <= 0) {
				return AnimationStatus::InvalidArgument;
			}
			sampleTick %= asset_->durationTicks;
			if(sampleTick < 0) {
				sampleTick += asset_->durationTicks;
			}
		} else {
			sampleTick = std::clamp(sampleTick, AnimationFixedTick{0}, asset_->durationTicks);
		}

		std::uint32_t segmentIndex = static_cast<std::uint32_t>(asset_->segments.size() - 1U);
		for(std::uint32_t index = 0U; index < asset_->segments.size(); ++index) {
			const AnimationClipSegment& segment = asset_->segments[index];
			if(sampleTick >= segment.startTick
				&& (sampleTick < segment.endTick
					|| (sampleTick == asset_->durationTicks && index + 1U == asset_->segments.size()))) {
				segmentIndex = index;
				break;
			}
		}

		const AnimationClipTrack* selectedTrack = nullptr;
		for(const AnimationClipTrack& track : asset_->tracks) {
			if(track.segmentIndex == segmentIndex && track.stableTargetId == stableTargetId
				&& track.kind == kind) {
				selectedTrack = &track;
				break;
			}
		}
		if(selectedTrack == nullptr) {
			return AnimationStatus::MissingAsset;
		}
		std::span<const std::uint8_t> bytes(asset_->streamData);
		std::uint64_t baseStreamOffset = 0U;
		if(streamedPayloads_) {
			const AnimationClipSegmentPayload* payload =
				streamedPayloads_->Find(segmentIndex);
			if(payload == nullptr) return AnimationStatus::NotReady;
			bytes = std::span<const std::uint8_t>(*payload->bytes);
			baseStreamOffset = payload->streamOffset;
		}

		AnimationCodecTrackHeaderV1 header{};
		std::uint64_t localTrackOffset = 0U;
		if(const AnimationStatus status = ReadAndValidateHeader(
			*asset_, *selectedTrack, bytes, baseStreamOffset, localTrackOffset, header);
			status != AnimationStatus::Ok) {
			return status;
		}
		if((header.flags & kAnimationCodecConstantTrack) != 0U) {
			AnimationDecodedValue decoded{};
			if(const AnimationStatus status = DecodeKey(
				bytes, localTrackOffset, header, 0U, decoded);
				status != AnimationStatus::Ok) {
				return status;
			}
			output = decoded;
			return AnimationStatus::Ok;
		}

		const std::uint64_t timeOffset = localTrackOffset + sizeof(header);
		const std::uint32_t localTick = static_cast<std::uint32_t>(sampleTick - header.segmentStartTick);
		std::uint32_t leftIndex = 0U;
		std::uint32_t rightIndex = header.keyCount - 1U;
		std::uint32_t leftTick = 0U;
		std::uint32_t rightTick = 0U;
		for(std::uint32_t index = 0U; index < header.keyCount; ++index) {
			std::uint32_t candidateTick = 0U;
			ReadValue(bytes, timeOffset + static_cast<std::uint64_t>(index) * sizeof(candidateTick), candidateTick);
			if(candidateTick <= localTick) {
				leftIndex = index;
				leftTick = candidateTick;
			}
			if(candidateTick >= localTick) {
				rightIndex = index;
				rightTick = candidateTick;
				break;
			}
		}
		if(rightIndex < leftIndex) {
			rightIndex = leftIndex;
			rightTick = leftTick;
		}

		AnimationDecodedValue left{};
		if(const AnimationStatus status = DecodeKey(
			bytes, localTrackOffset, header, leftIndex, left);
			status != AnimationStatus::Ok) {
			return status;
		}
		if(leftIndex == rightIndex || selectedTrack->interpolation == AnimationInterpolation::Constant
			|| selectedTrack->interpolation == AnimationInterpolation::Step) {
			output = left;
			return AnimationStatus::Ok;
		}
		AnimationDecodedValue right{};
		if(const AnimationStatus status = DecodeKey(
			bytes, localTrackOffset, header, rightIndex, right);
			status != AnimationStatus::Ok) {
			return status;
		}
		const float denominator = static_cast<float>(rightTick - leftTick);
		const float factor = SmoothFactor(
			denominator > 0.0F ? static_cast<float>(localTick - leftTick) / denominator : 0.0F,
			selectedTrack->interpolation);

		AnimationDecodedValue decoded{};
		if(header.encoding == AnimationCodecValueEncoding::Vector3Unorm16) {
			decoded.vectorValue = {
				left.vectorValue.x + (right.vectorValue.x - left.vectorValue.x) * factor,
				left.vectorValue.y + (right.vectorValue.y - left.vectorValue.y) * factor,
				left.vectorValue.z + (right.vectorValue.z - left.vectorValue.z) * factor
			};
		} else if(header.encoding == AnimationCodecValueEncoding::ScalarUnorm16) {
			decoded.scalarValue = left.scalarValue + (right.scalarValue - left.scalarValue) * factor;
		} else {
			AnimationQuaternion rightRotation = right.rotationValue;
			const float dot = left.rotationValue.x * rightRotation.x
				+ left.rotationValue.y * rightRotation.y
				+ left.rotationValue.z * rightRotation.z
				+ left.rotationValue.w * rightRotation.w;
			if(dot < 0.0F) {
				rightRotation.x = -rightRotation.x;
				rightRotation.y = -rightRotation.y;
				rightRotation.z = -rightRotation.z;
				rightRotation.w = -rightRotation.w;
			}
			decoded.rotationValue = Normalize({
				left.rotationValue.x + (rightRotation.x - left.rotationValue.x) * factor,
				left.rotationValue.y + (rightRotation.y - left.rotationValue.y) * factor,
				left.rotationValue.z + (rightRotation.z - left.rotationValue.z) * factor,
				left.rotationValue.w + (rightRotation.w - left.rotationValue.w) * factor
			});
		}
		output = decoded;
		return AnimationStatus::Ok;
	}
}
