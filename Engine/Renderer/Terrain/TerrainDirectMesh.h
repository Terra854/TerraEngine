// TerrainDirectMesh.h -- Worker-safe direct-resident terrain mesh generation.

#pragma once

#include "Core/Export.h"
#include "Terrain/TerrainTypes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

struct Mesh;

namespace TerraEngine::Renderer::Terrain
{
	struct TerrainDirectMeshInput
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		float sampleSpacingMetres = 1.0F;
		TerraEngine::Terrain::TerrainAreaActiveRect activeRect{0u, 0u, 256u, 256u};
		std::span<const std::int32_t> heightMillimetres{};
		// One bit per terrain cell. An empty span means that every cell is solid.
		std::span<const std::uint8_t> holeCellBits{};
	};

	struct TerrainDirectMeshResult
	{
		std::unique_ptr<Mesh> mesh;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		std::string diagnostic;
		bool cancelled = false;

		[[nodiscard]] bool Succeeded() const noexcept
		{
			return mesh != nullptr && diagnostic.empty() && !cancelled;
		}
	};

	// Builds a canonical 16-byte packed meshlet payload without touching ECS,
	// ResourceManager, or the graphics backend. Callers run this on ThreadPool and
	// publish the completed value only at their protected frame boundary.
	TERRAENGINE_API TerrainDirectMeshResult BuildTerrainDirectMesh(
		const TerrainDirectMeshInput& input,
		const std::atomic_bool* cancellation = nullptr);
}
