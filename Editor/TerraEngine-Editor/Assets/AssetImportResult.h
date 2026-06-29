// AssetImportResult.h - Persistence helpers for package import diagnostics.
//
// Generated package assets may import successfully while still producing useful
// warnings. Persisting the latest result beside metadata gives the inspector,
// tests, and MCP a stable place to read those diagnostics.

#pragma once

#include <filesystem>

#include "Editor/AssetCompilers/GltfImportTypes.h"
#include "Editor/AssetCompilers/ModelPackageImportTypes.h"

namespace TerraEngine::AssetPipeline
{
	[[nodiscard]] std::filesystem::path GetImportResultPath(const std::filesystem::path& sourceAssetPath);
	[[nodiscard]] bool SaveModelPackageImportResult(const std::filesystem::path& sourceAssetPath, const ModelPackageImportResult& result);
	[[nodiscard]] bool LoadModelPackageImportResult(const std::filesystem::path& sourceAssetPath, ModelPackageImportResult& outResult);
	[[nodiscard]] bool SaveImportResult(const std::filesystem::path& sourceAssetPath, const GltfImportResult& result);
	[[nodiscard]] bool LoadImportResult(const std::filesystem::path& sourceAssetPath, GltfImportResult& outResult);
}
