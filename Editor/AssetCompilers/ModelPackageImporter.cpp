// ModelPackageImporter.cpp - Shared detection and dispatch for model packages.
//
// This file is deliberately small: it centralizes extension/importer-name policy
// while the format-specific files own parsing details and diagnostics.

#include "ModelPackageImporter.h"

#include "FbxPackageImporter.h"
#include "GltfPackageImporter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

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

		bool SourceTextContains(const std::filesystem::path& filePath, std::string_view token)
		{
			const std::string ext = NormalizeExtension(filePath);
			if (ext != ".gltf")
			{
				return false;
			}

			std::ifstream in(filePath);
			if (!in)
			{
				return false;
			}

			const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			return text.find(token) != std::string::npos;
		}

		bool HasDiagnosticCode(const ModelPackageImportResult& result, std::string_view code)
		{
			for (const ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				if (diagnostic.code == code)
				{
					return true;
				}
			}
			return false;
		}

		void AddDiagnosticIfSourceContains(
			const ModelPackageImportOptions& options,
			ModelPackageImportResult& result,
			std::string_view token,
			ModelDiagnosticSeverity severity,
			std::string code,
			std::string source,
			std::string message,
			std::string fallback)
		{
			if (SourceTextContains(options.sourcePath, token) && !HasDiagnosticCode(result, code))
			{
				result.diagnostics.push_back({
					severity,
					std::move(code),
					std::move(source),
					std::move(message),
					std::move(fallback)
				});
			}
		}

		void AddFormatNeutralUnsupportedDiagnostics(const ModelPackageImportOptions& options, ModelPackageImportResult& result)
		{
			// Some authoring formats expose data that is not part of TerraEngine's
			// static package import contract yet. Report that content once, but
			// keep usable static meshes available to the editor and automation.
			AddDiagnosticIfSourceContains(options, result, "\"animations\"", ModelDiagnosticSeverity::Warning, "UnsupportedAnimationIgnored", "animations", "Animation data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
			AddDiagnosticIfSourceContains(options, result, "\"skins\"", ModelDiagnosticSeverity::Warning, "UnsupportedSkinIgnored", "skins", "Skinning data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
			AddDiagnosticIfSourceContains(options, result, "\"cameras\"", ModelDiagnosticSeverity::Warning, "UnsupportedCameraIgnored", "cameras", "Camera data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
			AddDiagnosticIfSourceContains(options, result, "\"lights\"", ModelDiagnosticSeverity::Warning, "UnsupportedLightIgnored", "lights", "Light data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
			AddDiagnosticIfSourceContains(options, result, "\"constraints\"", ModelDiagnosticSeverity::Warning, "UnsupportedConstraintIgnored", "constraints", "Constraint data was detected in the source package but is not imported for static assets.", "Renderable meshes were imported.");
		}

		void PopulateMissingVirtualGeometryMetadata(const ModelPackageImportOptions& options, ModelPackageImportResult& result)
		{
			for (GeneratedModelMeshAsset& mesh : result.generatedMeshes)
			{
				if (!mesh.compiledPath.empty() && mesh.virtualGeometryStatus == "Unknown")
				{
					PopulateGeneratedMeshVirtualGeometryMetadata(options.buildRoot / mesh.compiledPath, mesh, result.diagnostics);
				}
			}
		}

		void PromoteStatusFromDiagnostics(ModelPackageImportResult& result)
		{
			bool hasError = false;
			bool hasWarning = false;
			for (const ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				hasError = hasError || diagnostic.severity == ModelDiagnosticSeverity::Error;
				hasWarning = hasWarning || diagnostic.severity == ModelDiagnosticSeverity::Warning;
			}

			if (hasError || result.generatedMeshes.empty())
			{
				result.status = ModelImportStatus::Failed;
			}
			else if (hasWarning)
			{
				result.status = ModelImportStatus::SucceededWithWarnings;
			}
			else
			{
				result.status = ModelImportStatus::Succeeded;
			}
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
		ModelPackageImportResult result;
		if (IsFbxPackageSourceExtension(options.sourcePath))
		{
			result = ImportFbxPackage(options);
		}
		else
		{
			result = ImportGltfPackage(options);
		}

		AddFormatNeutralUnsupportedDiagnostics(options, result);
		PopulateMissingVirtualGeometryMetadata(options, result);
		PromoteStatusFromDiagnostics(result);
		return result;
	}
}
