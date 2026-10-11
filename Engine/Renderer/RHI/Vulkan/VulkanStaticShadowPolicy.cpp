#include "VulkanStaticShadowPolicy.h"
#include <algorithm>
#include <limits>
#include <new>

namespace TerraEngine::Renderer::VulkanStaticShadow
{
	namespace
	{
		bool ValidOwner(const StaticShadowAllocationOwner& owner) noexcept
		{
			return owner.sessionGeneration && owner.backendGeneration && owner.allocationIdentity
				&& owner.allocationGeneration && owner.actualBytes && owner.depthFormat
				&& owner.extent[0] && owner.extent[1] && (owner.layers == 1 || owner.layers == 6);
		}
		bool SamePhysicalAllocation(const StaticShadowAllocationOwner& a, const StaticShadowAllocationOwner& b) noexcept
		{
			return a.sessionGeneration == b.sessionGeneration && a.backendGeneration == b.backendGeneration
				&& a.allocationIdentity == b.allocationIdentity;
		}
		bool ViewMatchesOwner(const ShadowViewKey& view, const StaticShadowAllocationOwner& owner) noexcept
		{
			const auto kind = static_cast<std::uint32_t>(view.resourceKind);
			return view.rendererSessionGeneration == owner.sessionGeneration
				&& view.allocationGeneration == owner.allocationGeneration
				&& view.extent == owner.extent && view.depthFormat == owner.depthFormat
				&& kind <= 3 && owner.layers == (kind == 1 ? 6U : 1U);
		}
	}
	bool StaticShadowLifetimeTracker::RegisterAllocation(const StaticShadowAllocationOwner& owner)
	{
		if(!ValidOwner(owner)) return false;
		for(const auto& allocation : allocations)
		{
			if(allocation.owner == owner) return !allocation.retiring;
			// Changed generation/size cannot resurrect a still charged native handle.
			if(SamePhysicalAllocation(allocation.owner, owner)) return false;
		}
		if(owner.actualBytes > std::numeric_limits<std::uint64_t>::max() - ChargedBytes()) return false;
		try { allocations.push_back({owner}); }
		catch(const std::bad_alloc&) { return false; }
		return true;
	}
	bool StaticShadowLifetimeTracker::ReserveRecordedUse(const StaticShadowAllocationOwner& owner,
		std::uint64_t commandSerial, std::uint32_t frameIndex)
	{
		if(!commandSerial || frameIndex >= 3) return false;
		auto found = std::find_if(allocations.begin(), allocations.end(), [&](const auto& row) { return row.owner == owner; });
		if(found == allocations.end()) return false;
		// A primary may reference several images. Its frame/session must agree
		// everywhere before another reference to that command is admitted.
		for(const auto& allocation : allocations) for(const auto& use : allocation.uses)
			if(use.command == commandSerial && (use.frame != frameIndex
				|| allocation.owner.sessionGeneration != owner.sessionGeneration
				|| allocation.owner.backendGeneration != owner.backendGeneration
				|| (use.submit && allocation.owner != owner))) return false;
		for(const auto& use : found->uses)
			if(use.command == commandSerial) return use.frame == frameIndex;
		if(found->retiring || commandSerial <= found->lastCommand) return false;
		try { found->uses.push_back({commandSerial, 0, frameIndex}); }
		catch(const std::bad_alloc&) { return false; }
		found->lastCommand = commandSerial;
		return true;
	}
	bool StaticShadowLifetimeTracker::PreserveSubmittedUse(std::uint64_t commandSerial, std::uint64_t submitSerial) noexcept
	{
		if(!commandSerial || !submitSerial) return false;
		bool found = false;
		// Validate every reference first. Accepted work cannot be partly published
		// because another image carries a conflicting native receipt.
		for(const auto& allocation : allocations) for(const auto& use : allocation.uses)
			if(use.command == commandSerial)
			{
				found = true;
				if(use.submit)
				{
					if(use.submit != submitSerial) return false;
				}
				else if(submitSerial < allocation.lastSubmit || submitSerial <= allocation.lastCompletedSubmit) return false;
			}
		if(!found) return false;
		for(auto& allocation : allocations) for(auto& use : allocation.uses)
			if(use.command == commandSerial)
			{
				use.submit = submitSerial;
				allocation.lastSubmit = (std::max)(allocation.lastSubmit, submitSerial);
			}
		return true;
	}
	void StaticShadowLifetimeTracker::DiscardUnsubmitted(std::uint64_t commandSerial) noexcept
	{
		if(!commandSerial) return;
		for(auto& allocation : allocations)
			std::erase_if(allocation.uses, [&](const auto& use) { return use.command == commandSerial && !use.submit; });
	}
	void StaticShadowLifetimeTracker::CompleteSubmission(std::uint64_t submitSerial) noexcept
	{
		if(!submitSerial) return;
		for(auto& allocation : allocations)
		{
			// A newer/unrelated fence is not a wildcard completion. Prune only exact
			// accepted references so long-lived static images retain bounded storage.
			const bool matched = std::any_of(allocation.uses.begin(), allocation.uses.end(),
				[&](const auto& use) { return use.submit == submitSerial; });
			if(matched) allocation.lastCompletedSubmit = (std::max)(allocation.lastCompletedSubmit, submitSerial);
			std::erase_if(allocation.uses, [&](const auto& use) { return use.submit == submitSerial; });
		}
	}
	bool StaticShadowLifetimeTracker::RequestRetirement(const StaticShadowAllocationOwner& owner) noexcept
	{
		for(auto& allocation : allocations) if(allocation.owner == owner) { allocation.retiring = true; return true; }
		return false;
	}
	bool StaticShadowLifetimeTracker::CanRelease(const StaticShadowAllocationOwner& owner) const noexcept
	{
		for(const auto& allocation : allocations) if(allocation.owner == owner)
			return allocation.retiring && allocation.uses.empty();
		return false;
	}
	std::uint64_t StaticShadowLifetimeTracker::ChargedBytes() const noexcept
	{
		std::uint64_t bytes = 0;
		for(const auto& allocation : allocations) bytes += allocation.owner.actualBytes;
		return bytes;
	}
	std::uint64_t StaticShadowLifetimeTracker::ResidentBytes() const noexcept
	{
		std::uint64_t bytes = 0;
		for(const auto& allocation : allocations) if(!allocation.retiring) bytes += allocation.owner.actualBytes;
		return bytes;
	}
	std::uint64_t StaticShadowLifetimeTracker::RetiringBytes() const noexcept
	{
		std::uint64_t bytes = 0;
		for(const auto& allocation : allocations) if(allocation.retiring) bytes += allocation.owner.actualBytes;
		return bytes;
	}
	std::vector<StaticShadowAllocationReceipt> StaticShadowLifetimeTracker::CaptureAllocations() const
	{
		// Capture owns fresh storage. Allocation failure propagates to the
		// validation caller without changing any native lifetime reference.
		std::vector<StaticShadowAllocationReceipt> result;
		result.reserve(allocations.size());
		for(const auto& allocation : allocations)
		{
			StaticShadowAllocationReceipt receipt;
			receipt.owner = allocation.owner;
			receipt.retiring = allocation.retiring;
			receipt.uses.reserve(allocation.uses.size());
			for(const auto& use : allocation.uses)
				receipt.uses.push_back({use.command, use.submit, use.frame});
			result.push_back(std::move(receipt));
		}
		return result;
	}
	bool StaticShadowLifetimeTracker::ReleaseCompleted(const StaticShadowAllocationOwner& owner) noexcept
	{
		if(!CanRelease(owner)) return false;
		std::erase_if(allocations, [&](const auto& allocation) { return allocation.owner == owner; });
		return true;
	}
	bool StaticShadowLifetimeTracker::TerminalRelease(const StaticShadowAllocationOwner& owner, StaticShadowTerminalProof proof) noexcept
	{
		if(proof != StaticShadowTerminalProof::DeviceLost && proof != StaticShadowTerminalProof::DeviceIdle) return false;
		// Caller proves final teardown/device loss. Ordinary rebuilding must use
		// retirement and exact completion instead of this terminal escape path.
		const auto before = allocations.size();
		std::erase_if(allocations, [&](const auto& allocation) { return allocation.owner == owner; });
		return allocations.size() != before;
	}
	StaticShadowCompositionPlan BuildStaticShadowCompositionPlan(const StaticShadowCompositionRequest& request,
		const ShadowCachePublicationLedger& ledger) noexcept
	{
		StaticShadowCompositionPlan plan;
		if(!ValidOwner(request.source) || !ValidOwner(request.destination) || !request.currentCommandSerial
			|| SamePhysicalAllocation(request.source, request.destination)
			|| request.source.sessionGeneration != request.destination.sessionGeneration
			|| request.source.backendGeneration != request.destination.backendGeneration
			|| request.source.depthFormat != request.destination.depthFormat
			|| request.source.extent != request.destination.extent || request.source.layers != request.destination.layers
			|| !ViewMatchesOwner(request.staticView, request.source) || !ViewMatchesOwner(request.finalView, request.destination)) return plan;
		// Both images represent the same exact light/view/projection. Only the
		// physical allocation and region identities intentionally differ.
		auto logicalTarget = request.finalView;
		logicalTarget.allocationGeneration = request.staticView.allocationGeneration;
		logicalTarget.regionId = request.staticView.regionId;
		if(logicalTarget != request.staticView) return plan;
		const std::uint8_t required = request.source.layers == 6 ? 63 : 1;
		if(request.requiredFaceMask != required || request.sourceFaceMask != required
			|| !request.sourceTransferSupported || !request.destinationTransferSupported
			|| !request.sourceTransferUsage || !request.destinationTransferUsage
			|| !request.sameShadowPipeline || !request.lessDepthComparison
			|| !request.colorWritesDisabled || !request.cutoutBehaviorPreserved) return plan;
		const bool sourceAuthorized = request.sourceRecordedCommandSerial
			? request.sourceRecordedCommandSerial == request.currentCommandSerial
				&& ledger.HasRecordedCandidate(request.staticView, request.staticContentToken, request.currentCommandSerial)
			: ledger.CanReuse(request.staticView, request.staticContentToken);
		if(sourceAuthorized) { plan.route = StaticShadowCompositionRoute::CacheCopyOverlay; plan.requiredFaceMask = required; }
		return plan;
	}
}
