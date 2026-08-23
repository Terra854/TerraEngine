// TerrainRuntimeProxyLoader.h -- Worker-safe terrain proxy preparation from TEPAK.

#pragma once

#include "Core/Export.h"
#include "Renderer/Terrain/TerrainDirectMesh.h"

#include <string_view>

class ContentMount;

namespace TerraEngine::Runtime
{
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
