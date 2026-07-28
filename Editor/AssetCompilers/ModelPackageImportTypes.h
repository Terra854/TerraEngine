// ModelPackageImportTypes.h - Shared contracts for package-level model imports.
//
// Package importers such as glTF and FBX decode more than one loose file. These
// records describe the source package, generated sub-assets, material links, and
// diagnostics in a format that the AssetDatabase, inspector, browser, and MCP
// bridge can all read after the import boundary publishes the sidecar.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Utilities/UUID.h"

namespace TerraEngine::AssetPipeline
{
	enum class ModelPackageKind
	{
		GltfText,
		GltfBinary,
		Fbx
	};

	enum class ModelImportStatus
	{
		Succeeded,
		SucceededWithWarnings,
		Failed
	};

	enum class ModelDiagnosticSeverity
	{
		Info,
		Warning,
		Error
	};

	enum class ModelTextureUsage
	{
		BaseColor,
		Normal,
		MetallicRoughness,
		Occlusion,
		Emissive,
		Unknown
	};

	enum class ModelTextureSemantic
	{
		Color,
		Data
	};

	// Every generated package child has a semantic kind and key. The pair is
	// stable across source-array reorder and display-name changes, unlike an
	// importer-local index, so scenes and controllers can safely retain its GUID.
	enum class AnimationImportSubAssetKind : std::uint8_t
	{
		Skeleton,
		AnimationClip,
		DeformableMesh,
		MorphTarget,
		Material,
		Texture,
		Prefab,
		// Appended to preserve the serialized numeric values of every existing
		// animation-package kind while distinguishing ordinary render meshes.
		Mesh
	};

	struct ImportedModelPackage
	{
		std::filesystem::path sourcePath;
		ModelPackageKind packageKind = ModelPackageKind::GltfText;
		std::string sourceHashHex;
		std::filesystem::path importRoot;
		std::vector<std::filesystem::path> externalDependencies;
	};

	struct ModelImportDiagnostic
	{
		ModelDiagnosticSeverity severity = ModelDiagnosticSeverity::Info;
		std::string code;
		std::string source;
		std::string message;
		std::string fallback;
	};

