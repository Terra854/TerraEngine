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
#include <iomanip>
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

		std::optional<TerraEngine::UUID> FindMaterialForMesh(const ModelPackageImportResult& result, const TerraEngine::UUID& meshGuid)
		{
			for (const ModelMaterialAssignment& assignment : result.materialAssignments)
			{
				if (assignment.meshGuid == meshGuid && assignment.materialGuid != TerraEngine::EMPTY_UUID)
				{
					return assignment.materialGuid;
				}
			}
			return std::nullopt;
		}

		nlohmann::json MakeIdentityTransformJson()
		{
			return {
				{ "Position", { 0.0f, 0.0f, 0.0f } },
				{ "Rotation", { 1.0f, 0.0f, 0.0f, 0.0f } },
				{ "Scale", { 1.0f, 1.0f, 1.0f } }
			};
		}

		nlohmann::json MakeModelPackagePrefabObjectJson(
			const std::string& objectName,
			const TerraEngine::UUID& objectGuid,
			const TerraEngine::UUID& meshGuid,
			const TerraEngine::UUID& materialGuid)
		{
			// The importer writes prefab JSON directly because it runs outside a
			// live ObjectManager scene. Keep the shape identical to Serializer so
			// SceneManager can open the generated prefab without a special path.
			nlohmann::json object;
			object["name"] = objectName;
			object["tag"] = "Untagged";
			object["layer"] = "Default";
			object["active"] = true;
			object["isStatic"] = false;
			object["uuid"] = TerraEngine::ToHexString(objectGuid);
			object["components"] = {
				{ "Transform", MakeIdentityTransformJson() },
				{ "MeshRenderComponent", {
					{ "meshId", TerraEngine::ToHexString(meshGuid) },
					{ "materialId", TerraEngine::ToHexString(materialGuid) },
					{ "spatialObjectId", TerraEngine::ToHexString(TerraEngine::EMPTY_UUID) },
					{ "taskShaderCullingEnabled", true }
				} }
			};
			return object;
		}

		void GenerateModelPackagePrefab(const ModelPackageImportOptions& options, ModelPackageImportResult& result)
		{
			if (result.generatedMeshes.empty() || result.package.importRoot.empty())
			{
				return;
			}

			const std::string prefabName = options.sourcePath.stem().string().empty()
				? std::string("ImportedModel")
				: options.sourcePath.stem().string();
			GeneratedModelPrefabAsset prefab{};
			prefab.guid = BuildStableGeneratedModelUuid(result.packageGuid, "model-prefab", 0u);
			prefab.displayName = prefabName;
			prefab.sourcePath = result.package.importRoot / (prefabName + ".teprefab");

			nlohmann::json prefabDocument;
			prefabDocument["name"] = prefab.displayName;
			prefabDocument["objects"] = nlohmann::json::array();

			nlohmann::json root;
			root["name"] = prefab.displayName;
			root["tag"] = "Untagged";
			root["layer"] = "Default";
			root["active"] = true;
			root["isStatic"] = false;
			root["uuid"] = TerraEngine::ToHexString(BuildStableGeneratedModelUuid(result.packageGuid, "model-prefab-root", 0u));
			root["components"] = {
				{ "Transform", MakeIdentityTransformJson() }
			};
			root["children"] = nlohmann::json::array();

			for (std::size_t meshIndex = 0u; meshIndex < result.generatedMeshes.size(); ++meshIndex)
			{
				const GeneratedModelMeshAsset& mesh = result.generatedMeshes[meshIndex];
				const std::optional<TerraEngine::UUID> materialGuid = FindMaterialForMesh(result, mesh.guid);
				const TerraEngine::UUID objectGuid = BuildStableGeneratedModelUuid(result.packageGuid, "model-prefab-mesh-object", meshIndex);
				root["children"].push_back(MakeModelPackagePrefabObjectJson(
					mesh.displayName.empty() ? ("Mesh_" + std::to_string(meshIndex)) : mesh.displayName,
					objectGuid,
					mesh.guid,
					materialGuid.value_or(TerraEngine::EMPTY_UUID)));
				prefab.meshGuids.push_back(mesh.guid);
				if (materialGuid.has_value())
				{
					prefab.materialGuids.push_back(*materialGuid);
				}
			}

			prefabDocument["objects"].push_back(std::move(root));

			const std::filesystem::path prefabPath = options.assetRoot / prefab.sourcePath;
			std::error_code directoryError;
			std::filesystem::create_directories(prefabPath.parent_path(), directoryError);
			if (directoryError)
			{
				result.diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "GeneratedPrefabWriteFailed", prefab.sourcePath.generic_string(), "Failed to create the generated prefab directory.", "Meshes and materials were still imported." });
				return;
			}

			std::ofstream output(prefabPath, std::ios::binary | std::ios::trunc);
			if (!output)
			{
				result.diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "GeneratedPrefabWriteFailed", prefab.sourcePath.generic_string(), "Failed to write the generated prefab asset.", "Meshes and materials were still imported." });
				return;
			}
			output << std::setw(4) << prefabDocument << '\n';
			output.close();
			if (!output)
			{
				result.diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "GeneratedPrefabWriteFailed", prefab.sourcePath.generic_string(), "Generated prefab output failed while closing.", "Meshes and materials were still imported." });
				return;
			}

			result.generatedPrefabs.clear();
			result.generatedPrefabs.push_back(std::move(prefab));
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
		GenerateModelPackagePrefab(options, result);
		PromoteStatusFromDiagnostics(result);
		return result;
	}
}
