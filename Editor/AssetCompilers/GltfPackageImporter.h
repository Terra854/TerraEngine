// GltfPackageImporter.h - Package-level glTF/GLB import entry point.
//
// Unlike the legacy mesh compiler path, this importer treats glTF as a package
// that owns meshes, materials, textures, and assignments. It returns a durable
// result contract that the AssetDatabase and MCP bridge can consume.

#pragma once

#include <filesystem>

#include "GltfImportTypes.h"
#include "ModelPackageImporter.h"

namespace TerraEngine::AssetPipeline
{
	using GltfPackageImportOptions = ModelPackageImportOptions;

	[[nodiscard]] bool IsGltfPackageSourceExtension(const std::filesystem::path& filePath);
	[[nodiscard]] GltfImportResult ImportGltfPackage(const GltfPackageImportOptions& options);
}
