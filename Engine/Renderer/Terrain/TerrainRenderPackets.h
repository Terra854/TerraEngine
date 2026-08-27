// TerrainRenderPackets.h -- Immutable terrain view, pick, and VG packet values.

#pragma once

#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Renderer/Terrain/TerrainLodSelection.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
#include "Resource/VirtualGeometryPagePublication.h"

#include <optional>
#include <span>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	struct TerrainDrawMappingPacket
	{
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		double sampleSpacingMetres = 1.0;
		std::array<double, 2u> localSampleOrigin{0.0, 0.0};
		std::uint32_t controlExtentSamples = kTerrainControlExtentSamples;
		std::array<TerrainGpuLayerPhase, kTerrainGpuLayerLimit> layerPhases{};
		std::uint32_t layerCount = 0u;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct TerrainViewPacket
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerraEngine::Terrain::ContentHash authoredHash{};
		TerraEngine::Terrain::ContentHash pageSourceIdentity{};
		std::uint64_t authoredGeneration = 0u;
		std::uint32_t pageId = 0u;
		std::uint16_t requestedLod = 0u;
		// A complete-cut identity groups pages chosen for one view. It blocks a
		// renderer from mixing arbitrary resident detail with a regional fallback.
		TerraEngine::Terrain::ContentHash completeCutIdentity{};
		TerraEngine::Terrain::ContentHash neighborDependencyHash{};
		TerraEngine::Terrain::ContentHash surfaceControlIdentity{};
		std::uint64_t derivedGeneration = 0u;
		// This copied draw value is the only source for physical surface mapping.
		// It deliberately contains no translated render-world position.
		TerrainDrawMappingPacket mapping{};
		bool fallback = false;
	};

	struct TerrainPickIdentity
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		std::uint32_t localCellX = 0u;
		std::uint32_t localCellZ = 0u;
		TerraEngine::Terrain::ContentHash authoredHash{};
		std::uint64_t authoredGeneration = 0u;
		TerraEngine::Terrain::ContentHash pageSourceIdentity{};
		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct TerrainPreparedRenderPacket
	{
		TerrainViewPacket view{};
		TerrainPickIdentity pick{};
		TerrainDrawMappingPacket mapping{};
		TerraEngine::Resource::VirtualGeometryPagePublicationCandidate candidate{};
		bool usesLastKnownGood = false;
		bool usesRegionalFallback = false;
	};

	enum class TerrainGeometryRoute : std::uint8_t { Direct, PageBacked };
	enum class TerrainRenderPurpose : std::uint8_t { Camera, Shadow, Picking };

	struct TerrainAreaRenderRequest
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		TerraEngine::Terrain::TerrainAreaCoordinate area{};
		TerrainGeometryRoute route = TerrainGeometryRoute::PageBacked;
		TerrainRenderPurpose purpose = TerrainRenderPurpose::Camera;
		std::uint32_t desiredPageId = kInvalidTerrainPageId;
		std::uint32_t localCellX = 0u;
		std::uint32_t localCellZ = 0u;
		std::uint32_t priority = 0u;
	};

	struct TerrainAreaRenderPacket
	{
		TerrainGeometryRoute route = TerrainGeometryRoute::PageBacked;
		TerrainRenderPurpose purpose = TerrainRenderPurpose::Camera;
		TerraEngine::Terrain::ContentHash stableAreaIdentity{};
		TerraEngine::Terrain::ContentHash controlIdentity{};
		TerraEngine::Terrain::ContentHash holeIdentity{};
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		TerrainPickIdentity pick{};
		std::uint32_t desiredPageId = kInvalidTerrainPageId;
		std::uint32_t actualPageId = kInvalidTerrainPageId;
		bool usesAncestorPage = false;
		bool castsShadow = false;
	};

	struct TerrainAreaRenderPacketResult
	{
		std::vector<TerrainAreaRenderPacket> packets;
		std::vector<TerrainPageDemand> demands;
		bool complete = false;
	};

	[[nodiscard]] TERRAENGINE_API TerrainAreaRenderPacketResult PrepareTerrainAreaRenderPackets(
		const TerrainAssetRenderSnapshot& snapshot,
		std::span<const TerrainAreaRenderRequest> requests,
		std::span<const TerrainPageResidencyEntry> residency);

	// Resolve the immutable material publication for one area and optionally one
	// page-local sample origin. Control coordinates remain area-local while every
	// surface layer receives its independently bounded physical phase.
	TERRAENGINE_API std::optional<TerrainDrawMappingPacket> PrepareTerrainDrawMappingPacket(
		const TerrainMaterialRuntimeBinding& material,
		TerraEngine::Terrain::TerrainAreaCoordinate area,
		std::array<double, 2u> localSampleOrigin = {0.0, 0.0});

	TERRAENGINE_API std::optional<TerrainPreparedRenderPacket> PrepareTerrainRenderPacket(
		const TerrainViewPacket& view,
		const TerrainPageCookResult& cooked,
		const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication>& priorVisiblePages);
}
