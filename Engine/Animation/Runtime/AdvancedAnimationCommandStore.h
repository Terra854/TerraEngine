// AdvancedAnimationCommandStore.h - Bounded deferred advanced gameplay controls.
//
// Retargeting, motion matching, ragdoll, and deterministic history use fixed ABI
// records that cannot borrow evaluator or physics storage. This runtime-owned
// store copies requests from arbitrary scripting threads, drains them at the
// animation capture boundary, and publishes copied observations only after the
// protected authority boundary commits.

#pragma once

#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Scripting/AnimationScriptAbi.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	class TERRAENGINE_API AdvancedAnimationCommandStore final {
	public:
		explicit AdvancedAnimationCommandStore(
			std::size_t pendingCapacity = 4096U,
			std::size_t retainedPerAnimatorCapacity = 256U,
			std::size_t animatorStateCapacity = 4096U);

		[[nodiscard]] NativeAnimationRetargetReceipt Submit(
			const NativeAnimationRetargetRequest& request,
			const AnimatorInstanceStore& instances);
		[[nodiscard]] NativeAnimationMotionMatchReceipt Submit(
			const NativeAnimationMotionMatchRequest& request,
			const AnimatorInstanceStore& instances);
		[[nodiscard]] NativeAnimationMotionMatchQueryReceiptV2 Submit(
			const NativeAnimationMotionMatchQueryRequestV2& request,
			const AnimatorInstanceStore& instances);
		[[nodiscard]] NativeAnimationRagdollReceipt Submit(
			const NativeAnimationRagdollRequest& request,
			const AnimatorInstanceStore& instances);
		[[nodiscard]] NativeAnimationDeterminismReceipt Submit(
			const NativeAnimationDeterminismRequest& request,
			const AnimatorInstanceStore& instances);

		// Capture owns deterministic ordering. Future-tick requests remain queued;
		// due requests are moved into frame-owned vectors without exposing pending
		// storage to worker evaluation.
		void DrainThrough(
			AnimationFixedTick tick,
			std::vector<NativeAnimationRetargetRequest>& retarget,
			std::vector<NativeAnimationMotionMatchRequest>& motionMatch,
			std::vector<NativeAnimationMotionMatchQueryRequestV2>& motionMatchV2,
			std::vector<NativeAnimationRagdollRequest>& ragdoll,
			std::vector<NativeAnimationDeterminismRequest>& determinism);

		// Publication methods append one complete copied observation. Rejected work
		// may still publish a diagnostic snapshot, while a failed allocation leaves
		// the previous complete history unchanged.
		[[nodiscard]] AnimationStatus Publish(NativeAnimationRetargetSnapshot snapshot);
		[[nodiscard]] AnimationStatus Publish(NativeAnimationMotionMatchSnapshot snapshot);
		[[nodiscard]] AnimationStatus Publish(NativeAnimationMotionMatchResultV2 snapshot);
		[[nodiscard]] AnimationStatus Publish(NativeAnimationRagdollSnapshot snapshot);
		[[nodiscard]] AnimationStatus Publish(NativeAnimationDeterminismSnapshot snapshot);

		[[nodiscard]] AnimationStatus Copy(
			NativeAnimationAnimatorHandle animator,
			std::span<NativeAnimationRetargetSnapshot> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimationStatus Copy(
			NativeAnimationAnimatorHandle animator,
			std::span<NativeAnimationMotionMatchSnapshot> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimationStatus Copy(
			NativeAnimationAnimatorHandle animator,
			std::span<NativeAnimationMotionMatchResultV2> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimationStatus Copy(
			NativeAnimationAnimatorHandle animator,
			std::span<NativeAnimationRagdollSnapshot> destination,
			std::uint32_t& requiredCount) const;
		[[nodiscard]] AnimationStatus Copy(
			NativeAnimationAnimatorHandle animator,
			std::span<NativeAnimationDeterminismSnapshot> destination,
			std::uint32_t& requiredCount) const;

		void RetireAnimator(NativeAnimationAnimatorHandle animator) noexcept;
		void Clear() noexcept;
		[[nodiscard]] std::size_t PendingCount() const noexcept;

	private:
		struct AnimatorState {
			NativeAnimationAnimatorHandle animator{};
			std::vector<NativeAnimationRetargetSnapshot> retarget;
			std::vector<NativeAnimationMotionMatchSnapshot> motionMatch;
			std::vector<NativeAnimationMotionMatchResultV2> motionMatchV2;
			std::vector<NativeAnimationRagdollSnapshot> ragdoll;
			std::vector<NativeAnimationDeterminismSnapshot> determinism;
		};

		[[nodiscard]] AnimatorState* FindOrCreateStateLocked(
			NativeAnimationAnimatorHandle animator);
		[[nodiscard]] const AnimatorState* FindStateLocked(
			NativeAnimationAnimatorHandle animator) const noexcept;

		std::size_t pendingCapacity_ = 0U;
		std::size_t retainedPerAnimatorCapacity_ = 0U;
		std::size_t animatorStateCapacity_ = 0U;
		mutable std::mutex mutex_;
		std::vector<NativeAnimationRetargetRequest> pendingRetarget_;
		std::vector<NativeAnimationMotionMatchRequest> pendingMotionMatch_;
		std::vector<NativeAnimationMotionMatchQueryRequestV2> pendingMotionMatchV2_;
		std::vector<NativeAnimationRagdollRequest> pendingRagdoll_;
		std::vector<NativeAnimationDeterminismRequest> pendingDeterminism_;
		std::vector<AnimatorState> states_;
	};
}
