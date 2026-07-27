// CpuClipEvaluator.cpp - Atomic CPU reference evaluation for clip channels.

#include "Animation/Evaluation/CpuClipEvaluator.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		AnimationStatus ValidateStableIds(
			std::span<const std::uint64_t> ids,
			AnimationLimit limit) {
			if(ValidateAnimationHardLimit(limit, ids.size()) != AnimationStatus::Ok) {
				return AnimationStatus::HardLimitExceeded;
			}
			std::unordered_set<std::uint64_t> uniqueIds;
			uniqueIds.reserve(ids.size());
			for(const std::uint64_t id : ids) {
				if(id == 0U || !uniqueIds.insert(id).second) {
					return AnimationStatus::InvalidArgument;
				}
			}
			return AnimationStatus::Ok;
		}

		void AccountSample(
			AnimationStatus status,
			AnimationChannelSampleSource source,
			CpuClipEvaluationResult& result) noexcept {
			if(source != AnimationChannelSampleSource::Track) {
				++result.fallbackChannelCount;
			}
			if(status == AnimationStatus::NotReady) {
				++result.unavailableChannelCount;
			}
		}

		bool IsRecoverableUnavailable(AnimationStatus status) noexcept {
			return status == AnimationStatus::Ok || status == AnimationStatus::NotReady;
		}
	}

	AnimationStatus CpuClipEvaluator::Evaluate(
		const AnimationClipView& view,
		const CpuClipEvaluationRequest& request,
		CpuClipEvaluationResult& output) const noexcept {
		if(!view.IsOpen()) {
			return AnimationStatus::NotReady;
		}

		try {
			if(const AnimationStatus status = ValidateStableIds(
				request.jointStableIds, AnimationLimit::SkeletonJoints);
				status != AnimationStatus::Ok) {
				return status;
			}
			if(const AnimationStatus status = ValidateStableIds(
				request.morphStableIds, AnimationLimit::MeshMorphTargets);
				status != AnimationStatus::Ok) {
				return status;
			}
			if(const AnimationStatus status = ValidateStableIds(
				request.curveStableIds, AnimationLimit::ClipChannels);
				status != AnimationStatus::Ok) {
				return status;
			}

			CpuClipEvaluationResult staged;
			if(const AnimationStatus status = view.ResolveTime(request.tick, request.loop, staged.time);
				status != AnimationStatus::Ok) {
				return status;
			}
			staged.joints.reserve(request.jointStableIds.size());
			staged.morphs.reserve(request.morphStableIds.size());
			staged.curves.reserve(request.curveStableIds.size());

			AnimationStatus aggregateStatus = AnimationStatus::Ok;
			for(const std::uint64_t jointId : request.jointStableIds) {
				CpuClipJointSample joint;
				const AnimationStatus status = SampleJoint(view, jointId, staged.time, joint);
				if(!IsRecoverableUnavailable(status)) {
					return status;
				}
				for(const AnimationChannelSampleSource source : joint.sources) {
					AccountSample(status, source, staged);
				}
				if(status == AnimationStatus::NotReady) {
					aggregateStatus = AnimationStatus::NotReady;
				}
				staged.joints.push_back(joint);
			}

			for(const std::uint64_t morphId : request.morphStableIds) {
				CpuClipNamedFloatSample morph;
				const AnimationStatus status = SampleNamedFloat(
					view, morphId, AnimationClipTrackKind::MorphWeight, staged.time, morph);
				if(!IsRecoverableUnavailable(status)) {
					return status;
				}
				AccountSample(status, morph.source, staged);
				if(status == AnimationStatus::NotReady) {
					aggregateStatus = AnimationStatus::NotReady;
				}
				staged.morphs.push_back(morph);
			}

			for(const std::uint64_t curveId : request.curveStableIds) {
				CpuClipNamedFloatSample curve;
				const AnimationStatus status = SampleNamedFloat(
					view, curveId, AnimationClipTrackKind::Curve, staged.time, curve);
				if(!IsRecoverableUnavailable(status)) {
					return status;
				}
				AccountSample(status, curve.source, staged);
				if(status == AnimationStatus::NotReady) {
					aggregateStatus = AnimationStatus::NotReady;
				}
				staged.curves.push_back(curve);
			}

			if(request.includeRootMotionPoint) {
				const AnimationStatus status = SampleRootMotionPoint(view, staged.time, staged.rootMotion);
				if(!IsRecoverableUnavailable(status)) {
					return status;
				}
				if(staged.rootMotion.enabled) {
					AccountSample(status, staged.rootMotion.translationSource, staged);
					AccountSample(status, staged.rootMotion.yawSource, staged);
				}
				if(status == AnimationStatus::NotReady) {
					aggregateStatus = AnimationStatus::NotReady;
				}
			}
			if(request.includeEventsAtPoint) {
				if(const AnimationStatus status = CollectEventsAtPoint(view, staged.time, staged.pointEvents);
					status != AnimationStatus::Ok) {
					return status;
				}
			}

			output = std::move(staged);
			return aggregateStatus;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus CpuClipEvaluator::SampleJoint(
		const AnimationClipView& view,
		std::uint64_t stableJointId,
		const AnimationResolvedClipTime& time,
		CpuClipJointSample& output) const noexcept {
		if(stableJointId == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		CpuClipJointSample staged;
		staged.stableJointId = stableJointId;

		AnimationStatus aggregateStatus = AnimationStatus::Ok;
		AnimationClipChannelSample translation;
		const AnimationStatus translationStatus = view.SampleResolved(
			stableJointId, AnimationClipTrackKind::Translation, time, translation);
		if(!IsRecoverableUnavailable(translationStatus)) {
			return translationStatus;
		}
		staged.localTransform.translation = translation.value.vectorValue;
		staged.sources[0U] = translation.source;
		if(translationStatus == AnimationStatus::NotReady) aggregateStatus = translationStatus;

		AnimationClipChannelSample rotation;
		const AnimationStatus rotationStatus = view.SampleResolved(
			stableJointId, AnimationClipTrackKind::Rotation, time, rotation);
		if(!IsRecoverableUnavailable(rotationStatus)) {
			return rotationStatus;
		}
		staged.localTransform.rotation = rotation.value.rotationValue;
		staged.sources[1U] = rotation.source;
		if(rotationStatus == AnimationStatus::NotReady) aggregateStatus = rotationStatus;

		AnimationClipChannelSample scale;
		const AnimationStatus scaleStatus = view.SampleResolved(
			stableJointId, AnimationClipTrackKind::Scale, time, scale);
		if(!IsRecoverableUnavailable(scaleStatus)) {
			return scaleStatus;
		}
		staged.localTransform.scale = scale.value.vectorValue;
		staged.sources[2U] = scale.source;
		if(scaleStatus == AnimationStatus::NotReady) aggregateStatus = scaleStatus;

		if(!IsFinite(staged.localTransform) || !IsNormalized(staged.localTransform.rotation)) {
			return AnimationStatus::InvalidChunk;
		}
		output = staged;
		return aggregateStatus;
	}

	AnimationStatus CpuClipEvaluator::SampleNamedFloat(
		const AnimationClipView& view,
		std::uint64_t stableId,
		AnimationClipTrackKind kind,
		const AnimationResolvedClipTime& time,
		CpuClipNamedFloatSample& output) const noexcept {
		if(stableId == 0U || (kind != AnimationClipTrackKind::MorphWeight
			&& kind != AnimationClipTrackKind::Curve)) {
			return AnimationStatus::InvalidArgument;
		}
		AnimationClipChannelSample channel;
		const AnimationStatus status = view.SampleResolved(stableId, kind, time, channel);
		if(!IsRecoverableUnavailable(status)) {
			return status;
		}
		if(!std::isfinite(channel.value.scalarValue)) {
			return AnimationStatus::InvalidChunk;
		}
		output = {stableId, channel.value.scalarValue, channel.source};
		return status;
	}

	AnimationStatus CpuClipEvaluator::SampleRootMotionPoint(
		const AnimationClipView& view,
		const AnimationResolvedClipTime& time,
		CpuClipRootMotionPoint& output) const noexcept {
		const AnimationClipAsset* asset = view.Asset();
		if(asset == nullptr) {
			return AnimationStatus::NotReady;
		}
		CpuClipRootMotionPoint staged;
		if(!asset->rootMotion.enabled) {
			output = staged;
			return AnimationStatus::Ok;
		}
		if(asset->rootMotion.translationTrackIndex >= asset->tracks.size()
			|| asset->rootMotion.yawTrackIndex >= asset->tracks.size()) {
			return AnimationStatus::InvalidChunk;
		}
		const AnimationClipTrack& translationTrack = asset->tracks[asset->rootMotion.translationTrackIndex];
		const AnimationClipTrack& yawTrack = asset->tracks[asset->rootMotion.yawTrackIndex];
		if(translationTrack.kind != AnimationClipTrackKind::RootTranslation
			|| yawTrack.kind != AnimationClipTrackKind::RootYaw) {
			return AnimationStatus::InvalidChunk;
		}

		AnimationClipChannelSample translation;
		const AnimationStatus translationStatus = view.SampleResolved(
			translationTrack.stableTargetId, AnimationClipTrackKind::RootTranslation, time, translation);
		if(!IsRecoverableUnavailable(translationStatus)) {
			return translationStatus;
		}
		AnimationClipChannelSample yaw;
		const AnimationStatus yawStatus = view.SampleResolved(
			yawTrack.stableTargetId, AnimationClipTrackKind::RootYaw, time, yaw);
		if(!IsRecoverableUnavailable(yawStatus)) {
			return yawStatus;
		}

		staged.enabled = true;
		staged.translation = translation.value.vectorValue;
		staged.yawRadians = yaw.value.scalarValue;
		staged.translationSource = translation.source;
		staged.yawSource = yaw.source;
		if(!IsFinite(staged.translation) || !std::isfinite(staged.yawRadians)) {
			return AnimationStatus::InvalidChunk;
		}
		output = staged;
		return translationStatus == AnimationStatus::NotReady || yawStatus == AnimationStatus::NotReady
			? AnimationStatus::NotReady : AnimationStatus::Ok;
	}

	AnimationStatus CpuClipEvaluator::CollectEventsAtPoint(
		const AnimationClipView& view,
		const AnimationResolvedClipTime& time,
		std::vector<CpuClipPointEvent>& output) const noexcept {
		if(!view.IsOpen()) {
			return AnimationStatus::NotReady;
		}
		std::vector<CpuClipPointEvent> staged;
		try {
			for(const AnimationClipEvent& event : view.Events()) {
				if(event.tick == time.resolvedTick) {
					staged.push_back({&event, time.loopIndex});
				}
			}
			std::sort(staged.begin(), staged.end(), [](const CpuClipPointEvent& left, const CpuClipPointEvent& right) {
				return std::tie(left.event->tick, left.event->stableEventId)
					< std::tie(right.event->tick, right.event->stableEventId);
			});
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		output = std::move(staged);
		return AnimationStatus::Ok;
	}
}
