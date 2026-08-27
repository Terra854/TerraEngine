// TerrainRuntimeBootstrap.h -- protected export-scene terrain request capture.

#pragma once

#include "Core/Export.h"
#include "Terrain/TerrainMaterial.h"
#include "Utilities/UUID.h"

#include <string>
#include <vector>

namespace TerraEngine::Runtime
{
	struct MountedTerrainBootstrapRequest
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::string logicalPath;
		TerraEngine::Terrain::TerrainMaterialSettings materialSettings{};
	};

	// This function runs only at the protected export-runtime lifecycle boundary.
	// It copies durable terrain IDs, paths, and material settings out of live scene
	// storage so subsequent decode/cook jobs never access objects or components.
	[[nodiscard]] TERRAENGINE_API std::vector<MountedTerrainBootstrapRequest>
		CaptureMountedTerrainBootstrapRequests();
}
