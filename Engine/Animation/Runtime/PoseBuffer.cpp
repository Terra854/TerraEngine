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
			auto visualPayload = std::make_shared<PoseBufferVisualPayload>();
			visualPayload->localTransforms.resize(layout.jointCount);
			visualPayload->modelTransforms.resize(layout.jointCount);
			visualPayload->morphValues.resize(layout.morphCount);
			visualPayload->curveValues.resize(layout.curveCount);
			std::vector<AnimationEventRecord> events;
			events.reserve(layout.eventCapacity);
			std::vector<std::byte> scratch(layout.scratchBytes, std::byte{0});

			layout_ = layout;
			generation_ = generation;
			visualPayload_ = std::move(visualPayload);
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
		return sealed_ || !visualPayload_ ? std::span<AnimationTransform>{}
			: std::span<AnimationTransform>{visualPayload_->localTransforms};
	}

	std::span<AnimationTransform> PoseBuffer::MutableModelTransforms() noexcept {
		return sealed_ || !visualPayload_ ? std::span<AnimationTransform>{}
			: std::span<AnimationTransform>{visualPayload_->modelTransforms};
	}

	std::span<AnimationNamedFloatValue> PoseBuffer::MutableMorphValues() noexcept {
		return sealed_ || !visualPayload_ ? std::span<AnimationNamedFloatValue>{}
			: std::span<AnimationNamedFloatValue>{visualPayload_->morphValues};
	}

	std::span<AnimationNamedFloatValue> PoseBuffer::MutableCurveValues() noexcept {
		return sealed_ || !visualPayload_ ? std::span<AnimationNamedFloatValue>{}
			: std::span<AnimationNamedFloatValue>{visualPayload_->curveValues};
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
		if(!visualPayload_) return AnimationStatus::InvalidArgument;
		std::sort(visualPayload_->morphValues.begin(), visualPayload_->morphValues.end(), [](const auto& left, const auto& right) {
			return left.stableId < right.stableId;
		});
		std::sort(visualPayload_->curveValues.begin(), visualPayload_->curveValues.end(), [](const auto& left, const auto& right) {
			return left.stableId < right.stableId;
		});

		for(const AnimationTransform& transform : visualPayload_->localTransforms) {
			if(!IsFinite(transform) || !IsNormalized(transform.rotation)) return AnimationStatus::InvalidArgument;
		}
		for(const AnimationTransform& transform : visualPayload_->modelTransforms) {
			if(!IsFinite(transform) || !IsNormalized(transform.rotation)) return AnimationStatus::InvalidArgument;
		}
		if(!std::all_of(visualPayload_->morphValues.begin(), visualPayload_->morphValues.end(), IsFiniteNamedValue)
			|| !std::all_of(visualPayload_->curveValues.begin(), visualPayload_->curveValues.end(), IsFiniteNamedValue)
			|| HasDuplicateStableIds<AnimationNamedFloatValue>(visualPayload_->morphValues)
			|| HasDuplicateStableIds<AnimationNamedFloatValue>(visualPayload_->curveValues)) {
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

	AnimationStatus PoseBuffer::ShareSealedVisualPayloadFrom(
		const PoseBuffer& source,
		AnimationPoseGeneration generation) noexcept
	{
		// The sharing caller already applies the broader animator-state proof. These
		// local checks defend PoseBuffer when it is used directly by a future system.
		if(!source.sealed_ || !source.visualPayload_ || !source.events_.empty()
			|| source.rootMotion_.has_value() || !generation.animator.IsValid()
			|| generation.poseGeneration == 0U
			|| source.ValidateForPublication() != AnimationStatus::Ok) {
			return AnimationStatus::InvalidArgument;
		}
		layout_ = source.layout_;
		layout_.eventCapacity = 0U;
		layout_.scratchBytes = 0U;
		generation_ = generation;
		visualPayload_ = source.visualPayload_;
		events_.clear();
		scratch_.clear();
		rootMotion_.reset();
		sealed_ = true;
		return ValidateForPublication();
	}

	std::span<const AnimationTransform> PoseBuffer::LocalTransforms() const noexcept {
		return visualPayload_ ? std::span<const AnimationTransform>{visualPayload_->localTransforms}
			: std::span<const AnimationTransform>{};
	}

	std::span<const AnimationTransform> PoseBuffer::ModelTransforms() const noexcept {
		return visualPayload_ ? std::span<const AnimationTransform>{visualPayload_->modelTransforms}
			: std::span<const AnimationTransform>{};
	}

	std::span<const AnimationNamedFloatValue> PoseBuffer::MorphValues() const noexcept {
		return visualPayload_ ? std::span<const AnimationNamedFloatValue>{visualPayload_->morphValues}
			: std::span<const AnimationNamedFloatValue>{};
	}

	std::span<const AnimationNamedFloatValue> PoseBuffer::CurveValues() const noexcept {
		return visualPayload_ ? std::span<const AnimationNamedFloatValue>{visualPayload_->curveValues}
			: std::span<const AnimationNamedFloatValue>{};
	}

	AnimationStatus PoseBuffer::ValidateForPublication() const noexcept {
		if(!sealed_) return AnimationStatus::NotReady;
		if(!generation_.animator.IsValid() || generation_.poseGeneration == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(!visualPayload_ || visualPayload_->localTransforms.size() != layout_.jointCount
			|| visualPayload_->modelTransforms.size() != layout_.jointCount
			|| visualPayload_->morphValues.size() != layout_.morphCount
			|| visualPayload_->curveValues.size() != layout_.curveCount
			|| events_.size() > layout_.eventCapacity
			|| scratch_.size() != layout_.scratchBytes) {
			return AnimationStatus::InvalidArgument;
		}
		return AnimationStatus::Ok;
	}
}
