// CharacterMotionTrace.h - opt-in bounded copied ingress evidence for diagnostics.
#pragma once

#include "PhysicsCommands.h"

#include <algorithm>
#include <limits>
#include <variant>
#include <vector>

namespace TerraEngine::Physics {
	// Records describe accepted protected ingress, not completed simulation. Guards
	// are deliberately not retained: a diagnostic must never extend scene lifetime.
	using CharacterMotionTracePayload = std::variant<CharacterMotionLeaseChange,
		CharacterMotionTarget, RootMotionInterval>;
	struct CharacterMotionTraceRecord {
		std::uint64_t sequence = 0U;
		PhysicsTick precedingTick = 0U;
		std::uint64_t batch = 0U;
		std::uint32_t batchIndex = 0U;
		std::uint32_t batchCount = 1U;
		bool hadValidityGuard = false;
		CharacterMotionTracePayload payload;
	};
	struct CharacterMotionTraceSnapshot {
		bool enabled = false;
		bool lostRecords = false;
		std::uint64_t latestSequence = 0U;
		std::size_t reservedBytes = 0U;
		std::vector<CharacterMotionTraceRecord> records;
	};

	// PhysicsSystem owns synchronization. Enable allocates once; recording does
	// not allocate and disabled production runs perform only an empty check.
	class CharacterMotionTraceBuffer final {
	public:
		bool Configure(bool enabled, std::size_t capacity = 16384U) {
			if(enabled && (capacity == 0U || capacity > 65536U)) return false;
			try {
				std::vector<CharacterMotionTraceRecord> replacement(enabled ? capacity : 0U);
				ring_.swap(replacement);
			} catch(const std::bad_alloc&) { return false; }
			count_ = 0U;
			next_ = 0U;
			return true;
		}
		[[nodiscard]] bool Enabled() const noexcept { return !ring_.empty(); }
		[[nodiscard]] std::size_t ReservedBytes() const noexcept { return ring_.capacity() * sizeof(CharacterMotionTraceRecord); }
		void Record(CharacterMotionTraceRecord record) noexcept {
			if(ring_.empty() || sequence_ == std::numeric_limits<std::uint64_t>::max()) return;
			record.sequence = ++sequence_;
			if(auto* target = std::get_if<CharacterMotionTarget>(&record.payload)) {
				record.hadValidityGuard = target->validityGuard != nullptr;
				target->validityGuard.reset();
			} else if(auto* interval = std::get_if<RootMotionInterval>(&record.payload)) {
				record.hadValidityGuard = interval->validityGuard != nullptr;
				interval->validityGuard.reset();
			}
			ring_[next_] = std::move(record);
			next_ = (next_ + 1U) % ring_.size();
			count_ = std::min(count_ + 1U, ring_.size());
		}
		[[nodiscard]] CharacterMotionTraceSnapshot CopySince(std::uint64_t afterSequence) const {
			CharacterMotionTraceSnapshot result;
			result.enabled = Enabled();
			result.latestSequence = sequence_;
			result.reservedBytes = ring_.capacity() * sizeof(CharacterMotionTraceRecord);
			const std::uint64_t earliest = count_ == 0U
				? sequence_ + (sequence_ != std::numeric_limits<std::uint64_t>::max()) : sequence_ - count_ + 1U;
			result.lostRecords = afterSequence > sequence_
				|| (afterSequence < earliest && earliest - afterSequence > 1U)
				|| sequence_ == std::numeric_limits<std::uint64_t>::max();
			result.records.reserve(count_);
			for(std::size_t index = 0U; index < count_; ++index) {
				const auto& record = ring_[(next_ + ring_.size() - count_ + index) % ring_.size()];
				if(record.sequence > afterSequence) result.records.push_back(record);
			}
			return result;
		}
	private:
		std::vector<CharacterMotionTraceRecord> ring_;
		std::size_t count_ = 0U;
		std::size_t next_ = 0U;
		std::uint64_t sequence_ = 0U;
	};
}
