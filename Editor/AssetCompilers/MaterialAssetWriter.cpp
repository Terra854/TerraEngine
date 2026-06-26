// MaterialAssetWriter.cpp - TMAT binary writer for generated editor assets.
//
// The writer emits the same layout the ResourceManager already understands:
// a MaterialHeader followed by optional TextureBinding and FloatParameter
// arrays. Importers can therefore create ordinary Terra material assets.

#include "MaterialAssetWriter.h"

#include <cstring>
#include <fstream>

namespace TerraEngine::AssetPipeline
{
	MaterialHeader CreateDefaultMaterialHeader(const TerraEngine::UUID& shaderCollectionId)
	{
		MaterialHeader header{};
		std::memcpy(header.magic, "TMAT", sizeof(header.magic));
		header.version = kMaterialCurrentVersion;
		header.headerSize = static_cast<std::uint16_t>(sizeof(MaterialHeader));
		header.flags = static_cast<std::uint32_t>(ReceivesShadows);
		header.shadingModel = static_cast<std::uint32_t>(ShadingModel::MetallicRoughness);
		header.alphaMode = static_cast<std::uint32_t>(AlphaMode::Opaque);
		header.shaderCollectionId = shaderCollectionId;
		header.baseColorFactor[0] = 1.0f;
		header.baseColorFactor[1] = 1.0f;
		header.baseColorFactor[2] = 1.0f;
		header.baseColorFactor[3] = 1.0f;
		header.emissiveFactor[0] = 0.0f;
		header.emissiveFactor[1] = 0.0f;
		header.emissiveFactor[2] = 0.0f;
		header.metallicFactor = 0.0f;
		header.roughnessFactor = 1.0f;
		header.normalScale = 1.0f;
		header.occlusionStrength = 1.0f;
		header.alphaCutoff = 0.5f;
		header.textureArrayOffset = header.headerSize;
		header.floatParameterOffset = header.headerSize;
		return header;
	}

	bool WriteMaterialAsset(const std::filesystem::path& outputPath, const MaterialAssetWriteDesc& desc)
	{
		std::filesystem::create_directories(outputPath.parent_path());

		MaterialHeader header = desc.header;
		header.textureCount = static_cast<std::uint32_t>(desc.textureBindings.size());
		header.floatParameterCount = static_cast<std::uint32_t>(desc.floatParameters.size());
		header.textureArrayOffset = header.headerSize;
		header.floatParameterOffset = header.textureArrayOffset + static_cast<std::uint32_t>(desc.textureBindings.size() * sizeof(TextureBinding));

		std::ofstream out(outputPath, std::ios::binary);
		if (!out)
		{
			return false;
		}

		out.write(reinterpret_cast<const char*>(&header), sizeof(header));
		if (!desc.textureBindings.empty())
		{
			out.write(reinterpret_cast<const char*>(desc.textureBindings.data()), static_cast<std::streamsize>(desc.textureBindings.size() * sizeof(TextureBinding)));
		}
		if (!desc.floatParameters.empty())
		{
			out.write(reinterpret_cast<const char*>(desc.floatParameters.data()), static_cast<std::streamsize>(desc.floatParameters.size() * sizeof(FloatParameter)));
		}

		return static_cast<bool>(out);
	}
}
