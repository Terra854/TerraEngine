// AnimationHotReload.h - atomic last-known-good animation generation publisher.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Core/Export.h"
#include "Utilities/UUID.h"

namespace TerraEngine::Animation
{
	enum class AnimationReloadAssetType : std::uint8_t
	{
		Skeleton,
		AnimationClip,
		AnimationController,
		AnimationMask,
		RetargetProfile,
		MotionDatabase,
		RagdollProfile
	};

	struct AnimationReloadDependency
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t expectedGeneration = 0u;
		std::uint64_t compatibilityFingerprint = 0u;
		bool required = true;
	};

	struct AnimationReloadCandidate
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t generation = 0u;
		std::uint64_t compatibilityFingerprint = 0u;
		bool parsed = false;
		bool dependenciesReady = false;
		// New fields follow the original five-field aggregate prefix so existing
		// source initializers keep their stable meaning while gaining typed data.
		AnimationReloadAssetType assetType = AnimationReloadAssetType::AnimationClip;
		std::uint64_t contentFingerprint = 0u;
		std::vector<AnimationReloadDependency> dependencies;
		// False preserves externally registered/manual edges for legacy and
		// generic reloads. True means dependencies is the compiler-owned complete
		// declaration, including the intentional empty declaration.
		bool dependenciesAreCompleteDeclaration = false;
		// Publication identity uses the complete validated cooked-content hash.
		// contentFingerprint remains a compact diagnostic prefix only.
		std::array<std::uint8_t, 32U> contentHash{};
		bool contentHashPresent = false;
	};

	// Preparation and publication are deliberately separate so resource-cache
	// owners can stage their own generation first, then join the same protected
	// commit boundary without allocating publisher state inside that boundary.
	enum class AnimationReloadCommitStatus : std::uint8_t
	{
		NotAttempted,
		Committed,
		RejectedInvalidPreparation,
		RejectedStalePreparation,
		RejectedExternalPrecommit
	};

	struct AnimationReloadOutcome
	{
		// prepared is true only after the whole candidate batch has passed
		// validation and all allocation-heavy publisher state has been staged.
		bool prepared = false;
		bool published = false;
		bool retainedLastKnownGood = false;
		AnimationReloadCommitStatus commitStatus = AnimationReloadCommitStatus::NotAttempted;
		std::uint64_t activeGeneration = 0u;
		std::uint64_t publicationBoundary = 0u;
		std::uint32_t switchedInstanceCount = 0u;
		std::vector<TerraEngine::UUID> invalidatedDependents;
		std::string reason;
	};

	struct AnimationReloadGenerationView
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		AnimationReloadAssetType assetType = AnimationReloadAssetType::AnimationClip;
		std::uint64_t generation = 0u;
		std::uint64_t compatibilityFingerprint = 0u;
		std::uint64_t contentFingerprint = 0u;
		std::array<std::uint8_t, 32U> contentHash{};
		std::uint64_t publicationBoundary = 0u;
		bool dependencyInvalidated = false;
		std::string diagnostic;
	};

	struct AnimationReloadInstanceView
	{
		TerraEngine::UUID instanceId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t generation = 0u;
		std::uint64_t switchBoundary = 0u;
	};

	class TERRAENGINE_API AnimationHotReloadPublisher final
	{
		struct PreparedPublicationState;

	public:
		using PrecommitCallback = bool (*)(void* context) noexcept;
		// Runs only after publisher tables have been swapped. Coordinated cache
		// owners use it to release locks that deliberately covered the whole
		// cross-owner publication boundary.
		using PostcommitCallback = void (*)(void* context) noexcept;

		// PreparedBatch owns a private, movable snapshot of every publisher table
		// changed by publication. Callers can inspect validation outcomes without
		// gaining access to the publisher's private map representation.
		class PreparedBatch final
		{
		public:
			PreparedBatch() noexcept;
			~PreparedBatch();
			PreparedBatch(PreparedBatch&& other) noexcept;
			PreparedBatch& operator=(PreparedBatch&& other) noexcept;
			PreparedBatch(const PreparedBatch&) = delete;
			PreparedBatch& operator=(const PreparedBatch&) = delete;

			[[nodiscard]] bool Accepted() const noexcept;
			[[nodiscard]] bool Committed() const noexcept;
			[[nodiscard]] AnimationReloadCommitStatus CommitStatus() const noexcept;
			[[nodiscard]] std::uint64_t PublicationBoundary() const noexcept;
			[[nodiscard]] std::span<const AnimationReloadOutcome> Outcomes() const noexcept;
			[[nodiscard]] std::vector<AnimationReloadOutcome> ReleaseOutcomes() && noexcept;

		private:
			friend class AnimationHotReloadPublisher;

			bool m_accepted = false;
			AnimationReloadCommitStatus m_commitStatus = AnimationReloadCommitStatus::NotAttempted;
			std::uint64_t m_publicationBoundary = 0u;
			std::vector<AnimationReloadOutcome> m_outcomes;
			std::unique_ptr<PreparedPublicationState> m_state;
		};

		explicit AnimationHotReloadPublisher(
			std::uint32_t maximumAssets = 4096u,
			std::uint32_t maximumDependencyEdges = 16384u,
			std::uint32_t maximumInstances = 65536u,
			std::uint32_t maximumPendingRetirements = 16384u);
		// Project/session teardown calls Reset only at a protected editor boundary.
		// It releases bookkeeping after all consumers have stopped using the prior
		// project, preventing GUID collisions from leaking across project switches.
		void Reset();
		AnimationReloadOutcome Publish(const AnimationReloadCandidate& candidate);
		[[nodiscard]] PreparedBatch PrepareBatch(
			std::span<const AnimationReloadCandidate> candidates,
			std::uint64_t publicationBoundary = 0u);
		// The optional no-throw callback executes after stale-state validation while
		// the publisher is locked, immediately before the no-fail table swaps. It
		// must not re-enter this publisher. A false result leaves publisher state
		// untouched, allowing ResourceManager or AssetDatabase to fail atomically.
		AnimationReloadCommitStatus CommitPreparedBatch(
			PreparedBatch& prepared,
			PrecommitCallback externalPrecommit = nullptr,
			void* externalContext = nullptr,
			PostcommitCallback externalPostcommit = nullptr) noexcept;
		std::vector<AnimationReloadOutcome> PublishBatch(
			std::span<const AnimationReloadCandidate> candidates,
			std::uint64_t publicationBoundary = 0u);
		bool RegisterDependency(const TerraEngine::UUID& dependentAssetId, const TerraEngine::UUID& dependencyAssetId);
		bool UnregisterDependency(const TerraEngine::UUID& dependentAssetId, const TerraEngine::UUID& dependencyAssetId);
		bool BindInstance(
			const TerraEngine::UUID& instanceId,
			const TerraEngine::UUID& assetId,
			std::uint64_t expectedGeneration = 0u);
		bool UnbindInstance(const TerraEngine::UUID& instanceId);
		// Frame packets, GPU work, and managed handles retain the immutable
		// generation they captured. Publication may switch active instances, but
		// retirement cannot reclaim that old payload until every such owner releases
		// its explicit reference after the relevant protected boundary.
		bool RetainGeneration(const TerraEngine::UUID& assetId, std::uint64_t generation);
		bool ReleaseGeneration(const TerraEngine::UUID& assetId, std::uint64_t generation);
		[[nodiscard]] AnimationReloadOutcome Query(const TerraEngine::UUID& assetId) const;
		[[nodiscard]] std::optional<AnimationReloadGenerationView> QueryGeneration(
			const TerraEngine::UUID& assetId) const;
		[[nodiscard]] std::optional<AnimationReloadInstanceView> QueryInstance(
			const TerraEngine::UUID& instanceId) const;
		[[nodiscard]] std::vector<AnimationReloadGenerationView> Snapshot() const;
		using RetirementCallback = void (*)(void* context, const TerraEngine::UUID& assetId,
			std::uint64_t generation) noexcept;
		// Coordinated resource owners must provide the callback so publisher metadata
		// and the matching immutable payload ownership retire together.
		std::uint32_t RetireBefore(std::uint64_t protectedGeneration,
			RetirementCallback callback = nullptr, void* callbackContext = nullptr);
		std::uint32_t RetireThroughBoundary(std::uint64_t completedBoundary,
			RetirementCallback callback = nullptr, void* callbackContext = nullptr);
		[[nodiscard]] bool HasPendingRetirements() const noexcept;

	private:
		struct PublishedGeneration
		{
			AnimationReloadGenerationView view;
			std::vector<AnimationReloadDependency> dependencies;
		};
		struct PendingRetirement
		{
			TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
			std::uint64_t generation = 0u;
			std::uint64_t safeAfterBoundary = 0u;
		};
		struct InstanceBinding
		{
			AnimationReloadInstanceView view;
		};

		[[nodiscard]] static std::string MakeKey(const TerraEngine::UUID& id);
		[[nodiscard]] static AnimationReloadOutcome MakeRejectedOutcome(
			const AnimationReloadCandidate& candidate,
			const std::unordered_map<std::string, PublishedGeneration>& active,
			std::string reason);
		[[nodiscard]] bool HasInstanceReferenceLocked(
			const TerraEngine::UUID& assetId,
			std::uint64_t generation) const;
		[[nodiscard]] bool HasGenerationReferenceLocked(
			const TerraEngine::UUID& assetId,
			std::uint64_t generation) const;
		void AdvancePreparationEpochLocked() noexcept;

		std::uint32_t m_maximumAssets = 4096u;
		std::uint32_t m_maximumDependencyEdges = 16384u;
		std::uint32_t m_maximumInstances = 65536u;
		std::uint32_t m_maximumPendingRetirements = 16384u;
		std::uint64_t m_latestPublicationBoundary = 0u;
		std::uint64_t m_preparationEpoch = 1u;
		std::uint32_t m_dependencyEdgeCount = 0u;
		std::uint32_t m_compilerDependencyEdgeCount = 0u;
		mutable std::mutex m_mutex;
		std::unordered_map<std::string, PublishedGeneration> m_active;
		std::vector<PendingRetirement> m_pendingRetirement;
		std::unordered_map<std::string, std::unordered_set<std::string>> m_dependentsByDependency;
		std::unordered_map<std::string, std::unordered_set<std::string>> m_compilerDependentsByDependency;
		std::unordered_map<std::string, InstanceBinding> m_instanceBindings;
		std::unordered_map<std::string, std::uint32_t> m_generationRetains;
	};
}
