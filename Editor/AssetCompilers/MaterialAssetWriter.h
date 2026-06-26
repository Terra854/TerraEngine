// MaterialAssetWriter.h - Small helper for writing generated TMAT assets.
//
// glTF package import needs to author Terra materials from external material
// descriptions. Centralizing the binary write keeps the importer focused on
// package interpretation instead of TMAT layout details.

#pragma once

#include <filesystem>
#include <vector>

#include "Engine/File Formats/TerraEngineMaterialFile.h"

namespace TerraEngine::AssetPipeline
{
	struct MaterialAssetWriteDesc
	{
		MaterialHeader header{};
		std::vector<TextureBinding> textureBindings;
		std::vector<FloatParameter> floatParameters;
	};

	[[nodiscard]] MaterialHeader CreateDefaultMaterialHeader(const TerraEngine::UUID& shaderCollectionId);
	[[nodiscard]] bool WriteMaterialAsset(const std::filesystem::path& outputPath, const MaterialAssetWriteDesc& desc);
}
