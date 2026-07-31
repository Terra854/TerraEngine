// ModelPackageImportTypes.cpp - JSON and text helpers for model package results.
//
// The sidecar is a public editor/automation contract, so enum spellings and JSON
// fields live in one place instead of being re-created by each importer or UI.

#include "ModelPackageImportTypes.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/VirtualGeometryCodec.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <vector>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		constexpr std::uint64_t kManifestHashOffset = 1469598103934665603ull;
		constexpr std::uint64_t kManifestHashPrime = 1099511628211ull;

		void FoldManifestHash(std::uint64_t& hash, std::string_view value)
		{
			for(const char character : value)
			{
				hash ^= static_cast<std::uint8_t>(character);
				hash *= kManifestHashPrime;
			}
			// A separator prevents {"ab", "c"} from aliasing {"a", "bc"}.
			hash ^= 0xffu;
			hash *= kManifestHashPrime;
		}

		std::string NormalizeAnimationSemanticKey(std::string_view semanticKey)
		{
			std::string normalized;
			normalized.reserve(semanticKey.size());
			bool priorSlash = false;
			for(const unsigned char character : semanticKey)
			{
				const bool slash = character == '/' || character == '\\';
				if(slash)
				{
					if(!priorSlash) normalized.push_back('/');
					priorSlash = true;
					continue;
				}
				priorSlash = false;
				normalized.push_back(static_cast<char>(std::tolower(character)));
			}
			while(normalized.starts_with("./")) normalized.erase(0u, 2u);
			return normalized;
		}

		void FoldUuid(std::uint64_t& hash, const TerraEngine::UUID& uuid)
		{
			for(const std::uint8_t byte : uuid)
			{
				hash ^= byte;
				hash *= kManifestHashPrime;
			}
		}

		std::string HashToHex(std::uint64_t hash)
		{
			std::ostringstream stream;
			stream << std::hex << std::setfill('0') << std::setw(16) << hash;
			return stream.str();
		}

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

		template <typename T>
		bool ReadVirtualGeometryTable(std::ifstream& input, std::uint64_t offset, std::uint32_t count, std::vector<T>& outRecords)
		{
			outRecords.clear();
			if (count == 0u)
			{
				return true;
			}
			if (offset == 0u)
			{
				return false;
			}

			outRecords.resize(count);
			input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
			input.read(
				reinterpret_cast<char*>(outRecords.data()),
				static_cast<std::streamsize>(outRecords.size() * sizeof(T)));
			return static_cast<bool>(input);
		}

		std::uint32_t CountClusterMaterialSections(const std::vector<VirtualGeometryClusterRecord>& clusters)
		{
			std::vector<std::uint32_t> sectionIds;
			sectionIds.reserve(clusters.size());
			for (const VirtualGeometryClusterRecord& cluster : clusters)
			{
				if (std::find(sectionIds.begin(), sectionIds.end(), cluster.materialSectionId) == sectionIds.end())
				{
					sectionIds.push_back(cluster.materialSectionId);
				}
			}
			return static_cast<std::uint32_t>(sectionIds.size());
		}

		const char* VirtualGeometryTargetStatusText(std::uint32_t status)
		{
			switch(static_cast<VirtualGeometryDetailTargetRecordStatus>(status))
			{
			case VirtualGeometryDetailTargetRecordStatus::Requested: return "Requested";
			case VirtualGeometryDetailTargetRecordStatus::Generated: return "Generated";
			case VirtualGeometryDetailTargetRecordStatus::Merged: return "Merged";
			case VirtualGeometryDetailTargetRecordStatus::Skipped: return "Skipped";
			case VirtualGeometryDetailTargetRecordStatus::Rejected: return "Rejected";
			}
			return "Rejected";
		}

		const char* VirtualGeometrySkippedReasonText(std::uint32_t reason)
		{
			switch(static_cast<VirtualGeometrySkippedDetailRecordReason>(reason))
			{
			case VirtualGeometrySkippedDetailRecordReason::None: return "";
			case VirtualGeometrySkippedDetailRecordReason::MeshTooSmall: return "MeshTooSmall";
			case VirtualGeometrySkippedDetailRecordReason::TooFewTriangles: return "TooFewTriangles";
			case VirtualGeometrySkippedDetailRecordReason::TooFewMeshlets: return "TooFewMeshlets";
			case VirtualGeometrySkippedDetailRecordReason::InsufficientTriangleReduction:
				return "InsufficientTriangleReduction";
			case VirtualGeometrySkippedDetailRecordReason::InsufficientMeshletReduction:
				return "InsufficientMeshletReduction";
			case VirtualGeometrySkippedDetailRecordReason::InvalidSimplificationOutput:
				return "InvalidSimplificationOutput";
			case VirtualGeometrySkippedDetailRecordReason::MaterialSafetyRejected:
				return "MaterialSafetyRejected";
			case VirtualGeometrySkippedDetailRecordReason::GeometricErrorRejected:
				return "GeometricErrorRejected";
			case VirtualGeometrySkippedDetailRecordReason::DuplicateOfPreviousLevel:
				return "DuplicateOfPreviousLevel";
			}
			return "InvalidSimplificationOutput";
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

	const char* ToString(AnimationImportSubAssetKind kind)
	{
		switch(kind)
		{
		case AnimationImportSubAssetKind::Skeleton: return "Skeleton";
		case AnimationImportSubAssetKind::AnimationClip: return "AnimationClip";
		case AnimationImportSubAssetKind::DeformableMesh: return "DeformableMesh";
		case AnimationImportSubAssetKind::MorphTarget: return "MorphTarget";
		case AnimationImportSubAssetKind::Material: return "Material";
		case AnimationImportSubAssetKind::Texture: return "Texture";
		case AnimationImportSubAssetKind::Prefab: return "Prefab";
		case AnimationImportSubAssetKind::Mesh: return "Mesh";
		}
		return "AnimationClip";
	}

	TerraEngine::UUID BuildStableAnimationSubAssetUuid(
		const TerraEngine::UUID& packageGuid,
		AnimationImportSubAssetKind kind,
		std::string_view semanticKey)
	{
		const std::string normalizedKey = NormalizeAnimationSemanticKey(semanticKey);
		std::uint64_t high = kManifestHashOffset;
		std::uint64_t low = 1099511628211ull ^ 0x9e3779b97f4a7c15ull;
		FoldUuid(high, packageGuid);
		FoldUuid(low, packageGuid);
		FoldManifestHash(high, ToString(kind));
		FoldManifestHash(high, normalizedKey);
		FoldManifestHash(low, normalizedKey);
		FoldManifestHash(low, ToString(kind));
		TerraEngine::UUID uuid{};
		for(std::size_t byte = 0u; byte < 8u; ++byte)
		{
			uuid[byte] = static_cast<std::uint8_t>((high >> (byte * 8u)) & 0xffu);
			uuid[byte + 8u] = static_cast<std::uint8_t>((low >> (byte * 8u)) & 0xffu);
		}
		return uuid;
	}

	void CanonicalizeAnimationImportManifest(AnimationImportManifest& manifest)
	{
		for(GeneratedAnimationSubAsset& subAsset : manifest.subAssets)
		{
			subAsset.semanticKey = NormalizeAnimationSemanticKey(subAsset.semanticKey);
			std::sort(subAsset.dependencies.begin(), subAsset.dependencies.end(), [](const auto& left, const auto& right) {
				return std::tie(left.assetGuid, left.compatibilityFingerprint, left.required)
					< std::tie(right.assetGuid, right.compatibilityFingerprint, right.required);
			});
			subAsset.dependencies.erase(std::unique(
				subAsset.dependencies.begin(), subAsset.dependencies.end(), [](const auto& left, const auto& right) {
					return left.assetGuid == right.assetGuid
						&& left.compatibilityFingerprint == right.compatibilityFingerprint
						&& left.required == right.required;
				}), subAsset.dependencies.end());
		}
		std::sort(manifest.subAssets.begin(), manifest.subAssets.end(), [](const auto& left, const auto& right) {
			return std::tie(left.kind, left.semanticKey, left.guid)
				< std::tie(right.kind, right.semanticKey, right.guid);
		});
	}

	bool ValidateAnimationImportManifestDependencies(
		const AnimationImportManifest& manifest,
		std::string* failureReason)
	{
		const auto fail = [&](std::string reason) {
			if(failureReason != nullptr) *failureReason = std::move(reason);
			return false;
		};
		if(failureReason != nullptr) failureReason->clear();

		const auto findSubAsset = [&](const TerraEngine::UUID& guid) {
			return std::find_if(manifest.subAssets.begin(), manifest.subAssets.end(), [&](const auto& candidate) {
				return candidate.guid == guid;
			});
		};
		const auto isPrefabBinding = [](AnimationImportSubAssetKind kind) {
			return kind == AnimationImportSubAssetKind::Skeleton
				|| kind == AnimationImportSubAssetKind::AnimationClip
				|| kind == AnimationImportSubAssetKind::DeformableMesh
				|| kind == AnimationImportSubAssetKind::MorphTarget;
		};

		// Required dependency edges are internal package references. Reject them
		// before publication if their target is absent or their asserted binary
		// compatibility fingerprint disagrees with the generated target.
		for(const GeneratedAnimationSubAsset& subAsset : manifest.subAssets)
		{
			for(const AnimationImportDependency& dependency : subAsset.dependencies)
			{
				if(dependency.assetGuid == TerraEngine::EMPTY_UUID)
				{
					return fail("A package child contains an empty dependency GUID.");
				}
				const auto target = findSubAsset(dependency.assetGuid);
				if(target == manifest.subAssets.end())
				{
					if(dependency.required)
					{
						return fail("A required package-child dependency does not exist in the same generation.");
					}
					continue;
				}
				if(dependency.compatibilityFingerprint != 0u
					&& dependency.compatibilityFingerprint != target->compatibilityFingerprint)
				{
					return fail("A package-child dependency has a stale compatibility fingerprint.");
				}
			}
		}

		// A prefab is the playable package root. Its dependency list must retain
		// every animation binding emitted by this generation, while categories the
		// source does not contain naturally impose no requirement.
		for(const GeneratedAnimationSubAsset& prefab : manifest.subAssets)
		{
			if(prefab.kind != AnimationImportSubAssetKind::Prefab) continue;
			for(const GeneratedAnimationSubAsset& binding : manifest.subAssets)
			{
				if(!isPrefabBinding(binding.kind) || binding.guid == TerraEngine::EMPTY_UUID) continue;
				const auto dependency = std::find_if(
					prefab.dependencies.begin(), prefab.dependencies.end(), [&](const auto& candidate) {
						return candidate.assetGuid == binding.guid && candidate.required;
					});
				if(dependency == prefab.dependencies.end())
				{
					return fail("The generated prefab does not depend on every animation binding asset.");
				}
				if(dependency->compatibilityFingerprint != binding.compatibilityFingerprint)
				{
					return fail("The generated prefab records a stale animation binding fingerprint.");
				}
			}
		}
		return true;
	}

	std::string BuildAnimationImportDependencyFingerprint(const AnimationImportManifest& manifest)
	{
		AnimationImportManifest canonical = manifest;
		CanonicalizeAnimationImportManifest(canonical);
		std::uint64_t hash = kManifestHashOffset;
		FoldUuid(hash, canonical.packageGuid);
		FoldManifestHash(hash, canonical.sourceHashHex);
		FoldManifestHash(hash, canonical.importerName);
		FoldManifestHash(hash, canonical.importerVersion);
		FoldManifestHash(hash, canonical.settingsFingerprintHex);
		for(const GeneratedAnimationSubAsset& subAsset : canonical.subAssets)
		{
			FoldManifestHash(hash, ToString(subAsset.kind));
			FoldManifestHash(hash, subAsset.semanticKey);
			FoldUuid(hash, subAsset.guid);
			FoldManifestHash(hash, subAsset.contentHashHex);
			for(const AnimationImportDependency& dependency : subAsset.dependencies)
			{
				FoldUuid(hash, dependency.assetGuid);
				FoldManifestHash(hash, std::to_string(dependency.compatibilityFingerprint));
				FoldManifestHash(hash, dependency.required ? "required" : "optional");
			}
		}
		return HashToHex(hash);
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
			{ "sourceMaterialAssignmentCount", value.sourceMaterialAssignmentCount },
			{ "materialSectionCount", value.materialSectionCount },
			{ "clusterMaterialSectionCount", value.clusterMaterialSectionCount },
			{ "fallbackCoverageValid", value.fallbackCoverageValid },
			{ "materialCoverageValid", value.materialCoverageValid }
		};
	}

	nlohmann::json ToJson(const GeneratedModelMeshVirtualGeometryConeSafetySummary& value)
	{
		nlohmann::json disabledReasons = nlohmann::json::array();
		for (const std::string& reason : value.disabledReasons)
		{
			disabledReasons.push_back(reason);
		}

		return {
			{ "recordCount", value.recordCount },
			{ "eligibleClusterCount", value.eligibleClusterCount },
			{ "disabledClusterCount", value.disabledClusterCount },
			{ "metadataValid", value.metadataValid },
			{ "disabledReasons", disabledReasons }
		};
	}

	nlohmann::json ToJson(const GeneratedModelMeshAsset& value)
	{
		nlohmann::json virtualGeometryCoverageTags = nlohmann::json::array();
		for (const std::string& tag : value.virtualGeometryCoverageTags)
		{
			virtualGeometryCoverageTags.push_back(tag);
		}
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
			// Expose the optional palette owner so editor automation can diagnose
			// which synthetic animation skeleton drives a deformable mesh instance.
			{ "animationSkeletonGuid", TerraEngine::ToHexString(value.animationSkeletonGuid) },
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
				{ "coneSafety", ToJson(value.virtualGeometryConeSafetySummary) },
				{ "coverageTags", virtualGeometryCoverageTags },
				{ "warnings", virtualGeometryWarnings }
			} }
		};
	}

	nlohmann::json ToJson(const GeneratedModelPrefabAsset& value)
	{
		nlohmann::json meshGuids = nlohmann::json::array();
		for (const TerraEngine::UUID& guid : value.meshGuids)
		{
			meshGuids.push_back(TerraEngine::ToHexString(guid));
		}

		nlohmann::json materialGuids = nlohmann::json::array();
		for (const TerraEngine::UUID& guid : value.materialGuids)
		{
			materialGuids.push_back(TerraEngine::ToHexString(guid));
		}

		return {
			{ "guid", TerraEngine::ToHexString(value.guid) },
			{ "sourcePath", value.sourcePath.generic_string() },
			{ "name", value.displayName },
			{ "meshGuids", meshGuids },
			{ "materialGuids", materialGuids }
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

	nlohmann::json ToJson(const AnimationImportDependency& value)
	{
		return {
			{ "assetGuid", TerraEngine::ToHexString(value.assetGuid) },
			{ "compatibilityFingerprint", value.compatibilityFingerprint },
			{ "required", value.required }
		};
	}

	nlohmann::json ToJson(const GeneratedAnimationSubAsset& value)
	{
		nlohmann::json dependencies = nlohmann::json::array();
		for(const AnimationImportDependency& dependency : value.dependencies)
		{
			dependencies.push_back(ToJson(dependency));
		}
		return {
			{ "guid", TerraEngine::ToHexString(value.guid) },
			{ "kind", ToString(value.kind) },
			{ "semanticKey", value.semanticKey },
			{ "name", value.displayName },
			{ "sourcePath", value.sourcePath.generic_string() },
			{ "compiledPath", value.compiledPath.generic_string() },
			{ "compatibilityFingerprint", value.compatibilityFingerprint },
			{ "contentHash", value.contentHashHex },
			{ "dependencies", dependencies },
			{ "readOnlyPackageChild", value.readOnlyPackageChild }
		};
	}

	nlohmann::json ToJson(const AnimationImportManifest& value)
	{
		nlohmann::json subAssets = nlohmann::json::array();
		for(const GeneratedAnimationSubAsset& subAsset : value.subAssets)
		{
			subAssets.push_back(ToJson(subAsset));
		}
		return {
			{ "schemaVersion", value.schemaVersion },
			{ "packageGuid", TerraEngine::ToHexString(value.packageGuid) },
			{ "sourceHash", value.sourceHashHex },
			{ "importerName", value.importerName },
			{ "importerVersion", value.importerVersion },
			{ "settingsFingerprint", value.settingsFingerprintHex },
			{ "dependencyFingerprint", value.dependencyFingerprintHex },
			{ "candidateGeneration", value.candidateGeneration },
			{ "publishedGeneration", value.publishedGeneration },
			{ "lastKnownGoodGeneration", value.lastKnownGoodGeneration },
			{ "cancelled", value.cancelled },
			{ "publishedAtomically", value.publishedAtomically },
			{ "preservedLastKnownGood", value.preservedLastKnownGood },
			{ "subAssets", subAssets }
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

		nlohmann::json prefabs = nlohmann::json::array();
		for (const GeneratedModelPrefabAsset& prefab : value.generatedPrefabs)
		{
			prefabs.push_back(ToJson(prefab));
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
			{ "generatedPrefabs", prefabs },
			{ "materialAssignments", assignments },
			{ "animationManifest", ToJson(value.animationManifest) },
			{ "diagnostics", diagnostics }
		};
	}

	void PopulateGeneratedMeshVirtualGeometryMetadata(const std::filesystem::path& compiledMeshPath, GeneratedModelMeshAsset& mesh, std::vector<ModelImportDiagnostic>& diagnostics)
	{
		// Reinspection replaces every advisory field atomically. A direct route must
		// never retain virtual hierarchy rows from a prior cook or caller-owned cache.
		mesh.virtualGeometryRequested = true;
		mesh.virtualGeometryCooked = false;
		mesh.virtualGeometryStatus = "Failed";
		mesh.virtualGeometryDetailLevels = 0u;
		mesh.virtualGeometryMeshlets = 0u;
		mesh.virtualGeometrySourceTriangles = 0u;
		mesh.virtualGeometryTargets.clear();
		mesh.virtualGeometryWarnings.clear();
		mesh.virtualGeometryClusterSummary = {};
		mesh.virtualGeometryConeSafetySummary = {};

		std::ifstream input(compiledMeshPath, std::ios::binary);
		if (!input)
		{
			mesh.virtualGeometryWarnings.push_back("Compiled mesh was not available for canonical geometry metadata inspection.");
			diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "VirtualGeometryMetadataUnavailable", mesh.displayName, "Package mesh was imported, but canonical geometry metadata could not be inspected.", "The generated mesh remains available for rebuild." });
			return;
		}

		input.seekg(0, std::ios::end);
		const std::streamoff byteCount = input.tellg();
		input.seekg(0, std::ios::beg);
		if (byteCount <= 0)
		{
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Compiled mesh is empty and requires a canonical TEMFPRD1 recook.");
			return;
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byteCount));
		input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!input)
		{
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Compiled mesh could not be read completely and requires a canonical recook.");
			return;
		}

		TerraEngine::Resource::TemfProductionValidationLimits limits{};
		limits.availableCapabilities =
			kVirtualGeometryCodecCapabilityExpanded
			| kVirtualGeometryCodecCapabilityMeshoptimizer;
		const TerraEngine::Resource::TemfProductionValidationResult validated =
			TerraEngine::Resource::ValidateTemfProductionFile(bytes, limits);
		if (!validated.IsComplete())
		{
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Compiled mesh requires a canonical TEMFPRD1 recook: " + validated.diagnostic);
			diagnostics.push_back({ ModelDiagnosticSeverity::Warning, "VirtualGeometryNeedsRebuild", mesh.displayName, "Generated package mesh is not a canonical TEMFPRD1 payload.", "Reimport the package to rebuild generated mesh outputs." });
			return;
		}

		// Decode only the typed metadata streams needed by package inventory. This
		// follows the same bounded codec contract as ResourceManager without
		// publishing a temporary resource or mutating the live cache.
		const auto DecodeStream = [&](TemfStreamKind kind, std::uint64_t attributeMask, std::vector<std::uint8_t>& destination)
		{
			destination.clear();
			for (const TemfBlockDescriptor& descriptor : validated.directory.blocks)
			{
				if (descriptor.streamKind != static_cast<std::uint16_t>(kind)
					|| descriptor.attributeMask != attributeMask)
				{
					continue;
				}
				const std::span<const std::uint8_t> encoded(bytes.data() + descriptor.encodedPayloadOffset, static_cast<std::size_t>(descriptor.encodedBytes));
				const auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock(
					{ descriptor, encoded, validated.directory.quantizationDomains, limits.availableCapabilities },
					limits);
				if (!decoded.IsComplete())
				{
					destination.clear();
					return false;
				}
				destination.insert(destination.end(), decoded.bytes.begin(), decoded.bytes.end());
			}
			return !destination.empty();
		};
		const auto DecodeRecords = [&](std::uint64_t attributeMask, auto& destination)
		{
			using Record = typename std::remove_reference_t<decltype(destination)>::value_type;
			std::vector<std::uint8_t> decoded;
			if (!DecodeStream(TemfStreamKind::Metadata, attributeMask, decoded)
				|| decoded.size() % sizeof(Record) != 0u)
			{
				destination.clear();
				return false;
			}
			destination.resize(decoded.size() / sizeof(Record));
			std::memcpy(destination.data(), decoded.data(), decoded.size());
			return true;
		};

		std::vector<std::uint8_t> runtimeBytes;
		if (!DecodeStream(TemfStreamKind::Metadata, kTemfMetadataRuntimeLayout, runtimeBytes)
			|| runtimeBytes.size() != sizeof(TemfRuntimeDecodeMetadata))
		{
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Canonical runtime geometry metadata is missing or malformed.");
			return;
		}
		TemfRuntimeDecodeMetadata runtime{};
		std::memcpy(&runtime, runtimeBytes.data(), sizeof(runtime));
		mesh.virtualGeometryMeshlets = runtime.meshletCount;
		mesh.virtualGeometrySourceTriangles = runtime.meshletPrimitiveIndexCount / 3u;

		if ((validated.directory.header.representationFlags & kTemfRepresentationVirtualGeometry) == 0u)
		{
			std::vector<VirtualGeometryDetailLevelRecord> directDetailLevels;
			if (!DecodeRecords(kTemfMetadataVirtualDetailLevels, directDetailLevels)
				|| directDetailLevels.empty())
			{
				mesh.virtualGeometryStatus = "NeedsRebuild";
				mesh.virtualGeometryWarnings.push_back("Canonical direct-resident LOD ranges are missing or malformed.");
				return;
			}

			std::uint64_t coveredMeshlets = 0u;
			bool rangesValid = true;
			for (std::size_t detailIndex = 0u; detailIndex < directDetailLevels.size(); ++detailIndex)
			{
				const VirtualGeometryDetailLevelRecord& detail = directDetailLevels[detailIndex];
				// Direct-resident levels share one packed meshlet allocation. Requiring
				// dense, ordered ranges keeps tooling aligned with the runtime loader and
				// prevents a corrupt report from hiding meshlets or reading past the buffer.
				if (detail.lodIndex != detailIndex
					|| detail.meshletOffset != coveredMeshlets
					|| detail.meshletCount == 0u)
				{
					rangesValid = false;
					break;
				}
				coveredMeshlets += detail.meshletCount;
			}
			if (!rangesValid || coveredMeshlets != runtime.meshletCount)
			{
				mesh.virtualGeometryStatus = "NeedsRebuild";
				mesh.virtualGeometryWarnings.push_back("Canonical direct-resident LOD ranges do not cover the packed meshlet allocation.");
				return;
			}

			mesh.virtualGeometryDetailLevels = static_cast<std::uint32_t>(directDetailLevels.size());
			mesh.virtualGeometrySourceTriangles = directDetailLevels.front().primitiveCount;
			mesh.virtualGeometryStatus = "Skipped";
			mesh.virtualGeometryWarnings.push_back("Compiled mesh uses selectable direct-resident LOD ranges; no virtual pages were built.");
			return;
		}

		std::vector<std::uint8_t> virtualHeaderBytes;
		VirtualGeometryHeader virtualHeader{};
		std::vector<VirtualGeometryDetailLevelRecord> detailLevels;
		std::vector<VirtualGeometryMaterialSectionRecord> materialSections;
		std::vector<VirtualGeometryCullableGroupRecord> cullableGroups;
		std::vector<VirtualGeometryPageRecord> pages;
		std::vector<VirtualGeometryRequestedTargetRecord> requestedTargets;
		std::vector<VirtualGeometrySkippedDetailRecord> skippedDetails;
		std::vector<VirtualGeometryClusterRecord> clusters;
		std::vector<VirtualGeometryClusterPageLinkRecord> clusterPageLinks;
		std::vector<VirtualGeometryConeSafetyRecord> coneSafety;
		std::vector<std::uint8_t> stringBytes;
		const bool hierarchyComplete =
			DecodeStream(TemfStreamKind::Metadata, kTemfMetadataVirtualHeader, virtualHeaderBytes)
			&& virtualHeaderBytes.size() == sizeof(VirtualGeometryHeader)
			&& DecodeRecords(kTemfMetadataVirtualDetailLevels, detailLevels)
			&& DecodeRecords(kTemfMetadataVirtualMaterialSections, materialSections)
			&& DecodeRecords(kTemfMetadataVirtualCullableGroups, cullableGroups)
			&& DecodeRecords(kTemfMetadataVirtualPages, pages)
			&& DecodeRecords(kTemfMetadataVirtualClusters, clusters)
			&& DecodeRecords(kTemfMetadataVirtualClusterPageLinks, clusterPageLinks)
			&& DecodeRecords(kTemfMetadataVirtualConeSafety, coneSafety);
		if (!hierarchyComplete)
		{
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Canonical virtual-geometry hierarchy metadata is incomplete.");
			return;
		}
		std::memcpy(&virtualHeader, virtualHeaderBytes.data(), sizeof(virtualHeader));
		DecodeRecords(kTemfMetadataVirtualRequestedTargets, requestedTargets);
		DecodeRecords(kTemfMetadataVirtualSkippedDetails, skippedDetails);
		DecodeStream(TemfStreamKind::Metadata, kTemfMetadataVirtualDiagnosticStrings, stringBytes);
		const std::string stringTable(reinterpret_cast<const char*>(stringBytes.data()), stringBytes.size());

		if (detailLevels.size() != virtualHeader.detailLevelCount
			|| materialSections.size() != virtualHeader.materialSectionCount
			|| cullableGroups.size() != virtualHeader.cullableGroupCount
			|| pages.size() != virtualHeader.pageCount
			|| clusters.size() != virtualHeader.clusterCount
			|| clusterPageLinks.size() != virtualHeader.clusterPageLinkCount
			|| coneSafety.size() != virtualHeader.coneSafetyCount)
		{
			mesh.virtualGeometryStatus = "NeedsRebuild";
			mesh.virtualGeometryWarnings.push_back("Canonical virtual-geometry record counts disagree with their root metadata.");
			return;
		}

		mesh.virtualGeometryCooked = true;
		mesh.virtualGeometryStatus = "Cooked";
		mesh.virtualGeometryDetailLevels = virtualHeader.detailLevelCount;
		mesh.virtualGeometrySourceTriangles = virtualHeader.sourceTriangleCount;
		mesh.virtualGeometryClusterSummary.clusterCount = virtualHeader.clusterCount;
		mesh.virtualGeometryClusterSummary.pageLinkCount = virtualHeader.clusterPageLinkCount;
		mesh.virtualGeometryClusterSummary.materialSectionCount = virtualHeader.materialSectionCount;
		mesh.virtualGeometryClusterSummary.fallbackCoverageValid =
			(virtualHeader.flags & kVirtualGeometryValidationFlagFallbackCoverageValidated) != 0u;
		mesh.virtualGeometryClusterSummary.clusterMaterialSectionCount =
			CountClusterMaterialSections(clusters);
		mesh.virtualGeometryClusterSummary.materialCoverageValid =
			mesh.virtualGeometryClusterSummary.fallbackCoverageValid
			&& mesh.virtualGeometryClusterSummary.clusterMaterialSectionCount <= virtualHeader.materialSectionCount;

		mesh.virtualGeometryConeSafetySummary.recordCount =
			static_cast<std::uint32_t>(coneSafety.size());
		mesh.virtualGeometryConeSafetySummary.metadataValid =
			(virtualHeader.flags & kVirtualGeometryValidationFlagConeSafetyMetadataValidated) != 0u
			&& coneSafety.size() == clusters.size();
		for (const VirtualGeometryConeSafetyRecord& cone : coneSafety)
		{
			if ((cone.flags & kVirtualGeometryConeSafetyFlagEligible) != 0u)
			{
				++mesh.virtualGeometryConeSafetySummary.eligibleClusterCount;
				continue;
			}
			++mesh.virtualGeometryConeSafetySummary.disabledClusterCount;
			const std::size_t reasonEnd =
				static_cast<std::size_t>(cone.disabledReasonStringOffset)
				+ cone.disabledReasonStringSize;
			if (cone.disabledReasonStringSize == 0u || reasonEnd > stringTable.size())
			{
				continue;
			}
			const std::string reason = stringTable.substr(
				cone.disabledReasonStringOffset,
				cone.disabledReasonStringSize);
			if (std::find(
					mesh.virtualGeometryConeSafetySummary.disabledReasons.begin(),
					mesh.virtualGeometryConeSafetySummary.disabledReasons.end(),
					reason)
				== mesh.virtualGeometryConeSafetySummary.disabledReasons.end())
			{
				mesh.virtualGeometryConeSafetySummary.disabledReasons.push_back(reason);
			}
		}

		mesh.virtualGeometryTargets.reserve(requestedTargets.size());
		for (const VirtualGeometryRequestedTargetRecord& requested : requestedTargets)
		{
			GeneratedModelMeshVirtualGeometryTarget target{};
			target.targetIndex = requested.targetIndex;
			target.requestedRatio = requested.requestedRatio;
			target.status = VirtualGeometryTargetStatusText(requested.status);
			target.desiredTriangleCount = requested.desiredTriangleCount;
			target.desiredMeshletCount = requested.desiredMeshletCount;
			target.actualRatio = requested.requestedRatio;
			target.triangleCount = requested.desiredTriangleCount;
			target.meshletCount = requested.desiredMeshletCount;

			const auto generated = std::find_if(
				detailLevels.begin(),
				detailLevels.end(),
				[&requested](const VirtualGeometryDetailLevelRecord& detail)
				{
					return detail.requestedTargetIndex == requested.targetIndex;
				});
			if (generated != detailLevels.end())
			{
				target.status = "Generated";
				target.actualRatio = generated->targetRatio;
				target.actualTriangleCount = generated->primitiveCount;
				target.actualMeshletCount = generated->meshletCount;
				target.triangleCount = generated->primitiveCount;
				target.meshletCount = generated->meshletCount;
				target.nearestGeneratedLevelIndex = generated->lodIndex;
				target.geometricError = generated->geometricError;
			}

			const auto skipped = std::find_if(
				skippedDetails.begin(),
				skippedDetails.end(),
				[&requested](const VirtualGeometrySkippedDetailRecord& record)
				{
					return record.targetIndex == requested.targetIndex;
				});
			if (skipped != skippedDetails.end())
			{
				target.status = VirtualGeometryTargetStatusText(skipped->status);
				target.reason = VirtualGeometrySkippedReasonText(skipped->reason);
				target.nearestGeneratedLevelIndex = skipped->nearestGeneratedLodIndex;
				target.actualRatio = skipped->actualCandidateRatio > 0.0f
					? skipped->actualCandidateRatio
					: skipped->actualNearestRatio;
				target.actualTriangleCount = skipped->candidateTriangleCount;
				target.actualMeshletCount = skipped->candidateMeshletCount;
				if (target.actualTriangleCount > 0u) target.triangleCount = target.actualTriangleCount;
				if (target.actualMeshletCount > 0u) target.meshletCount = target.actualMeshletCount;
				const std::size_t messageEnd =
					static_cast<std::size_t>(skipped->messageStringOffset)
					+ skipped->messageStringSize;
				if (messageEnd <= stringTable.size())
				{
					target.message = stringTable.substr(
						skipped->messageStringOffset,
						skipped->messageStringSize);
				}
			}
			mesh.virtualGeometryTargets.push_back(std::move(target));
		}
	}

}
