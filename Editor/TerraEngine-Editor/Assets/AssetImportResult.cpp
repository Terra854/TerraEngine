// AssetImportResult.cpp - JSON sidecar persistence for import results.

#include "AssetImportResult.h"

#include <fstream>
#include <iomanip>
#include <system_error>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		ModelImportStatus ParseImportStatus(const std::string& value)
		{
			if (value == "Succeeded")
			{
				return ModelImportStatus::Succeeded;
			}
			if (value == "SucceededWithWarnings")
			{
				return ModelImportStatus::SucceededWithWarnings;
			}
			return ModelImportStatus::Failed;
		}

		ModelPackageKind ParsePackageKind(const std::string& value)
		{
			if (value == "Fbx")
			{
				return ModelPackageKind::Fbx;
			}
			return value == "GltfBinary" ? ModelPackageKind::GltfBinary : ModelPackageKind::GltfText;
		}

		ModelDiagnosticSeverity ParseDiagnosticSeverity(const std::string& value)
		{
			if (value == "Info")
			{
				return ModelDiagnosticSeverity::Info;
			}
			if (value == "Warning")
			{
				return ModelDiagnosticSeverity::Warning;
			}
			return ModelDiagnosticSeverity::Error;
		}

		ModelTextureUsage ParseTextureUsage(const std::string& value)
		{
			if (value == "BaseColor") return ModelTextureUsage::BaseColor;
			if (value == "Normal") return ModelTextureUsage::Normal;
			if (value == "MetallicRoughness") return ModelTextureUsage::MetallicRoughness;
			if (value == "Occlusion") return ModelTextureUsage::Occlusion;
			if (value == "Emissive") return ModelTextureUsage::Emissive;
			return ModelTextureUsage::Unknown;
		}

		ModelTextureSemantic ParseTextureSemantic(const std::string& value)
		{
			return value == "Color" ? ModelTextureSemantic::Color : ModelTextureSemantic::Data;
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

		std::array<float, 3> ReadBoundsVector(const nlohmann::json& object, const char* flatKey, const char* nestedKey, const std::array<float, 3>& fallback)
		{
			if (object.contains(flatKey))
			{
				return ReadFloat3(object, flatKey, fallback);
			}
			const nlohmann::json bounds = object.value("localBounds", nlohmann::json::object());
			return ReadFloat3(bounds, nestedKey, fallback);
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

		bool PublishImportResultAtomically(const std::filesystem::path& stagedPath, const std::filesystem::path& resultPath)
		{
			std::error_code ec;
			const std::filesystem::path backupPath = resultPath.string() + ".previous";
			std::filesystem::remove(backupPath, ec);
			ec.clear();

			// Sidecar publication mirrors compiled mesh publication: move the
			// previous readable result away first, then either install the staged
			// result or restore the previous one. That keeps failed rebuilds from
			// replacing a good package graph with a half-written JSON file.
			const bool hadPrevious = std::filesystem::exists(resultPath, ec);
			ec.clear();
			if (hadPrevious)
			{
				std::filesystem::rename(resultPath, backupPath, ec);
				if (ec)
				{
					std::filesystem::remove(stagedPath, ec);
					return false;
				}
			}

			std::filesystem::rename(stagedPath, resultPath, ec);
			if (ec)
			{
				if (hadPrevious)
				{
					std::error_code restoreError;
					std::filesystem::rename(backupPath, resultPath, restoreError);
				}
				std::filesystem::remove(stagedPath, ec);
				return false;
			}

			if (hadPrevious)
			{
				std::filesystem::remove(backupPath, ec);
			}
			return true;
		}
	}

	std::filesystem::path GetImportResultPath(const std::filesystem::path& sourceAssetPath)
	{
		return std::filesystem::path(sourceAssetPath.string() + ".import.json");
	}

	bool SaveModelPackageImportResult(const std::filesystem::path& sourceAssetPath, const ModelPackageImportResult& result)
	{
		const std::filesystem::path resultPath = GetImportResultPath(sourceAssetPath);
		std::filesystem::create_directories(resultPath.parent_path());
		const std::filesystem::path stagedPath = resultPath.string() + ".tmp";
		std::error_code cleanupError;
		std::filesystem::remove(stagedPath, cleanupError);

		std::ofstream out(stagedPath);
		if (!out)
		{
			return false;
		}

		out << std::setw(4) << ToJson(result) << '\n';
		out.close();
		if (!out)
		{
			std::filesystem::remove(stagedPath, cleanupError);
			return false;
		}
		return PublishImportResultAtomically(stagedPath, resultPath);
	}

	bool LoadModelPackageImportResult(const std::filesystem::path& sourceAssetPath, ModelPackageImportResult& outResult)
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

			ModelPackageImportResult result;
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
				GeneratedModelMeshAsset record;
				record.guid = ReadUuid(mesh, "guid");
				record.sourceMeshIndex = mesh.value("sourceMeshIndex", std::size_t{ 0 });
				record.sourcePrimitiveIndex = ReadOptionalIndex(mesh, "sourcePrimitiveIndex");
				record.sourceNodePath = mesh.value("sourceNodePath", std::string());
				record.sourcePath = mesh.value("sourcePath", std::string());
				record.compiledPath = mesh.value("compiledPath", std::string());
				record.displayName = mesh.value("name", std::string());
				record.localBoundsMin = ReadBoundsVector(mesh, "localBoundsMin", "min", record.localBoundsMin);
				record.localBoundsMax = ReadBoundsVector(mesh, "localBoundsMax", "max", record.localBoundsMax);
				const nlohmann::json virtualGeometry = mesh.value("virtualGeometry", nlohmann::json::object());
				record.virtualGeometryRequested = virtualGeometry.value("requested", record.virtualGeometryRequested);
				record.virtualGeometryCooked = virtualGeometry.value("cooked", record.virtualGeometryCooked);
				record.virtualGeometryStatus = virtualGeometry.value("status", record.virtualGeometryStatus);
				record.virtualGeometryDetailLevels = virtualGeometry.value("detailLevels", record.virtualGeometryDetailLevels);
				record.virtualGeometryMeshlets = virtualGeometry.value("meshlets", record.virtualGeometryMeshlets);
				record.virtualGeometrySourceTriangles = virtualGeometry.value("sourceTriangles", record.virtualGeometrySourceTriangles);
				const nlohmann::json settings = virtualGeometry.value("settings", nlohmann::json::object());
				record.virtualGeometrySettings.hasOverride = settings.value("hasOverride", record.virtualGeometrySettings.hasOverride);
				record.virtualGeometrySettings.settingsSource = settings.value("settingsSource", record.virtualGeometrySettings.settingsSource);
				record.virtualGeometrySettings.targetRatios.clear();
				for (const nlohmann::json& ratio : settings.value("targetRatios", nlohmann::json::array()))
				{
					if (ratio.is_number())
					{
						record.virtualGeometrySettings.targetRatios.push_back(ratio.get<float>());
					}
				}
				record.virtualGeometrySettings.minimumSourceTriangles = settings.value("minimumSourceTriangles", record.virtualGeometrySettings.minimumSourceTriangles);
				record.virtualGeometrySettings.minimumGeneratedTriangles = settings.value("minimumGeneratedTriangles", record.virtualGeometrySettings.minimumGeneratedTriangles);
				record.virtualGeometrySettings.minimumMeshletsPerGeneratedLevel = settings.value("minimumMeshletsPerGeneratedLevel", record.virtualGeometrySettings.minimumMeshletsPerGeneratedLevel);
				record.virtualGeometrySettings.minimumUsefulTriangleReduction = settings.value("minimumUsefulTriangleReduction", record.virtualGeometrySettings.minimumUsefulTriangleReduction);
				record.virtualGeometrySettings.minimumUsefulMeshletReduction = settings.value("minimumUsefulMeshletReduction", record.virtualGeometrySettings.minimumUsefulMeshletReduction);
				record.virtualGeometrySettings.maximumGeometricError = settings.value("maximumGeometricError", record.virtualGeometrySettings.maximumGeometricError);
				for (const nlohmann::json& tag : virtualGeometry.value("coverageTags", nlohmann::json::array()))
				{
					if (tag.is_string())
					{
						record.virtualGeometryCoverageTags.push_back(tag.get<std::string>());
					}
				}
				for (const nlohmann::json& warning : virtualGeometry.value("warnings", nlohmann::json::array()))
				{
					record.virtualGeometryWarnings.push_back(warning.get<std::string>());
				}
				for (const nlohmann::json& target : virtualGeometry.value("targets", nlohmann::json::array()))
				{
					GeneratedModelMeshVirtualGeometryTarget targetRecord;
					targetRecord.targetIndex = target.value("targetIndex", targetRecord.targetIndex);
					targetRecord.requestedRatio = target.value("requestedRatio", targetRecord.requestedRatio);
					targetRecord.status = target.value("status", targetRecord.status);
					targetRecord.reason = target.value("reason", targetRecord.reason);
					targetRecord.desiredTriangleCount = target.value("desiredTriangleCount", target.value("triangleCount", targetRecord.desiredTriangleCount));
					targetRecord.desiredMeshletCount = target.value("desiredMeshletCount", target.value("meshletCount", targetRecord.desiredMeshletCount));
					targetRecord.actualRatio = target.value("actualRatio", targetRecord.actualRatio);
					targetRecord.actualTriangleCount = target.value("actualTriangleCount", target.value("triangleCount", targetRecord.actualTriangleCount));
					targetRecord.actualMeshletCount = target.value("actualMeshletCount", target.value("meshletCount", targetRecord.actualMeshletCount));
					targetRecord.triangleCount = target.value("triangleCount", targetRecord.actualTriangleCount);
					targetRecord.meshletCount = target.value("meshletCount", targetRecord.actualMeshletCount);
					targetRecord.nearestGeneratedLevelIndex = target.value("nearestGeneratedLevelIndex", targetRecord.nearestGeneratedLevelIndex);
					targetRecord.geometricError = target.value("geometricError", targetRecord.geometricError);
					targetRecord.message = target.value("message", targetRecord.message);
					record.virtualGeometryTargets.push_back(std::move(targetRecord));
				}
				const nlohmann::json clusterSummary = virtualGeometry.value("clusterSummary", nlohmann::json::object());
				record.virtualGeometryClusterSummary.clusterCount = clusterSummary.value("clusterCount", record.virtualGeometryClusterSummary.clusterCount);
				record.virtualGeometryClusterSummary.pageLinkCount = clusterSummary.value("pageLinkCount", record.virtualGeometryClusterSummary.pageLinkCount);
				record.virtualGeometryClusterSummary.sourceMaterialAssignmentCount = clusterSummary.value("sourceMaterialAssignmentCount", record.virtualGeometryClusterSummary.sourceMaterialAssignmentCount);
				record.virtualGeometryClusterSummary.materialSectionCount = clusterSummary.value("materialSectionCount", record.virtualGeometryClusterSummary.materialSectionCount);
				record.virtualGeometryClusterSummary.clusterMaterialSectionCount = clusterSummary.value("clusterMaterialSectionCount", record.virtualGeometryClusterSummary.clusterMaterialSectionCount);
				record.virtualGeometryClusterSummary.fallbackCoverageValid = clusterSummary.value("fallbackCoverageValid", record.virtualGeometryClusterSummary.fallbackCoverageValid);
				record.virtualGeometryClusterSummary.materialCoverageValid = clusterSummary.value("materialCoverageValid", record.virtualGeometryClusterSummary.materialCoverageValid);
				const nlohmann::json coneSafety = virtualGeometry.value("coneSafety", nlohmann::json::object());
				record.virtualGeometryConeSafetySummary.recordCount = coneSafety.value("recordCount", record.virtualGeometryConeSafetySummary.recordCount);
				record.virtualGeometryConeSafetySummary.eligibleClusterCount = coneSafety.value("eligibleClusterCount", record.virtualGeometryConeSafetySummary.eligibleClusterCount);
				record.virtualGeometryConeSafetySummary.disabledClusterCount = coneSafety.value("disabledClusterCount", record.virtualGeometryConeSafetySummary.disabledClusterCount);
				record.virtualGeometryConeSafetySummary.metadataValid = coneSafety.value("metadataValid", record.virtualGeometryConeSafetySummary.metadataValid);
				for (const nlohmann::json& reason : coneSafety.value("disabledReasons", nlohmann::json::array()))
				{
					if (reason.is_string())
					{
						record.virtualGeometryConeSafetySummary.disabledReasons.push_back(reason.get<std::string>());
					}
				}
				result.generatedMeshes.push_back(std::move(record));
			}

			for (const nlohmann::json& material : document.value("generatedMaterials", nlohmann::json::array()))
			{
				GeneratedModelMaterialAsset record;
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
				GeneratedModelTextureAsset record;
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
				ModelMaterialAssignment record;
				record.meshGuid = ReadUuid(assignment, "meshGuid");
				record.materialGuid = ReadUuid(assignment, "materialGuid");
				record.sourceMeshIndex = assignment.value("sourceMeshIndex", std::size_t{ 0 });
				record.sourcePrimitiveIndex = assignment.value("sourcePrimitiveIndex", std::size_t{ 0 });
				record.slotName = assignment.value("slotName", std::string());
				result.materialAssignments.push_back(std::move(record));
			}

			for (const nlohmann::json& diagnostic : document.value("diagnostics", nlohmann::json::array()))
			{
				ModelImportDiagnostic record;
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

	bool SaveImportResult(const std::filesystem::path& sourceAssetPath, const GltfImportResult& result)
	{
		return SaveModelPackageImportResult(sourceAssetPath, result);
	}

	bool LoadImportResult(const std::filesystem::path& sourceAssetPath, GltfImportResult& outResult)
	{
		return LoadModelPackageImportResult(sourceAssetPath, outResult);
	}
}
