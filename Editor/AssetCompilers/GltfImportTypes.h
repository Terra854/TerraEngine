// GltfImportTypes.h - Shared data contracts for package-level glTF imports.
//
// These types are intentionally editor-facing rather than renderer-facing. They
// describe what the importer discovered, generated, skipped, or had to fall back
// from so the AssetDatabase, inspector, tests, and MCP bridge can all consume
// the same import result.

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
	enum class GltfPackageKind
	{
		GltfText,
		GltfBinary
	};

	enum class GltfImportStatus
	{
		Succeeded,
		SucceededWithWarnings,
		Failed
	};

	enum class GltfDiagnosticSeverity
	{
		Info,
		Warning,
		Error
	};

	enum class GltfTextureUsage
	{
		BaseColor,
		Normal,
		MetallicRoughness,
		Occlusion,
		Emissive,
		Unknown
	};

	enum class GltfTextureSemantic
	{
		Color,
		Data
	};

	struct ImportedGltfPackage
	{
		std::filesystem::path sourcePath;
		GltfPackageKind packageKind = GltfPackageKind::GltfText;
		std::string sourceHashHex;
		std::filesystem::path importRoot;
		std::vector<std::filesystem::path> externalDependencies;
	};

	struct GltfImportDiagnostic
	{
		GltfDiagnosticSeverity severity = GltfDiagnosticSeverity::Info;
		std::string code;
		std::string source;
		std::string message;
		std::string fallback;
	};

	struct GeneratedGltfTextureAsset
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::optional<std::size_t> sourceImageIndex;
		std::optional<std::size_t> sourceTextureIndex;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		std::string displayName;
		GltfTextureUsage usage = GltfTextureUsage::Unknown;
		GltfTextureSemantic semantic = GltfTextureSemantic::Data;
	};

	struct GeneratedGltfMaterialAsset
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

	struct GeneratedGltfMeshAsset
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::size_t sourceMeshIndex = 0;
		std::optional<std::size_t> sourcePrimitiveIndex;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		std::string displayName;
		std::array<float, 3> localBoundsMin = { 0.0f, 0.0f, 0.0f };
		std::array<float, 3> localBoundsMax = { 0.0f, 0.0f, 0.0f };
	};

	struct GltfMaterialAssignment
	{
		TerraEngine::UUID meshGuid = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialGuid = TerraEngine::EMPTY_UUID;
		std::size_t sourceMeshIndex = 0;
		std::size_t sourcePrimitiveIndex = 0;
		std::string slotName;
	};

	struct GltfImportResult
	{
		GltfImportStatus status = GltfImportStatus::Failed;
		TerraEngine::UUID packageGuid = TerraEngine::EMPTY_UUID;
		ImportedGltfPackage package;
		std::vector<GeneratedGltfMeshAsset> generatedMeshes;
		std::vector<GeneratedGltfMaterialAsset> generatedMaterials;
		std::vector<GeneratedGltfTextureAsset> generatedTextures;
		std::vector<GltfMaterialAssignment> materialAssignments;
		std::vector<GltfImportDiagnostic> diagnostics;
	};

	[[nodiscard]] const char* ToString(GltfImportStatus status);
	[[nodiscard]] const char* ToString(GltfDiagnosticSeverity severity);
	[[nodiscard]] const char* ToString(GltfTextureUsage usage);
	[[nodiscard]] const char* ToString(GltfTextureSemantic semantic);

	[[nodiscard]] nlohmann::json ToJson(const ImportedGltfPackage& value);
	[[nodiscard]] nlohmann::json ToJson(const GltfImportDiagnostic& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedGltfTextureAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedGltfMaterialAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const GeneratedGltfMeshAsset& value);
	[[nodiscard]] nlohmann::json ToJson(const GltfMaterialAssignment& value);
	[[nodiscard]] nlohmann::json ToJson(const GltfImportResult& value);
}
