#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Offline TETF compiler: loads common 2D images (PNG/JPG/BMP/TGA/etc.) via stb_image,
// wraps them in the TerraEngine texture container, and computes metadata + CRC so the
// runtime can stream them without extra dependencies.

#include "Engine/File Formats/TerraEngineTextureFile.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_HDR
#define STBI_ONLY_GIF
#define STBI_ONLY_PIC
#define STBI_ONLY_PNM
#include "External/stb/stb_image.h"

namespace
{
	struct CommandLine
	{
		std::filesystem::path input;
		std::filesystem::path output;
	};

	std::uint32_t AlignUp32(std::uint32_t value, std::uint32_t alignment)
	{
		const std::uint32_t mask = alignment - 1;
		return (value + mask) & ~mask;
	}

	bool ParseCommandLine(int argc, char** argv, CommandLine& args)
	{
		for(int i = 1; i < argc; ++i)
		{
			const std::string_view flag{argv[i]};
			if((flag == "-i" || flag == "--input") && i + 1 < argc)
			{
				args.input = argv[++i];
			}
			else if((flag == "-o" || flag == "--output") && i + 1 < argc)
			{
				args.output = argv[++i];
			}
			else
			{
				std::cerr << "Unrecognized or incomplete argument: " << flag << "\n";
				return false;
			}
		}

		if(args.input.empty() || args.output.empty())
		{
			std::cerr << "Usage: TerraEngineTextureCompiler -i <input image> -o <output.tetf>\n";
			return false;
		}

		return true;
	}
}

// Definition for the format-wide CRC helper declared in TerraEngineTextureFile.h.
std::uint32_t ComputeCrc32(const std::uint8_t* data, size_t size)
{
	static std::uint32_t table[256];
	static bool initialized = false;
	if(!initialized)
	{
		for(std::uint32_t i = 0; i < 256; ++i)
		{
			std::uint32_t crc = i;
			for(int j = 0; j < 8; ++j)
			{
				const bool bit = crc & 1u;
				crc = (crc >> 1) ^ (bit ? 0xEDB88320u : 0u);
			}
			table[i] = crc;
		}
		initialized = true;
	}

	std::uint32_t crc = 0xFFFFFFFFu;
	for(size_t i = 0; i < size; ++i)
	{
		const std::uint8_t byte = data[i];
		crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFu;
}

int main(int argc, char** argv)
{
	CommandLine args{};
	if(!ParseCommandLine(argc, argv, args))
	{
		return 1;
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	std::uint8_t* pixels = stbi_load(args.input.string().c_str(), &width, &height, &channels, 4);
	if(!pixels)
	{
		std::cerr << "Failed to load image: " << args.input << " (" << stbi_failure_reason() << ")\n";
		return 1;
	}

	const std::size_t mipSize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
	std::vector<std::uint8_t> imageData(pixels, pixels + mipSize);
	stbi_image_free(pixels);

	TETFHeader header{};
	header.magic = kMagic;
	header.versionMajor = kVersionMajor;
	header.versionMinor = kVersionMinor;
	header.headerSize = static_cast<std::uint16_t>(sizeof(TETFHeader));
	header.flags = 0;
	header.alignment = 16;
	header.chunkCount = 2; // DESC + MIP0
	header.fileCrc32 = 0;
	header.totalSize = 0;
	header.tocOffset = AlignUp(sizeof(TETFHeader), header.alignment);

	TETFDesc desc{};
	desc.width = static_cast<std::uint16_t>(width);
	desc.height = static_cast<std::uint16_t>(height);
	desc.depth = 1;
	desc.layers = 1;
	desc.mipCount = 1;
	desc.faces = 1;
	desc.format = static_cast<std::uint8_t>(TETFFormat::RGBA8);
	desc.channelType = static_cast<std::uint8_t>(TETFChannelType::UNORM);
	desc.blockWidth = 1;
	desc.blockHeight = 1;
	desc.blockBytes = 4;
	desc.semantic = 0;
	desc.mipOffsetChunk = 1;
	desc.userMetadataChunk = TETF_INVALID_CHUNK_INDEX;
	desc.thumbnailChunk = TETF_INVALID_CHUNK_INDEX;

	TETFMipChunkHeader mipHeader{};
	mipHeader.mipLevel = 0;
	mipHeader.sliceCount = 1;
	mipHeader.sliceStride = static_cast<std::uint32_t>(mipSize);

	const std::uint32_t tocSize = header.chunkCount * static_cast<std::uint32_t>(sizeof(TETFChunkEntry));
	const std::uint32_t descOffset = AlignUp32(header.tocOffset + tocSize, header.alignment);
	const std::uint32_t descSize = static_cast<std::uint32_t>(sizeof(TETFDesc));
	const std::uint32_t mipOffset = AlignUp32(descOffset + descSize, header.alignment);
	const std::uint32_t mipHeaderSize = static_cast<std::uint32_t>(sizeof(TETFMipChunkHeader));
	const std::uint32_t mipDataOffset = mipOffset + mipHeaderSize;

	std::vector<TETFChunkEntry> entries;
	entries.resize(header.chunkCount);
	entries[0] = {MakeFourCC('D', 'E', 'S', 'C'), 0, descOffset, descSize};
	// Keep the MIP chunk tag stable because the mip level lives in the payload header.
	entries[1] = {MakeFourCC('M', 'I', 'P', ' '), 0, mipOffset, mipHeaderSize + static_cast<std::uint32_t>(imageData.size())};

	const std::uint32_t totalSize = AlignUp32(mipDataOffset + static_cast<std::uint32_t>(imageData.size()), header.alignment);
	header.totalSize = totalSize;
	header.chunkCount = static_cast<std::uint32_t>(entries.size());

	std::vector<std::uint8_t> fileData;
	fileData.resize(totalSize, 0);
	std::memcpy(fileData.data(), &header, sizeof(header));
	std::memcpy(fileData.data() + header.tocOffset, entries.data(), entries.size() * sizeof(TETFChunkEntry));
	std::memcpy(fileData.data() + descOffset, &desc, sizeof(desc));
	std::memcpy(fileData.data() + mipOffset, &mipHeader, sizeof(mipHeader));
	std::memcpy(fileData.data() + mipDataOffset, imageData.data(), imageData.size());

	const std::uint32_t crc = ComputeCrc32(fileData.data(), fileData.size());
	std::memcpy(fileData.data() + offsetof(TETFHeader, fileCrc32), &crc, sizeof(crc));

	std::ofstream out(args.output, std::ios::binary);
	if(!out)
	{
		std::cerr << "Failed to open output path: " << args.output << "\n";
		return 1;
	}

	out.write(reinterpret_cast<const char*>(fileData.data()), static_cast<std::streamsize>(fileData.size()));
	std::cout << "Wrote TETF: " << args.output << " (" << width << "x" << height << ")\n";
	return 0;
}
