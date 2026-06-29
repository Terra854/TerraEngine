// ModelPackageImportTypes.cpp - JSON and text helpers for model package results.
//
// The sidecar is a public editor/automation contract, so enum spellings and JSON
// fields live in one place instead of being re-created by each importer or UI.

#include "ModelPackageImportTypes.h"

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		template <typename T>
		nlohmann::json ArrayToJson(const T& values)
		{
			nlohmann::json result = nlohmann::json::array();
			for (const auto& value : values)
			{
				result.push_back(value);
			}
			return result;
		}

		nlohmann::json OptionalIndexToJson(const std::optional<std::size_t>& value)
		{
			return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
		}
	}

	const char* ToString(ModelPackageKind kind)
	{
		switch (kind)
		{
		case ModelPackageKind::GltfText: return "GltfText";
		case ModelPackageKind::GltfBinary: return "GltfBinary";
		case ModelPackageKind::Fbx: return "Fbx";
		}
		return "GltfText";
	}

	const char* ToString(ModelImportStatus status)
	{
		switch (status)
		{
		case ModelImportStatus::Succeeded: return "Succeeded";
		case ModelImportStatus::SucceededWithWarnings: return "SucceededWithWarnings";
		case ModelImportStatus::Failed: return "Failed";
		}
		return "Failed";
	}

	const char* ToString(ModelDiagnosticSeverity severity)
	{
		switch (severity)
		{
		case ModelDiagnosticSeverity::Info: return "Info";
		case ModelDiagnosticSeverity::Warning: return "Warning";
		case ModelDiagnosticSeverity::Error: return "Error";
		}
		return "Error";
	}

	const char* ToString(ModelTextureUsage usage)
	{
		switch (usage)
		{
		case ModelTextureUsage::BaseColor: return "BaseColor";
		case ModelTextureUsage::Normal: return "Normal";
		case ModelTextureUsage::MetallicRoughness: return "MetallicRoughness";
		case ModelTextureUsage::Occlusion: return "Occlusion";
		case ModelTextureUsage::Emissive: return "Emissive";
		case ModelTextureUsage::Unknown: return "Unknown";
		}
		return "Unknown";
	}

	const char* ToString(ModelTextureSemantic semantic)
	{
		switch (semantic)
		{
		case ModelTextureSemantic::Color: return "Color";
		case ModelTextureSemantic::Data: return "Data";
		}
		return "Data";
	}

	nlohmann::json ToJson(const ImportedModelPackage& value)
	{
		nlohmann::json dependencies = nlohmann::json::array();
		for (const std::filesystem::path& dependency : value.externalDependencies)
		{
			dependencies.push_back(dependency.generic_string());
		}

		return {
			{ "sourcePath", value.sourcePath.generic_string() },
			{ "packageKind", ToString(value.packageKind) },
			{ "sourceHash", value.sourceHashHex },
			{ "importRoot", value.importRoot.generic_string() },
			{ "externalDependencies", dependencies }
		};
	}

	nlohmann::json ToJson(const ModelImportDiagnostic& value)
	{
		return {
			{ "severity", ToString(value.severity) },
			{ "code", value.code },
			{ "source", value.source },
			{ "message", value.message },
			{ "fallback", value.fallback }
		};
	}

	nlohmann::json ToJson(const GeneratedModelTextureAsset& value)
	{
		return {
			{ "guid", TerraEngine::ToHexString(value.guid) },
			{ "sourceImageIndex", OptionalIndexToJson(value.sourceImageIndex) },
			{ "sourceTextureIndex", OptionalIndexToJson(value.sourceTextureIndex) },
			{ "sourcePath", value.sourcePath.generic_string() },
			{ "compiledPath", value.compiledPath.generic_string() },
			{ "name", value.displayName },
			{ "usage", ToString(value.usage) },
			{ "semantic", ToString(value.semantic) }
		};
	}

	nlohmann::json ToJson(const GeneratedModelMaterialAsset& value)
	{
		nlohmann::json textureGuids = nlohmann::json::array();
		for (const TerraEngine::UUID& textureGuid : value.textureGuids)
		{
			textureGuids.push_back(TerraEngine::ToHexString(textureGuid));
		}

		return {
			{ "guid", TerraEngine::ToHexString(value.guid) },
			{ "sourceMaterialIndex", OptionalIndexToJson(value.sourceMaterialIndex) },
			{ "sourcePath", value.sourcePath.generic_string() },
			{ "compiledPath", value.compiledPath.generic_string() },
			{ "name", value.displayName },
			{ "baseColorFactor", ArrayToJson(value.baseColorFactor) },
			{ "emissiveFactor", ArrayToJson(value.emissiveFactor) },
			{ "metallicFactor", value.metallicFactor },
			{ "roughnessFactor", value.roughnessFactor },
			{ "normalScale", value.normalScale },
			{ "occlusionStrength", value.occlusionStrength },
			{ "doubleSided", value.doubleSided },
			{ "textureGuids", textureGuids }
		};
	}

	nlohmann::json ToJson(const GeneratedModelMeshAsset& value)
	{
		return {
			{ "guid", TerraEngine::ToHexString(value.guid) },
			{ "sourceMeshIndex", value.sourceMeshIndex },
			{ "sourcePrimitiveIndex", OptionalIndexToJson(value.sourcePrimitiveIndex) },
			{ "sourceNodePath", value.sourceNodePath },
			{ "sourcePath", value.sourcePath.generic_string() },
			{ "compiledPath", value.compiledPath.generic_string() },
			{ "name", value.displayName },
			{ "localBoundsMin", ArrayToJson(value.localBoundsMin) },
			{ "localBoundsMax", ArrayToJson(value.localBoundsMax) },
			{ "localBounds", {
				{ "min", ArrayToJson(value.localBoundsMin) },
				{ "max", ArrayToJson(value.localBoundsMax) }
			} }
		};
	}

	nlohmann::json ToJson(const ModelMaterialAssignment& value)
	{
		return {
			{ "meshGuid", TerraEngine::ToHexString(value.meshGuid) },
			{ "materialGuid", TerraEngine::ToHexString(value.materialGuid) },
			{ "sourceMeshIndex", value.sourceMeshIndex },
			{ "sourcePrimitiveIndex", value.sourcePrimitiveIndex },
			{ "slotName", value.slotName }
		};
	}

	nlohmann::json ToJson(const ModelPackageImportResult& value)
	{
		nlohmann::json meshes = nlohmann::json::array();
		for (const GeneratedModelMeshAsset& mesh : value.generatedMeshes)
		{
			meshes.push_back(ToJson(mesh));
		}

		nlohmann::json materials = nlohmann::json::array();
		for (const GeneratedModelMaterialAsset& material : value.generatedMaterials)
		{
			materials.push_back(ToJson(material));
		}

		nlohmann::json textures = nlohmann::json::array();
		for (const GeneratedModelTextureAsset& texture : value.generatedTextures)
		{
			textures.push_back(ToJson(texture));
		}

		nlohmann::json assignments = nlohmann::json::array();
		for (const ModelMaterialAssignment& assignment : value.materialAssignments)
		{
			assignments.push_back(ToJson(assignment));
		}

		nlohmann::json diagnostics = nlohmann::json::array();
		for (const ModelImportDiagnostic& diagnostic : value.diagnostics)
		{
			diagnostics.push_back(ToJson(diagnostic));
		}

		return {
			{ "status", ToString(value.status) },
			{ "packageGuid", TerraEngine::ToHexString(value.packageGuid) },
			{ "package", ToJson(value.package) },
			{ "generatedMeshes", meshes },
			{ "generatedMaterials", materials },
			{ "generatedTextures", textures },
			{ "materialAssignments", assignments },
			{ "diagnostics", diagnostics }
		};
	}
}
