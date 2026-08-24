// TerrainRuntimeProxyLoader.h -- Worker-safe terrain proxy preparation from TEPAK.

#pragma once

#include "Core/Export.h"
#include "Physics/PhysicsTypes.h"
#include "Renderer/Terrain/TerrainDirectMesh.h"

#include <string_view>

class ContentMount;

namespace TerraEngine::Runtime
{
	struct MountedTerrainRuntimeResult
	{
		TerraEngine::Renderer::Terrain::TerrainDirectMeshResult renderProxy;
		TerraEngine::Physics::TerrainHeightFieldRecipe collisionRecipe;
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Terrain::Generation sourcePublicationGeneration = 0U;

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return renderProxy.Succeeded() && collisionRecipe.IsComplete()
				&& sourceRootHash.IsValid() && sourcePublicationGeneration != 0U;
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
