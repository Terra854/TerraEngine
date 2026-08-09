// AnimationAuthorityTestHelpers.h - Test-only immutable authority-root access.

#pragma once

#include "Animation/AnimationSystem.h"

#include <memory>

namespace TerraEngine::Tests {
	// Tests pin the complete root while inspecting a row. This deliberately models
	// production consumers: a row pointer never outlives its immutable authority
	// frame and tests cannot reopen mutable AnimationSystem runtime records.
	class PinnedAnimationAuthorityRow final {
	public:
		[[nodiscard]] static PinnedAnimationAuthorityRow ForAnimator(
			Animation::AnimationSystem& system,
			Animation::AnimatorInstanceHandle animator) noexcept
		{
			PinnedAnimationAuthorityRow result;
			result.frame_ = system.LoadPublishedAuthorityFrame();
			result.row_ = result.frame_ ? result.frame_->Find(animator) : nullptr;
			return result;
		}

		[[nodiscard]] static PinnedAnimationAuthorityRow ForObject(
			Animation::AnimationSystem& system,
			const UUID& ownerObjectId) noexcept
		{
			return ForAnimator(system, system.ResolveAnimatorForObject(ownerObjectId));
		}

		[[nodiscard]] explicit operator bool() const noexcept { return row_ != nullptr; }
		[[nodiscard]] const Animation::AnimationAuthorityRow* operator->() const noexcept
		{
			return row_;
		}
		[[nodiscard]] const Animation::AnimationAuthorityRow& operator*() const noexcept
		{
			return *row_;
		}
		[[nodiscard]] bool operator==(const PinnedAnimationAuthorityRow& other) const noexcept
		{
			return frame_ == other.frame_ && row_ == other.row_;
		}
		[[nodiscard]] bool operator==(std::nullptr_t) const noexcept
		{
			return row_ == nullptr;
		}

	private:
		std::shared_ptr<const Animation::AnimationAuthorityFrame> frame_;
		const Animation::AnimationAuthorityRow* row_ = nullptr;
	};
}
