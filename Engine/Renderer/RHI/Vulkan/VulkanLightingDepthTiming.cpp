#include "VulkanLightingDepthTiming.h"
#include <cmath>

namespace TerraEngine::Renderer::VulkanLightingDepthTiming
{
	namespace
	{
		bool ValidConfiguration(std::uint64_t session, std::uint64_t backend, std::uint64_t pool, std::uint32_t bits, double period)
		{
			return session && backend && pool && bits && bits <= 64 && std::isfinite(period) && period > 0;
		}
		bool ValidOwner(const Owner& owner)
		{
			return owner.rendererSessionGeneration && owner.backendResourceGeneration && owner.queryPoolGeneration && owner.frameSerial && owner.commandSerial
				&& owner.frameSlot < kFrameCount && owner.snapshotSlot < kSnapshotCount
				&& (owner.purpose == Purpose::Production || owner.purpose == Purpose::Oracle)
				&& (owner.view == View::Game || owner.view == View::Scene) && owner.scope == Scope::IsolatedDepth;
		}
		std::uint32_t SlotIndex(const Owner& owner)
		{
			return (owner.frameSlot * kSnapshotCount + owner.snapshotSlot) * kPurposeCount + static_cast<std::uint32_t>(owner.purpose);
		}
	}
	bool TimingState::Configure(std::uint64_t nextSession, std::uint64_t nextBackend, std::uint64_t nextPool, std::uint32_t bits, double nextPeriod)
	{
		if(Outstanding() || !ValidConfiguration(nextSession, nextBackend, nextPool, bits, nextPeriod)) return false;
		session = nextSession; backend = nextBackend; pool = nextPool; validBits = bits; period = nextPeriod; accepting = true;
		return true;
	}
	bool TimingState::Begin(const Owner& owner, QueryRange& range)
	{
		if(!accepting || !ValidOwner(owner) || owner.rendererSessionGeneration != session || owner.backendResourceGeneration != backend || owner.queryPoolGeneration != pool) return false;
		const auto index = SlotIndex(owner); auto& slot = slots[index];
		if(slot.occupied || slot.lastOwner == owner) return false;
		// Reset/write commands belong to this exact reservation. The caller must
		// not reset the physical query pair before this successful admission.
		slot.owner = owner; slot.occupied = true; slot.ended = slot.canceled = false; slot.submit = slot.completion = 0;
		range = {index * 2, 2}; return true;
	}
	TimingState::Slot* TimingState::Find(const Owner& owner)
	{
		if(!ValidOwner(owner)) return nullptr;
		auto& slot = slots[SlotIndex(owner)];
		return slot.occupied && slot.owner == owner ? &slot : nullptr;
	}
	bool TimingState::End(const Owner& owner)
	{
		auto* slot = Find(owner);
		if(!slot || slot->canceled || slot->submit || slot->ended) return false;
		slot->ended = true; return true;
	}
	bool TimingState::ConfirmSubmitted(std::span<const std::uint64_t> orderedCommands, std::uint64_t submit)
	{
		if(!submit || orderedCommands.empty()) return false;
		for(std::size_t i = 0; i < orderedCommands.size(); ++i)
		{
			if(!orderedCommands[i]) return false;
			for(std::size_t j = 0; j < i; ++j) if(orderedCommands[i] == orderedCommands[j]) return false;
		}
		const auto included = [&](std::uint64_t command) { for(const auto current : orderedCommands) if(current == command) return true; return false; };
		bool matched = false;
		// Preflight all command owners before writing any receipt. Numeric command
		// serial order is not inferred: the caller supplies actual submission order.
		for(const auto& slot : slots) if(slot.occupied && included(slot.owner.commandSerial))
		{
			matched = true; if(slot.submit && slot.submit != submit) return false;
		}
		if(!matched) return false;
		for(auto& slot : slots) if(slot.occupied && included(slot.owner.commandSerial)) slot.submit = submit;
		return true;
	}
	bool TimingState::CompleteSubmission(std::uint64_t submit, std::uint64_t completion)
	{
		if(!submit || completion < submit) return false;
		bool matched = false;
		for(const auto& slot : slots) if(slot.occupied && slot.submit == submit)
		{
			matched = true; if(slot.completion && slot.completion != completion) return false;
		}
		if(!matched) return false;
		for(auto& slot : slots) if(slot.occupied && slot.submit == submit) slot.completion = completion;
		return true;
	}
	void TimingState::Finish(Slot& slot, Result& result, Status status, Reason reason) const
	{
		result.owner = slot.owner; result.submitSerial = slot.submit; result.completionSerial = slot.completion;
		result.timestampValidBits = validBits; result.timestampPeriodNanoseconds = period; result.status = status; result.reason = reason;
		slot.lastOwner = slot.owner; slot.occupied = false; slot.ended = slot.canceled = false; slot.submit = slot.completion = 0;
	}
	bool TimingState::Resolve(const Owner& owner, const QueryRead& queries, bool nonemptyWork, Result& output)
	{
		auto* slot = Find(owner);
		if(!slot || !slot->submit || !slot->completion) return false;
		Result result; result.owner = owner; result.submitSerial = slot->submit; result.completionSerial = slot->completion;
		result.timestampValidBits = validBits; result.timestampPeriodNanoseconds = period; result.nonemptyWork = nonemptyWork;
		if(slot->canceled) Finish(*slot, result, Status::Unavailable, Reason::Canceled);
		else if(!slot->ended) Finish(*slot, result, Status::Unavailable, Reason::MissingMarkers);
		else if(!queries.readSucceeded) Finish(*slot, result, Status::Unavailable, Reason::ReadFailed);
		else
		{
			result.beginTicks = queries.beginTicks; result.endTicks = queries.endTicks;
			result.beginAvailable = queries.beginAvailable; result.endAvailable = queries.endAvailable;
			if(!queries.beginAvailable || !queries.endAvailable)
			{
				// Availability can arrive late even after the accepted fence receipt.
				// Keep its range held; no WAIT query flag or synthetic duration exists.
				result.status = Status::Pending; result.reason = Reason::MissingAvailability;
			}
			else
			{
				const auto mask = validBits == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << validBits) - 1);
				result.rawTicks = (queries.endTicks - queries.beginTicks) & mask;
				result.durationNanoseconds = static_cast<double>(result.rawTicks) * period;
				if(!std::isfinite(result.durationNanoseconds)) Finish(*slot, result, Status::Unavailable, Reason::InvalidDuration);
				else if(nonemptyWork && !result.rawTicks) Finish(*slot, result, Status::Unavailable, Reason::ZeroNonemptyDuration);
				else Finish(*slot, result, Status::Valid, Reason::None);
			}
		}
		output = result; return true;
	}
	bool TimingState::AbandonCompleted(const Owner& owner, Result& output)
	{
		auto* slot = Find(owner); if(!slot || !slot->submit || !slot->completion) return false;
		Result result; Finish(*slot, result, Status::Unavailable, Reason::Abandoned); output = result; return true;
	}
	bool TimingState::DiscardCommand(std::uint64_t command)
	{
		if(!command) return false; bool matched = false;
		for(const auto& slot : slots) if(slot.occupied && slot.owner.commandSerial == command)
		{
			matched = true; if(slot.submit) return false;
		}
		if(!matched) return false;
		for(auto& slot : slots) if(slot.occupied && slot.owner.commandSerial == command)
		{
			slot.lastOwner = slot.owner; slot.occupied = false; slot.ended = slot.canceled = false;
		}
		return true;
	}
	bool TimingState::Reset(std::uint64_t nextSession, std::uint64_t nextBackend, std::uint64_t nextPool)
	{
		if(!ValidConfiguration(nextSession, nextBackend, nextPool, validBits, period)) return false;
		accepting = false;
		for(auto& slot : slots) if(slot.occupied) slot.canceled = true;
		// Retain old configuration as well as its physical ranges until drained.
		// A collector may keep this retired state/pool beside a fresh owner module.
		if(Outstanding()) return false;
		return Configure(nextSession, nextBackend, nextPool, validBits, period);
	}
	std::uint32_t TimingState::Outstanding() const
	{
		std::uint32_t count = 0; for(const auto& slot : slots) if(slot.occupied) ++count; return count;
	}
	bool QualifiesProductionSpeedProof(const Result& result)
	{
		return result.status == Status::Valid && result.reason == Reason::None && ValidOwner(result.owner)
			&& result.owner.purpose == Purpose::Production && result.nonemptyWork && result.beginAvailable && result.endAvailable
			&& result.submitSerial && result.completionSerial >= result.submitSerial && result.rawTicks
			&& result.timestampValidBits && result.timestampValidBits <= 64 && std::isfinite(result.timestampPeriodNanoseconds)
			&& result.timestampPeriodNanoseconds > 0 && std::isfinite(result.durationNanoseconds) && result.durationNanoseconds > 0;
	}
}
