// ModelPackageImportTypes.cpp - JSON and text helpers for model package results.
//
// The sidecar is a public editor/automation contract, so enum spellings and JSON
// fields live in one place instead of being re-created by each importer or UI.

#include "ModelPackageImportTypes.h"

#include "File Formats/TerraEngineMeshFile.h"

#include <cstring>
#include <fstream>

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

	TerraEngine::UUID BuildStableGeneratedModelUuid(const TerraEngine::UUID& packageGuid, std::string_view role, std::size_t index)
	{
		// Generated package children must survive reimport when the package GUID
		// and source role/index are stable. A compact FNV-1a fold gives each
		// child type a deterministic UUID without storing extra state in .meta files.
		std::uint64_t high = 1469598103934665603ull;
		std::uint64_t low = 1099511628211ull;
		constexpr std::uint64_t prime = 1099511628211ull;
		for (std::uint8_t byte : packageGuid)
		{
			high ^= byte;
			high *= prime;
			low ^= static_cast<std::uint8_t>(byte + 31u);
			low *= prime;
		}
		for (char ch : role)
		{
			high ^= static_cast<std::uint8_t>(ch);
			high *= prime;
			low ^= static_cast<std::uint8_t>(ch * 3);
			low *= prime;
		}
		for (std::size_t shift = 0; shift < sizeof(index); ++shift)
		{
			const auto byte = static_cast<std::uint8_t>((index >> (shift * 8u)) & 0xffu);
			high ^= byte;
			high *= prime;
			low ^= static_cast<std::uint8_t>(byte + 17u);
			low *= prime;
		}

		TerraEngine::UUID uuid{};
		for (std::size_t byte = 0; byte < 8u; ++byte)
		{
			uuid[byte] = static_cast<std::uint8_t>((high >> (byte * 8u)) & 0xffu);
			uuid[byte + 8u] = static_cast<std::uint8_t>((low >> (byte * 8u)) & 0xffu);
		}
		return uuid;
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

	nlohmann::json ToJson(const GeneratedModelMeshVirtualGeometrySettings& value)
	{
		nlohmann::json targetRatios = nlohmann::json::array();
		for (float ratio : value.targetRatios)
		{
			targetRatios.push_back(ratio);
		}

		return {
			{ "hasOverride", value.hasOverride },
			{ "settingsSource", value.settingsSource },
			{ "targetRatios", targetRatios },
			{ "minimumSourceTriangles", value.minimumSourceTriangles },
			{ "minimumGeneratedTriangles", value.minimumGeneratedTriangles },
			{ "minimumMeshletsPerGeneratedLevel", value.minimumMeshletsPerGeneratedLevel },
			{ "minimumUsefulTriangleReduction", value.minimumUsefulTriangleReduction },
			{ "minimumUsefulMeshletReduction", value.minimumUsefulMeshletReduction },
			{ "maximumGeometricError", value.maximumGeometricError }
		};
	}

	nlohmann::json ToJson(const GeneratedModelMeshVirtualGeometryTarget& value)
	{
		return {
			{ "targetIndex", value.targetIndex },
			{ "requestedRatio", value.requestedRatio },
			{ "status", value.status },
			{ "reason", value.reason },
			{ "desiredTriangleCount", value.desiredTriangleCount },
			{ "desiredMeshletCount", value.desiredMeshletCount },
			{ "actualRatio", value.actualRatio },
			{ "actualTriangleCount", value.actualTriangleCount },
			{ "actualMeshletCount", value.actualMeshletCount },
			{ "triangleCount", value.triangleCount },
			{ "meshletCount", value.meshletCount },
			{ "nearestGeneratedLevelIndex", value.nearestGeneratedLevelIndex },
			{ "geometricError", value.geometricError },
			{ "message", value.message }
		};
	}

	nlohmann::json ToJson(const GeneratedModelMeshVirtualGeometryClusterSummary& value)
	{
		return {
			{ "clusterCount", value.clusterCount },
			{ "pageLinkCount", value.pageLinkCount },
			{ "fallbackCoverageValid", value.fallbackCoverageValid }
		};
	}

	nlohmann::json ToJson(const GeneratedModelMeshAsset& value)
	{
		nlohmann::json virtualGeometryWarnings = nlohmann::json::array();
		for (const std::string& warning : value.virtualGeometryWarnings)
		{
			virtualGeometryWarnings.push_back(warning);
		}
		nlohmann::json virtualGeometryTargets = nlohmann::json::array();
		for (const GeneratedModelMeshVirtualGeometryTarget& target : value.virtualGeometryTargets)
		{
			virtualGeometryTargets.push_back(ToJson(target));
		}

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
			} },
			{ "virtualGeometry", {
				{ "requested", value.virtualGeometryRequested },
				{ "cooked", value.virtualGeometryCooked },
				{ "status", value.virtualGeometryStatus },
				{ "detailLevels", value.virtualGeometryDetailLevels },
				{ "meshlets", value.virtualGeometryMeshlets },
				{ "sourceTriangles", value.virtualGeometrySourceTriangles },
				{ "settings", ToJson(value.virtualGeometrySettings) },
				{ "targets", virtualGeometryTargets },
				{ "clusterSummary", ToJson(value.virtualGeometryClusterSummary) },
				{ "warnings", virtualGeometryWarnings }
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

	void PopulateGeneratedMeshVirtualGeometryMetadata(const std::filesystem::path& compiledMeshPath, GeneratedModelMeshAsset& mesh, std::vector<ModelImportDiagnostic>& diagnostics)
	{
		mesh.virtualGeometryRequested = true;
		mesh.virtualGeometryCooked = false;
		mesh.virtualGeometryStatus = "Failed";
		mesh.virtualGeometryWarnings.clear();

		std::ifstream input(compiledMeshPath, std::ios::binary);
		if (!input)
		{
			mesh.virtualGeometryStatus = "Failed";
			mesh.virtualGeometryWarnings.push_back("Compiled mesh was not available for virtual geometry metadata inspection.");
			diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "VirtualGeometryMetadataUnavailable", mesh.displayName, "Package mesh was imported, but virtual geometry metadata could not be inspected.", "The generated mesh remains available for rebuild." });
			return;
		}

		MeshHeader header{};
		input.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!input || std::memcmp(header.magic, "TEMF", 4) != 0)
		{
			mesh.virtualGeometryStatus = "Failed";
			mesh.virtualGeometryWarnings.push_back("Compiled mesh header is not a valid TEMF file.");
			diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "VirtualGeometryInvalidCompiledMesh", mesh.displayName, "Package mesh compiled output is not a readable TEMF file.", "Reimport the package to regenerate mesh outputs." });
			return;
		}

		mesh.virtualGeometryMeshlets = header.meshletCount;
		if ((header.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) == 0u)
		{
			mesh.virtualGeometryStatus = "Skipped";
			mesh.virtualGeometryWarnings.push_back("Compiled mesh does not contain a virtual geometry payload.");
			return;
		}

		if (header.virtualGeometryHeaderOffset == 0u
			|| header.virtualGeometryHeaderSize != sizeof(VirtualGeometryHeader)
			|| header.virtualGeometryPayloadGeneration != kVirtualGeometryPayloadGeneration)
		{
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Virtual geometry payload metadata is stale or incomplete.");
			diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "VirtualGeometryNeedsRebuild", mesh.displayName, "Generated package mesh has stale virtual geometry metadata.", "Reimport the package to rebuild generated mesh outputs." });
			return;
		}

		VirtualGeometryHeader virtualHeader{};
		input.seekg(static_cast<std::streamoff>(header.virtualGeometryHeaderOffset), std::ios::beg);
		input.read(reinterpret_cast<char*>(&virtualHeader), sizeof(virtualHeader));
		if (!input || virtualHeader.payloadGeneration != kVirtualGeometryPayloadGeneration)
		{
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Virtual geometry root header could not be read.");
			diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "VirtualGeometryRootHeaderUnreadable", mesh.displayName, "Generated package mesh virtual geometry root header could not be read.", "Reimport the package to rebuild generated mesh outputs." });
			return;
		}

		mesh.virtualGeometryCooked = true;
		mesh.virtualGeometryStatus = "Cooked";
		mesh.virtualGeometryDetailLevels = virtualHeader.detailLevelCount;
		mesh.virtualGeometrySourceTriangles = virtualHeader.sourceTriangleCount;
		mesh.virtualGeometryClusterSummary.clusterCount = virtualHeader.clusterCount;
		mesh.virtualGeometryClusterSummary.pageLinkCount = virtualHeader.clusterPageLinkCount;
		mesh.virtualGeometryClusterSummary.fallbackCoverageValid =
			(virtualHeader.flags & kVirtualGeometryValidationFlagFallbackCoverageValidated) != 0u;
		if (virtualHeader.detailLevelCount == 0u || virtualHeader.cullableGroupCount == 0u)
		{
			mesh.virtualGeometryCooked = false;
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Virtual geometry payload has no renderable details or cullable groups.");
		}
	}
}
