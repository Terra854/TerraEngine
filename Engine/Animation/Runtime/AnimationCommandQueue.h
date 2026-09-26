// AnimationCommandQueue.h - Bounded typed commands and copied authoritative outputs.

#pragma once

#include "Animation/Assets/AnimationAssetTypes.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

namespace TerraEngine::Animation {
	enum class AnimationParameterType : std::uint8_t {
		Trigger = 0U,
		Boolean = 1U,
		Integer = 2U,
		Float = 3U,
		Vector3 = 4U,
		Quaternion = 5U,
		Transform = 6U
	};
	using AnimationParameterValue = std::variant<
		std::monostate,
		bool,
		std::int32_t,
		float,
		AnimationFloat3,
		AnimationQuaternion,
		AnimationTransform>;
	// Playback controls use explicit typed payload fields below. Appending values
	// preserves the numeric identity of the foundational command kinds.
	enum class AnimationCommandKind : std::uint8_t {
		SetParameter = 0U,
		FireTrigger = 1U,
		Play = 2U,
		Stop = 3U,
		Pause = 4U,
		Resume = 5U,
		Seek = 6U,
		SetLooping = 7U,
		SetReverse = 8U,
		SetSpeed = 9U,
		CrossFade = 10U,
		SetLayerWeight = 11U
	};

	struct AnimationCommand {
		AnimatorInstanceHandle target;
		// A protected producer may pin the exact authored controller generation.
		// Invalid preserves the pre-existing unqualified command behavior.
		AnimationAssetHandle expectedController;
		AnimationCommandKind kind = AnimationCommandKind::SetParameter;
		AnimationParameterType parameterType = AnimationParameterType::Float;
		std::uint32_t parameterId = 0U;
		AnimationParameterValue value;
		// These copied fields avoid smuggling playback control through the
		// parameter variant. Only the field selected by kind is interpreted.
		AnimationFixedTick playbackTick = 0;
		float playbackSpeed = 1.0F;
		bool playbackFlag = false;
		// Direct controller commands retain their state/layer payloads in the typed
		// queue. They are interpreted only on the protected animation frame and are
		// never smuggled through a generic parameter value.
		std::uint64_t stateId = 0U;
		AnimationFixedTick transitionTicks = 0;
		std::uint32_t layerIndex = 0U;
		float layerWeight = 1.0F;
		AnimationFixedTick targetTick = 0;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		std::uint64_t correlationId = 0U;
		// Non-zero identities let scripting callers retry and cancel without
		// retaining a pointer to native queue storage. Native callers may leave
		// either value zero when they do not need that behavior.
		std::uint64_t idempotencyKey = 0U;
		std::uint64_t cancellationToken = 0U;
	};

	// Receipts copy every ordering/target identity needed to reconcile an async
	// result without retaining a pointer to the queued command or live instance.
	struct AnimationCommandReceipt {
		AnimatorInstanceHandle target;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		std::uint64_t correlationId = 0U;
		std::uint64_t idempotencyKey = 0U;
		std::uint64_t cancellationToken = 0U;
		AnimationStatus status = AnimationStatus::InvalidArgument;
		AnimationFixedTick targetTick = 0;
		AnimationFixedTick observableTick = 0;
		std::uint64_t publicationGeneration = 0U;
		bool duplicate = false;
	};
	// A batch receipt is all-or-nothing. A non-Queued status guarantees that no
	// command from the submitted span entered pending or retained receipt storage.
	struct AnimationCommandBatchReceipt {
		AnimationStatus status = AnimationStatus::InvalidArgument;
		std::vector<AnimationCommandReceipt> commands;
	};
	struct AnimationAuthoritativeOutput { AnimatorInstanceHandle source; AnimationFixedTick tick = 0; std::uint64_t sequence = 0U; };
	struct AnimationEventPacket { AnimatorInstanceHandle source; AnimationFixedTick tick = 0; std::uint64_t sequence = 0U; std::uint32_t eventId = 0U; };
	struct AnimationRootMotionPacket { AnimatorInstanceHandle source; AnimationFixedTick tick = 0; std::uint64_t sequence = 0U; float x = 0.0F; float y = 0.0F; float z = 0.0F; };

	// Authoritative output queues use copied packets and the same deterministic
	// identity ordering as commands. They deliberately never expose store memory.
	template<typename Packet>
	class AnimationOutputQueue {
	public:
		explicit AnimationOutputQueue(std::size_t capacity = 4096U) : capacity_(capacity) {}

