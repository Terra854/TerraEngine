// AnimationHotReload.cpp - atomic, generation-safe animation reload publication.

#include "Animation/Streaming/AnimationHotReload.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <stdexcept>

namespace TerraEngine::Animation
{
	struct AnimationHotReloadPublisher::PreparedPublicationState
	{
		AnimationHotReloadPublisher* owner = nullptr;
		std::uint64_t preparationEpoch = 0u;
		std::uint64_t publicationBoundary = 0u;
		std::unordered_map<std::string, PublishedGeneration> active;
		std::vector<PendingRetirement> pendingRetirement;
		std::unordered_map<std::string, InstanceBinding> instanceBindings;
		std::unordered_map<std::string, std::unordered_set<std::string>> dependentsByDependency;
		std::uint32_t dependencyEdgeCount = 0u;
		std::unordered_map<std::string, std::unordered_set<std::string>> compilerDependentsByDependency;
		std::uint32_t compilerDependencyEdgeCount = 0u;
	};

	AnimationHotReloadPublisher::PreparedBatch::PreparedBatch() noexcept = default;
	AnimationHotReloadPublisher::PreparedBatch::~PreparedBatch() = default;
	AnimationHotReloadPublisher::PreparedBatch::PreparedBatch(PreparedBatch&& other) noexcept = default;
	AnimationHotReloadPublisher::PreparedBatch& AnimationHotReloadPublisher::PreparedBatch::operator=(
		PreparedBatch&& other) noexcept = default;

	bool AnimationHotReloadPublisher::PreparedBatch::Accepted() const noexcept
	{
		return m_accepted;
	}

	bool AnimationHotReloadPublisher::PreparedBatch::Committed() const noexcept
	{
		return m_commitStatus == AnimationReloadCommitStatus::Committed;
	}

	AnimationReloadCommitStatus AnimationHotReloadPublisher::PreparedBatch::CommitStatus() const noexcept
	{
		return m_commitStatus;
	}

	std::uint64_t AnimationHotReloadPublisher::PreparedBatch::PublicationBoundary() const noexcept
	{
		return m_publicationBoundary;
	}

	std::span<const AnimationReloadOutcome> AnimationHotReloadPublisher::PreparedBatch::Outcomes() const noexcept
	{
		return m_outcomes;
	}

	std::vector<AnimationReloadOutcome>
	AnimationHotReloadPublisher::PreparedBatch::ReleaseOutcomes() && noexcept
	{
		return std::move(m_outcomes);
	}

	AnimationHotReloadPublisher::AnimationHotReloadPublisher(
		std::uint32_t maximumAssets,
		std::uint32_t maximumDependencyEdges,
		std::uint32_t maximumInstances,
		std::uint32_t maximumPendingRetirements)
		: m_maximumAssets(std::max(1u, maximumAssets)),
		m_maximumDependencyEdges(std::max(1u, maximumDependencyEdges)),
		m_maximumInstances(std::max(1u, maximumInstances)),
		m_maximumPendingRetirements(std::max(1u, maximumPendingRetirements))
	{
	}

	void AnimationHotReloadPublisher::Reset()
	{
		std::scoped_lock lock(m_mutex);
		m_latestPublicationBoundary = 0u;
		m_dependencyEdgeCount = 0u;
		m_active.clear();
		m_pendingRetirement.clear();
		m_dependentsByDependency.clear();
		m_compilerDependentsByDependency.clear();
		m_compilerDependencyEdgeCount = 0u;
		m_instanceBindings.clear();
		m_generationRetains.clear();
		AdvancePreparationEpochLocked();
	}

	void AnimationHotReloadPublisher::AdvancePreparationEpochLocked() noexcept
	{
		// Epoch zero remains reserved for an invalid/unowned preparation. Wrapping
		// after the full uint64 range therefore still produces a usable token.
		++m_preparationEpoch;
		if (m_preparationEpoch == 0u)
		{
			m_preparationEpoch = 1u;
		}
	}

	std::string AnimationHotReloadPublisher::MakeKey(const TerraEngine::UUID& id)
	{
		return TerraEngine::ToHexString(id);
	}

