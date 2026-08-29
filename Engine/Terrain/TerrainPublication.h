// TerrainPublication.h -- Protected, all-or-nothing terrain root publication.

#pragma once

#include "Terrain/TerrainMutationQueue.h"

namespace TerraEngine::Terrain
{
	// A resized root is consumable only when every downstream view has prepared
	// the same immutable generation. This value contains identities and readiness
	// flags only; worker jobs never pass live renderer or physics handles here.
	struct TerrainCommonReadyCandidate
	{
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		ContentHash rootHash{};
		ContentHash derivedHaloIdentity{};
		Generation publicationGeneration = 0u;
		Generation renderGeneration = 0u;
		Generation pickingGeneration = 0u;
		Generation spatialGeneration = 0u;
		Generation residencyGeneration = 0u;
		Generation collisionGeneration = 0u;
		bool rootComplete = false;
		bool derivedHalosComplete = false;
		bool renderComplete = false;
		bool pickingComplete = false;
		bool spatialComplete = false;
		bool residencyRetirementsComplete = false;
		bool demandedCollisionComplete = false;
	};

	// Prevalidation is side-effect free, allowing all failure paths to preserve
	// the previously published complete generation.
	TERRAENGINE_API TerrainCodecResult ValidateTerrainCommonReadyCandidate(
		const TerrainCommonReadyCandidate& candidate,
		const TerrainAssetStore& store);

	// Terminal editor state and history must be derived from the exact immutable
	// request that crossed the protected boundary, never reconstructed afterward.
	struct TerrainAuthoringCompletion
	{
		TerrainEditTransaction transaction;
		TerrainTransactionReceipt receipt;
	};

	class TERRAENGINE_API TerrainPublication
	{
	public:
		explicit TerrainPublication(TerrainAssetStore& store) : assetStore(store) {}
		void SetInjectFailureForTests(bool value) noexcept { injectFailureForTests = value; }
		[[nodiscard]] TerrainTransactionReceipt Publish(TerrainEditTransaction transaction);
		[[nodiscard]] std::vector<TerrainTransactionReceipt> PublishQueued(TerrainMutationQueue& queue);
		[[nodiscard]] std::vector<TerrainAuthoringCompletion> PublishQueuedCompletions(TerrainMutationQueue& queue);

	private:
		TerrainAssetStore& assetStore;
		bool injectFailureForTests = false;
	};
}
