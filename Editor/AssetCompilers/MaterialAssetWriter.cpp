// MaterialAssetWriter.cpp - TMAT binary writer for generated editor assets.
//
// The writer emits the same layout the ResourceManager already understands:
// a MaterialHeader followed by optional TextureBinding and FloatParameter
// arrays. Importers can therefore create ordinary Terra material assets.

#include "MaterialAssetWriter.h"

#include <cstring>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>

#if defined(_WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#endif

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		bool RangeFits(
			std::size_t totalBytes,
			std::uint32_t offset,
			std::uint32_t count,
			std::size_t stride)
		{
			// Division avoids overflowing count * stride when validating corrupt
			// editor input. An empty range may end exactly at the file boundary.
			return offset <= totalBytes
				&& count <= (totalBytes - offset) / stride;
		}

		bool ValidateMaterialBytes(std::span<const std::uint8_t> bytes)
		{
			if (bytes.size() < sizeof(MaterialHeader))
			{
				return false;
			}

			MaterialHeader header{};
			std::memcpy(&header, bytes.data(), sizeof(header));
			std::uint32_t magic = 0u;
			std::memcpy(&magic, header.magic, sizeof(magic));
			if (magic != kMaterialMagicValue
				|| header.version != kMaterialCurrentVersion
				|| header.headerSize != sizeof(MaterialHeader)
				|| header.textureCount > kMaxTextureBindings
				|| header.floatParameterCount > kMaxFloatParameters
				|| !RangeFits(bytes.size(), header.textureArrayOffset,
					header.textureCount, sizeof(TextureBinding))
				|| !RangeFits(bytes.size(), header.floatParameterOffset,
					header.floatParameterCount, sizeof(FloatParameter)))
			{
				return false;
			}

			const std::size_t textureEnd = static_cast<std::size_t>(header.textureArrayOffset)
				+ static_cast<std::size_t>(header.textureCount) * sizeof(TextureBinding);
			const std::size_t floatEnd = static_cast<std::size_t>(header.floatParameterOffset)
				+ static_cast<std::size_t>(header.floatParameterCount) * sizeof(FloatParameter);
			const std::size_t extensionOffset =
				textureEnd > floatEnd ? textureEnd : floatEnd;
			if (extensionOffset == bytes.size())
			{
				return true;
			}
			if (bytes.size() - extensionOffset
				!= sizeof(VirtualGeometryMaterialExtensionHeader)
					+ sizeof(VirtualGeometryMaterialExtension))
			{
				return false;
			}

			VirtualGeometryMaterialExtensionHeader extensionHeader{};
			VirtualGeometryMaterialExtension extension{};
			std::memcpy(&extensionHeader, bytes.data() + extensionOffset,
				sizeof(extensionHeader));
			std::memcpy(&extension,
				bytes.data() + extensionOffset + sizeof(extensionHeader),
				sizeof(extension));
			return extensionHeader.magic
					== kVirtualGeometryMaterialExtensionMagicValue
				&& extensionHeader.payloadSize
					== sizeof(VirtualGeometryMaterialExtension)
				&& IsValidVirtualGeometryMaterialExtension(extension);
		}

		// Replace a sibling file only after its complete candidate was closed. The
		// Windows API preserves replace semantics that std::filesystem::rename
		// cannot provide when the destination already exists.
		bool ReplaceFileAtomically(const std::filesystem::path& candidate,
			const std::filesystem::path& destination)
		{
#if defined(_WIN32)
			return ::MoveFileExW(candidate.c_str(), destination.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
			std::error_code error;
			std::filesystem::rename(candidate, destination, error);
			return !error;
#endif
		}

		// Give each writer a private sibling path. Keeping candidate and backup on
		// the destination volume makes the final replacement atomic in practice.
		std::filesystem::path MakePrivateSiblingPath(const std::filesystem::path& outputPath,
			std::string_view suffix)
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			return outputPath.parent_path() / (outputPath.filename().string()
				+ std::string(suffix) + "." + std::to_string(stamp));
		}
	}

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
		// Reject malformed optional policy before touching the destination. This
		// preserves the previous material file when an importer provides an
		// unsupported virtual-geometry fallback value.
		if(desc.virtualGeometryExtension
			&& !IsValidVirtualGeometryMaterialExtension(*desc.virtualGeometryExtension))
		{
			return false;
		}
		if(desc.textureBindings.size() > kMaxTextureBindings
			|| desc.floatParameters.size() > kMaxFloatParameters)
		{
			return false;
		}
		std::error_code directoryError;
		if(!outputPath.parent_path().empty())
		{
			std::filesystem::create_directories(outputPath.parent_path(), directoryError);
			if(directoryError)
			{
				return false;
			}
		}

		MaterialHeader header = desc.header;
		std::uint32_t magic = 0u;
		std::memcpy(&magic, header.magic, sizeof(magic));
		if(magic != kMaterialMagicValue || header.version != kMaterialCurrentVersion
			|| header.headerSize != sizeof(MaterialHeader))
		{
			return false;
		}
		header.textureCount = static_cast<std::uint32_t>(desc.textureBindings.size());
		header.floatParameterCount = static_cast<std::uint32_t>(desc.floatParameters.size());
		header.textureArrayOffset = header.headerSize;
		header.floatParameterOffset = header.textureArrayOffset + static_cast<std::uint32_t>(desc.textureBindings.size() * sizeof(TextureBinding));

		const std::filesystem::path candidatePath =
			MakePrivateSiblingPath(outputPath, ".candidate");
		std::ofstream out(candidatePath, std::ios::binary | std::ios::trunc);
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
		if(desc.virtualGeometryExtension)
		{
			VirtualGeometryMaterialExtensionHeader extensionHeader{};
			extensionHeader.payloadSize = sizeof(VirtualGeometryMaterialExtension);
			out.write(reinterpret_cast<const char*>(&extensionHeader), sizeof(extensionHeader));
			out.write(reinterpret_cast<const char*>(&*desc.virtualGeometryExtension),
				static_cast<std::streamsize>(sizeof(VirtualGeometryMaterialExtension)));
		}
		out.close();
		if(!out)
		{
			std::error_code removeError;
			std::filesystem::remove(candidatePath, removeError);
			return false;
		}

		// Preserve the last complete destination before publication. A failed
		// backup or replace leaves the original asset untouched and removes only
		// this call's private candidate.
		std::error_code existingError;
		const bool destinationExists = std::filesystem::exists(outputPath, existingError);
		if(existingError)
		{
			std::filesystem::remove(candidatePath, existingError);
			return false;
		}
		if(destinationExists)
		{
			const std::filesystem::path backupCandidatePath =
				MakePrivateSiblingPath(outputPath, ".previous.candidate");
			std::error_code copyError;
			std::filesystem::copy_file(outputPath, backupCandidatePath,
				std::filesystem::copy_options::none, copyError);
			if(copyError || !ReplaceFileAtomically(backupCandidatePath,
				outputPath.string() + ".previous"))
			{
				std::error_code removeError;
				std::filesystem::remove(candidatePath, removeError);
				std::filesystem::remove(backupCandidatePath, removeError);
				return false;
			}
		}
		if(!ReplaceFileAtomically(candidatePath, outputPath))
		{
			std::error_code removeError;
			std::filesystem::remove(candidatePath, removeError);
			return false;
		}
		return true;
	}

	bool PublishMaterialAssetBytes(
		const std::filesystem::path& outputPath,
		std::span<const std::uint8_t> bytes)
	{
		// Reject malformed ranges and optional extension data before creating a
		// candidate, so a failed Inspector edit cannot alter the live material.
		if (!ValidateMaterialBytes(bytes))
		{
			return false;
		}

		std::error_code directoryError;
		if (!outputPath.parent_path().empty())
		{
			std::filesystem::create_directories(
				outputPath.parent_path(), directoryError);
			if (directoryError)
			{
				return false;
			}
		}

		const std::filesystem::path candidatePath =
			MakePrivateSiblingPath(outputPath, ".candidate");
		std::ofstream output(candidatePath, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			return false;
		}
		output.write(
			reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		output.close();
		if (!output)
		{
			std::filesystem::remove(candidatePath, directoryError);
			return false;
		}

		const bool destinationExists =
			std::filesystem::exists(outputPath, directoryError);
		if (directoryError)
		{
			std::filesystem::remove(candidatePath, directoryError);
			return false;
		}
		if (destinationExists)
		{
			const std::filesystem::path backupCandidatePath =
				MakePrivateSiblingPath(outputPath, ".previous.candidate");
			std::error_code copyError;
			std::filesystem::copy_file(
				outputPath,
				backupCandidatePath,
				std::filesystem::copy_options::none,
				copyError);
			if (copyError
				|| !ReplaceFileAtomically(
					backupCandidatePath,
					outputPath.string() + ".previous"))
			{
				std::error_code removeError;
				std::filesystem::remove(candidatePath, removeError);
				std::filesystem::remove(backupCandidatePath, removeError);
				return false;
			}
		}

		if (!ReplaceFileAtomically(candidatePath, outputPath))
		{
			std::error_code removeError;
			std::filesystem::remove(candidatePath, removeError);
			return false;
		}
		return true;
	}
}