	AnimationReloadOutcome AnimationHotReloadPublisher::MakeRejectedOutcome(
		const AnimationReloadCandidate& candidate,
		const std::unordered_map<std::string, PublishedGeneration>& active,
		std::string reason)
	{
		AnimationReloadOutcome outcome;
		const auto previous = active.find(MakeKey(candidate.assetId));
		outcome.retainedLastKnownGood = previous != active.end();
		outcome.activeGeneration = previous != active.end() ? previous->second.view.generation : 0u;
		outcome.publicationBoundary = previous != active.end()
			? previous->second.view.publicationBoundary : 0u;
		outcome.reason = std::move(reason);
		return outcome;
	}

	AnimationReloadOutcome AnimationHotReloadPublisher::Publish(const AnimationReloadCandidate& candidate)
	{
		const std::vector<AnimationReloadOutcome> outcomes = PublishBatch(
			std::span<const AnimationReloadCandidate>(&candidate, 1u));
		return outcomes.empty()
			? MakeRejectedOutcome(candidate, {}, "No reload candidate was published.")
			: outcomes.front();
	}

	AnimationHotReloadPublisher::PreparedBatch AnimationHotReloadPublisher::PrepareBatch(
		std::span<const AnimationReloadCandidate> candidates,
		std::uint64_t publicationBoundary)
	{
		std::scoped_lock lock(m_mutex);
		PreparedBatch prepared;
		if (candidates.empty())
		{
			return prepared;
		}

		const auto rejectBatch = [&](const std::string& reason) {
			prepared.m_accepted = false;
			prepared.m_outcomes.clear();
			prepared.m_outcomes.reserve(candidates.size());
			for (const AnimationReloadCandidate& candidate : candidates)
			{
				prepared.m_outcomes.push_back(MakeRejectedOutcome(candidate, m_active, reason));
			}
		};

		constexpr std::size_t kMaximumAtomicReloadBatch = 256u;
		if (candidates.size() > kMaximumAtomicReloadBatch)
		{
			rejectBatch("The atomic animation reload batch exceeds the hard 256-asset bound.");
			return prepared;
		}

		if (publicationBoundary == 0u)
		{
			if (m_latestPublicationBoundary == std::numeric_limits<std::uint64_t>::max())
			{
				rejectBatch("Animation reload publication boundary exhausted.");
				return prepared;
			}
			publicationBoundary = m_latestPublicationBoundary + 1u;
		}
		prepared.m_publicationBoundary = publicationBoundary;
		if (publicationBoundary <= m_latestPublicationBoundary)
		{
			rejectBatch("Reload publication boundary is stale or was already consumed.");
			return prepared;
		}

		std::unordered_map<std::string, const AnimationReloadCandidate*> batchByAsset;
		batchByAsset.reserve(candidates.size());
		std::string validationFailure;
		for (const AnimationReloadCandidate& candidate : candidates)
		{
			const std::string key = MakeKey(candidate.assetId);
			if (candidate.assetId == TerraEngine::EMPTY_UUID || candidate.generation == 0u
				|| candidate.compatibilityFingerprint == 0u || !candidate.parsed || !candidate.dependenciesReady
				|| static_cast<std::uint8_t>(candidate.assetType)
					> static_cast<std::uint8_t>(AnimationReloadAssetType::RagdollProfile))
			{
				validationFailure = "Every candidate must be parsed, dependency-complete, typed, and carry nonzero generation/compatibility identity.";
				break;
			}
			if (candidate.dependencies.size() > 256u)
			{
				validationFailure = "A reload candidate exceeds the hard 256-dependency bound.";
				break;
			}
			if (!batchByAsset.emplace(key, &candidate).second)
			{
				validationFailure = "An atomic reload batch contains the same asset more than once.";
				break;
			}
			if (const auto current = m_active.find(key); current != m_active.end())
			{
				if (candidate.generation <= current->second.view.generation)
				{
					validationFailure = "A reload candidate does not advance the active generation.";
					break;
				}
				if (candidate.assetType != current->second.view.assetType
					|| candidate.compatibilityFingerprint != current->second.view.compatibilityFingerprint)
				{
					validationFailure = "An incompatible reload cannot replace the active last-known-good generation.";
					break;
				}
			}
		}

		if (validationFailure.empty())
		{
			std::size_t newAssetCount = m_active.size();
			std::size_t replacementCount = 0u;
			for (const auto& [key, candidate] : batchByAsset)
			{
				(void)candidate;
				if (m_active.contains(key))
				{
					++replacementCount;
				}
				else
				{
					++newAssetCount;
				}
			}
			if (newAssetCount > m_maximumAssets)
			{
				validationFailure = "The bounded hot-reload asset table is full.";
			}
			else if (replacementCount > m_maximumPendingRetirements
				|| m_pendingRetirement.size() > m_maximumPendingRetirements - replacementCount)
			{
				validationFailure = "The bounded old-generation retirement queue is full; complete a protected retirement boundary before publishing again.";
			}
		}

		if (validationFailure.empty())
		{
			for (const AnimationReloadCandidate& candidate : candidates)
			{
				for (const AnimationReloadDependency& dependency : candidate.dependencies)
				{
					if (!dependency.required)
					{
						continue;
					}
					if (dependency.assetId == TerraEngine::EMPTY_UUID)
					{
						validationFailure = "A required reload dependency has an empty asset identity.";
						break;
					}
					std::uint64_t generation = 0u;
					std::uint64_t fingerprint = 0u;
					const std::string dependencyKey = MakeKey(dependency.assetId);
					if (const auto staged = batchByAsset.find(dependencyKey); staged != batchByAsset.end())
					{
						generation = staged->second->generation;
						fingerprint = staged->second->compatibilityFingerprint;
					}
					else if (const auto active = m_active.find(dependencyKey); active != m_active.end())
					{
						generation = active->second.view.generation;
						fingerprint = active->second.view.compatibilityFingerprint;
					}
					if (generation == 0u
						|| (dependency.expectedGeneration != 0u && generation != dependency.expectedGeneration)
						|| (dependency.compatibilityFingerprint != 0u
							&& fingerprint != dependency.compatibilityFingerprint))
					{
						validationFailure = "A required dependency generation or compatibility fingerprint is unavailable.";
						break;
					}
				}
				if (!validationFailure.empty())
				{
					break;
				}
			}
		}

		if (!validationFailure.empty())
		{
			rejectBatch(validationFailure
				+ " The entire batch was rejected without changing live state.");
			return prepared;
		}

		try
		{
			auto staged = std::make_unique<PreparedPublicationState>();
			staged->owner = this;
			staged->preparationEpoch = m_preparationEpoch;
			staged->publicationBoundary = publicationBoundary;
			staged->active = m_active;
			staged->pendingRetirement = m_pendingRetirement;
			staged->instanceBindings = m_instanceBindings;
			staged->dependentsByDependency = m_dependentsByDependency;
			staged->dependencyEdgeCount = m_dependencyEdgeCount;
			staged->compilerDependentsByDependency = m_compilerDependentsByDependency;
			staged->compilerDependencyEdgeCount = m_compilerDependencyEdgeCount;
			// Each replacement declares the complete outgoing dependency set. Remove
			// old reverse edges first, then stage the new required edges in the same
			// copy that later becomes visible with the generation publication.
			for (const AnimationReloadCandidate& candidate : candidates)
			{
				if (!candidate.dependenciesAreCompleteDeclaration)
				{
					continue;
				}
				const std::string dependentKey = MakeKey(candidate.assetId);
				for (auto iterator = staged->compilerDependentsByDependency.begin();
					iterator != staged->compilerDependentsByDependency.end(); )
				{
					if (iterator->second.erase(dependentKey) != 0u)
					{
						--staged->compilerDependencyEdgeCount;
					}
					if (iterator->second.empty()) iterator = staged->compilerDependentsByDependency.erase(iterator);
					else ++iterator;
				}
				for (const AnimationReloadDependency& dependency : candidate.dependencies)
				{
					if (!dependency.required) continue;
					const std::string dependencyKey = MakeKey(dependency.assetId);
					auto& dependents = staged->compilerDependentsByDependency[dependencyKey];
					if (!dependents.contains(dependentKey))
					{
						if (static_cast<std::uint64_t>(staged->dependencyEdgeCount)
							+ staged->compilerDependencyEdgeCount >= m_maximumDependencyEdges
							|| dependents.size() >= 256u)
						{
							throw std::runtime_error("The staged animation dependency graph is full.");
						}
						dependents.insert(dependentKey);
						++staged->compilerDependencyEdgeCount;
					}
				}
			}
			prepared.m_outcomes.resize(candidates.size());
			for (std::size_t index = 0u; index < candidates.size(); ++index)
			{
				const AnimationReloadCandidate& candidate = candidates[index];
				const std::string key = MakeKey(candidate.assetId);
				if (const auto current = staged->active.find(key); current != staged->active.end())
				{
					staged->pendingRetirement.push_back({candidate.assetId,
						current->second.view.generation, publicationBoundary});
				}

				PublishedGeneration generation;
				generation.view.assetId = candidate.assetId;
				generation.view.assetType = candidate.assetType;
				generation.view.generation = candidate.generation;
				generation.view.compatibilityFingerprint = candidate.compatibilityFingerprint;
				generation.view.contentFingerprint = candidate.contentFingerprint;
				generation.view.contentHash = candidate.contentHash;
				generation.view.publicationBoundary = publicationBoundary;
				generation.view.diagnostic = "Compatible complete generation is active.";
				generation.dependencies = candidate.dependencies;
				staged->active.insert_or_assign(key, std::move(generation));

				AnimationReloadOutcome& outcome = prepared.m_outcomes[index];
				outcome.prepared = true;
				outcome.activeGeneration = candidate.generation;
				outcome.publicationBoundary = publicationBoundary;
				outcome.reason = "Compatible generation is fully staged for atomic protected-boundary publication.";
				for (auto& [instanceKey, binding] : staged->instanceBindings)
				{
					(void)instanceKey;
					if (binding.view.assetId == candidate.assetId)
					{
						binding.view.generation = candidate.generation;
						binding.view.switchBoundary = publicationBoundary;
						++outcome.switchedInstanceCount;
					}
				}
			}

			// Invalidate every transitive dependent that was not rebuilt in this
			// batch. Its old immutable generation remains available for diagnostics,
			// but evaluators must use their declared safe fallback until a compatible
			// dependent generation is published.
			for (std::size_t index = 0u; index < candidates.size(); ++index)
			{
				std::queue<std::string> pending;
				std::unordered_set<std::string> visited;
				pending.push(MakeKey(candidates[index].assetId));
				while (!pending.empty())
				{
					const std::string dependencyKey = pending.front();
					pending.pop();
					if (!visited.insert(dependencyKey).second)
					{
						continue;
					}
					const auto visitDependents = [&](const auto& graph) {
						const auto dependents = graph.find(dependencyKey);
						if (dependents == graph.end()) return;
						for (const std::string& dependentKey : dependents->second)
					{
						pending.push(dependentKey);
						if (batchByAsset.contains(dependentKey))
						{
							continue;
						}
						if (auto dependent = staged->active.find(dependentKey); dependent != staged->active.end())
						{
							dependent->second.view.dependencyInvalidated = true;
							dependent->second.view.diagnostic =
								"A dependency changed; rebuild is required before evaluator publication.";
							prepared.m_outcomes[index].invalidatedDependents.push_back(dependent->second.view.assetId);
						}
						}
					};
					visitDependents(staged->dependentsByDependency);
					visitDependents(staged->compilerDependentsByDependency);
				}
				std::sort(prepared.m_outcomes[index].invalidatedDependents.begin(),
					prepared.m_outcomes[index].invalidatedDependents.end(),
					[](const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
					return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
				});
				prepared.m_outcomes[index].invalidatedDependents.erase(std::unique(
					prepared.m_outcomes[index].invalidatedDependents.begin(),
					prepared.m_outcomes[index].invalidatedDependents.end()),
					prepared.m_outcomes[index].invalidatedDependents.end());
			}

			prepared.m_accepted = true;
			prepared.m_state = std::move(staged);
			return prepared;
		}
		catch (const std::exception& exception)
		{
			rejectBatch("Reload staging failed before publication: " + std::string(exception.what()));
			return prepared;
		}
	}

