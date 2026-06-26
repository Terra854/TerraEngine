// GltfImportTypes.cpp - JSON and text helpers for glTF import contracts.
//
// Keeping serialization beside the data contracts makes import diagnostics
// stable for tests, editor UI, and MCP without each caller reinventing field
// names or enum spellings.

#include "GltfImportTypes.h"

#include <algorithm>

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

	const char* ToString(GltfImportStatus status)
	{
		switch (status)
		{
		case GltfImportStatus::Succeeded: return "Succeeded";
		case GltfImportStatus::SucceededWithWarnings: return "SucceededWithWarnings";
		case GltfImportStatus::Failed: return "Failed";
		}
		return "Failed";
	}

	const char* ToString(GltfDiagnosticSeverity severity)
	{
		switch (severity)
		{
		case GltfDiagnosticSeverity::Info: return "Info";
		case GltfDiagnosticSeverity::Warning: return "Warning";
		case GltfDiagnosticSeverity::Error: return "Error";
		}
		return "Error";
	}

	const char* ToString(GltfTextureUsage usage)
	{
		switch (usage)
		{
		case GltfTextureUsage::BaseColor: return "BaseColor";
		case GltfTextureUsage::Normal: return "Normal";
		case GltfTextureUsage::MetallicRoughness: return "MetallicRoughness";
		case GltfTextureUsage::Occlusion: return "Occlusion";
		case GltfTextureUsage::Emissive: return "Emissive";
		case GltfTextureUsage::Unknown: return "Unknown";
		}
		return "Unknown";
	}

	const char* ToString(GltfTextureSemantic semantic)
	{
		switch (semantic)
		{
		case GltfTextureSemantic::Color: return "Color";
		case GltfTextureSemantic::Data: return "Data";
		}
		return "Data";
	}

	nlohmann::json ToJson(const ImportedGltfPackage& value)
	{
		nlohmann::json dependencies = nlohmann::json::array();
		for (const std::filesystem::path& dependency : value.externalDependencies)
		{
			dependencies.push_back(dependency.generic_string());
		}

		return {
			{ "sourcePath", value.sourcePath.generic_string() },
			{ "packageKind", value.packageKind == GltfPackageKind::GltfBinary ? "GltfBinary" : "GltfText" },
			{ "sourceHash", value.sourceHashHex },
			{ "importRoot", value.importRoot.generic_string() },
			{ "externalDependencies", dependencies }
		};
	}

	nlohmann::json ToJson(const GltfImportDiagnostic& value)
	{
		return {
			{ "severity", ToString(value.severity) },
			{ "code", value.code },
			{ "source", value.source },
			{ "message", value.message },
			{ "fallback", value.fallback }
		};
	}

	nlohmann::json ToJson(const GeneratedGltfTextureAsset& value)
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

	nlohmann::json ToJson(const GeneratedGltfMaterialAsset& value)
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

	nlohmann::json ToJson(const GeneratedGltfMeshAsset& value)
	{
		return {
			{ "guid", TerraEngine::ToHexString(value.guid) },
			{ "sourceMeshIndex", value.sourceMeshIndex },
			{ "sourcePrimitiveIndex", OptionalIndexToJson(value.sourcePrimitiveIndex) },
			{ "sourcePath", value.sourcePath.generic_string() },
			{ "compiledPath", value.compiledPath.generic_string() },
			{ "name", value.displayName },
			{ "localBounds", {
				{ "min", ArrayToJson(value.localBoundsMin) },
				{ "max", ArrayToJson(value.localBoundsMax) }
			} }
		};
	}

	nlohmann::json ToJson(const GltfMaterialAssignment& value)
	{
		return {
			{ "meshGuid", TerraEngine::ToHexString(value.meshGuid) },
			{ "materialGuid", TerraEngine::ToHexString(value.materialGuid) },
			{ "sourceMeshIndex", value.sourceMeshIndex },
			{ "sourcePrimitiveIndex", value.sourcePrimitiveIndex },
			{ "slotName", value.slotName }
		};
	}

	nlohmann::json ToJson(const GltfImportResult& value)
	{
		nlohmann::json meshes = nlohmann::json::array();
		for (const GeneratedGltfMeshAsset& mesh : value.generatedMeshes)
		{
			meshes.push_back(ToJson(mesh));
		}

		nlohmann::json materials = nlohmann::json::array();
		for (const GeneratedGltfMaterialAsset& material : value.generatedMaterials)
		{
			materials.push_back(ToJson(material));
		}

		nlohmann::json textures = nlohmann::json::array();
		for (const GeneratedGltfTextureAsset& texture : value.generatedTextures)
		{
			textures.push_back(ToJson(texture));
		}

		nlohmann::json assignments = nlohmann::json::array();
		for (const GltfMaterialAssignment& assignment : value.materialAssignments)
		{
			assignments.push_back(ToJson(assignment));
		}

		nlohmann::json diagnostics = nlohmann::json::array();
		for (const GltfImportDiagnostic& diagnostic : value.diagnostics)
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
