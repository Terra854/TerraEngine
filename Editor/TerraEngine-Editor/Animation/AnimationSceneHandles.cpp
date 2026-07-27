// AnimationSceneHandles.cpp - atomic validation and detached handle edits.

#include "AnimationSceneHandles.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace TerraEngine::Editor::Animation {
	bool AnimationSceneHandles::IsFinite(const AnimationSceneHandle& handle) noexcept {
		return std::isfinite(handle.x) && std::isfinite(handle.y) && std::isfinite(handle.z)
			&& std::isfinite(handle.endX) && std::isfinite(handle.endY) && std::isfinite(handle.endZ)
			&& std::isfinite(handle.rotationX) && std::isfinite(handle.rotationY)
			&& std::isfinite(handle.rotationZ) && std::isfinite(handle.rotationW)
			&& std::isfinite(handle.weight) && handle.weight >= 0.0F && handle.weight <= 1.0F
			&& handle.label.size() <= 256U;
	}

	bool AnimationSceneHandles::Set(std::vector<AnimationSceneHandle> handles) noexcept {
		try {
			if(handles.size() > kMaximumAnimationSceneHandles
				|| std::any_of(handles.begin(), handles.end(),
					[](const AnimationSceneHandle& handle) { return !IsFinite(handle); })) {
				return false;
			}

			// Stable ordering makes overlapping pick circles deterministic across
			// different worker collection orders and keeps test captures reproducible.
			std::stable_sort(handles.begin(), handles.end(),
				[](const AnimationSceneHandle& left, const AnimationSceneHandle& right) {
					if(left.kind != right.kind) return left.kind < right.kind;
					return left.ownerId < right.ownerId;
				});
			handles_ = std::move(handles);

			if(selection_ && Selected() == nullptr) {
				selection_.reset();
				edit_.reset();
			}
			return true;
		} catch(...) {
			return false;
		}
	}

	bool AnimationSceneHandles::Select(AnimationHandleKind kind, std::uint64_t ownerId) noexcept {
		selection_ = std::pair{kind, ownerId};
		if(Selected() == nullptr) {
			selection_.reset();
			return false;
		}
		edit_.reset();
		return true;
	}

	void AnimationSceneHandles::ClearSelection() noexcept {
		selection_.reset();
		edit_.reset();
	}

	const AnimationSceneHandle* AnimationSceneHandles::Selected() const noexcept {
		if(!selection_) return nullptr;
		const auto found = std::find_if(handles_.begin(), handles_.end(),
			[&](const AnimationSceneHandle& handle) {
				return handle.kind == selection_->first && handle.ownerId == selection_->second;
			});
		return found == handles_.end() ? nullptr : &*found;
	}

	bool AnimationSceneHandles::BeginEdit(std::uint64_t expectedSourceGeneration) noexcept {
		const AnimationSceneHandle* selected = Selected();
		if(selected == nullptr || !selected->editable || expectedSourceGeneration == 0U
			|| selected->sourceGeneration != expectedSourceGeneration) {
			return false;
		}
		edit_ = AnimationSceneHandleEdit{*selected, *selected, expectedSourceGeneration};
		return true;
	}

	bool AnimationSceneHandles::UpdateEditPosition(float x, float y, float z) noexcept {
		if(!edit_) return false;
		AnimationSceneHandle candidate = edit_->candidate;
		candidate.x = x;
		candidate.y = y;
		candidate.z = z;
		if(!IsFinite(candidate)) return false;
		edit_->candidate = std::move(candidate);
		return true;
	}

	bool AnimationSceneHandles::UpdateEditRotation(float x, float y, float z, float w) noexcept {
		if(!edit_) return false;
		AnimationSceneHandle candidate = edit_->candidate;
		candidate.rotationX = x;
		candidate.rotationY = y;
		candidate.rotationZ = z;
		candidate.rotationW = w;
		const float lengthSquared = x * x + y * y + z * z + w * w;
		if(!IsFinite(candidate) || std::abs(lengthSquared - 1.0F) > 0.01F) return false;
		edit_->candidate = std::move(candidate);
		return true;
	}

	bool AnimationSceneHandles::CommitEdit(
		std::uint64_t currentSourceGeneration,
		AnimationSceneHandle& output) noexcept {
		if(!edit_ || currentSourceGeneration != edit_->expectedSourceGeneration) {
			return false;
		}
		output = edit_->candidate;
		edit_.reset();
		return true;
	}
}
