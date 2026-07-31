// VirtualGeometryPagePublication.cpp - validation for all-or-nothing page publication.

#include "VirtualGeometryPagePublication.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace TerraEngine::Resource
{
	bool VirtualGeometryPagePayloadCandidate::IsComplete() const noexcept
	{
		return storedByteCount != 0u && decodedByteCount != 0u && integrityValue != 0u && storedBytesVerified && decodedBlocksVerified;
	}

	bool VirtualGeometryHierarchyFixupCandidate::IsComplete() const noexcept
	{
		return !clusterIds.empty() && fallbackPageId != kInvalidVirtualGeometryPageId && fallbackCoverageVerified && hierarchyAcyclic;
	}

	bool VirtualGeometryPageDescriptorIntent::IsComplete() const noexcept
	{
		return allocationId != 0u && bufferIndex != kInvalidVirtualGeometryDescriptorIndex && descriptorIndex != kInvalidVirtualGeometryDescriptorIndex &&
			   uploadCompleted;
	}

	bool VirtualGeometryPageRetirementIntent::IsComplete() const noexcept { return allocationId != 0u && completionFence != 0u; }

	bool VirtualGeometryPagePublicationCandidate::IsComplete() const noexcept
	{
		if (candidateId == 0u || publicationGeneration == 0u || !key.IsComplete() || !payload.IsComplete() || !hierarchy.IsComplete() ||
			!descriptor.IsComplete() || !dependenciesVerified || !residencyTransitionVerified)
		{
			return false;
		}

		return std::all_of(
			retirements.begin(), retirements.end(), [](const VirtualGeometryPageRetirementIntent &retirement) { return retirement.IsComplete(); });
	}

	bool VirtualGeometryPagePublication::IsVisible() const noexcept
	{
		return state == VirtualGeometryPagePublicationState::Resident && candidateId != 0u && publicationGeneration != 0u && publicationFrame != 0u &&
			   key.IsComplete() && payload.IsComplete() && hierarchy.IsComplete() && descriptor.IsComplete();
	}

	VirtualGeometryPagePublicationResult PublishVirtualGeometryPageAtomically(
		const VirtualGeometryPagePublication &prior, const VirtualGeometryPagePublicationCandidate &candidate, std::uint64_t publicationFrame)
	{
		VirtualGeometryPagePublicationResult result;
		result.visible = prior;

		if (publicationFrame == 0u)
		{
			result.rejectionReason = "Publication requires a declared non-zero frame boundary.";
			return result;
		}
		if (!candidate.IsComplete())
		{
			result.rejectionReason = "Page publication candidate is incomplete.";
			return result;
		}
		if (prior.IsVisible())
		{
			if (candidate.key.asset.assetId != prior.key.asset.assetId || candidate.key.pageId != prior.key.pageId)
			{
				result.rejectionReason = "Candidate identity conflicts with the currently visible page.";
				return result;
			}
			if (candidate.publicationGeneration <= prior.publicationGeneration)
			{
				result.rejectionReason = "Candidate publication generation is not newer.";
				return result;
			}
			if (candidate.key.generations.geometryFormatGeneration < prior.key.generations.geometryFormatGeneration ||
				candidate.key.generations.hierarchyGeneration < prior.key.generations.hierarchyGeneration ||
				candidate.key.generations.pageDirectoryGeneration < prior.key.generations.pageDirectoryGeneration ||
				candidate.key.generations.residencyGeneration < prior.key.generations.residencyGeneration ||
				candidate.key.generations.descriptorGeneration < prior.key.generations.descriptorGeneration ||
				candidate.key.pageContentGeneration < prior.key.pageContentGeneration)
			{
				result.rejectionReason = "Candidate geometry or content generation is stale.";
				return result;
			}
		}

		std::unordered_set<std::uint32_t> dependencies;
		for (const std::uint32_t dependencyPageId : candidate.dependencyPageIds)
		{
			if (dependencyPageId == kInvalidVirtualGeometryPageId
				|| dependencyPageId == candidate.key.pageId
				|| !dependencies.insert(dependencyPageId).second)
			{
				result.rejectionReason = "Candidate dependency proof contains an invalid or duplicate page.";
				return result;
			}
		}

		std::unordered_set<std::uint64_t> retiredAllocations;
		for (const VirtualGeometryPageRetirementIntent &retirement : candidate.retirements)
		{
			if (retirement.allocationId == candidate.descriptor.allocationId || !retiredAllocations.insert(retirement.allocationId).second)
			{
				result.rejectionReason = "Candidate retires its live allocation or repeats a retirement.";
				return result;
			}
		}

		result.visible.candidateId = candidate.candidateId;
		result.visible.publicationGeneration = candidate.publicationGeneration;
		result.visible.key = candidate.key;
		result.visible.payload = candidate.payload;
		result.visible.hierarchy = candidate.hierarchy;
		result.visible.descriptor = candidate.descriptor;
		result.visible.dependencyPageIds = candidate.dependencyPageIds;
		result.visible.retirements = candidate.retirements;
		result.visible.state = VirtualGeometryPagePublicationState::Resident;
		result.visible.publicationFrame = publicationFrame;
		result.accepted = true;
		return result;
	}

	VirtualGeometryPagePublicationBatchResult PublishVirtualGeometryPagesAtomically(const std::vector<VirtualGeometryPagePublication> &priors,
		const std::vector<VirtualGeometryPagePublicationCandidate> &candidates,
		std::uint64_t publicationFrame)
	{
		VirtualGeometryPagePublicationBatchResult result;
		result.visible = priors;
		if (candidates.empty() || publicationFrame == 0u)
		{
			result.rejectionReason = "Publication batches require candidates and a declared frame boundary.";
			return result;
		}

		// Validate every candidate against a temporary copy first.  Assigning the
		// copy only after the loop is the rollback guarantee for the whole batch.
		std::vector<VirtualGeometryPagePublication> staged = priors;
		std::unordered_set<std::string> identities;
		std::unordered_map<std::uint64_t, std::string> liveAllocationOwners;
		std::unordered_set<std::uint64_t> retiredAllocations;
		for (const VirtualGeometryPagePublication &prior : priors)
		{
			if (!prior.IsVisible())
			{
				continue;
			}
			const std::string identity = TerraEngine::ToHexString(prior.key.asset.assetId) + ":" + std::to_string(prior.key.pageId);
			if (!identities.insert(identity).second
				|| !liveAllocationOwners.emplace(prior.descriptor.allocationId, identity).second)
			{
				result.rejectionReason = "Visible publication store contains duplicate page or allocation identities.";
				return result;
			}
		}

		// Validate cross-row descriptor/retirement conflicts before changing the
		// staged rows. A batch can otherwise retire an allocation another row exposes.
		std::unordered_set<std::string> candidateIdentities;
		std::unordered_set<std::uint64_t> candidateAllocations;
		for (const VirtualGeometryPagePublicationCandidate &candidate : candidates)
		{
			const std::string identity = TerraEngine::ToHexString(candidate.key.asset.assetId) + ":" + std::to_string(candidate.key.pageId);
			if (!candidateIdentities.insert(identity).second
				|| !candidateAllocations.insert(candidate.descriptor.allocationId).second)
			{
				result.rejectionReason = "Publication batch contains duplicate page or descriptor allocation identities.";
				return result;
			}
			const auto existingAllocation = liveAllocationOwners.find(candidate.descriptor.allocationId);
			if (existingAllocation != liveAllocationOwners.end() && existingAllocation->second != identity)
			{
				result.rejectionReason = "Publication batch reuses an allocation owned by another visible page.";
				return result;
			}
			for (const VirtualGeometryPageRetirementIntent &retirement : candidate.retirements)
			{
				if (!retiredAllocations.insert(retirement.allocationId).second)
				{
					result.rejectionReason = "Publication batch repeats a retirement allocation.";
					return result;
				}
				const auto existingOwner = liveAllocationOwners.find(retirement.allocationId);
				if (existingOwner != liveAllocationOwners.end() && existingOwner->second != identity)
				{
					result.rejectionReason = "Publication batch retires an allocation owned by another visible page.";
					return result;
				}
			}
		}
		for (const std::uint64_t retirementAllocation : retiredAllocations)
		{
			if (candidateAllocations.contains(retirementAllocation))
			{
				result.rejectionReason = "Publication batch retires an allocation exposed by the same batch.";
				return result;
			}
		}

		identities.clear();
		for (const VirtualGeometryPagePublicationCandidate &candidate : candidates)
		{
			const std::string identity = TerraEngine::ToHexString(candidate.key.asset.assetId) + ":" + std::to_string(candidate.key.pageId);
			if (!identities.insert(identity).second)
			{
				result.rejectionReason = "Publication batch contains duplicate page identities.";
				return result;
			}

			auto prior = std::find_if(staged.begin(),
				staged.end(),
				[&candidate](const VirtualGeometryPagePublication &value)
				{ return value.key.asset.assetId == candidate.key.asset.assetId && value.key.pageId == candidate.key.pageId; });
			const VirtualGeometryPagePublication empty{};
			const VirtualGeometryPagePublicationResult row =
				PublishVirtualGeometryPageAtomically(prior == staged.end() ? empty : *prior, candidate, publicationFrame);
			if (!row.accepted)
			{
				result.rejectionReason = row.rejectionReason;
				return result;
			}
			if (prior == staged.end())
			{
				staged.push_back(row.visible);
			}
			else
			{
				*prior = row.visible;
			}
		}

		result.visible = std::move(staged);
		result.accepted = true;
		return result;
	}

	std::vector<VirtualGeometryPagePublication> VirtualGeometryPagePublicationStore::Snapshot() const
	{
		return visiblePages;
	}

	std::uint64_t VirtualGeometryPagePublicationStore::LastPublicationFrame() const noexcept
	{
		return lastPublicationFrame;
	}

	VirtualGeometryPagePublicationBatchResult VirtualGeometryPagePublicationStore::PublishAtProtectedBoundary(
		const std::vector<VirtualGeometryPagePublicationCandidate>& candidates,
		std::uint64_t publicationFrame)
	{
		// The assignment is deliberately after validation so consumers see either
		// the previous immutable snapshot or the complete accepted replacement.
		VirtualGeometryPagePublicationBatchResult result =
			PublishVirtualGeometryPagesAtomically(visiblePages, candidates, publicationFrame);
		if (result.accepted)
		{
			visiblePages = result.visible;
			lastPublicationFrame = publicationFrame;
		}
		return result;
	}

	void VirtualGeometryPagePublicationStore::Clear() noexcept
	{
		visiblePages.clear();
		lastPublicationFrame = 0u;
	}

	VirtualGeometryPhysicalPageResidencySnapshot VirtualGeometryPhysicalPageResidencyStore::Snapshot() const
	{
		return visibleSnapshot;
	}

	VirtualGeometryPhysicalPageResidencyBatchResult
	VirtualGeometryPhysicalPageResidencyStore::PublishAtProtectedBoundary(
		const std::vector<VirtualGeometryPhysicalPageResidencyRow>& updates,
		std::uint64_t publicationFrame)
	{
		VirtualGeometryPhysicalPageResidencyBatchResult result;
		result.visible = visibleSnapshot;
		if (publicationFrame == 0u || updates.empty())
		{
			result.rejectionReason = "Physical residency publication requires updates and a declared frame boundary.";
			return result;
		}

		VirtualGeometryPhysicalPageResidencySnapshot staged = visibleSnapshot;
		std::unordered_set<std::string> updateIdentities;
		for (const VirtualGeometryPhysicalPageResidencyRow& update : updates)
		{
			const std::string identity = TerraEngine::ToHexString(update.assetId) + ":" + std::to_string(update.pageId);
			if (update.assetId == TerraEngine::EMPTY_UUID
				|| update.pageId == kInvalidVirtualGeometryPageId
				|| update.residencyGeneration == 0u
				|| update.state == VirtualGeometryPagePublicationState::Missing
				|| !updateIdentities.insert(identity).second)
			{
				result.rejectionReason = "Physical residency batch contains an invalid or duplicate page update.";
				return result;
			}

			auto existing = std::find_if(staged.pages.begin(), staged.pages.end(), [&update](const auto& row)
			{
				return row.assetId == update.assetId && row.pageId == update.pageId;
			});
			if (existing != staged.pages.end()
				&& existing->state == VirtualGeometryPagePublicationState::Resident
				&& update.state != VirtualGeometryPagePublicationState::Resident)
			{
				// IO/decode failure or a pending upload must not evict last-known-good
				// geometry. Retirement remains an explicit future protected operation.
				continue;
			}
			if (existing == staged.pages.end())
			{
				staged.pages.push_back(update);
			}
			else
			{
				*existing = update;
			}
		}

		std::sort(staged.pages.begin(), staged.pages.end(), [](const auto& left, const auto& right)
		{
			const std::string leftAsset = TerraEngine::ToHexString(left.assetId);
			const std::string rightAsset = TerraEngine::ToHexString(right.assetId);
			return leftAsset != rightAsset ? leftAsset < rightAsset : left.pageId < right.pageId;
		});
		staged.publicationFrame = publicationFrame;
		staged.publicationGeneration = visibleSnapshot.publicationGeneration + 1u;
		if (staged.publicationGeneration == 0u)
		{
			staged.publicationGeneration = 1u;
		}
		visibleSnapshot = staged;
		result.visible = std::move(staged);
		result.accepted = true;
		return result;
	}

	void VirtualGeometryPhysicalPageResidencyStore::Clear() noexcept
	{
		visibleSnapshot = {};
	}
} // namespace TerraEngine::Resource
