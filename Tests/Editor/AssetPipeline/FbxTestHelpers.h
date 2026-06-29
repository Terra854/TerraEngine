// FbxTestHelpers.h - Shared helpers for FBX package importer tests.
//
// The FBX fixtures are binary upstream assets from the vendored Assimp tree, so
// tests centralize their paths and temp output roots instead of copying those
// blobs into every test file.

#pragma once

#include "FbxPackageImporter.h"

#include <filesystem>
#include <string_view>

namespace TerraEngine::EditorTests
{
	inline std::filesystem::path AssimpFbxFixturePath(std::string_view fixtureName)
	{
		return std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "External/assimp/test/models/FBX" / fixtureName;
	}

	inline TerraEngine::AssetPipeline::ModelPackageImportResult ImportFbxFixture(
		std::string_view fixtureName,
		const TerraEngine::UUID& packageGuid = TerraEngine::GenerateUUID())
	{
		const std::filesystem::path fixturePath = AssimpFbxFixturePath(fixtureName);
		const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineFbxImporterTests" / fixtureName;

		TerraEngine::AssetPipeline::FbxPackageImportOptions options{};
		options.sourcePath = fixturePath;
		options.assetRoot = tempRoot / "Assets";
		options.buildRoot = tempRoot / "Build";
		options.sourceRelativePath = std::filesystem::path("Models") / fixturePath.filename();
		options.packageGuid = packageGuid;
		options.forceReimport = true;
		return TerraEngine::AssetPipeline::ImportFbxPackage(options);
	}
}
