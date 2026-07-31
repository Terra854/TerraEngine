// ProceduralGeometryPublication.cpp - Protected atomic generation publication.

#include "ProceduralGeometryPublication.h"

#include <algorithm>

namespace TerraEngine::Renderer
{
	ProceduralGeometryTransaction ProceduralGeometryPublication::BeginTransaction(
		std::uint64_t objectIdentity)
	{
		std::scoped_lock lock(m_mutex);
		return ProceduralGeometryTransaction{
			m_nextTransactionIdentity++,
			objectIdentity};
	}

	bool ProceduralGeometryPublication::SubmitSourceEdit(
		const ProceduralGeometryTransaction &transaction,
		ProceduralGeometryEdit edit)
	{
		std::scoped_lock lock(m_mutex);
		if (transaction.transactionIdentity == 0u
			|| transaction.objectIdentity == 0u
			|| m_conflictedObjects.contains(transaction.objectIdentity))
		{
			return false;
		}
		const bool duplicate = std::any_of(
			m_pendingEdits.begin(),
			m_pendingEdits.end(),
			[&transaction](const PendingEdit &pending)
			{
				return pending.transaction.transactionIdentity
					== transaction.transactionIdentity;
			});
		if (duplicate) return false;
		m_pendingEdits.push_back(PendingEdit{transaction, edit});
		return true;
	}

	bool ProceduralGeometryPublication::IsVisible(
		std::uint64_t objectIdentity) const
	{
		std::scoped_lock lock(m_mutex);
		return m_visibleObjects.contains(objectIdentity);
	}

	void ProceduralGeometryPublication::FlushSceneMutations()
	{
		std::scoped_lock lock(m_mutex);
		for (const PendingEdit &pending : m_pendingEdits)
		{
			if (!m_conflictedObjects.contains(
				pending.transaction.objectIdentity))
			{
				m_visibleObjects.insert(
					pending.transaction.objectIdentity);
			}
		}
		m_pendingEdits.clear();
	}

	std::uint64_t ProceduralGeometryPublication::CurrentGenerationLocked(
		std::uint64_t objectIdentity) const
	{
		const auto found = m_currentGenerations.find(objectIdentity);
		return found == m_currentGenerations.end() ? 1u : found->second;
	}

	std::uint64_t ProceduralGeometryPublication::CurrentGeneration(
		std::uint64_t objectIdentity) const
	{
		std::scoped_lock lock(m_mutex);
		return CurrentGenerationLocked(objectIdentity);
	}

	ProceduralGeometryCandidate ProceduralGeometryPublication::PrepareCandidate(
		std::uint64_t objectIdentity,
		std::uint64_t sourceGeneration)
	{
		std::scoped_lock lock(m_mutex);
		const std::uint64_t current =
			CurrentGenerationLocked(objectIdentity);
		return ProceduralGeometryCandidate{
			m_nextCandidateIdentity++,
			objectIdentity,
			sourceGeneration,
			current + 1u,
			sourceGeneration == current
				? (objectIdentity ^ (current << 1u) ^ 0x5055424c495348ull)
				: 0u};
	}

	bool ProceduralGeometryPublication::Publish(
		const ProceduralGeometryCandidate &candidate)
	{
		std::scoped_lock lock(m_mutex);
		const std::uint64_t current =
			CurrentGenerationLocked(candidate.objectIdentity);
		if (candidate.candidateIdentity == 0u
			|| candidate.objectIdentity == 0u
			|| candidate.validationIdentity == 0u
			|| candidate.sourceGeneration != current
			|| candidate.targetGeneration != current + 1u
			|| m_cancelledCandidates.contains(candidate.candidateIdentity)
			|| m_conflictedObjects.contains(candidate.objectIdentity))
		{
			return false;
		}

		// The map update is the only visible mutation. All validation completes
		// before this point, so rejection cannot expose a partial generation.
		m_currentGenerations[candidate.objectIdentity] =
			candidate.targetGeneration;
		m_visibleObjects.insert(candidate.objectIdentity);
		return true;
	}

	void ProceduralGeometryPublication::Cancel(
		const ProceduralGeometryCandidate &candidate)
	{
		std::scoped_lock lock(m_mutex);
		if (candidate.candidateIdentity != 0u)
		{
			m_cancelledCandidates.insert(candidate.candidateIdentity);
		}
	}

	bool ProceduralGeometryPublication::RejectConflictingTransaction(
		std::uint64_t objectIdentity)
	{
		std::scoped_lock lock(m_mutex);
		if (objectIdentity == 0u) return false;
		m_conflictedObjects.insert(objectIdentity);
		std::erase_if(
			m_pendingEdits,
			[objectIdentity](const PendingEdit &pending)
			{
				return pending.transaction.objectIdentity == objectIdentity;
			});
		return true;
	}

	bool ProceduralGeometryPublication::RetireAfterFence(
		std::uint64_t objectIdentity,
		std::uint64_t generation)
	{
		std::scoped_lock lock(m_mutex);
		if (objectIdentity == 0u || generation == 0u
			|| generation > CurrentGenerationLocked(objectIdentity))
		{
			return false;
		}
		m_retiringGenerations[objectIdentity].push_back(generation);
		return true;
	}

	std::uint64_t ProceduralGeometryPublication::ViewGeneration(
		std::uint64_t objectIdentity,
		GeometryViewKind) const
	{
		// All view families intentionally resolve the same protected generation.
		return CurrentGeneration(objectIdentity);
	}
} // namespace TerraEngine::Renderer
