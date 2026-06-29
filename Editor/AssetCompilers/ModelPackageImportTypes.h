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

	struct GeneratedModelMeshAsset
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::size_t sourceMeshIndex = 0;
		std::optional<std::size_t> sourcePrimitiveIndex;
		std::string sourceNodePath;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		std::string displayName;
		std::array<float, 3> localBoundsMin = { 0.0f, 0.0f, 0.0f };
		std::array<float, 3> localBoundsMax = { 0.0f, 0.0f, 0.0f };
	};

	struct ModelMaterialAssignment
	{
		TerraEngine::UUID meshGuid = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialGuid = TerraEngine::EMPTY_UUID;
		std::size_t sourceMeshIndex = 0;
		std::size_t sourcePrimitiveIndex = 0;
		std::string slotName;
	};

	struct ModelPackageImportResult
	{
		ModelImportStatus status = ModelImportStatus::Failed;
		TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
		ImportedModelPackage package;
		std::vector<GeneratedModelMeshAsset> generatedMeshes;
		std::vector<GeneratedModelMaterialAsset> generatedMaterials;
		std::vector<GeneratedModelTextureAsset> generatedTextures;
		std::vector<ModelMaterialAssignment> materialAssignments;
		std::vector<ModelImportDiagnostic> diagnostics;
	};

	[[nodiscard]] const char* ToString(ModelPackageKind kind);
	[[nodiscard]] const char* ToString(ModelImportStatus status);
	[[nodiscard]] const char* ToString(ModelDiagnosticSeverity severity);
	[[nodiscard]] const char* ToString(ModelTextureUsage usage);
	[[nodiscard]] const char* ToString(ModelTextureSemantic semantic);

	[[nodiscard]] nlohmann::json ToJson(const ImportedModelPackage& value);
	[[nodiscard]] nlohmann::json ToJson(const ModelImportDiagnostic& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelTextureAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelMaterialAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedModelMeshAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const ModelMaterialAssignment& value);
	[[nodiscard]] nlohmann::json ToJson(const ModelPackageImportResult& value);
}
