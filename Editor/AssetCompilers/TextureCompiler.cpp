#include "TextureCompiler.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

#include "basisu_comp.h"
#include "basisu_transcoder.h"
#include "basisu_enc.h"

namespace {
	constexpr std::array<std::string_view, 11> kSupportedTextureSourceExtensions = {
		".png",
		".jpg",
		".jpeg",
		".bmp",
		".tga",
		".psd",
		".hdr",
		".gif",
		".pic",
		".ppm",
		".pgm"};

	std::uint32_t AlignUp32(std::uint32_t value, std::uint32_t alignment) {
		const std::uint32_t mask = alignment - 1;
		return (value + mask) & ~mask;
	}

	std::string NormalizeExtension(const std::filesystem::path& filePath) {
		std::string ext = filePath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return ext;
	}

	void InitializeBasisu() {
		static bool initialized = false;
		if (!initialized) {
			basisu::basisu_encoder_init();
			initialized = true;
		}
	}

	const std::uint32_t* GetCrc32Table() {
		static std::uint32_t table[256];
		static bool initialized = false;
		if (!initialized) {
			for (std::uint32_t i = 0; i < 256; ++i) {
				std::uint32_t crc = i;
				for (int j = 0; j < 8; ++j) {
					const bool bit = crc & 1u;
					crc = (crc >> 1) ^ (bit ? 0xEDB88320u : 0u);
				}
				table[i] = crc;
			}
			initialized = true;
		}

		return table;
	}

	std::uint32_t UpdateCrc32(std::uint32_t crc, const std::uint8_t* data, std::size_t size) {
		const std::uint32_t* table = GetCrc32Table();
		for (std::size_t i = 0; i < size; ++i) {
			const std::uint8_t byte = data[i];
			crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
		}
		return crc;
	}

	bool WriteBytesAndUpdateCrc(std::ofstream& out, std::uint32_t& runningCrc, const void* data, std::size_t size) {
		if (size == 0) {
			return true;
		}

		out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
		if (!out) {
			return false;
		}

		runningCrc = UpdateCrc32(runningCrc, static_cast<const std::uint8_t*>(data), size);
		return true;
	}

	bool WritePaddingAndUpdateCrc(std::ofstream& out, std::uint32_t& runningCrc, std::uint64_t targetOffset) {
		const std::streamoff currentOffset = static_cast<std::streamoff>(out.tellp());
		if (currentOffset < 0) {
			return false;
		}

		std::uint64_t current = static_cast<std::uint64_t>(currentOffset);
		if (current >= targetOffset) {
			return true;
		}

		const std::array<std::uint8_t, 4096> zeroBytes{};
		while (current < targetOffset) {
			const std::size_t chunkSize = static_cast<std::size_t>(std::min<std::uint64_t>(
				targetOffset - current,
				static_cast<std::uint64_t>(zeroBytes.size())));
			if (!WriteBytesAndUpdateCrc(out, runningCrc, zeroBytes.data(), chunkSize)) {
				return false;
			}
			current += chunkSize;
		}

		return true;
	}
}

bool IsSupportedTextureSourceExtension(const std::filesystem::path& filePath) {
    const std::string ext = NormalizeExtension(filePath);
    return std::find(
        kSupportedTextureSourceExtensions.begin(),
        kSupportedTextureSourceExtensions.end(),
        std::string_view(ext)) != kSupportedTextureSourceExtensions.end();
}

std::string GetSupportedTextureSourceExtensionsText() {
	std::string result;
	for (std::size_t index = 0; index < kSupportedTextureSourceExtensions.size(); ++index) {
		if (!result.empty()) {
			result += ", ";
		}
		result += kSupportedTextureSourceExtensions[index];
	}
	return result;
}