	AnimationReloadCommitStatus AnimationHotReloadPublisher::CommitPreparedBatch(
		PreparedBatch& prepared,
		PrecommitCallback externalPrecommit,
		void* externalContext,
		PostcommitCallback externalPostcommit) noexcept
	{
		const auto setOutcomeStatus = [&](AnimationReloadCommitStatus status, bool published) noexcept {
			prepared.m_commitStatus = status;
			for (AnimationReloadOutcome& outcome : prepared.m_outcomes)
			{
				outcome.commitStatus = status;
				outcome.published = published;
			}
		};

		std::scoped_lock lock(m_mutex);
		if (prepared.m_commitStatus == AnimationReloadCommitStatus::Committed)
		{
			return AnimationReloadCommitStatus::RejectedInvalidPreparation;
		}
		if (!prepared.m_accepted || !prepared.m_state || prepared.m_state->owner != this)
		{
			setOutcomeStatus(AnimationReloadCommitStatus::RejectedInvalidPreparation, false);
			return AnimationReloadCommitStatus::RejectedInvalidPreparation;
		}

		PreparedPublicationState& staged = *prepared.m_state;
		if (staged.preparationEpoch != m_preparationEpoch
			|| staged.publicationBoundary != prepared.m_publicationBoundary
			|| staged.publicationBoundary <= m_latestPublicationBoundary)
		{
			setOutcomeStatus(AnimationReloadCommitStatus::RejectedStalePreparation, false);
			return AnimationReloadCommitStatus::RejectedStalePreparation;
		}

		// The callback participates in the same protected boundary. It runs only
		// after stale validation, and a false result prevents every publisher swap.
		if (externalPrecommit != nullptr && !externalPrecommit(externalContext))
		{
			setOutcomeStatus(AnimationReloadCommitStatus::RejectedExternalPrecommit, false);
			return AnimationReloadCommitStatus::RejectedExternalPrecommit;
		}

		m_active.swap(staged.active);
		m_pendingRetirement.swap(staged.pendingRetirement);
		m_instanceBindings.swap(staged.instanceBindings);
		m_dependentsByDependency.swap(staged.dependentsByDependency);
		m_dependencyEdgeCount = staged.dependencyEdgeCount;
		m_compilerDependentsByDependency.swap(staged.compilerDependentsByDependency);
		m_compilerDependencyEdgeCount = staged.compilerDependencyEdgeCount;
		m_latestPublicationBoundary = staged.publicationBoundary;
		AdvancePreparationEpochLocked();
		// Cache locks are intentionally released only after every publisher table
		// above is visible, eliminating a cache-new/publisher-old observation gap.
		if (externalPostcommit != nullptr)
		{
			externalPostcommit(externalContext);
		}
		setOutcomeStatus(AnimationReloadCommitStatus::Committed, true);
		return AnimationReloadCommitStatus::Committed;
	}

