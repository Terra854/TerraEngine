// ModelPackageImporter.h - Format-neutral package importer entry points.
//
// Editor systems should ask this layer whether a source file is a model package
// before falling back to the legacy loose mesh importer. The dispatcher keeps
// glTF and FBX behavior aligned at the AssetDatabase, Project Browser, inspector,
// and MCP automation boundaries.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "ModelPackageImportTypes.h"
#include "Utilities/UUID.h"

namespace TerraEngine::AssetPipeline
{
	struct ModelPackageMeshVirtualGeometryOverride
	{
		TerraEngine::UUID meshGuid = TerraEngine::EMPTY_UUID;
		GeneratedModelMeshVirtualGeometrySettings settings;
	};

	// A process can prepare complete outputs in owner-selected private roots.
	// The owner retains and removes these roots after guarded publication.
	struct ModelPackagePreparationRoots
	{
		std::filesystem::path assetRoot;
		std::filesystem::path buildRoot;
	};

	struct ModelPackageImportOptions
	{
		std::filesystem::path sourcePath;
		std::filesystem::path assetRoot;
		std::filesystem::path buildRoot;
		std::filesystem::path sourceRelativePath;
		// Authored dependency spelling uses this read root while generated files
		// use assetRoot. Empty preserves the historical direct-import behavior.
		std::filesystem::path sourceAssetRoot;
		std::optional<ModelPackagePreparationRoots> preparationRoots;
		TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID shaderCollectionId = TerraEngine::EMPTY_UUID;
		GeneratedModelMeshVirtualGeometrySettings virtualGeometrySettings;
		bool hasVirtualGeometrySettings = false;
		// Package children need independent recipes. The process-isolated compiler
		// restores these records from the previous sidecar before rebuilding.
		std::vector<ModelPackageMeshVirtualGeometryOverride>
			virtualGeometryMeshOverrides;
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
	// Publish already validated private outputs, including their candidate
	// sidecar, with the existing rollback transaction. The caller holds its
	// lifecycle/publication arbiter; this helper does no compile or physical wait.
	[[nodiscard]] bool PublishPreparedModelPackage(
		const ModelPackagePreparationRoots& roots, const ModelPackageImportOptions& options);
}
