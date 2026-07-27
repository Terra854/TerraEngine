// AnimationEventStream.h - Deterministic bounded traversal of authored clip events.

#pragma once

#include "Animation/Runtime/PoseBuffer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <unordered_set>
#include <vector>

namespace TerraEngine::Animation {
	// Seek, preview, rollback, and late-join callers select one explicit policy.
	// Ordinary playback uses DeliverUndelivered.
	enum class AnimationHistoricalEventPolicy : std::uint8_t {
		Suppress = 0U,
		DeliverUndelivered = 1U,
		Reemit = 2U
	};

	struct AnimationEventStreamConfig {
		std::size_t maxEventsPerUpdate = static_cast<std::size_t>(
			GetAnimationRuntimeLimit(AnimationLimit::AnimatorEventsPerUpdate).operationalDefault);
		std::size_t retainedEventCapacity = static_cast<std::size_t>(
			GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).operationalDefault);
	};

	struct AnimationEventTraversalRequest {
		AnimatorInstanceHandle animator;
		std::uint64_t stateInstanceId = 0U;
		std::uint64_t sourceStateId = 0U;
		std::uint32_t layerIndex = 0U;
		AnimationFixedTick previousAbsoluteTick = 0;
		AnimationFixedTick currentAbsoluteTick = 0;
		std::uint64_t correlationId = 0U;
		std::uint64_t producerOrderBase = 0U;
		AnimatorEventPolicy eventPolicy = AnimatorEventPolicy::Dispatch;
		AnimationHistoricalEventPolicy historicalPolicy = AnimationHistoricalEventPolicy::DeliverUndelivered;
	};

	struct AnimationEventTraversalResult {
		AnimationStatus status = AnimationStatus::Ok;
		std::vector<AnimationEventRecord> events;
		bool overflowed = false;
		// Traversal stops after proving the first dropped event. This is therefore
		// an explicit lower bound rather than an expensive unbounded recount.
		std::uint64_t droppedEventLowerBound = 0U;
		std::uint64_t retainedEventEvictions = 0U;
	};

	struct AnimationEventStreamDiagnostics {
		std::uint64_t traversals = 0U;
		std::uint64_t emittedEvents = 0U;
		std::uint64_t duplicateEventsSuppressed = 0U;
		std::uint64_t overflowUpdates = 0U;
		std::uint64_t retainedEventEvictions = 0U;
	};

	class TERRAENGINE_API AnimationEventStream {
	public:
		AnimationStatus Configure(
			AnimationAssetHandle clip,
			AnimationFixedTick durationTicks,
			std::span<const AnimationClipEvent> events,
			AnimationEventStreamConfig config = {});
		AnimationStatus Traverse(
			const AnimationEventTraversalRequest& request,
			AnimationEventTraversalResult& output);
		void ResetDeliveryHistory() noexcept;
		void CopyRetainedEvents(std::vector<AnimationEventIdentity>& output) const;
		// Rollback stages a complete copied delivery set, validates it against the
		// configured clip and capacity, then swaps it atomically. A rejected restore
		// therefore cannot lose the last-good one-shot event watermark.
		AnimationStatus RestoreDeliveryHistory(
			std::span<const AnimationEventIdentity> deliveredEvents);

		[[nodiscard]] const AnimationAssetHandle& Clip() const noexcept { return clip_; }
		[[nodiscard]] AnimationFixedTick DurationTicks() const noexcept { return durationTicks_; }
		[[nodiscard]] const AnimationEventStreamDiagnostics& Diagnostics() const noexcept { return diagnostics_; }

	private:
		struct OrderedEvent {
			AnimationClipEvent authored;
			std::uint64_t authoredOrder = 0U;
		};

		struct EventIdentityHash {
			std::size_t operator()(const AnimationEventIdentity& identity) const noexcept;
		};

		bool WasDelivered(const AnimationEventIdentity& identity) const;
		void RememberDelivered(const AnimationEventIdentity& identity, AnimationEventTraversalResult& output);

		AnimationAssetHandle clip_{};
		AnimationFixedTick durationTicks_ = 0;
		AnimationEventStreamConfig config_{};
		std::vector<OrderedEvent> forwardEvents_;
		std::vector<OrderedEvent> reverseEvents_;
		std::deque<AnimationEventIdentity> retainedEvents_;
		std::unordered_set<AnimationEventIdentity, EventIdentityHash> retainedEventSet_;
		AnimationEventStreamDiagnostics diagnostics_{};
		bool configured_ = false;
	};
}
