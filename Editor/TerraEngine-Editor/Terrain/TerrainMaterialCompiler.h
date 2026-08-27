// TerrainMaterialCompiler.h -- Deterministic terrain ORM artifact preparation.

#pragma once

#include "Terrain/TerrainAreaCodec.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Terrain
{
	struct TerrainMaterialSourcePlane
	{
		std::span<const std::uint8_t> samples;
		TerraEngine::Terrain::ContentHash sourceIdentity{};
		TerraEngine::Terrain::Generation sourceGeneration = 0u;
		std::uint8_t neutralValue = 255u;
	};

	struct TerrainOrmCompileRequest
	{
		std::uint32_t width = 0u;
		std::uint32_t height = 0u;
		TerrainMaterialSourcePlane occlusion;
		TerrainMaterialSourcePlane roughness;
		TerrainMaterialSourcePlane metallic{ {}, {}, 0u, 0u };
		std::filesystem::path generatedContentDirectory;
	};

	struct TerrainOrmCompileReceipt
	{
		bool succeeded = false;
		TerraEngine::Terrain::ContentHash derivationKey{};
		std::filesystem::path artifactPath;
		std::vector<TerraEngine::Terrain::ContentHash> sourceDependencies;
		std::string message;
	};

	// Packs R=AO, G=roughness, B=metallic and routes the resulting image through
	// the repository-owned texture compiler. Missing optional planes use their
	// declared neutral byte and therefore never require a new runtime shader path.
	[[nodiscard]] TerrainOrmCompileReceipt CompileTerrainOrmArtifact(
		const TerrainOrmCompileRequest& request);
}
