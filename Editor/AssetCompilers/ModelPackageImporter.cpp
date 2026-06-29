// ModelPackageImporter.cpp - Shared detection and dispatch for model packages.
//
// This file is deliberately small: it centralizes extension/importer-name policy
// while the format-specific files own parsing details and diagnostics.

#include "ModelPackageImporter.h"

#include "FbxPackageImporter.h"
#include "GltfPackageImporter.h"

#include <algorithm>
#include <cctype>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		std::string NormalizeExtension(const std::filesystem::path& filePath)
		{
			std::string ext = filePath.extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return ext;
		}
	}

	bool IsFbxPackageSourceExtension(const std::filesystem::path& filePath)
	{
		return NormalizeExtension(filePath) == ".fbx";
	}

	bool IsModelPackageSourceExtension(const std::filesystem::path& filePath)
	{
		return IsGltfPackageSourceExtension(filePath) || IsFbxPackageSourceExtension(filePath);
	}

	const char* GetModelPackageImporterName(const std::filesystem::path& filePath)
	{
		if (IsFbxPackageSourceExtension(filePath))
		{
			return "FbxPackageImporter";
		}
		if (IsGltfPackageSourceExtension(filePath))
		{
			return "GltfPackageImporter";
		}
		return "";
	}

	bool IsModelPackageImporterName(std::string_view importerName)
	{
		return importerName == "GltfPackageImporter" || importerName == "FbxPackageImporter";
	}

	ModelPackageImportResult ImportModelPackage(const ModelPackageImportOptions& options)
	{
		if (IsFbxPackageSourceExtension(options.sourcePath))
		{
			return ImportFbxPackage(options);
		}
		return ImportGltfPackage(options);
	}
}