	std::vector<AnimationReloadOutcome> AnimationHotReloadPublisher::PublishBatch(
		std::span<const AnimationReloadCandidate> candidates,
		std::uint64_t publicationBoundary)
	{
		PreparedBatch prepared = PrepareBatch(candidates, publicationBoundary);
		if (prepared.Accepted())
		{
			CommitPreparedBatch(prepared);
		}
		return std::move(prepared).ReleaseOutcomes();
	}

	bool AnimationHotReloadPublisher::RegisterDependency(
		const TerraEngine::UUID& dependentAssetId,
		const TerraEngine::UUID& dependencyAssetId)
	{
		if (dependentAssetId == TerraEngine::EMPTY_UUID || dependencyAssetId == TerraEngine::EMPTY_UUID
			|| dependentAssetId == dependencyAssetId)
		{
			return false;
		}
		std::scoped_lock lock(m_mutex);
		const std::string dependencyKey = MakeKey(dependencyAssetId);
		const std::string dependentKey = MakeKey(dependentAssetId);
		const auto existing = m_dependentsByDependency.find(dependencyKey);
		if (existing != m_dependentsByDependency.end()
			&& existing->second.contains(dependentKey))
		{
			return true;
		}
		const std::size_t dependentCount = existing == m_dependentsByDependency.end()
			? 0u : existing->second.size();
		// Manual and compiler-owned edges share one bounded publisher budget even
		// though their provenance is stored separately for atomic reconciliation.
		if (static_cast<std::uint64_t>(m_dependencyEdgeCount)
			+ m_compilerDependencyEdgeCount >= m_maximumDependencyEdges
			|| dependentCount >= 256u)
		{
			return false;
		}
		auto& dependents = m_dependentsByDependency[dependencyKey];
		dependents.insert(dependentKey);
		++m_dependencyEdgeCount;
		AdvancePreparationEpochLocked();
		return true;
	}