	struct GeneratedModelTextureAsset
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::optional<std::size_t> sourceImageIndex;
		std::optional<std::size_t> sourceTextureIndex;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		std::string displayName;
		ModelTextureUsage usage = ModelTextureUsage::Unknown;
		ModelTextureSemantic semantic = ModelTextureSemantic::Data;
	};

	struct GeneratedModelMaterialAsset
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::optional<std::size_t> sourceMaterialIndex;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		std::string displayName;
		std::array<float, 4> baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::array<float, 3> emissiveFactor = { 0.0f, 0.0f, 0.0f };
		float metallicFactor = 0.0f;
		float roughnessFactor = 1.0f;
		float normalScale = 1.0f;
		float occlusionStrength = 1.0f;
		bool doubleSided = false;
		std::vector<TerraEngine::UUID> textureGuids;
	};

	struct GeneratedModelMeshVirtualGeometrySettings
	{
		bool hasOverride = false;
		std::vector<float> targetRatios;
		// Generated meshes inherit permissive defaults, while a saved per-mesh
		// override can still require a chosen percentage reduction.
		std::uint32_t minimumSourceTriangles = 1u;
		std::uint32_t minimumGeneratedTriangles = 1u;
		std::uint32_t minimumMeshletsPerGeneratedLevel = 1u;
		float minimumUsefulTriangleReduction = 0.0f;
		float minimumUsefulMeshletReduction = 0.0f;
		float maximumGeometricError = 0.1f;
		std::string settingsSource = "ProjectDefaults";
	};

	struct GeneratedModelMeshVirtualGeometryTarget
	{
		std::uint32_t targetIndex = 0u;
		float requestedRatio = 1.0f;
		std::string status = "Requested";
		std::string reason;
		// Desired counts are the advisory request; actual counts are the
		// generated or attempted candidate that the cooker evaluated.
		std::uint32_t desiredTriangleCount = 0u;
		std::uint32_t desiredMeshletCount = 0u;
		float actualRatio = 1.0f;
		std::uint32_t actualTriangleCount = 0u;
		std::uint32_t actualMeshletCount = 0u;
		std::uint32_t triangleCount = 0u;
		std::uint32_t meshletCount = 0u;
		std::uint32_t nearestGeneratedLevelIndex = 0u;
		float geometricError = 0.0f;
		std::string message;
	};

	struct GeneratedModelMeshVirtualGeometryClusterSummary
	{
		std::uint32_t clusterCount = 0u;
		std::uint32_t pageLinkCount = 0u;
		std::uint32_t sourceMaterialAssignmentCount = 0u;
		std::uint32_t materialSectionCount = 0u;
		std::uint32_t clusterMaterialSectionCount = 0u;
		bool fallbackCoverageValid = false;
		bool materialCoverageValid = false;
	};

	struct GeneratedModelMeshVirtualGeometryConeSafetySummary
	{
		std::uint32_t recordCount = 0u;
		std::uint32_t eligibleClusterCount = 0u;
		std::uint32_t disabledClusterCount = 0u;
		bool metadataValid = false;
		std::vector<std::string> disabledReasons;
	};

	struct GeneratedModelMeshAsset
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::size_t sourceMeshIndex = 0;
		std::optional<std::size_t> sourcePrimitiveIndex;
		std::string sourceNodePath;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		std::string displayName;
		// Animated glTF morph instances keep the skeleton that owns their source
		// node. The prefab writer uses this only when one package contains several
		// independent animation roots; ordinary static meshes leave it empty.
		TerraEngine::UUID animationSkeletonGuid = TerraEngine::EMPTY_UUID;
		std::array<float, 3> localBoundsMin = { 0.0f, 0.0f, 0.0f };
		std::array<float, 3> localBoundsMax = { 0.0f, 0.0f, 0.0f };
		bool virtualGeometryRequested = true;
		bool virtualGeometryCooked = false;
		std::string virtualGeometryStatus = "Unknown";
		std::uint32_t virtualGeometryDetailLevels = 0u;
		std::uint32_t virtualGeometryMeshlets = 0u;
		std::uint32_t virtualGeometrySourceTriangles = 0u;
		GeneratedModelMeshVirtualGeometrySettings virtualGeometrySettings;
		std::vector<GeneratedModelMeshVirtualGeometryTarget> virtualGeometryTargets;
		GeneratedModelMeshVirtualGeometryClusterSummary virtualGeometryClusterSummary;
		GeneratedModelMeshVirtualGeometryConeSafetySummary virtualGeometryConeSafetySummary;
		// Coverage tags are intentionally plain strings because they are
		// developer-facing diagnostics, not runtime behavior. Importers use them
		// to say which hardening fixture categories a generated package mesh can
		// exercise in clean-checkout tests.
		std::vector<std::string> virtualGeometryCoverageTags;
		std::vector<std::string> virtualGeometryWarnings;
	};

	struct GeneratedModelPrefabAsset
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::filesystem::path sourcePath;
		std::string displayName;
		std::vector<TerraEngine::UUID> meshGuids;
		std::vector<TerraEngine::UUID> materialGuids;
	};

	struct ModelMaterialAssignment
	{
		TerraEngine::UUID meshGuid = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialGuid = TerraEngine::EMPTY_UUID;
		std::size_t sourceMeshIndex = 0;
		std::size_t sourcePrimitiveIndex = 0;
		std::string slotName;
	};

	struct AnimationImportDependency
	{
		TerraEngine::UUID assetGuid = TerraEngine::EMPTY_UUID;
		std::uint64_t compatibilityFingerprint = 0u;
		bool required = true;
	};

	struct GeneratedAnimationSubAsset
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		AnimationImportSubAssetKind kind = AnimationImportSubAssetKind::AnimationClip;
		std::string semanticKey;
		std::string displayName;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		std::uint64_t compatibilityFingerprint = 0u;
		std::string contentHashHex;
		std::vector<AnimationImportDependency> dependencies;
		bool readOnlyPackageChild = true;
	};

	// The manifest is the durable publication boundary for one complete package
	// generation. Candidate data is assembled in staging and becomes observable
	// only after every output and this record have published successfully.
	struct AnimationImportManifest
	{
		std::uint32_t schemaVersion = 1u;
		TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
		std::string sourceHashHex;
		std::string importerName;
		std::string importerVersion = "1";
		std::string settingsFingerprintHex;
		std::string dependencyFingerprintHex;
		std::uint64_t candidateGeneration = 0u;
		std::uint64_t publishedGeneration = 0u;
		std::uint64_t lastKnownGoodGeneration = 0u;
		bool cancelled = false;
		bool publishedAtomically = false;
		bool preservedLastKnownGood = false;
		std::vector<GeneratedAnimationSubAsset> subAssets;
	};

	struct ModelPackageImportResult
	{
		ModelImportStatus status = ModelImportStatus::Failed;
		TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
		ImportedModelPackage package;
		std::vector<GeneratedModelMeshAsset> generatedMeshes;
		std::vector<GeneratedModelMaterialAsset> generatedMaterials;
		std::vector<GeneratedModelTextureAsset> generatedTextures;
		std::vector<GeneratedModelPrefabAsset> generatedPrefabs;
		std::vector<ModelMaterialAssignment> materialAssignments;
		AnimationImportManifest animationManifest;
		std::vector<ModelImportDiagnostic> diagnostics;
	};

	[[nodiscard]] const char* ToString(ModelPackageKind kind);
	[[nodiscard]] const char* ToString(ModelImportStatus status);
	[[nodiscard]] const char* ToString(ModelDiagnosticSeverity severity);
	[[nodiscard]] const char* ToString(ModelTextureUsage usage);
	[[nodiscard]] const char* ToString(ModelTextureSemantic semantic);
	[[nodiscard]] const char* ToString(AnimationImportSubAssetKind kind);
	[[nodiscard]] TerraEngine::UUID BuildStableGeneratedModelUuid(const TerraEngine::UUID& packageGuid, std::string_view role, std::size_t index);
	[[nodiscard]] TerraEngine::UUID BuildStableAnimationSubAssetUuid(
		const TerraEngine::UUID& packageGuid,
		AnimationImportSubAssetKind kind,
		std::string_view semanticKey);
	void CanonicalizeAnimationImportManifest(AnimationImportManifest& manifest);
	// Dependency validation is shared by the publication boundary and editor
	// metadata readers. It rejects dangling required references and proves that a
	// generated prefab retains every animation asset needed to bind it later.
	[[nodiscard]] bool ValidateAnimationImportManifestDependencies(
		const AnimationImportManifest& manifest,
		std::string* failureReason = nullptr);
	[[nodiscard]] std::string BuildAnimationImportDependencyFingerprint(const AnimationImportManifest& manifest);

	[[nodiscard]] nlohmann::json ToJson(const ImportedModelPackage& value);
	[[nodiscard]] nlohmann::json ToJson(const ModelImportDiagnostic& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelTextureAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelMaterialAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelMeshVirtualGeometrySettings& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelMeshVirtualGeometryTarget& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelMeshVirtualGeometryClusterSummary& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelMeshVirtualGeometryConeSafetySummary& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelMeshAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelPrefabAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const ModelMaterialAssignment& value);
	[[nodiscard]] nlohmann::json ToJson(const AnimationImportDependency& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedAnimationSubAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const AnimationImportManifest& value);
	[[nodiscard]] nlohmann::json ToJson(const ModelPackageImportResult& value);
	void PopulateGeneratedMeshVirtualGeometryMetadata(const std::filesystem::path& compiledMeshPath, GeneratedModelMeshAsset& mesh, std::vector<ModelImportDiagnostic>& diagnostics);
}
