// ProceduralCommandStore.h - Bounded, copied procedural gameplay commands.
//
// Gameplay and managed callbacks submit ABI records from arbitrary threads. This
// store owns copied records until the animation protected boundary consumes
// them, so no producer can mutate a graph or evaluator-owned result directly.

#pragma once

#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Scripting/AnimationScriptAbi.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	// One range points into a frame-owned flat goal array. Capture keeps ranges in
	// animator-handle order, allowing AnimationSystem to merge them with its already
	// ordered live-instance snapshot without a mutex acquisition per animator.
	struct ActiveProceduralGoalRange {
		NativeAnimationAnimatorHandle animator{};
		std::uint32_t firstGoal = 0U;
		std::uint32_t goalCount = 0U;
	};

	// A store instance belongs to the animation runtime rather than a global
	// singleton. This makes scene/runtime shutdown explicit and prevents old
	// managed commands from leaking into a newly created animation system.
	class TERRAENGINE_API ProceduralCommandStore {
	public:
		explicit ProceduralCommandStore(
			std::size_t commandCapacity = 4096U,
			std::size_t receiptCapacity = 4096U,
			std::size_t retainedResultCapacity = 36000U);

		// Submit copies one ABI command after checking all untrusted values. A
		// successful receipt is Queued only; live goals change later in
		// PublishCompletion at the protected publication boundary.
		[[nodiscard]] NativeAnimationProceduralGoalReceipt Submit(
			const NativeAnimationProceduralGoalCommand& command,
			const AnimatorInstanceStore& instances);

		// Cancellation succeeds only while a command remains in the private
		// pending queue. The returned receipt carries the cancellation call's
		// correlation, while retained receipt lookup keeps submission correlation.
		[[nodiscard]] NativeAnimationProceduralGoalReceipt Cancel(
			std::uint64_t cancellationToken,
			std::uint64_t correlationId);
		[[nodiscard]] NativeAnimationProceduralGoalReceipt ReadReceipt(
			std::uint32_t producerId,
			std::uint64_t producerSequence,
			std::uint64_t correlationId) const;

		// Drain runs only from the animation capture boundary. It rechecks the
		// animator generation before a command becomes evaluator-owned and returns
		// a deterministic batch. Call PublishCompletion for every drained command.
		void DrainThrough(
			AnimationFixedTick tick,
			std::vector<NativeAnimationProceduralGoalCommand>& output,
			const AnimatorInstanceStore& instances);

		// Completion is the sole mutation path for active goals. A successful set
		// replaces its stable goal id or appends a new goal up to the hard cap;
		// clear removes exactly that stable goal. Rejection leaves active state intact.
		void PublishCompletion(
			const NativeAnimationProceduralGoalCommand& command,
			AnimationStatus status,
			AnimationFixedTick observableTick,
			std::uint64_t publicationGeneration);

		// Copies active goals after their protected mutation boundary. The caller
		// receives an all-or-nothing span, avoiding aliases to live store memory.
		[[nodiscard]] AnimationStatus CopyActiveGoals(
			NativeAnimationAnimatorHandle animator,
			std::span<NativeAnimationProceduralGoalCommand> destination,
			std::uint32_t& requiredCount) const;
		// Captures all active procedural state under one lock. The caller supplies
		// reusable flat storage so ordinary frames with no procedural animators do
		// not perform one protected lookup for every member of a large crowd.
		[[nodiscard]] AnimationStatus CaptureActiveGoals(
			std::vector<ActiveProceduralGoalRange>& ranges,
			std::vector<NativeAnimationProceduralGoalCommand>& goals) const;

		// Replaces one animator's retained result batch atomically after validating
		// every copied snapshot. Failed validation preserves the prior publication.
		[[nodiscard]] AnimationStatus PublishResults(
			NativeAnimationAnimatorHandle animator,
			std::span<const NativeAnimationProceduralResultSnapshot> results,
			const AnimatorInstanceStore& instances);

		// This follows the normal ABI count/capacity protocol: requiredCount is
		// always written, destination is untouched when capacity is insufficient,
		// and a successful call writes one complete coherent publication.
		[[nodiscard]] AnimationStatus CopyResults(
			NativeAnimationAnimatorHandle animator,
			std::span<NativeAnimationProceduralResultSnapshot> destination,
			std::uint32_t& requiredCount) const;

		// Retirement is explicit at lifecycle boundaries and also happens lazily
		// during DrainThrough. It removes pending, active, receipt, and result data
		// for precisely one generation-safe animator handle.
		void RetireAnimator(NativeAnimationAnimatorHandle animator);
		void Clear();
		[[nodiscard]] std::size_t PendingCount() const;

	private:
		struct ReceiptRecord {
			NativeAnimationProceduralGoalCommand command{};
			NativeAnimationProceduralGoalReceipt receipt{};
		};

		struct AnimatorState {
			NativeAnimationAnimatorHandle animator{};
			std::vector<NativeAnimationProceduralGoalCommand> activeGoals;
			std::vector<NativeAnimationProceduralResultSnapshot> results;
		};

		[[nodiscard]] AnimatorState* FindStateLocked(NativeAnimationAnimatorHandle animator);
		[[nodiscard]] const AnimatorState* FindStateLocked(NativeAnimationAnimatorHandle animator) const;
		[[nodiscard]] AnimatorState* FindOrCreateStateLocked(NativeAnimationAnimatorHandle animator);
		void ReclaimTerminalReceiptLocked();

		std::size_t commandCapacity_;
		std::size_t receiptCapacity_;
		std::size_t retainedResultCapacity_;
		mutable std::mutex mutex_;
		std::vector<NativeAnimationProceduralGoalCommand> pending_;
		std::vector<ReceiptRecord> receipts_;
		std::vector<AnimatorState> states_;
	};
}