	bool AnimationHotReloadPublisher::UnregisterDependency(
		const TerraEngine::UUID& dependentAssetId,
		const TerraEngine::UUID& dependencyAssetId)
	{
		std::scoped_lock lock(m_mutex);
		const auto dependency = m_dependentsByDependency.find(MakeKey(dependencyAssetId));
		if (dependency == m_dependentsByDependency.end()
			|| dependency->second.erase(MakeKey(dependentAssetId)) == 0u)
		{
			return false;
		}
		--m_dependencyEdgeCount;
		if (dependency->second.empty())
		{
			m_dependentsByDependency.erase(dependency);
		}
		AdvancePreparationEpochLocked();
		return true;
	}

	bool AnimationHotReloadPublisher::BindInstance(
		const TerraEngine::UUID& instanceId,
		const TerraEngine::UUID& assetId,
		std::uint64_t expectedGeneration)
	{
		if (instanceId == TerraEngine::EMPTY_UUID || assetId == TerraEngine::EMPTY_UUID)
		{
			return false;
		}
		std::scoped_lock lock(m_mutex);
		const auto active = m_active.find(MakeKey(assetId));
		if (active == m_active.end() || (expectedGeneration != 0u
			&& active->second.view.generation != expectedGeneration))
		{
			return false;
		}
		const std::string instanceKey = MakeKey(instanceId);
		if (!m_instanceBindings.contains(instanceKey) && m_instanceBindings.size() >= m_maximumInstances)
		{
			return false;
		}
		InstanceBinding binding;
		binding.view.instanceId = instanceId;
		binding.view.assetId = assetId;
		binding.view.generation = active->second.view.generation;
		binding.view.switchBoundary = active->second.view.publicationBoundary;
		m_instanceBindings.insert_or_assign(instanceKey, std::move(binding));
		AdvancePreparationEpochLocked();
		return true;
	}

