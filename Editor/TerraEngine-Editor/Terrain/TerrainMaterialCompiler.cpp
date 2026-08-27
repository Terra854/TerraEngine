// TerrainMaterialCompiler.cpp -- Content-addressed generated terrain ORM cooker.

#include "TerrainMaterialCompiler.h"

#include "Editor/AssetCompilers/TextureCompiler.h"

#include <array>
#include <cstring>
#include <system_error>

namespace TerraEngine::Editor::Terrain
{
	namespace
	{
		bool ValidPlane(const TerrainMaterialSourcePlane& plane, std::size_t sampleCount)
		{
			return plane.samples.empty() || (plane.samples.size() == sampleCount
				&& plane.sourceIdentity.IsValid() && plane.sourceGeneration != 0u);
		}

		void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
		{
			for(std::size_t byteIndex = 0u; byteIndex < sizeof(value); ++byteIndex)
				bytes.push_back(static_cast<std::uint8_t>(value >> (byteIndex * 8u)));
		}

		std::vector<std::uint8_t> BuildUncompressedTga(
			std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba)
		{
			// An uncompressed 32-bit TGA is deliberately simple and deterministic. The
			// existing texture compiler decodes it with the vendored stb implementation.
			std::vector<std::uint8_t> bytes(18u + rgba.size(), 0u);
			bytes[2] = 2u;
			bytes[12] = static_cast<std::uint8_t>(width);
			bytes[13] = static_cast<std::uint8_t>(width >> 8u);
			bytes[14] = static_cast<std::uint8_t>(height);
			bytes[15] = static_cast<std::uint8_t>(height >> 8u);
			bytes[16] = 32u;
			bytes[17] = 0x28u;
			for(std::size_t pixel = 0u; pixel < rgba.size() / 4u; ++pixel)
			{
				bytes[18u + pixel * 4u + 0u] = rgba[pixel * 4u + 2u];
				bytes[18u + pixel * 4u + 1u] = rgba[pixel * 4u + 1u];
				bytes[18u + pixel * 4u + 2u] = rgba[pixel * 4u + 0u];
				bytes[18u + pixel * 4u + 3u] = rgba[pixel * 4u + 3u];
			}
			return bytes;
		}
	}

	TerrainOrmCompileReceipt CompileTerrainOrmArtifact(const TerrainOrmCompileRequest& request)
	{
		TerrainOrmCompileReceipt receipt;
		const std::uint64_t sampleCount64 = static_cast<std::uint64_t>(request.width) * request.height;
		if(request.width == 0u || request.height == 0u || request.width > 65535u
			|| request.height > 65535u || sampleCount64 > 256u * 1024u * 1024u)
		{
			receipt.message = "Terrain ORM generation requires bounded non-zero image dimensions.";
			return receipt;
		}
		const std::size_t sampleCount = static_cast<std::size_t>(sampleCount64);
		if(!ValidPlane(request.occlusion, sampleCount)
			|| !ValidPlane(request.roughness, sampleCount)
			|| !ValidPlane(request.metallic, sampleCount))
		{
			receipt.message = "Terrain ORM source planes must match dimensions and carry a valid source generation.";
			return receipt;
		}

		std::vector<std::uint8_t> derivationBytes;
		AppendU64(derivationBytes, request.width);
		AppendU64(derivationBytes, request.height);
		for(const TerrainMaterialSourcePlane* plane : {&request.occlusion, &request.roughness, &request.metallic})
		{
			AppendU64(derivationBytes, plane->sourceIdentity.low64);
			AppendU64(derivationBytes, plane->sourceIdentity.high64);
			AppendU64(derivationBytes, plane->sourceGeneration);
			derivationBytes.push_back(plane->neutralValue);
			derivationBytes.insert(derivationBytes.end(), plane->samples.begin(), plane->samples.end());
			if(plane->sourceIdentity.IsValid()) receipt.sourceDependencies.push_back(plane->sourceIdentity);
		}
		receipt.derivationKey = TerraEngine::Terrain::HashTerrainBytes(derivationBytes);
		if(!receipt.derivationKey.IsValid())
		{
			receipt.message = "Terrain ORM derivation did not produce a valid content identity.";
			return receipt;
		}

		std::vector<std::uint8_t> rgba(sampleCount * 4u, 255u);
		for(std::size_t sample = 0u; sample < sampleCount; ++sample)
		{
			rgba[sample * 4u + 0u] = request.occlusion.samples.empty()
				? request.occlusion.neutralValue : request.occlusion.samples[sample];
			rgba[sample * 4u + 1u] = request.roughness.samples.empty()
				? request.roughness.neutralValue : request.roughness.samples[sample];
			rgba[sample * 4u + 2u] = request.metallic.samples.empty()
				? request.metallic.neutralValue : request.metallic.samples[sample];
		}

		std::error_code error;
		std::filesystem::create_directories(request.generatedContentDirectory, error);
		if(error)
		{
			receipt.message = "Terrain ORM generated-content directory could not be created.";
			return receipt;
		}
		receipt.artifactPath = request.generatedContentDirectory
			/ (TerraEngine::Terrain::ToHexString(receipt.derivationKey) + ".tetf");
		if(std::filesystem::exists(receipt.artifactPath))
		{
			receipt.succeeded = true;
			receipt.message = "Reused the content-addressed terrain ORM artifact.";
			return receipt;
		}

		const std::vector<std::uint8_t> tga = BuildUncompressedTga(request.width, request.height, rgba);
		const std::filesystem::path temporary = receipt.artifactPath.string() + ".pending";
		if(!CompileTextureFromBytes(tga, ".tga", temporary, TextureFileSemantic::Data,
			1u, true, TextureFilterType::Lanczos))
		{
			std::filesystem::remove(temporary, error);
			receipt.message = "The repository texture compiler rejected the generated terrain ORM image.";
			return receipt;
		}
		std::filesystem::rename(temporary, receipt.artifactPath, error);
		if(error)
		{
			std::filesystem::remove(temporary, error);
			receipt.message = "The generated terrain ORM artifact could not be published atomically.";
			return receipt;
		}
		receipt.succeeded = true;
		receipt.message = "Generated canonical R=AO, G=roughness, B=metallic terrain content.";
		return receipt;
	}
}
