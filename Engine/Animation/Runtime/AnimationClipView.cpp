// AnimationClipView.cpp - Checked time resolution, residency, and channel fallback.

#include "Animation/Runtime/AnimationClipView.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		bool IsKnownTrackKind(AnimationClipTrackKind kind) noexcept {
			return kind >= AnimationClipTrackKind::Translation
				&& kind <= AnimationClipTrackKind::RootYaw;
		}

		AnimationStatus ValidateReferenceSkeleton(
			const AnimationClipAsset& clip,
			const SkeletonAsset& skeleton) {
			if(skeleton.assetId != clip.skeletonAssetId
				|| skeleton.compatibilityFingerprint == 0U
				|| skeleton.compatibilityFingerprint != clip.compatibilityFingerprint
				|| ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, skeleton.joints.size())
					!= AnimationStatus::Ok) {
				return AnimationStatus::IncompatibleAsset;
			}

			// Loaded skeleton assets are already validated, but a public runtime view
			// still checks every fallback value before worker jobs can publish it.
			std::unordered_set<std::uint64_t> stableIds;
			stableIds.reserve(skeleton.joints.size());
			for(const SkeletonJoint& joint : skeleton.joints) {
				if(joint.stableId == 0U || !stableIds.insert(joint.stableId).second
					|| !IsFinite(joint.localBindTransform)
					|| !IsNormalized(joint.localBindTransform.rotation)) {
					return AnimationStatus::InvalidArgument;
				}
			}
			return AnimationStatus::Ok;
		}

		bool IsFiniteDecodedValue(
			AnimationClipTrackKind kind,
			const AnimationDecodedValue& value) noexcept {
			switch(kind) {
			case AnimationClipTrackKind::Translation:
			case AnimationClipTrackKind::Scale:
			case AnimationClipTrackKind::RootTranslation:
				return IsFinite(value.vectorValue);
			case AnimationClipTrackKind::Rotation:
				return IsNormalized(value.rotationValue);
			case AnimationClipTrackKind::MorphWeight:
			case AnimationClipTrackKind::Curve:
			case AnimationClipTrackKind::RootYaw:
				return std::isfinite(value.scalarValue);
			}
			return false;
		}
	}

	AnimationStatus ResolveAnimationClipTime(
		AnimationFixedTick durationTicks,
		AnimationFixedTick requestedTick,
		bool loop,
		AnimationResolvedClipTime& output) noexcept {
		if(durationTicks < 0) {
			return AnimationStatus::InvalidArgument;
		}

		AnimationResolvedClipTime staged;
		staged.requestedTick = requestedTick;
		staged.looped = loop;
		if(durationTicks == 0) {
			// A zero-duration authored/preview clip is a stable point sample. It never
			// divides by zero or invents loop progress, even for a huge seek value.
			staged.resolvedTick = 0;
			staged.exactEndBoundary = true;
			staged.clampedToStart = requestedTick < 0;
			staged.clampedToEnd = requestedTick > 0;
			output = staged;
			return AnimationStatus::Ok;
		}

		if(!loop) {
			staged.resolvedTick = std::clamp(requestedTick, AnimationFixedTick{0}, durationTicks);
			staged.clampedToStart = requestedTick < 0;
			staged.clampedToEnd = requestedTick > durationTicks;
			staged.exactEndBoundary = staged.resolvedTick == durationTicks;
			output = staged;
			return AnimationStatus::Ok;
		}

		// C++ integer division truncates toward zero. Correct a negative remainder
		// to obtain mathematical floor division and a non-negative local tick.
		std::int64_t loopIndex = requestedTick / durationTicks;
		AnimationFixedTick resolvedTick = requestedTick % durationTicks;
		if(resolvedTick < 0) {
			resolvedTick += durationTicks;
			--loopIndex;
		}
		staged.resolvedTick = resolvedTick;
		staged.loopIndex = loopIndex;
		staged.exactEndBoundary = requestedTick != 0 && resolvedTick == 0;
		output = staged;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationClipView::Open(
		const AnimationClipAsset& asset,
		const SkeletonAsset* referenceSkeleton,
		std::span<const std::uint8_t> residentSegments,
		AnimationGeneration streamedGeneration,
		std::shared_ptr<const AnimationClipSegmentPayloadSet> streamedPayloads) noexcept {
		if(Matches(asset, referenceSkeleton, residentSegments, streamedGeneration, streamedPayloads)) {
			return AnimationStatus::Ok;
		}
		const bool validatedAssetPairUnchanged = asset_ == &asset
			&& referenceSkeleton_ == referenceSkeleton;
		if((streamedPayloads && residentSegments.size() != asset.segments.size())
			|| (!streamedPayloads && !residentSegments.empty()
				&& residentSegments.size() != asset.segments.size())) {
			return AnimationStatus::InvalidArgument;
		}
		for(const std::uint8_t resident : residentSegments) {
			if(resident > 1U) {
				return AnimationStatus::InvalidArgument;
			}
		}
		if(streamedPayloads) {
			for(std::uint32_t index = 0U; index < residentSegments.size(); ++index) {
				// The bitmap and pinned publication are one atomic capture. Reject a
				// mismatched pair instead of treating stale bytes as newly resident.
				if((residentSegments[index] != 0U)
					!= (streamedPayloads->Find(index) != nullptr)) {
					return AnimationStatus::InvalidArgument;
				}
			}
		}
		if(referenceSkeleton != nullptr && !validatedAssetPairUnchanged) {
			try {
				if(const AnimationStatus status = ValidateReferenceSkeleton(asset, *referenceSkeleton);
					status != AnimationStatus::Ok) {
					return status;
				}
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}
		// Segment streaming never changes the immutable clip or reference skeleton.
		// Retain their completed validation and rebuild only the codec/residency view;
		// crowd instances can then promote together without allocating one joint set
		// per animator on the protected thread.

		AnimationCodecView stagedCodec;
		const AnimationStatus codecStatus = streamedPayloads
			? stagedCodec.Open(asset, streamedGeneration, streamedPayloads)
			: stagedCodec.Open(asset);
		if(codecStatus != AnimationStatus::Ok) {
			return codecStatus;
		}

		try {
			// Reserve before changing any visible view field. Existing animator views
			// normally already have this exact segment capacity, so a crowd-wide stream
			// promotion performs no allocator traffic. A failed growth preserves the
			// complete previous publication.
			residentSegments_.reserve(asset.segments.size());
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}

		// uint8_t assignment cannot throw after the exact capacity was reserved.
		// Commit the bitmap first, then publish its matching immutable identities.
		if(residentSegments.empty()) {
			residentSegments_.assign(asset.segments.size(), 1U);
		} else {
			residentSegments_.assign(residentSegments.begin(), residentSegments.end());
		}
		asset_ = &asset;
		referenceSkeleton_ = referenceSkeleton;
		codec_ = stagedCodec;
		streamedGeneration_ = streamedGeneration;
		streamedPayloads_ = std::move(streamedPayloads);
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationClipView::CopyStreamingStateFrom(
		const AnimationClipView& validated) noexcept
	{
		if(!IsOpen() || !validated.IsOpen()
			|| asset_ != validated.asset_
			|| referenceSkeleton_ != validated.referenceSkeleton_) {
			return AnimationStatus::InvalidArgument;
		}
		if(streamedGeneration_ == validated.streamedGeneration_
			&& streamedPayloads_ == validated.streamedPayloads_
			&& residentSegments_ == validated.residentSegments_) {
			return AnimationStatus::Ok;
		}
		try {
			// reserve provides the strong exception guarantee. Once it succeeds, all
			// following value and shared-ownership copies are non-throwing.
			residentSegments_.reserve(validated.residentSegments_.size());
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		residentSegments_.assign(
			validated.residentSegments_.begin(),
			validated.residentSegments_.end());
		codec_ = validated.codec_;
		streamedGeneration_ = validated.streamedGeneration_;
		streamedPayloads_ = validated.streamedPayloads_;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationClipView::AdoptValidatedPublicationFrom(
		const AnimationClipView& validated) noexcept
	{
		if(!validated.IsOpen()) {
			return AnimationStatus::InvalidArgument;
		}
		try {
			// Prove storage before changing the pointer pair. All assignments after this
			// reserve are non-allocating, so a failed promotion leaves the old view intact.
			residentSegments_.reserve(validated.residentSegments_.size());
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		residentSegments_.assign(
			validated.residentSegments_.begin(),
			validated.residentSegments_.end());
		asset_ = validated.asset_;
		referenceSkeleton_ = validated.referenceSkeleton_;
		codec_ = validated.codec_;
		streamedGeneration_ = validated.streamedGeneration_;
		streamedPayloads_ = validated.streamedPayloads_;
		return AnimationStatus::Ok;
	}

	void AnimationClipView::Close() noexcept {
		asset_ = nullptr;
		referenceSkeleton_ = nullptr;
		codec_ = {};
		residentSegments_.clear();
		streamedGeneration_ = 0U;
		streamedPayloads_.reset();
	}

	bool AnimationClipView::Matches(
		const AnimationClipAsset& asset,
		const SkeletonAsset* referenceSkeleton,
		std::span<const std::uint8_t> residentSegments,
		AnimationGeneration streamedGeneration,
		const std::shared_ptr<const AnimationClipSegmentPayloadSet>& streamedPayloads) const noexcept {
		if(asset_ != &asset || referenceSkeleton_ != referenceSkeleton
			|| streamedGeneration_ != streamedGeneration
			|| streamedPayloads_ != streamedPayloads) {
			return false;
		}
		if(residentSegments.empty()) {
			return !streamedPayloads && residentSegments_.size() == asset.segments.size()
				&& std::all_of(residentSegments_.begin(), residentSegments_.end(),
					[](std::uint8_t value) { return value == 1U; });
		}
		return residentSegments_.size() == residentSegments.size()
			&& std::equal(
				residentSegments_.begin(), residentSegments_.end(), residentSegments.begin());
	}

	AnimationStatus AnimationClipView::ResolveTime(
		AnimationFixedTick requestedTick,
		bool loop,
		AnimationResolvedClipTime& output) const noexcept {
		if(asset_ == nullptr) {
			return AnimationStatus::NotReady;
		}
		return ResolveAnimationClipTime(asset_->durationTicks, requestedTick, loop, output);
	}

	AnimationStatus AnimationClipView::Sample(
		const AnimationClipSampleRequest& request,
		AnimationClipChannelSample& output) const noexcept {
		if(asset_ == nullptr) {
			return AnimationStatus::NotReady;
		}
		AnimationResolvedClipTime time;
		if(const AnimationStatus status = ResolveTime(request.tick, request.loop, time);
			status != AnimationStatus::Ok) {
			return status;
		}
		return SampleResolved(request.stableTargetId, request.kind, time, output);
	}

	AnimationStatus AnimationClipView::SampleResolved(
		std::uint64_t stableTargetId,
		AnimationClipTrackKind kind,
		const AnimationResolvedClipTime& time,
		AnimationClipChannelSample& output) const noexcept {
		if(asset_ == nullptr) {
			return AnimationStatus::NotReady;
		}
		if(stableTargetId == 0U || !IsKnownTrackKind(kind)
			|| time.resolvedTick < 0 || time.resolvedTick > asset_->durationTicks
			|| (time.looped && asset_->durationTicks > 0 && time.resolvedTick == asset_->durationTicks)) {
			return AnimationStatus::InvalidArgument;
		}

		AnimationClipChannelSample staged;
		if(const AnimationStatus fallbackStatus = BuildFallback(stableTargetId, kind, staged);
			fallbackStatus != AnimationStatus::Ok) {
			return fallbackStatus;
		}

		const std::uint32_t segmentIndex = FindSegment(time.resolvedTick);
		staged.segmentIndex = segmentIndex;
		if(segmentIndex == kInvalidAnimationClipSegment) {
			// Zero-duration or intentionally channel-less clips still provide their
			// documented reference/default sample. A timeline hole is unavailable.
			output = staged;
			return asset_->durationTicks == 0 || asset_->segments.empty()
				? AnimationStatus::Ok : AnimationStatus::NotReady;
		}
		if(!IsSegmentResident(segmentIndex)) {
			// Residency matters only when this exact segment owns encoded bytes for
			// the requested channel. An omitted channel is already fully described by
			// its reference/default fallback and must not inherit another track's stall.
			const bool segmentContainsChannel = std::any_of(
				asset_->tracks.begin(), asset_->tracks.end(),
				[segmentIndex, stableTargetId, kind](const AnimationClipTrack& track) {
					return track.segmentIndex == segmentIndex
						&& track.stableTargetId == stableTargetId && track.kind == kind;
				});
			if(!segmentContainsChannel) {
				output = staged;
				return AnimationStatus::Ok;
			}
			output = staged;
			return AnimationStatus::NotReady;
		}

		AnimationDecodedValue decoded;
		const AnimationStatus decodeStatus = codec_.Decode(
			stableTargetId, kind, time.resolvedTick, false, decoded);
		if(decodeStatus == AnimationStatus::MissingAsset) {
			output = staged;
			return AnimationStatus::Ok;
		}
		if(decodeStatus != AnimationStatus::Ok) {
			return decodeStatus;
		}
		if(!IsFiniteDecodedValue(kind, decoded)) {
			return AnimationStatus::InvalidChunk;
		}

		staged.value = decoded;
		staged.source = AnimationChannelSampleSource::Track;
		output = staged;
		return AnimationStatus::Ok;
	}

	bool AnimationClipView::IsSegmentResident(std::uint32_t segmentIndex) const noexcept {
		return segmentIndex < residentSegments_.size() && residentSegments_[segmentIndex] != 0U;
	}

	std::span<const AnimationClipEvent> AnimationClipView::Events() const noexcept {
		return asset_ != nullptr ? std::span<const AnimationClipEvent>(asset_->events)
			: std::span<const AnimationClipEvent>{};
	}

	AnimationStatus AnimationClipView::BuildFallback(
		std::uint64_t stableTargetId,
		AnimationClipTrackKind kind,
		AnimationClipChannelSample& output) const noexcept {
		AnimationClipChannelSample staged;
		if(kind == AnimationClipTrackKind::Scale) {
			staged.value.vectorValue = {1.0F, 1.0F, 1.0F};
		}

		if(referenceSkeleton_ != nullptr
			&& (kind == AnimationClipTrackKind::Translation
				|| kind == AnimationClipTrackKind::Rotation
				|| kind == AnimationClipTrackKind::Scale)) {
			const auto joint = std::find_if(
				referenceSkeleton_->joints.begin(), referenceSkeleton_->joints.end(),
				[stableTargetId](const SkeletonJoint& candidate) {
					return candidate.stableId == stableTargetId;
				});
			if(joint != referenceSkeleton_->joints.end()) {
				if(kind == AnimationClipTrackKind::Translation) {
					staged.value.vectorValue = joint->localBindTransform.translation;
				} else if(kind == AnimationClipTrackKind::Rotation) {
					staged.value.rotationValue = joint->localBindTransform.rotation;
				} else {
					staged.value.vectorValue = joint->localBindTransform.scale;
				}
				staged.source = AnimationChannelSampleSource::ReferencePose;
			}
		}
		output = staged;
		return AnimationStatus::Ok;
	}

	std::uint32_t AnimationClipView::FindSegment(AnimationFixedTick resolvedTick) const noexcept {
		if(asset_ == nullptr || asset_->segments.empty()) {
			return kInvalidAnimationClipSegment;
		}
		const auto upper = std::upper_bound(
			asset_->segments.begin(), asset_->segments.end(), resolvedTick,
			[](AnimationFixedTick tick, const AnimationClipSegment& segment) {
				return tick < segment.startTick;
			});
		if(upper == asset_->segments.begin()) {
			return kInvalidAnimationClipSegment;
		}
		const std::size_t index = static_cast<std::size_t>(std::distance(asset_->segments.begin(), upper) - 1);
		const AnimationClipSegment& segment = asset_->segments[index];
		const bool insideHalfOpenRange = resolvedTick >= segment.startTick && resolvedTick < segment.endTick;
		const bool finalBoundary = resolvedTick == asset_->durationTicks
			&& segment.endTick == asset_->durationTicks;
		const bool zeroDurationPoint = asset_->durationTicks == 0
			&& segment.startTick == 0 && segment.endTick == 0;
		return insideHalfOpenRange || finalBoundary || zeroDurationPoint
			? static_cast<std::uint32_t>(index)
			: kInvalidAnimationClipSegment;
	}
}
