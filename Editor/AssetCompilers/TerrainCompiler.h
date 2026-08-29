// TerrainCompiler.h -- Checked heightfield decoding for terrain authoring.

#pragma once

#include "Utilities/UUID.h"
#include "Terrain/TerrainTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <optional>
#include <utility>
#include <vector>

namespace TerraEngine::AssetPipeline
{
	enum class TerrainSourceFormat : std::uint8_t { Flat, Png16, Raw16LittleEndian };
	struct TerrainImportRequest
	{
		TerrainSourceFormat sourceFormat = TerrainSourceFormat::Flat;
		std::filesystem::path sourcePath;
		std::uint32_t rawWidth = 0U;
		std::uint32_t rawHeight = 0U;
		std::uint32_t flatWidth = 256U;
		std::uint32_t flatDepth = 256U;
		float heightScaleMetres = 1.0F;
		float heightOffsetMetres = 0.0F;
		float sampleSpacingMetres = 1.0F;
		std::size_t maximumSamples = 256U * 256U * 64U;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	};
	struct TerrainHeightfield
	{
		std::uint32_t width = 0U;
		std::uint32_t height = 0U;
		std::vector<std::int32_t> heightsMm;
	};
	struct TerrainCompileResult
	{
		bool succeeded = false;
		bool cancelled = false;
		TerrainHeightfield heightfield{};
		std::string message;
	};
	using TerrainCompileProgressCallback = std::function<bool(float progress, std::string_view stage, std::string_view message)>;

	// Decode input without publishing it. The creation service validates and
	// atomically registers the parent terrain asset only after this returns success.
	[[nodiscard]] TerrainCompileResult CompileTerrainHeightfield(
		const TerrainImportRequest& request, TerrainCompileProgressCallback progress = {});
	// Parent descriptor dimensions are compatibility metadata only. Derive them
	// from the validated root bounds so stale pre-resize dimensions never regain
	// authority during save or export.
	[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
		TerrainCompatibilityDimensions(TerraEngine::Terrain::TerrainSampleBounds bounds) noexcept;
}
