// GltfPackageImporter.h - Package-level glTF/GLB import entry point.
//
// Unlike the legacy mesh compiler path, this importer treats glTF as a package
// that owns meshes, materials, textures, and assignments. It returns a durable
// result contract that the AssetDatabase and MCP bridge can consume.

#pragma once

#include <filesystem>

#include "GltfImportTypes.h"
#include "Utilities/UUID.h"

namespace TerraEngine::AssetPipeline
{
	struct GltfPackageImportOptions
	{
		std::filesystem::path sourcePath;
		std::filesystem::path assetRoot;
		std::filesystem::path buildRoot;
		std::filesystem::path sourceRelativePath;
		TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID shaderCollectionId = TerraEngine::EMPTY_UUID;
		bool forceReimport = false;
	};

	[[nodiscard]] bool IsGltfPackageSourceExtension(const std::filesystem::path& filePath);
	[[nodiscard]] GltfImportResult ImportGltfPackage(const GltfPackageImportOptions& options);
}