	bool AnimationHotReloadPublisher::UnbindInstance(const TerraEngine::UUID& instanceId)
	{
		std::scoped_lock lock(m_mutex);
		const bool removed = m_instanceBindings.erase(MakeKey(instanceId)) != 0u;
		if (removed)
		{
			AdvancePreparationEpochLocked();
		}
		return removed;
	}

	bool AnimationHotReloadPublisher::RetainGeneration(
		const TerraEngine::UUID& assetId,
		std::uint64_t generation)
	{
		if (assetId == TerraEngine::EMPTY_UUID || generation == 0u)
		{
			return false;
		}
		std::scoped_lock lock(m_mutex);
		const bool isActive = [&]() {
			const auto active = m_active.find(MakeKey(assetId));
			return active != m_active.end() && active->second.view.generation == generation;
		}();
		const bool isPending = std::any_of(m_pendingRetirement.begin(), m_pendingRetirement.end(),
			[&](const PendingRetirement& pending) {
				return pending.assetId == assetId && pending.generation == generation;
			});
		if (!isActive && !isPending)
		{
			return false;
		}
		const std::string generationKey = MakeKey(assetId) + ":" + std::to_string(generation);
		std::uint32_t& count = m_generationRetains[generationKey];
		if (count == std::numeric_limits<std::uint32_t>::max())
		{
			return false;
		}
		++count;
		AdvancePreparationEpochLocked();
		return true;
	}

	bool AnimationHotReloadPublisher::ReleaseGeneration(
		const TerraEngine::UUID& assetId,
		std::uint64_t generation)
	{
		std::scoped_lock lock(m_mutex);
		const std::string generationKey = MakeKey(assetId) + ":" + std::to_string(generation);
		const auto retained = m_generationRetains.find(generationKey);
		if (retained == m_generationRetains.end() || retained->second == 0u)
		{
			return false;
		}
		if (--retained->second == 0u)
		{
			m_generationRetains.erase(retained);
		}
		AdvancePreparationEpochLocked();
		return true;
	}

	AnimationReloadOutcome AnimationHotReloadPublisher::Query(const TerraEngine::UUID& assetId) const
	{
		std::scoped_lock lock(m_mutex);
		AnimationReloadCandidate query;
		query.assetId = assetId;
		const auto found = m_active.find(MakeKey(assetId));
		if (found == m_active.end())
		{
			return MakeRejectedOutcome(query, m_active, "No generation has been published.");
		}
		AnimationReloadOutcome outcome;
		outcome.retainedLastKnownGood = true;
		outcome.activeGeneration = found->second.view.generation;
		outcome.publicationBoundary = found->second.view.publicationBoundary;
		outcome.reason = found->second.view.diagnostic;
		return outcome;
	}

