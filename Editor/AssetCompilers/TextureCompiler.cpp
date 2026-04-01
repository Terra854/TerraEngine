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

    std::uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        const std::uint8_t byte = data[i];
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
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
    std::memcpy(sourceImage.get_ptr(), pixels, width * height * 4);
    stbi_image_free(pixels);

    basisu::basis_compressor_params params;
    params.m_uastc = true;
    params.m_quality_level = -1; // Use uastc level flags instead
    
    // Map quality 0-3 to basisu levels
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

    params.m_source_images.push_back(sourceImage);
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

    basisu::basis_compressor::error_code result = compressor.process();
    if (result != basisu::basis_compressor::cECSuccess) {
        std::cerr << "Basis compression failed with error code: " << result << "\n";
        return false;
    }

    const basisu::uint8_vec& ktx2Data = compressor.get_output_ktx2_file();
    
    // Parse KTX2 to extract level table
    if (ktx2Data.size() < sizeof(basist::ktx2_header)) {
        std::cerr << "Invalid KTX2 data returned from compressor\n";
        return false;
    }

    const basist::ktx2_header* ktx2Header = reinterpret_cast<const basist::ktx2_header*>(ktx2Data.data());
    const std::uint32_t mipCount = ktx2Header->m_level_count.get_uint64();
    
    std::vector<TETFLevelEntry> levelTable;
    levelTable.resize(mipCount);

    const basist::ktx2_level_index* levelIndex = reinterpret_cast<const basist::ktx2_level_index*>(ktx2Data.data() + sizeof(basist::ktx2_header));
    
    // KTX2 stores levels from largest to smallest? 
    // Actually KTX2 level index is at the end of header.
    // Let's verify the offset.
    
    std::uint64_t totalUastcSize = 0;
    for (std::uint32_t i = 0; i < mipCount; ++i) {
        levelTable[i].offset = totalUastcSize;
        levelTable[i].size = levelIndex[i].m_byte_length.get_uint64();
        totalUastcSize += levelIndex[i].m_byte_length.get_uint64();
    }

    // We want to store ONLY the raw UASTC blocks in our UAST chunk.
    // KTX2 includes DFD, KVD, etc. which we don't need if we are just a container.
    // However, to keep it simple, we'll just extract the level data blobs.
    
    std::vector<uint8_t> rawUastcPayload;
    rawUastcPayload.reserve(totalUastcSize);
    for (std::uint32_t i = 0; i < mipCount; ++i) {
        const std::uint64_t ktxOffset = levelIndex[i].m_byte_offset.get_uint64();
        const std::uint64_t ktxSize = levelIndex[i].m_byte_length.get_uint64();
        rawUastcPayload.insert(rawUastcPayload.end(), ktx2Data.begin() + ktxOffset, ktx2Data.begin() + ktxOffset + ktxSize);
    }

    TETFHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.headerSize = static_cast<std::uint16_t>(sizeof(TETFHeader));
    header.flags = isSRGB ? 1 : 0; // Bit 0 is sRGB
    header.alignment = 16;
    header.chunkCount = 2; 
    header.fileCrc32 = 0;
    header.totalSize = 0;
    header.tocOffset = AlignUp32(sizeof(TETFHeader), header.alignment);

    TETFDesc desc{};
    desc.width = static_cast<std::uint16_t>(width);
    desc.height = static_cast<std::uint16_t>(height);
    desc.depth = 1;
    desc.layers = 1;
    desc.mipCount = static_cast<std::uint8_t>(mipCount);
    desc.faces = 1;
    desc.reserved = 0;
    desc.reserved2 = 0;
    desc.blockWidth = 4; // UASTC is 4x4
    desc.blockHeight = 4;
    desc.blockBytes = 16; // UASTC is 128 bits per block
    desc.semantic = static_cast<std::uint8_t>(semantic);
    desc.payloadChunkIndex = 1;
    desc.userMetadataChunk = TETF_INVALID_CHUNK_INDEX;
    desc.thumbnailChunk = TETF_INVALID_CHUNK_INDEX;
    desc.vkFormat = isSRGB ? 146 : 145; // VK_FORMAT_BC7_SRGB_BLOCK=146, VK_FORMAT_BC7_UNORM_BLOCK=145

    const std::uint32_t tocSize = header.chunkCount * static_cast<std::uint32_t>(sizeof(TETFChunkEntry));
    const std::uint32_t descFixedSize = static_cast<std::uint32_t>(sizeof(TETFDesc));
    const std::uint32_t levelTableSize = static_cast<std::uint32_t>(levelTable.size() * sizeof(TETFLevelEntry));
    const std::uint32_t descTotalSize = descFixedSize + levelTableSize;

    const std::uint32_t descOffset = AlignUp32(header.tocOffset + tocSize, header.alignment);
    const std::uint32_t uastOffset = AlignUp32(descOffset + descTotalSize, header.alignment);

    std::vector<TETFChunkEntry> entries;
    entries.resize(header.chunkCount);
    entries[0] = {kChunkDESC, 0, descOffset, descTotalSize};
    entries[1] = {kChunkUAST, 0, uastOffset, static_cast<std::uint64_t>(rawUastcPayload.size())};

    const std::uint32_t totalSize = AlignUp32(uastOffset + static_cast<std::uint32_t>(rawUastcPayload.size()), header.alignment);
    header.totalSize = totalSize;

    std::vector<std::uint8_t> fileData;
    fileData.resize(totalSize, 0);
    std::memcpy(fileData.data(), &header, sizeof(header));
    std::memcpy(fileData.data() + header.tocOffset, entries.data(), entries.size() * sizeof(TETFChunkEntry));
    
    // Copy DESC fixed part
    std::memcpy(fileData.data() + descOffset, &desc, descFixedSize);
    // Copy Level Table
    std::memcpy(fileData.data() + descOffset + descFixedSize, levelTable.data(), levelTableSize);
    
    // Copy raw UASTC payload
    std::memcpy(fileData.data() + uastOffset, rawUastcPayload.data(), rawUastcPayload.size());

    const std::uint32_t crc = ComputeCrc32(fileData.data(), fileData.size());
    std::memcpy(fileData.data() + offsetof(TETFHeader, fileCrc32), &crc, sizeof(crc));

    std::ofstream out(output, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open output path: " << output << "\n";
        return false;
    }

    out.write(reinterpret_cast<const char*>(fileData.data()), static_cast<std::streamsize>(fileData.size()));
    return true;
}

