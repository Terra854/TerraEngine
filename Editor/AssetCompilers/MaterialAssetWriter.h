// MaterialAssetWriter.h - Small helper for writing generated TMAT assets.
//
// glTF package import needs to author Terra materials from external material
// descriptions. Centralizing the binary write keeps the importer focused on
// package interpretation instead of TMAT layout details.

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <cstdint>
#include <vector>

#include "Engine/File Formats/TerraEngineMaterialFile.h"

namespace TerraEngine::AssetPipeline
{
	struct MaterialAssetWriteDesc
	{
		MaterialHeader header{};
		std::vector<TextureBinding> textureBindings;
		std::vector<FloatParameter> floatParameters;
		// Optional policy is emitted after the legacy TMAT arrays so old readers
		// retain ordinary material behavior without interpreting this record.
		std::optional<VirtualGeometryMaterialExtension> virtualGeometryExtension;
	};

	[[nodiscard]] MaterialHeader CreateDefaultMaterialHeader(const TerraEngine::UUID& shaderCollectionId);
	[[nodiscard]] bool WriteMaterialAsset(const std::filesystem::path& outputPath, const MaterialAssetWriteDesc& desc);

	// Validate and atomically publish an editor-authored TMAT byte buffer. The
	// Inspector edits an existing material in place, so this entry point keeps
	// those saves on the same last-known-good publication path as generated
	// materials instead of writing directly over the live asset.
	[[nodiscard]] bool PublishMaterialAssetBytes(
		const std::filesystem::path& outputPath,
		std::span<const std::uint8_t> bytes);
}
