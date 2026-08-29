// TerrainSurfaceCache.h -- bounded, generation-tagged terrain control-page residency.

#pragma once

#include "Core/Export.h"
#include "Terrain/TerrainLayerStack.h"
#include "Terrain/TerrainTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <span>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	// Magenta is intentionally conspicuous and renderer-owned. Authored layers
	// cannot claim its reserved stable ID, so broken paint never aliases valid art.
	struct TerrainDiagnosticSurfaceMaterial
	{
		std::uint16_t stableLayerId = TerraEngine::Terrain::kTerrainDiagnosticFallbackLayerId;
		std::array<float, 4u> baseColor{1.0F, 0.0F, 1.0F, 1.0F};
		std::array<float, 3u> normal{0.0F, 0.0F, 1.0F};
		std::array<float, 3u> orm{1.0F, 1.0F, 0.0F};
		std::array<float, 3u> emissive{0.15F, 0.0F, 0.15F};
	};

	struct TerrainSurfaceControlResolution
	{
		TerraEngine::Terrain::TerrainResolvedWeightSample weights{};
		bool requiredControlMissing = false;
	};

	[[nodiscard]] TERRAENGINE_API const TerrainDiagnosticSurfaceMaterial&
		GetTerrainDiagnosticSurfaceMaterial() noexcept;
	[[nodiscard]] TERRAENGINE_API TerrainSurfaceControlResolution ResolveTerrainSurfaceControlSample(
		const TerraEngine::Terrain::TerrainWeightSample* sample,
		const TerraEngine::Terrain::TerrainLayerStack& stack) noexcept;

	struct TerrainSurfacePageKey
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash sourceHash{};
		std::uint64_t authoredGeneration = 0u;
		TerraEngine::Terrain::TerrainAreaActiveRect activeRect{0u, 0u, 256u, 256u};
		[[nodiscard]] bool IsComplete() const noexcept;
	};
	struct TerrainSurfacePageHandle
	{
		TerrainSurfacePageKey key{};
		std::uint32_t atlasTile = 0xffffffffu;
		std::size_t byteCount = 0u;
		std::uint64_t publicationGeneration = 0u;
		bool lastKnownGood = false;
		bool protectedFromEviction = false;
		[[nodiscard]] bool IsResident() const noexcept;
	};
	struct TerrainSurfaceCacheRequest { TerrainSurfacePageKey key{}; std::size_t byteCount = 0u; std::uint32_t priority = 0u; bool protectedFromEviction = false; };
	struct TerrainSurfaceCachePlan { std::uint64_t observedGeneration = 0u; std::vector<TerrainSurfacePageHandle> candidate; std::vector<TerrainSurfacePageKey> evicted; bool cancelled = false; };

	TERRAENGINE_API TerrainSurfaceCachePlan PrepareTerrainSurfaceCachePlan(
		std::uint64_t observedGeneration, std::span<const TerrainSurfacePageHandle> prior,
		std::vector<TerrainSurfaceCacheRequest> requests, std::size_t budgetBytes, const std::atomic_bool* cancellation = nullptr);
	TERRAENGINE_API bool PublishTerrainSurfaceCachePlan(std::vector<TerrainSurfacePageHandle>& inOutVisible, std::uint64_t& inOutGeneration, const TerrainSurfaceCachePlan& plan, std::uint64_t currentGeneration);
}
