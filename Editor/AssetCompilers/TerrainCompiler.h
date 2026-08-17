// TerrainCompiler.h -- Checked heightfield decoding for terrain authoring.

#pragma once

#include "Utilities/UUID.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
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
		float heightScaleMetres = 1.0F;
		float heightOffsetMetres = 0.0F;
		float sampleSpacingMetres = 1.0F;
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
}
