// AnimationSnapshots.cpp - Atomic validation and immutable snapshot publication.

#include "Animation/Runtime/AnimationSnapshots.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		bool IsFiniteParameter(const AnimationParameterSnapshot& parameter) noexcept {
			if(parameter.parameterId == 0U) return false;
			switch(parameter.type) {
			case AnimationParameterType::Trigger:
				return std::holds_alternative<std::monostate>(parameter.value);
			case AnimationParameterType::Boolean:
				return std::holds_alternative<bool>(parameter.value);
			case AnimationParameterType::Integer:
				return std::holds_alternative<std::int32_t>(parameter.value);
			case AnimationParameterType::Float:
				return std::holds_alternative<float>(parameter.value)
					&& std::isfinite(std::get<float>(parameter.value));
			default:
				return false;
			}
		}

		AnimationStatus ValidateSnapshot(const AnimationSnapshot& snapshot) noexcept {
			if(!snapshot.animator.IsValid() || snapshot.publicationGeneration == 0U || !snapshot.pose) {
				return AnimationStatus::InvalidArgument;
			}
			if(snapshot.pose->ValidateForPublication() != AnimationStatus::Ok) return AnimationStatus::NotReady;
			const AnimationPoseGeneration& poseGeneration = snapshot.pose->Generation();
			if(poseGeneration.animator != snapshot.animator
				|| poseGeneration.poseGeneration != snapshot.publicationGeneration
				|| poseGeneration.tick != snapshot.tick) {
				return AnimationStatus::StaleHandle;
			}
			if(!std::isfinite(snapshot.playback.transitionProgress)
				|| snapshot.playback.transitionProgress < 0.0F || snapshot.playback.transitionProgress > 1.0F
				|| !std::isfinite(snapshot.playback.normalizedTime)
				|| snapshot.playback.normalizedTime < 0.0F || snapshot.playback.normalizedTime > 1.0F
				|| !std::isfinite(snapshot.playback.effectiveSpeed)) {
				return AnimationStatus::InvalidArgument;
			}
			for(const AnimationLayerSnapshot& layer : snapshot.playback.activeLayers) {
				if(!std::isfinite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F) {
					return AnimationStatus::InvalidArgument;
				}
			}
			for(const AnimationParameterSnapshot& parameter : snapshot.playback.parameters) {
				if(!IsFiniteParameter(parameter)) return AnimationStatus::InvalidArgument;
			}
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus AnimationSnapshotPublisher::Publish(
		AnimationSnapshot snapshot,
		const AnimatorInstanceStore& store) {
		if(!store.IsAlive(snapshot.animator)) return AnimationStatus::StaleHandle;
		if(const AnimationStatus status = ValidateSnapshot(snapshot); status != AnimationStatus::Ok) return status;

		// Canonical ordering makes copied queries stable even when worker packets
		// completed in a different order.
		std::sort(snapshot.playback.activeLayers.begin(), snapshot.playback.activeLayers.end(),
			[](const AnimationLayerSnapshot& left, const AnimationLayerSnapshot& right) {
				return left.layerIndex < right.layerIndex;
			});
		std::sort(snapshot.playback.parameters.begin(), snapshot.playback.parameters.end(),
			[](const AnimationParameterSnapshot& left, const AnimationParameterSnapshot& right) {
				return left.parameterId < right.parameterId;
			});
		for(std::size_t index = 1U; index < snapshot.playback.activeLayers.size(); ++index) {
			if(snapshot.playback.activeLayers[index - 1U].layerIndex
				== snapshot.playback.activeLayers[index].layerIndex) return AnimationStatus::InvalidArgument;
		}
		for(std::size_t index = 1U; index < snapshot.playback.parameters.size(); ++index) {
			if(snapshot.playback.parameters[index - 1U].parameterId
				== snapshot.playback.parameters[index].parameterId) return AnimationStatus::InvalidArgument;
		}

		std::shared_ptr<const AnimationSnapshot> candidate;
		try {
			candidate = std::make_shared<const AnimationSnapshot>(std::move(snapshot));
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}

		std::shared_ptr<const AnimationSnapshot> current = latest_.load(std::memory_order_acquire);
		while(true) {
			if(current && candidate->publicationGeneration <= current->publicationGeneration) {
				return AnimationStatus::StaleHandle;
			}
			if(!store.IsAlive(candidate->animator)) return AnimationStatus::StaleHandle;
			if(latest_.compare_exchange_weak(
				current, candidate, std::memory_order_release, std::memory_order_acquire)) {
				return AnimationStatus::Ok;
			}
		}
	}

	std::shared_ptr<const AnimationSnapshot> AnimationSnapshotPublisher::Load() const noexcept {
		return latest_.load(std::memory_order_acquire);
	}

	void AnimationSnapshotPublisher::Clear() noexcept {
		latest_.store({}, std::memory_order_release);
	}
}
