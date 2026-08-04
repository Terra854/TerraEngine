// AnimationEventStream.cpp - Exactly-once clip-event traversal and retention.

#include "Animation/Runtime/AnimationEventStream.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		AnimationFixedTick FloorDivide(AnimationFixedTick numerator, AnimationFixedTick denominator) noexcept {
			AnimationFixedTick quotient = numerator / denominator;
			if(numerator % denominator < 0) --quotient;
			return quotient;
		}

		bool TryMakeOccurrence(
			std::int64_t loop,
			AnimationFixedTick duration,
			AnimationFixedTick eventTick,
			AnimationFixedTick& output) noexcept {
			constexpr AnimationFixedTick kMinimum = std::numeric_limits<AnimationFixedTick>::min();
			constexpr AnimationFixedTick kMaximum = std::numeric_limits<AnimationFixedTick>::max();
			if(loop > 0 && loop > (kMaximum - eventTick) / duration) return false;
			if(loop < 0 && loop < kMinimum / duration) return false;
			const AnimationFixedTick product = loop * duration;
			if(eventTick > 0 && product > kMaximum - eventTick) return false;
			output = product + eventTick;
			return true;
		}

		void SaturatingIncrement(std::uint64_t& value) noexcept {
			if(value != std::numeric_limits<std::uint64_t>::max()) ++value;
		}

		bool IsKnownEventPolicy(AnimatorEventPolicy policy) noexcept {
			return policy == AnimatorEventPolicy::Dispatch
				|| policy == AnimatorEventPolicy::Retain
				|| policy == AnimatorEventPolicy::Suppress;
		}

		bool IsKnownHistoricalPolicy(AnimationHistoricalEventPolicy policy) noexcept {
			return policy == AnimationHistoricalEventPolicy::Suppress
				|| policy == AnimationHistoricalEventPolicy::DeliverUndelivered
				|| policy == AnimationHistoricalEventPolicy::Reemit;
		}
	}

	std::size_t AnimationEventStream::EventIdentityHash::operator()(
		const AnimationEventIdentity& identity) const noexcept {
		// FNV-1a over every identity field keeps the set lookup independent from
		// pointer values and process-specific random seeds.
		std::uint64_t hash = 14695981039346656037ULL;
		auto append = [&hash](const void* data, std::size_t size) {
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for(std::size_t index = 0U; index < size; ++index) {
				hash ^= bytes[index];
				hash *= 1099511628211ULL;
			}
		};
		append(&identity.animator.slotIndex, sizeof(identity.animator.slotIndex));
		append(&identity.animator.generation, sizeof(identity.animator.generation));
		append(&identity.stateInstanceId, sizeof(identity.stateInstanceId));
		append(identity.clipAssetId.value.data(), identity.clipAssetId.value.size());
		append(&identity.eventId, sizeof(identity.eventId));
		append(&identity.sourceTick, sizeof(identity.sourceTick));
		append(&identity.loopCount, sizeof(identity.loopCount));
		return static_cast<std::size_t>(hash);
	}

	AnimationStatus AnimationEventStream::Configure(
		AnimationAssetHandle clip,
		AnimationFixedTick durationTicks,
		std::span<const AnimationClipEvent> events,
		AnimationEventStreamConfig config) {
		if(!clip.IsValid() || durationTicks < 0 || config.maxEventsPerUpdate == 0U
			|| config.retainedEventCapacity == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(ValidateAnimationHardLimit(AnimationLimit::ClipAuthoredEvents, events.size()) != AnimationStatus::Ok
			|| ValidateAnimationHardLimit(AnimationLimit::AnimatorEventsPerUpdate, config.maxEventsPerUpdate) != AnimationStatus::Ok
			|| ValidateAnimationHardLimit(AnimationLimit::AnimatorRetainedEvents, config.retainedEventCapacity) != AnimationStatus::Ok) {
			return AnimationStatus::HardLimitExceeded;
		}

		try {
			auto authored = std::make_shared<ImmutableAuthoredEvents>();
			authored->forward.reserve(events.size());
			authored->reverse.reserve(events.size());
			for(std::size_t index = 0U; index < events.size(); ++index) {
				const AnimationClipEvent& event = events[index];
				if(event.stableEventId == 0U || event.tick < 0 || event.tick > durationTicks
					|| (!event.fireForward && !event.fireReverse)) {
					return AnimationStatus::InvalidArgument;
				}
				const OrderedEvent ordered{event, static_cast<std::uint64_t>(index)};
				if(event.fireForward) authored->forward.push_back(ordered);
				if(event.fireReverse) authored->reverse.push_back(ordered);
			}
			auto eventLess = [](const OrderedEvent& left, const OrderedEvent& right) noexcept {
				if(left.authored.tick != right.authored.tick) return left.authored.tick < right.authored.tick;
				if(left.authored.stableEventId != right.authored.stableEventId) {
					return left.authored.stableEventId < right.authored.stableEventId;
				}
				return left.authoredOrder < right.authoredOrder;
			};
			std::sort(authored->forward.begin(), authored->forward.end(), eventLess);
			std::sort(authored->reverse.begin(), authored->reverse.end(), eventLess);

			// Publication is atomic: only a fully validated, sorted configuration
			// replaces the previous stream and its delivery watermark.
			clip_ = clip;
			durationTicks_ = durationTicks;
			config_ = config;
			authoredEvents_ = std::move(authored);
			ResetDeliveryHistory();
			diagnostics_ = {};
			configured_ = true;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		return AnimationStatus::Ok;
	}

	bool AnimationEventStream::WasDelivered(const AnimationEventIdentity& identity) const {
		return retainedEventSet_.contains(identity);
	}

	void AnimationEventStream::RememberDelivered(
		const AnimationEventIdentity& identity,
		AnimationEventTraversalResult& output) {
		if(retainedEventSet_.contains(identity)) return;
		if(retainedEvents_.size() == config_.retainedEventCapacity) {
			retainedEventSet_.erase(retainedEvents_.front());
			retainedEvents_.pop_front();
			SaturatingIncrement(output.retainedEventEvictions);
			SaturatingIncrement(diagnostics_.retainedEventEvictions);
		}
		retainedEvents_.push_back(identity);
		retainedEventSet_.insert(identity);
	}

	AnimationStatus AnimationEventStream::Traverse(
		const AnimationEventTraversalRequest& request,
		AnimationEventTraversalResult& output) {
		output = {};
		if(!configured_) {
			output.status = AnimationStatus::NotReady;
			return output.status;
		}
		if(!request.animator.IsValid() || request.stateInstanceId == 0U
			|| !IsKnownEventPolicy(request.eventPolicy)
			|| !IsKnownHistoricalPolicy(request.historicalPolicy)) {
			output.status = AnimationStatus::InvalidArgument;
			return output.status;
		}
		SaturatingIncrement(diagnostics_.traversals);
		// A zero-duration clip is a valid stable point sample. It has no open time
		// interval to traverse, so skip floor division and never manufacture a loop.
		if(durationTicks_ == 0) return AnimationStatus::Ok;
		if(request.previousAbsoluteTick == request.currentAbsoluteTick
			|| request.eventPolicy == AnimatorEventPolicy::Suppress
			|| request.historicalPolicy == AnimationHistoricalEventPolicy::Suppress) {
			return AnimationStatus::Ok;
		}

		const bool forward = request.currentAbsoluteTick > request.previousAbsoluteTick;
		if(!authoredEvents_) {
			output.status = AnimationStatus::NotReady;
			return output.status;
		}
		const std::vector<OrderedEvent>& authoredEvents = forward
			? authoredEvents_->forward : authoredEvents_->reverse;
		if(authoredEvents.empty()) return AnimationStatus::Ok;
		try {
			output.events.reserve(config_.maxEventsPerUpdate);
		} catch(const std::bad_alloc&) {
			output.status = AnimationStatus::InternalError;
			return output.status;
		}

		std::int64_t loop = FloorDivide(request.previousAbsoluteTick, durationTicks_);
		const std::int64_t finalLoop = FloorDivide(request.currentAbsoluteTick, durationTicks_);
		bool stop = false;
		auto visit = [&](const OrderedEvent& ordered, std::int64_t eventLoop) {
			AnimationFixedTick occurrence = 0;
			if(!TryMakeOccurrence(eventLoop, durationTicks_, ordered.authored.tick, occurrence)) return;
			const bool crossed = forward
				? occurrence > request.previousAbsoluteTick && occurrence <= request.currentAbsoluteTick
				: occurrence < request.previousAbsoluteTick && occurrence >= request.currentAbsoluteTick;
			if(!crossed) return;

			AnimationEventRecord record;
			record.identity = {
				request.animator,
				request.stateInstanceId,
				clip_.assetId,
				ordered.authored.stableEventId,
				ordered.authored.tick,
				eventLoop
			};
			const bool allowDuplicate = request.historicalPolicy == AnimationHistoricalEventPolicy::Reemit;
			if(!allowDuplicate && WasDelivered(record.identity)) {
				SaturatingIncrement(diagnostics_.duplicateEventsSuppressed);
				return;
			}
			if(output.events.size() == config_.maxEventsPerUpdate) {
				output.overflowed = true;
				output.droppedEventLowerBound = 1U;
				output.status = AnimationStatus::BufferTooSmall;
				SaturatingIncrement(diagnostics_.overflowUpdates);
				stop = true;
				return;
			}

			record.payloadType = ordered.authored.payloadType;
			record.payload = ordered.authored.payload;
			record.sourceStateId = request.sourceStateId;
			record.layerIndex = request.layerIndex;
			record.direction = forward ? AnimationPlaybackDirection::Forward : AnimationPlaybackDirection::Reverse;
			record.correlationId = request.correlationId;
			record.producerOrder = request.producerOrderBase + static_cast<std::uint64_t>(output.events.size());
			record.historical = allowDuplicate;
			RememberDelivered(record.identity, output);
			output.events.push_back(std::move(record));
			SaturatingIncrement(diagnostics_.emittedEvents);
		};

		// Each complete loop has at least one direction-eligible event. Therefore
		// the event output bound also bounds loop traversal, even for a huge catch-up
		// interval; no work scales with an arbitrary number of crossed empty ticks.
		while(!stop) {
			if(forward) {
				for(const OrderedEvent& event : authoredEvents) {
					visit(event, loop);
					if(stop) break;
				}
				if(loop >= finalLoop || loop == std::numeric_limits<std::int64_t>::max()) break;
				++loop;
			} else {
				// Reverse time visits tick groups in descending order but keeps stable
				// event-ID order inside a shared timestamp.
				std::size_t groupEnd = authoredEvents.size();
				while(groupEnd > 0U && !stop) {
					std::size_t groupBegin = groupEnd - 1U;
					while(groupBegin > 0U
						&& authoredEvents[groupBegin - 1U].authored.tick == authoredEvents[groupEnd - 1U].authored.tick) {
						--groupBegin;
					}
					for(std::size_t index = groupBegin; index < groupEnd; ++index) {
						visit(authoredEvents[index], loop);
						if(stop) break;
					}
					groupEnd = groupBegin;
				}
				if(loop <= finalLoop || loop == std::numeric_limits<std::int64_t>::min()) break;
				--loop;
			}
		}
		return output.status;
	}

	void AnimationEventStream::ResetDeliveryHistory() noexcept {
		retainedEvents_.clear();
		retainedEventSet_.clear();
	}

	void AnimationEventStream::CopyRetainedEvents(std::vector<AnimationEventIdentity>& output) const {
		output.assign(retainedEvents_.begin(), retainedEvents_.end());
	}

	AnimationStatus AnimationEventStream::RestoreDeliveryHistory(
		std::span<const AnimationEventIdentity> deliveredEvents) {
		if(!configured_) return AnimationStatus::NotReady;
		if(deliveredEvents.size() > config_.retainedEventCapacity) {
			return AnimationStatus::HardLimitExceeded;
		}
		try {
			std::deque<AnimationEventIdentity> candidateEvents;
			std::unordered_set<AnimationEventIdentity, EventIdentityHash> candidateSet;
			candidateEvents.insert(
				candidateEvents.end(), deliveredEvents.begin(), deliveredEvents.end());
			candidateSet.reserve(deliveredEvents.size());
			for(const AnimationEventIdentity& identity : deliveredEvents) {
				if(!identity.animator.IsValid() || identity.stateInstanceId == 0U
					|| identity.eventId == 0U || identity.clipAssetId != clip_.assetId
					|| !candidateSet.insert(identity).second) {
					return AnimationStatus::InvalidArgument;
				}
			}
			retainedEvents_ = std::move(candidateEvents);
			retainedEventSet_ = std::move(candidateSet);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}
}
