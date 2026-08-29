// TerrainRuntimeProxyLoader.h -- Worker-safe terrain proxy preparation from TEPAK.

#pragma once

#include "Core/Export.h"
#include "Physics/PhysicsTypes.h"
#include "Renderer/Terrain/TerrainDirectMesh.h"
#include "Renderer/Terrain/TerrainPageCooker.h"

#include <algorithm>
#include <string_view>
#include <vector>

class ContentMount;

namespace TerraEngine::Runtime
{
	struct MountedTerrainAreaRuntime
	{
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		TerraEngine::Terrain::Generation authoredGeneration = 0U;
		TerraEngine::Terrain::TerrainAreaActiveRect activeRect{};
		bool implicitDefault = false;
		TerraEngine::Renderer::Terrain::TerrainDirectMeshResult directMesh;
		TerraEngine::Renderer::Terrain::TerrainPageCookResult pageCook;
		TerraEngine::Physics::TerrainHeightFieldRecipe collisionRecipe;
		bool missingOuterHalo = false;

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return authoredHash.IsValid() && authoredGeneration != 0U
				&& directMesh.Succeeded() && pageCook.complete && collisionRecipe.IsComplete();
		}
	};

	struct MountedTerrainRuntimeResult
	{
		// The complete immutable area set is the canonical runtime result. The two
		// single values below mirror area {0,0} only for callers that have not yet
		// moved their collision/direct-route bootstrap to area packets.
		std::vector<MountedTerrainAreaRuntime> areas;
		TerraEngine::Renderer::Terrain::TerrainDirectMeshResult renderProxy;
		TerraEngine::Physics::TerrainHeightFieldRecipe collisionRecipe;
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Terrain::Generation sourcePublicationGeneration = 0U;
		TerraEngine::Terrain::TerrainSampleBounds authoredBounds{};

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return !areas.empty()
				&& std::all_of(areas.begin(), areas.end(), [](const auto& area) { return area.Succeeded(); })
				&& renderProxy.Succeeded() && collisionRecipe.IsComplete()
				&& sourceRootHash.IsValid() && sourcePublicationGeneration != 0U
				&& authoredBounds.IsValid();
		}
	};

	// Decodes the mounted origin area once and prepares both default runtime
	// products. Terrain therefore becomes visible and collidable from the same
	// immutable package generation without requiring a collider component.
	[[nodiscard]] TERRAENGINE_API MountedTerrainRuntimeResult BuildMountedTerrainRuntime(
		const ::ContentMount& mount,
		const TerraEngine::UUID& expectedAssetId,
		std::string_view logicalTerrainPath);

	// Reads one terrain's immutable origin area exclusively from the supplied
	// content mount and builds the same direct mesh consumed by editor previews.
	// The result remains detached from ResourceManager, ECS, and renderer state so
	// callers may execute this preparation concurrently on ThreadPool workers.
	[[nodiscard]] TERRAENGINE_API TerraEngine::Renderer::Terrain::TerrainDirectMeshResult
		BuildMountedTerrainDirectMesh(
			const ::ContentMount& mount,
			const TerraEngine::UUID& expectedAssetId,
			std::string_view logicalTerrainPath);
}