		AnimationStatus Enqueue(Packet packet, const AnimatorInstanceStore& store) {
			if(!packet.source.IsValid() || packet.sequence == 0U) return AnimationStatus::InvalidArgument;
			if(!store.IsAlive(packet.source)) return AnimationStatus::StaleHandle;
			std::scoped_lock lock(mutex_);
			if(std::any_of(pending_.begin(), pending_.end(), [&packet](const Packet& existing) {
				return existing.source == packet.source && existing.tick == packet.tick && existing.sequence == packet.sequence;
			})) return AnimationStatus::Rejected;
			if(pending_.size() >= capacity_) return AnimationStatus::Rejected;
			pending_.push_back(std::move(packet));
			return AnimationStatus::Queued;
		}

		void DrainThrough(AnimationFixedTick tick, std::vector<Packet>& output, const AnimatorInstanceStore& store) {
			output.clear();
			std::scoped_lock lock(mutex_);
			std::vector<Packet> future;
			future.reserve(pending_.size());
			for(Packet& packet : pending_) {
				// Revalidate once before either delivery or retention. A stale future
				// packet must not survive until a reused slot reaches its target tick.
				if(!store.IsAlive(packet.source)) continue;
				if(packet.tick <= tick) output.push_back(std::move(packet));
				else future.push_back(std::move(packet));
			}
			pending_ = std::move(future);
			std::sort(output.begin(), output.end(), [](const Packet& left, const Packet& right) {
				if(left.tick != right.tick) return left.tick < right.tick;
				if(left.source.slotIndex != right.source.slotIndex) return left.source.slotIndex < right.source.slotIndex;
				return left.sequence < right.sequence;
			});
		}

	private:
		std::size_t capacity_;
		std::mutex mutex_;
		std::vector<Packet> pending_;
	};

	using AnimationAuthoritativeOutputQueue = AnimationOutputQueue<AnimationAuthoritativeOutput>;
	using AnimationEventQueue = AnimationOutputQueue<AnimationEventPacket>;
	using AnimationRootMotionQueue = AnimationOutputQueue<AnimationRootMotionPacket>;

	class TERRAENGINE_API AnimationCommandQueue {
	public:
		explicit AnimationCommandQueue(std::size_t capacity = 4096U)
			: capacity_(capacity), receiptCapacity_(std::max(capacity, kMinimumReceiptCapacity))
		{
		}
		AnimationCommandReceipt Enqueue(AnimationCommand command, const AnimatorInstanceStore& store) noexcept;
		[[nodiscard]] AnimationCommandBatchReceipt EnqueueBatch(
			std::span<const AnimationCommand> commands,
			const AnimatorInstanceStore& store);
		// Tests inject a reservation failure before either queue vector changes.
		// This makes the atomic failure contract reproducible without exhausting
		// machine memory or replacing the process-wide allocator.
		void FailNextEnqueueReservationForTesting() noexcept;
		// Cancellation only removes a still-pending request. A completed request is
		// returned unchanged so callers can distinguish "too late" from success.
		AnimationCommandReceipt Cancel(std::uint64_t cancellationToken, std::uint64_t correlationId);
		AnimationCommandReceipt ReadReceipt(
			std::uint32_t producerId,
			std::uint64_t producerSequence,
			std::uint64_t correlationId) const;
		void PublishCompletion(
			const AnimationCommand& command,
			AnimationStatus status,
			AnimationFixedTick observableTick,
			std::uint64_t publicationGeneration);
		void DrainThrough(AnimationFixedTick tick, std::vector<AnimationCommand>& output, const AnimatorInstanceStore& store);
		std::size_t PendingCount() const;

	private:
		// Pending commands are removed at protected capture, but their receipts must
		// remain addressable until the later protected publication completes. Keeping
		// this separate, finite budget prevents a small intake queue from rejecting
		// later work merely because an earlier frame is still being evaluated.
		static constexpr std::size_t kMinimumReceiptCapacity = 64U;

		struct ReceiptRecord {
			AnimationCommand command;
			AnimationCommandReceipt receipt;
		};
		// Enqueue reserves both vectors before it changes logical state. These
		// guarantees ensure the following moves and terminal-record erase cannot
		// throw after one vector has changed but the other has not.
		static_assert(std::is_nothrow_move_constructible_v<AnimationCommand>);
		static_assert(std::is_nothrow_move_assignable_v<AnimationCommand>);
		static_assert(std::is_nothrow_move_constructible_v<ReceiptRecord>);
		static_assert(std::is_nothrow_move_assignable_v<ReceiptRecord>);

		std::size_t capacity_;
		std::size_t receiptCapacity_;
		mutable std::mutex mutex_;
		std::vector<AnimationCommand> pending_;
		std::vector<ReceiptRecord> receipts_;
		bool failNextEnqueueReservationForTesting_ = false;
	};
}
