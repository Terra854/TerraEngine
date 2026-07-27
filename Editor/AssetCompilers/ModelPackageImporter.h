// ModelPackageImporter.h - Format-neutral package importer entry points.
//
// Editor systems should ask this layer whether a source file is a model package
// before falling back to the legacy loose mesh importer. The dispatcher keeps
// glTF and FBX behavior aligned at the AssetDatabase, Project Browser, inspector,
// and MCP automation boundaries.

#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

#include "ModelPackageImportTypes.h"
#include "Utilities/UUID.h"

namespace TerraEngine::AssetPipeline
{
	struct ModelPackageImportOptions
	{
		std::filesystem::path sourcePath;
		std::filesystem::path assetRoot;
		std::filesystem::path buildRoot;
		std::filesystem::path sourceRelativePath;
		TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID shaderCollectionId = TerraEngine::EMPTY_UUID;
		GeneratedModelMeshVirtualGeometrySettings virtualGeometrySettings;
		bool hasVirtualGeometrySettings = false;
		bool forceReimport = false;
		// progressCallback may be invoked from a background import worker. The
		// receiver must copy the values into thread-safe UI state before drawing.
		std::function<void(float progress, std::string_view stage, std::string_view message)> progressCallback;
		// Import workers poll this callback only at bounded transaction points. A
		// true result rejects the staged generation before live outputs are exposed.
		std::function<bool()> cancellationRequested;
	};

	[[nodiscard]] bool IsModelPackageSourceExtension(const std::filesystem::path& filePath);
	[[nodiscard]] bool IsFbxPackageSourceExtension(const std::filesystem::path& filePath);
	[[nodiscard]] const char* GetModelPackageImporterName(const std::filesystem::path& filePath);
	// The recipe version invalidates otherwise complete generated packages when
	// importer behavior changes without changing the source asset itself.
	[[nodiscard]] const char* GetModelPackageImporterVersion(const std::filesystem::path& filePath);
	[[nodiscard]] bool IsModelPackageImporterName(std::string_view importerName);
	[[nodiscard]] ModelPackageImportResult ImportModelPackage(const ModelPackageImportOptions& options);
}
