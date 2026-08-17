// TerrainCollisionSystem.h -- worker-built, protected terrain heightfield replacement.

#pragma once

#include "Core/SystemBase.h"
#include "Physics/Assets/ColliderRecipe.h"
#include "Physics/PhysicsTransactions.h"
#include "Terrain/TerrainAssetStore.h"

#include <atomic>
#include <functional>
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
	struct TerrainCollisionSnapshot
	{
		TerrainCollisionStatus status = TerrainCollisionStatus::Absent;
		TerrainHeightFieldRecipe currentRecipe{};
		TerrainHeightFieldRecipe lastKnownGoodRecipe{};
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Terrain::Generation sourcePublicationGeneration = 0u;
		// The previous full root identity remains visible while a newer collision
		// candidate is stale, cancelled, or failed, so repair UI never guesses from
		// the compact provider recipe hash.
		TerraEngine::Terrain::ContentHash lastKnownGoodSourceRootHash{};
		TerraEngine::Terrain::Generation lastKnownGoodSourcePublicationGeneration = 0u;
		double ageMilliseconds = 0.0;
		double buildLatencyMilliseconds = 0.0;
		std::string message;
	};
	TERRAENGINE_API TerrainCollisionCandidate PrepareTerrainCollisionCandidate(
		TerrainHeightFieldRecipe recipe, TerraEngine::Terrain::ContentHash sourceRootHash,
		TerraEngine::Terrain::Generation sourcePublicationGeneration, const std::atomic_bool* cancellation = nullptr);
	TERRAENGINE_API bool PublishTerrainCollisionCandidate(
		TerrainCollisionSnapshot& inOutSnapshot, const TerrainCollisionCandidate& candidate,
		const TerraEngine::Terrain::TerrainAssetStore& store);

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
			std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commit);
		void ClearPublication(const TerraEngine::Terrain::TerrainAssetStore* store) noexcept;
		void SetPreparedCandidate(TerrainCollisionCandidate candidate);
		void CancelPreparedCandidate(std::string message = "Terrain collision refresh was cancelled before publication.");
		[[nodiscard]] bool PublishBeforePhysics(const TerraEngine::Terrain::TerrainAssetStore& store);
	private:
		TerrainCollisionSystem() = default;
		std::atomic<bool> initialized{false};
		mutable std::mutex mutex;
		std::optional<TerrainCollisionCandidate> prepared;
		TerrainCollisionSnapshot snapshot{};
		const TerraEngine::Terrain::TerrainAssetStore* publicationStore = nullptr;
		std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commitReplacement;
	};
}
