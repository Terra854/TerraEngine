// TerrainCollisionSystem.h -- worker-built, protected terrain heightfield replacement.

#pragma once

#include "Core/SystemBase.h"
#include "Physics/Assets/ColliderRecipe.h"
#include "Physics/PhysicsTransactions.h"
#include "Terrain/TerrainAssetStore.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

namespace TerraEngine::Physics::Terrain
{
	enum class TerrainCollisionStatus : std::uint8_t { Absent, Pending, Current, Stale, Failed, Cancelled };
	struct TerrainCollisionCandidate
	{
		TerrainHeightFieldRecipe recipe{};
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Terrain::Generation sourcePublicationGeneration = 0u;
		bool complete = false;
		double buildLatencyMilliseconds = 0.0;
		const std::atomic_bool* cancellation = nullptr;
	};
	// Exported terrain is decoded from immutable package bytes rather than an
	// editor TerrainAssetStore. Keep its evidence in a distinct value type so the
	// protected collision boundary cannot accidentally skip authored-root checks.
	struct MountedTerrainCollisionCandidate
	{
		TerrainHeightFieldRecipe recipe{};
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Terrain::Generation sourcePublicationGeneration = 0u;
		bool complete = false;
	};
	// Mounted export scenes can contain several terrain assets, and a terrain
	// asset can itself span several collision areas. This ordered value map keeps
	// every distinct copied recipe until the protected Physics boundary consumes
	// it. The nested map also makes duplicate startup requests deterministic: the
	// most recently decoded immutable recipe replaces only its matching area.
	using MountedTerrainCollisionCandidates = std::map<TerraEngine::UUID,
		std::map<TerraEngine::Terrain::TerrainAreaCoordinate, MountedTerrainCollisionCandidate>>;
	struct TerrainCollisionRegionCandidate
	{
		TerraEngine::Terrain::TerrainAreaCoordinate coordinate{};
		TerrainHeightFieldRecipe recipe{};
	};
	struct TerrainCollisionRegionBatch
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Terrain::Generation sourcePublicationGeneration = 0u;
		std::vector<TerrainCollisionRegionCandidate> upserts;
		std::vector<TerraEngine::Terrain::TerrainAreaCoordinate> removals;
		bool complete = false;
		const std::atomic_bool* cancellation = nullptr;
	};
	struct TerrainCollisionSnapshot
	{
		TerrainCollisionStatus status = TerrainCollisionStatus::Absent;
		TerrainHeightFieldRecipe currentRecipe{};
		TerrainHeightFieldRecipe lastKnownGoodRecipe{};
		// Regional recipes are swapped as one value map. The legacy currentRecipe
		// remains a compatibility view for one-area physics backends.
		std::map<TerraEngine::Terrain::TerrainAreaCoordinate, TerrainHeightFieldRecipe> regions;
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Terrain::Generation sourcePublicationGeneration = 0u;
		// The previous full root identity remains visible while a newer collision
		// candidate is stale, cancelled, or failed, so repair UI never guesses from
		// the compact provider recipe hash.
		TerraEngine::Terrain::ContentHash lastKnownGoodSourceRootHash{};
		TerraEngine::Terrain::Generation lastKnownGoodSourcePublicationGeneration = 0u;
		// Mounted export terrain crosses two lifecycle boundaries: package decode
		// stages copied values, then the Physics phase calls the provider. These
		// counters make a missing protected workload distinguishable from a provider
		// rejection without exposing provider pointers or mutable queued recipes.
		std::uint32_t mountedPendingCandidateCount = 0u;
		std::uint64_t mountedCandidatesStaged = 0u;
		std::uint64_t mountedPublicationAttempts = 0u;
		std::uint64_t mountedPublicationCommits = 0u;
		std::uint64_t mountedPublicationRejections = 0u;
		double ageMilliseconds = 0.0;
		double buildLatencyMilliseconds = 0.0;
		std::string message;
	};
	TERRAENGINE_API TerrainCollisionCandidate PrepareTerrainCollisionCandidate(
		TerrainHeightFieldRecipe recipe, TerraEngine::Terrain::ContentHash sourceRootHash,
		TerraEngine::Terrain::Generation sourcePublicationGeneration, const std::atomic_bool* cancellation = nullptr);
	TERRAENGINE_API MountedTerrainCollisionCandidate PrepareMountedTerrainCollisionCandidate(
		TerrainHeightFieldRecipe recipe, TerraEngine::Terrain::ContentHash sourceRootHash,
		TerraEngine::Terrain::Generation sourcePublicationGeneration);
	TERRAENGINE_API bool PublishTerrainCollisionCandidate(
		TerrainCollisionSnapshot& inOutSnapshot, const TerrainCollisionCandidate& candidate,
		const TerraEngine::Terrain::TerrainAssetStore& store);
	TERRAENGINE_API bool PublishTerrainCollisionRegionBatch(
		TerrainCollisionSnapshot& inOutSnapshot, const TerrainCollisionRegionBatch& batch,
		const TerraEngine::Terrain::TerrainAssetStore& store, bool providerBatchAccepted = true);
	// The mounted path receives package-decoded values only. It calls the provider
	// before changing the published snapshot, so a rejection leaves the previous
	// physically-backed recipe available to navigation and diagnostics.
	TERRAENGINE_API bool PublishMountedTerrainCollisionCandidate(
		TerrainCollisionSnapshot& inOutSnapshot, const MountedTerrainCollisionCandidate& candidate,
		const std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)>& commit);

	class TERRAENGINE_API TerrainCollisionSystem final : public SystemsBase
	{
	public:
		static TerrainCollisionSystem& Instance();
		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;
		[[nodiscard]] TerrainCollisionSnapshot Snapshot() const;
		// The editor/runtime owner supplies the immutable store and provider commit
		// seam. Worker preparation never receives a live PhysicsWorld pointer.
		void ConfigurePublication(
			const TerraEngine::Terrain::TerrainAssetStore* store,
			std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commit,
			std::function<bool(const TerrainCollisionRegionBatch&)> commitRegions = {});
		void ClearPublication(const TerraEngine::Terrain::TerrainAssetStore* store) noexcept;
		void SetPreparedCandidate(TerrainCollisionCandidate candidate);
		// Stages one package-decoded terrain value for the declared protected Before
		// Physics boundary. It retains no mount, scene object, or ECS reference.
		[[nodiscard]] bool SetMountedRuntimeCandidate(MountedTerrainCollisionCandidate candidate);
		// Tests may supply a value-only receipt seam. Production uses PhysicsSystem's
		// active primary-world committer when this optional callback is empty.
		void ConfigureMountedRuntimeCommitter(
			std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commit);
		[[nodiscard]] bool PublishMountedRuntimeBeforePhysics();
		void SetPreparedRegionBatch(TerrainCollisionRegionBatch batch);
		// Physics worlds are generation-scoped. When Play Mode or a settings change
		// creates a fresh world, stage the latest authored terrain again so users do
		// not have to rebuild or attach collision components by hand.
		void RequestRepublish();
		void CancelPreparedCandidate(std::string message = "Terrain collision refresh was cancelled before publication.");
		[[nodiscard]] bool PublishBeforePhysics(const TerraEngine::Terrain::TerrainAssetStore& store);
	private:
		TerrainCollisionSystem() = default;
		std::atomic<bool> initialized{false};
		mutable std::mutex mutex;
		std::optional<TerrainCollisionCandidate> prepared;
		std::optional<TerrainCollisionCandidate> desired;
		MountedTerrainCollisionCandidates mountedPrepared;
		MountedTerrainCollisionCandidates mountedDesired;
		std::optional<TerrainCollisionRegionBatch> preparedRegions;
		std::optional<TerrainCollisionRegionBatch> desiredRegions;
		TerrainCollisionSnapshot snapshot{};
		const TerraEngine::Terrain::TerrainAssetStore* publicationStore = nullptr;
		std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commitReplacement;
		std::function<bool(const TerrainCollisionRegionBatch&)> commitRegionBatch;
		std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> mountedCommitter;
	};
}
