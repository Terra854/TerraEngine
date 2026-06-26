// AssetImportResult.cpp - JSON sidecar persistence for import results.

#include "AssetImportResult.h"

#include <fstream>
#include <iomanip>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		GltfImportStatus ParseImportStatus(const std::string& value)
		{
			if (value == "Succeeded")
			{
				return GltfImportStatus::Succeeded;
			}
			if (value == "SucceededWithWarnings")
			{
				return GltfImportStatus::SucceededWithWarnings;
			}
			return GltfImportStatus::Failed;
		}

		GltfPackageKind ParsePackageKind(const std::string& value)
		{
			return value == "GltfBinary" ? GltfPackageKind::GltfBinary : GltfPackageKind::GltfText;
		}

		GltfDiagnosticSeverity ParseDiagnosticSeverity(const std::string& value)
		{
			if (value == "Info")
			{
				return GltfDiagnosticSeverity::Info;
			}
			if (value == "Warning")
			{
				return GltfDiagnosticSeverity::Warning;
			}
			return GltfDiagnosticSeverity::Error;
		}

		GltfTextureUsage ParseTextureUsage(const std::string& value)
		{
			if (value == "BaseColor") return GltfTextureUsage::BaseColor;
			if (value == "Normal") return GltfTextureUsage::Normal;
			if (value == "MetallicRoughness") return GltfTextureUsage::MetallicRoughness;
			if (value == "Occlusion") return GltfTextureUsage::Occlusion;
			if (value == "Emissive") return GltfTextureUsage::Emissive;
			return GltfTextureUsage::Unknown;
		}

		GltfTextureSemantic ParseTextureSemantic(const std::string& value)
		{
			return value == "Color" ? GltfTextureSemantic::Color : GltfTextureSemantic::Data;
		}

		std::optional<std::size_t> ReadOptionalIndex(const nlohmann::json& object, const char* key)
		{
			if (!object.contains(key) || object[key].is_null())
			{
				return std::nullopt;
			}
			return object[key].get<std::size_t>();
		}

		std::array<float, 3> ReadFloat3(const nlohmann::json& object, const char* key, const std::array<float, 3>& fallback)
		{
			if (!object.contains(key) || !object[key].is_array() || object[key].size() < 3)
			{
				return fallback;
			}
			return { object[key][0].get<float>(), object[key][1].get<float>(), object[key][2].get<float>() };
		}

		std::array<float, 4> ReadFloat4(const nlohmann::json& object, const char* key, const std::array<float, 4>& fallback)
		{
			if (!object.contains(key) || !object[key].is_array() || object[key].size() < 4)
			{
				return fallback;
			}
			return { object[key][0].get<float>(), object[key][1].get<float>(), object[key][2].get<float>(), object[key][3].get<float>() };
		}

		TerraEngine::UUID ReadUuid(const nlohmann::json& object, const char* key)
		{
			if (!object.contains(key))
			{
				return TerraEngine::EMPTY_UUID;
			}

			const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(object[key].get<std::string>());
			return parsed.value_or(TerraEngine::EMPTY_UUID);
		}
	}

	std::filesystem::path GetImportResultPath(const std::filesystem::path& sourceAssetPath)
	{
		return std::filesystem::path(sourceAssetPath.string() + ".import.json");
	}

	bool SaveImportResult(const std::filesystem::path& sourceAssetPath, const GltfImportResult& result)
	{
		const std::filesystem::path resultPath = GetImportResultPath(sourceAssetPath);
		std::filesystem::create_directories(resultPath.parent_path());

		std::ofstream out(resultPath);
		if (!out)
		{
			return false;
		}

		out << std::setw(4) << ToJson(result) << '\n';
		return static_cast<bool>(out);
	}

	bool LoadImportResult(const std::filesystem::path& sourceAssetPath, GltfImportResult& outResult)
	{
		const std::filesystem::path resultPath = GetImportResultPath(sourceAssetPath);
		std::ifstream input(resultPath);
		if (!input)
		{
			return false;
		}

		nlohmann::json document;
		try
		{
			input >> document;

			GltfImportResult result;
			result.status = ParseImportStatus(document.value("status", std::string("Failed")));
			result.packageGuid = ReadUuid(document, "packageGuid");

			const nlohmann::json package = document.value("package", nlohmann::json::object());
			result.package.sourcePath = package.value("sourcePath", std::string());
			result.package.packageKind = ParsePackageKind(package.value("packageKind", std::string("GltfText")));
			result.package.sourceHashHex = package.value("sourceHash", std::string());
			result.package.importRoot = package.value("importRoot", std::string());
			for (const nlohmann::json& dependency : package.value("externalDependencies", nlohmann::json::array()))
			{
				result.package.externalDependencies.emplace_back(dependency.get<std::string>());
			}

			// Rehydrate generated child records so every editor surface can read the same durable import result.
			for (const nlohmann::json& mesh : document.value("generatedMeshes", nlohmann::json::array()))
			{
				GeneratedGltfMeshAsset record;
				record.guid = ReadUuid(mesh, "guid");
				record.sourceMeshIndex = mesh.value("sourceMeshIndex", std::size_t{ 0 });
				record.sourcePrimitiveIndex = ReadOptionalIndex(mesh, "sourcePrimitiveIndex");
				record.sourcePath = mesh.value("sourcePath", std::string());
				record.compiledPath = mesh.value("compiledPath", std::string());
				record.displayName = mesh.value("name", std::string());
				record.localBoundsMin = ReadFloat3(mesh, "localBoundsMin", record.localBoundsMin);
				record.localBoundsMax = ReadFloat3(mesh, "localBoundsMax", record.localBoundsMax);
				result.generatedMeshes.push_back(std::move(record));
			}

			for (const nlohmann::json& material : document.value("generatedMaterials", nlohmann::json::array()))
			{
				GeneratedGltfMaterialAsset record;
				record.guid = ReadUuid(material, "guid");
				record.sourceMaterialIndex = ReadOptionalIndex(material, "sourceMaterialIndex");
				record.sourcePath = material.value("sourcePath", std::string());
				record.compiledPath = material.value("compiledPath", std::string());
				record.displayName = material.value("name", std::string());
				record.baseColorFactor = ReadFloat4(material, "baseColorFactor", record.baseColorFactor);
				record.emissiveFactor = ReadFloat3(material, "emissiveFactor", record.emissiveFactor);
				record.metallicFactor = material.value("metallicFactor", record.metallicFactor);
				record.roughnessFactor = material.value("roughnessFactor", record.roughnessFactor);
				record.normalScale = material.value("normalScale", record.normalScale);
				record.occlusionStrength = material.value("occlusionStrength", record.occlusionStrength);
				record.doubleSided = material.value("doubleSided", record.doubleSided);
				for (const nlohmann::json& textureGuid : material.value("textureGuids", nlohmann::json::array()))
				{
					if (const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(textureGuid.get<std::string>()))
					{
						record.textureGuids.push_back(*parsed);
					}
				}
				result.generatedMaterials.push_back(std::move(record));
			}

			for (const nlohmann::json& texture : document.value("generatedTextures", nlohmann::json::array()))
			{
				GeneratedGltfTextureAsset record;
				record.guid = ReadUuid(texture, "guid");
				record.sourceImageIndex = ReadOptionalIndex(texture, "sourceImageIndex");
				record.sourceTextureIndex = ReadOptionalIndex(texture, "sourceTextureIndex");
				record.sourcePath = texture.value("sourcePath", std::string());
				record.compiledPath = texture.value("compiledPath", std::string());
				record.displayName = texture.value("name", std::string());
				record.usage = ParseTextureUsage(texture.value("usage", std::string("Unknown")));
				record.semantic = ParseTextureSemantic(texture.value("semantic", std::string("Data")));
				result.generatedTextures.push_back(std::move(record));
			}

			for (const nlohmann::json& assignment : document.value("materialAssignments", nlohmann::json::array()))
			{
				GltfMaterialAssignment record;
				record.meshGuid = ReadUuid(assignment, "meshGuid");
				record.materialGuid = ReadUuid(assignment, "materialGuid");
				record.sourceMeshIndex = assignment.value("sourceMeshIndex", std::size_t{ 0 });
				record.sourcePrimitiveIndex = assignment.value("sourcePrimitiveIndex", std::size_t{ 0 });
				record.slotName = assignment.value("slotName", std::string());
				result.materialAssignments.push_back(std::move(record));
			}

			for (const nlohmann::json& diagnostic : document.value("diagnostics", nlohmann::json::array()))
			{
				GltfImportDiagnostic record;
				record.severity = ParseDiagnosticSeverity(diagnostic.value("severity", std::string("Error")));
				record.code = diagnostic.value("code", std::string());
				record.source = diagnostic.value("source", std::string());
				record.message = diagnostic.value("message", std::string());
				record.fallback = diagnostic.value("fallback", std::string());
				result.diagnostics.push_back(std::move(record));
			}

			outResult = std::move(result);
			return true;
		}
		catch (const nlohmann::json::exception&)
		{
			return false;
		}
	}
}