std::uint32_t ComputeCrc32(const std::uint8_t* data, size_t size) {
	std::uint32_t crc = 0xFFFFFFFFu;
	crc = UpdateCrc32(crc, data, size);
	return crc ^ 0xFFFFFFFFu;
}

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic, std::uint32_t quality, bool generateMipmaps, TextureFilterType filter) {
	InitializeBasisu();

	int width = 0;
	int height = 0;
	int channels = 0;
	std::uint8_t* pixels = stbi_load(input.string().c_str(), &width, &height, &channels, 4);
	if (!pixels) {
		std::cerr << "Failed to load image: " << input << " (" << stbi_failure_reason() << ")\n";
		return false;
	}

	const bool isSRGB = (semantic == TextureFileSemantic::Color);

	basisu::image sourceImage(width, height);
	std::memcpy(sourceImage.get_ptr(), pixels, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
	stbi_image_free(pixels);

	basisu::basis_compressor_params params;
	params.m_uastc = true;
	params.m_quality_level = -1; // Use uastc level flags instead

	// Map quality 0-3 to basisu levels.
	params.m_pack_uastc_ldr_4x4_flags = (quality == 0) ? basisu::cPackUASTCLevelFastest :
		(quality == 2) ? basisu::cPackUASTCLevelSlower :
		(quality == 3) ? basisu::cPackUASTCLevelVerySlow :
		basisu::cPackUASTCLevelDefault;

	params.m_mip_gen = generateMipmaps;
	params.m_mip_srgb = isSRGB;

	switch (filter) {
		case TextureFilterType::Box: params.m_mip_filter = "box"; break;
		case TextureFilterType::Triangle: params.m_mip_filter = "tent"; break;
		case TextureFilterType::Lanczos: params.m_mip_filter = "lanczos3"; break;
		case TextureFilterType::Kaiser: params.m_mip_filter = "kaiser"; break;
	}

	// Move the decoded image into BasisU so the compiler does not keep two full
	// level-0 copies alive in parallel before compression begins.
	params.m_source_images.push_back(std::move(sourceImage));
	params.m_create_ktx2_file = true;
	params.m_ktx2_and_basis_srgb_transfer_function = isSRGB;
	params.m_multithreading = true;

	basisu::job_pool jpool(std::thread::hardware_concurrency());
	params.m_pJob_pool = &jpool;

	basisu::basis_compressor compressor;
	if (!compressor.init(params)) {
		std::cerr << "Failed to initialize basis compressor\n";
		return false;
	}

	const basisu::basis_compressor::error_code result = compressor.process();
	if (result != basisu::basis_compressor::cECSuccess) {
		std::cerr << "Basis compression failed with error code: " << result << "\n";
		return false;
	}

	const basisu::uint8_vec& ktx2Data = compressor.get_output_ktx2_file();

	// Parse the returned KTX2 container once, then stream only the raw UASTC
	// mip payloads into the Terra texture file instead of building a second blob.
	if (ktx2Data.size() < sizeof(basist::ktx2_header)) {
		std::cerr << "Invalid KTX2 data returned from compressor\n";
		return false;
	}

	const basist::ktx2_header* ktx2Header = reinterpret_cast<const basist::ktx2_header*>(ktx2Data.data());
	const std::uint32_t mipCount = static_cast<std::uint32_t>(ktx2Header->m_level_count.get_uint64());
	std::vector<TETFLevelEntry> levelTable(mipCount);

	const std::size_t levelIndexBytes = static_cast<std::size_t>(mipCount) * sizeof(basist::ktx2_level_index);
	if (sizeof(basist::ktx2_header) + levelIndexBytes > ktx2Data.size()) {
		std::cerr << "KTX2 level table overruns encoded data for " << input << "\n";
		return false;
	}

	const basist::ktx2_level_index* levelIndex = reinterpret_cast<const basist::ktx2_level_index*>(ktx2Data.data() + sizeof(basist::ktx2_header));

	std::uint64_t totalUastcSize = 0;
	for (std::uint32_t i = 0; i < mipCount; ++i) {
		const std::uint64_t ktxOffset = levelIndex[i].m_byte_offset.get_uint64();
		const std::uint64_t ktxSize = levelIndex[i].m_byte_length.get_uint64();
		if ((ktxOffset + ktxSize) > static_cast<std::uint64_t>(ktx2Data.size())) {
			std::cerr << "KTX2 mip payload overruns encoded data for " << input << "\n";
			return false;
		}

		levelTable[i].offset = totalUastcSize;
		levelTable[i].size = ktxSize;
		totalUastcSize += ktxSize;
	}

	TETFHeader header{};
	header.magic = kMagic;
	header.version = kVersion;
	header.headerSize = static_cast<std::uint16_t>(sizeof(TETFHeader));
	header.flags = isSRGB ? 1u : 0u; // Bit 0 is sRGB.
	header.alignment = 16;
	header.chunkCount = 2;
	header.fileCrc32 = 0;
	header.totalSize = 0;
	header.tocOffset = AlignUp32(static_cast<std::uint32_t>(sizeof(TETFHeader)), header.alignment);

	TETFDesc desc{};
	desc.width = static_cast<std::uint16_t>(width);
	desc.height = static_cast<std::uint16_t>(height);
	desc.depth = 1;
	desc.layers = 1;
	desc.mipCount = static_cast<std::uint8_t>(mipCount);
	desc.faces = 1;
	desc.reserved = 0;
	desc.reserved2 = 0;
	desc.blockWidth = 4; // UASTC is 4x4.
	desc.blockHeight = 4;
	desc.blockBytes = 16; // UASTC is 128 bits per block.
	desc.semantic = static_cast<std::uint8_t>(semantic);
	desc.payloadChunkIndex = 1;
	desc.userMetadataChunk = TETF_INVALID_CHUNK_INDEX;
	desc.thumbnailChunk = TETF_INVALID_CHUNK_INDEX;
	desc.vkFormat = isSRGB ? 146u : 145u; // BC7 SRGB/UNORM runtime transcode target.

	const std::uint32_t tocSize = header.chunkCount * static_cast<std::uint32_t>(sizeof(TETFChunkEntry));
	const std::uint32_t descFixedSize = static_cast<std::uint32_t>(sizeof(TETFDesc));
	const std::uint32_t levelTableSize = static_cast<std::uint32_t>(levelTable.size() * sizeof(TETFLevelEntry));
	const std::uint32_t descTotalSize = descFixedSize + levelTableSize;

	const std::uint32_t descOffset = AlignUp32(static_cast<std::uint32_t>(header.tocOffset) + tocSize, header.alignment);
	const std::uint32_t uastOffset = AlignUp32(descOffset + descTotalSize, header.alignment);
	const std::uint64_t totalSize = AlignUp(static_cast<std::uint64_t>(uastOffset) + totalUastcSize, static_cast<std::uint64_t>(header.alignment));

	std::vector<TETFChunkEntry> entries(header.chunkCount);
	entries[0] = {kChunkDESC, 0, descOffset, descTotalSize};
	entries[1] = {kChunkUAST, 0, uastOffset, totalUastcSize};
	header.totalSize = totalSize;

	std::ofstream out(output, std::ios::binary | std::ios::trunc);
	if (!out) {
		std::cerr << "Failed to open output path: " << output << "\n";
		return false;
	}

	std::uint32_t runningCrc = 0xFFFFFFFFu;
	if (!WriteBytesAndUpdateCrc(out, runningCrc, &header, sizeof(header))
		|| !WritePaddingAndUpdateCrc(out, runningCrc, header.tocOffset)
		|| !WriteBytesAndUpdateCrc(out, runningCrc, entries.data(), entries.size() * sizeof(TETFChunkEntry))
		|| !WritePaddingAndUpdateCrc(out, runningCrc, descOffset)
		|| !WriteBytesAndUpdateCrc(out, runningCrc, &desc, descFixedSize)
		|| !WriteBytesAndUpdateCrc(out, runningCrc, levelTable.data(), levelTableSize)
		|| !WritePaddingAndUpdateCrc(out, runningCrc, uastOffset)) {
		std::cerr << "Failed while streaming texture output: " << output << "\n";
		return false;
	}

	for (std::uint32_t i = 0; i < mipCount; ++i) {
		const std::uint64_t ktxOffset = levelIndex[i].m_byte_offset.get_uint64();
		const std::uint64_t ktxSize = levelIndex[i].m_byte_length.get_uint64();
		if (!WriteBytesAndUpdateCrc(
				out,
				runningCrc,
				ktx2Data.data() + static_cast<std::size_t>(ktxOffset),
				static_cast<std::size_t>(ktxSize))) {
			std::cerr << "Failed to stream UASTC payload for " << output << "\n";
			return false;
		}
	}

	if (!WritePaddingAndUpdateCrc(out, runningCrc, header.totalSize)) {
		std::cerr << "Failed to finalize padded texture output: " << output << "\n";
		return false;
	}

	const std::uint32_t crc = runningCrc ^ 0xFFFFFFFFu;
	out.seekp(static_cast<std::streamoff>(offsetof(TETFHeader, fileCrc32)), std::ios::beg);
	if (!out) {
		std::cerr << "Failed to seek back for CRC patch: " << output << "\n";
		return false;
	}

	out.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
	return static_cast<bool>(out);
}