	std::optional<AnimationReloadGenerationView> AnimationHotReloadPublisher::QueryGeneration(
		const TerraEngine::UUID& assetId) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_active.find(MakeKey(assetId));
		return found == m_active.end()
			? std::optional<AnimationReloadGenerationView>{}
			: std::optional<AnimationReloadGenerationView>{found->second.view};
	}

	std::optional<AnimationReloadInstanceView> AnimationHotReloadPublisher::QueryInstance(
		const TerraEngine::UUID& instanceId) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_instanceBindings.find(MakeKey(instanceId));
		return found == m_instanceBindings.end()
			? std::optional<AnimationReloadInstanceView>{}
			: std::optional<AnimationReloadInstanceView>{found->second.view};
	}

	std::vector<AnimationReloadGenerationView> AnimationHotReloadPublisher::Snapshot() const
	{
		std::scoped_lock lock(m_mutex);
		std::vector<AnimationReloadGenerationView> snapshot;
		snapshot.reserve(m_active.size());
		for (const auto& [key, generation] : m_active)
		{
			(void)key;
			snapshot.push_back(generation.view);
		}
		std::sort(snapshot.begin(), snapshot.end(), [](const auto& left, const auto& right) {
			return TerraEngine::ToHexString(left.assetId) < TerraEngine::ToHexString(right.assetId);
		});
		return snapshot;
	}

	bool AnimationHotReloadPublisher::HasInstanceReferenceLocked(
		const TerraEngine::UUID& assetId,
		std::uint64_t generation) const
	{
		return std::any_of(m_instanceBindings.begin(), m_instanceBindings.end(),
			[&](const auto& pair) {
				return pair.second.view.assetId == assetId && pair.second.view.generation == generation;
			});
	}

	bool AnimationHotReloadPublisher::HasGenerationReferenceLocked(
		const TerraEngine::UUID& assetId,
		std::uint64_t generation) const
	{
		const auto retained = m_generationRetains.find(
			MakeKey(assetId) + ":" + std::to_string(generation));
		return retained != m_generationRetains.end() && retained->second != 0u;
	}

	std::uint32_t AnimationHotReloadPublisher::RetireBefore(std::uint64_t protectedGeneration,
		RetirementCallback callback, void* callbackContext)
	{
		std::scoped_lock lock(m_mutex);
		if (protectedGeneration == 0u)
		{
			return 0u;
		}
		std::uint32_t retired = 0u;
		for (auto iterator = m_pendingRetirement.begin(); iterator != m_pendingRetirement.end(); )
		{
			if (iterator->generation < protectedGeneration
				&& !HasInstanceReferenceLocked(iterator->assetId, iterator->generation)
				&& !HasGenerationReferenceLocked(iterator->assetId, iterator->generation))
			{
				if (callback != nullptr)
				{
					callback(callbackContext, iterator->assetId, iterator->generation);
				}
				iterator = m_pendingRetirement.erase(iterator);
				++retired;
			}
			else
			{
				++iterator;
			}
		}
		if (retired != 0u)
		{
			AdvancePreparationEpochLocked();
		}
		return retired;
	}

	std::uint32_t AnimationHotReloadPublisher::RetireThroughBoundary(std::uint64_t completedBoundary,
		RetirementCallback callback, void* callbackContext)
	{
		std::scoped_lock lock(m_mutex);
		std::uint32_t retired = 0u;
		for (auto iterator = m_pendingRetirement.begin(); iterator != m_pendingRetirement.end(); )
		{
			if (iterator->safeAfterBoundary <= completedBoundary
				&& !HasInstanceReferenceLocked(iterator->assetId, iterator->generation)
				&& !HasGenerationReferenceLocked(iterator->assetId, iterator->generation))
			{
				if (callback != nullptr)
				{
					callback(callbackContext, iterator->assetId, iterator->generation);
				}
				iterator = m_pendingRetirement.erase(iterator);
				++retired;
			}
			else
			{
				++iterator;
			}
		}
		if (retired != 0u)
		{
			AdvancePreparationEpochLocked();
		}
		return retired;
	}

	bool AnimationHotReloadPublisher::HasPendingRetirements() const noexcept
	{
		std::scoped_lock lock(m_mutex);
		return !m_pendingRetirement.empty();
	}
}
