// PoseBuffer.cpp - Checked staging and sealing for animation output storage.

#include "Animation/Runtime/PoseBuffer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		bool IsFiniteNamedValue(const AnimationNamedFloatValue& value) noexcept {
			return value.stableId != 0U && std::isfinite(value.value);
		}

		bool IsFiniteRootMotion(const AnimationRootMotionDelta& value) noexcept {
			return value.animator.IsValid()
				&& value.generation != 0U
				&& value.sourceBeginTick != value.sourceEndTick
				&& IsFinite(value.localTranslation)
				&& IsFinite(value.worldTranslation)
				&& std::isfinite(value.localYawRadians)
				&& std::isfinite(value.worldYawRadians);
		}

		bool IsValidEvent(const AnimationEventRecord& event) noexcept {
			return event.identity.animator.IsValid()
				&& event.identity.stateInstanceId != 0U
				&& event.identity.clipAssetId.IsValid()
				&& event.identity.eventId != 0U
				&& event.direction != AnimationPlaybackDirection::Stationary;
		}

		template<typename Value>
		bool HasDuplicateStableIds(std::span<const Value> values) noexcept {
			for(std::size_t index = 1U; index < values.size(); ++index) {
				if(values[index - 1U].stableId == values[index].stableId) return true;
			}
			return false;
		}
	}

	AnimationStatus PoseBuffer::BeginWrite(PoseBufferLayout layout, AnimationPoseGeneration generation) {
		if(!generation.animator.IsValid() || generation.poseGeneration == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, layout.jointCount) != AnimationStatus::Ok
			|| ValidateAnimationHardLimit(AnimationLimit::MeshMorphTargets, layout.morphCount) != AnimationStatus::Ok
			|| ValidateAnimationHardLimit(AnimationLimit::ClipChannels, layout.curveCount) != AnimationStatus::Ok
			|| ValidateAnimationHardLimit(AnimationLimit::AnimatorEventsPerUpdate, layout.eventCapacity) != AnimationStatus::Ok
			|| layout.scratchBytes > kAnimationPoseScratchHardMaximumBytes) {
			return AnimationStatus::HardLimitExceeded;
		}

		// Allocate and initialize a complete replacement before touching this
		// buffer. An allocation failure therefore preserves the last sealed output.
		try {
			std::vector<AnimationTransform> local(layout.jointCount);
			std::vector<AnimationTransform> model(layout.jointCount);
			std::vector<AnimationNamedFloatValue> morph(layout.morphCount);
			std::vector<AnimationNamedFloatValue> curve(layout.curveCount);
			std::vector<AnimationEventRecord> events;
			events.reserve(layout.eventCapacity);
			std::vector<std::byte> scratch(layout.scratchBytes, std::byte{0});

			layout_ = layout;
			generation_ = generation;
			localTransforms_ = std::move(local);
			modelTransforms_ = std::move(model);
			morphValues_ = std::move(morph);
			curveValues_ = std::move(curve);
			events_ = std::move(events);
			scratch_ = std::move(scratch);
			rootMotion_.reset();
			sealed_ = false;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		return AnimationStatus::Ok;
	}

	std::span<AnimationTransform> PoseBuffer::MutableLocalTransforms() noexcept {
		return sealed_ ? std::span<AnimationTransform>{} : std::span<AnimationTransform>{localTransforms_};
	}

	std::span<AnimationTransform> PoseBuffer::MutableModelTransforms() noexcept {
		return sealed_ ? std::span<AnimationTransform>{} : std::span<AnimationTransform>{modelTransforms_};
	}

	std::span<AnimationNamedFloatValue> PoseBuffer::MutableMorphValues() noexcept {
		return sealed_ ? std::span<AnimationNamedFloatValue>{} : std::span<AnimationNamedFloatValue>{morphValues_};
	}

	std::span<AnimationNamedFloatValue> PoseBuffer::MutableCurveValues() noexcept {
		return sealed_ ? std::span<AnimationNamedFloatValue>{} : std::span<AnimationNamedFloatValue>{curveValues_};
	}

	std::span<std::byte> PoseBuffer::MutableScratch() noexcept {
		return sealed_ ? std::span<std::byte>{} : std::span<std::byte>{scratch_};
	}

	AnimationStatus PoseBuffer::AppendEvent(AnimationEventRecord event) {
		if(sealed_) return AnimationStatus::Rejected;
		if(!IsValidEvent(event)) return AnimationStatus::InvalidArgument;
		if(events_.size() >= layout_.eventCapacity) return AnimationStatus::BufferTooSmall;
		try {
			events_.push_back(std::move(event));
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		return AnimationStatus::Ok;
	}

	AnimationStatus PoseBuffer::SetRootMotion(AnimationRootMotionDelta rootMotion) {
		if(sealed_) return AnimationStatus::Rejected;
		if(!IsFiniteRootMotion(rootMotion) || rootMotion.animator != generation_.animator
			|| rootMotion.generation != generation_.poseGeneration) {
			return AnimationStatus::InvalidArgument;
		}
		rootMotion_ = rootMotion;
		return AnimationStatus::Ok;
	}

	AnimationStatus PoseBuffer::Seal() noexcept {
		if(sealed_) return AnimationStatus::Ok;

		// Stable IDs make named lookup deterministic. Sort before checking for
		// duplicates so input packet order cannot alter the accepted result.
		std::sort(morphValues_.begin(), morphValues_.end(), [](const auto& left, const auto& right) {
			return left.stableId < right.stableId;
		});
		std::sort(curveValues_.begin(), curveValues_.end(), [](const auto& left, const auto& right) {
			return left.stableId < right.stableId;
		});

		for(const AnimationTransform& transform : localTransforms_) {
			if(!IsFinite(transform) || !IsNormalized(transform.rotation)) return AnimationStatus::InvalidArgument;
		}
		for(const AnimationTransform& transform : modelTransforms_) {
			if(!IsFinite(transform) || !IsNormalized(transform.rotation)) return AnimationStatus::InvalidArgument;
		}
		if(!std::all_of(morphValues_.begin(), morphValues_.end(), IsFiniteNamedValue)
			|| !std::all_of(curveValues_.begin(), curveValues_.end(), IsFiniteNamedValue)
			|| HasDuplicateStableIds<AnimationNamedFloatValue>(morphValues_)
			|| HasDuplicateStableIds<AnimationNamedFloatValue>(curveValues_)) {
			return AnimationStatus::InvalidArgument;
		}
		for(const AnimationEventRecord& event : events_) {
			if(!IsValidEvent(event) || event.identity.animator != generation_.animator) {
				return AnimationStatus::InvalidArgument;
			}
		}
		if(rootMotion_.has_value() && !IsFiniteRootMotion(*rootMotion_)) {
			return AnimationStatus::InvalidArgument;
		}

		sealed_ = true;
		return AnimationStatus::Ok;
	}

	AnimationStatus PoseBuffer::ValidateForPublication() const noexcept {
		if(!sealed_) return AnimationStatus::NotReady;
		if(!generation_.animator.IsValid() || generation_.poseGeneration == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(localTransforms_.size() != layout_.jointCount
			|| modelTransforms_.size() != layout_.jointCount
			|| morphValues_.size() != layout_.morphCount
			|| curveValues_.size() != layout_.curveCount
			|| events_.size() > layout_.eventCapacity
			|| scratch_.size() != layout_.scratchBytes) {
			return AnimationStatus::InvalidArgument;
		}
		return AnimationStatus::Ok;
	}
}
