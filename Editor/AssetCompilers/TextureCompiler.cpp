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

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic, std::uint32_t quality) {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::uint8_t* pixels = stbi_load(input.string().c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        std::cerr << "Failed to load image: " << input << " (" << stbi_failure_reason() << ")\n";
        return false;
    }

    const std::size_t mipSize = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;

    // Initialize KTX texture
    ktxTexture2* ktx = nullptr;
    const bool isSRGB = (semantic == TextureFileSemantic::Color);
    
    ktxTextureCreateInfo createInfo{};
    // UASTC usually starts from RGBA8
    createInfo.vkFormat = isSRGB ? 43 : 37; 
    createInfo.baseWidth = static_cast<std::uint32_t>(width);
    createInfo.baseHeight = static_cast<std::uint32_t>(height);
    createInfo.baseDepth = 1;
    createInfo.numDimensions = 2;
    createInfo.numLevels = 1; 
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    KTX_error_code ktxResult = ktxTexture2_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &ktx);
    if (ktxResult != KTX_SUCCESS) {
        std::cerr << "Failed to create KTX texture: " << ktxResult << "\n";
        stbi_image_free(pixels);
        return false;
    }

    ktxResult = ktxTexture_SetImageFromMemory(ktxTexture(ktx), 0, 0, 0, pixels, mipSize);
    stbi_image_free(pixels);

    if (ktxResult != KTX_SUCCESS) {
        std::cerr << "Failed to set KTX image data: " << ktxResult << "\n";
        ktxTexture_Destroy(ktxTexture(ktx));
        return false;
    }

    // Compress to UASTC
    ktxBasisParams params{};
    params.structSize = sizeof(params);
    params.uastc = KTX_TRUE;
    // Map quality 0-3 to basisu levels (approx)
    // 0: Level 0 (Fastest), 1: Level 2 (Default), 2: Level 3 (Slower), 3: Level 4 (Very Slow)
    params.uastcFlags = (quality == 0) ? KTX_PACK_UASTC_LEVEL_FASTEST : 
                        (quality == 2) ? KTX_PACK_UASTC_LEVEL_SLOWER :
                        (quality == 3) ? KTX_PACK_UASTC_LEVEL_VERYSLOW : 
                        KTX_PACK_UASTC_LEVEL_DEFAULT;

    ktxResult = ktxTexture2_CompressBasisEx(ktx, &params);
    if (ktxResult != KTX_SUCCESS) {
        std::cerr << "Failed to compress KTX to UASTC: " << ktxResult << "\n";
        ktxTexture_Destroy(ktxTexture(ktx));
        return false;
    }

    // Extract level table
    std::vector<TETFLevelEntry> levelTable;
    levelTable.resize(ktx->numLevels);
    for (std::uint32_t i = 0; i < ktx->numLevels; ++i) {
        ktx_size_t offset;
        ktxTexture_GetImageOffset(ktxTexture(ktx), i, 0, 0, &offset);
        levelTable[i].offset = static_cast<std::uint64_t>(offset);
        levelTable[i].size = static_cast<std::uint64_t>(ktxTexture_GetImageSize(ktxTexture(ktx), i));
    }

    const std::uint8_t* rawKtxData = ktxTexture_GetData(ktxTexture(ktx));
    const std::size_t rawKtxDataSize = ktxTexture_GetDataSize(ktxTexture(ktx));

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
    desc.mipCount = static_cast<std::uint8_t>(ktx->numLevels);
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
    // For UASTC, we store the *target* hardware format if possible, 
    // or just signal UASTC via chunk ID. 
    // The spec says: vkFormat stores suggested hardware format.
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
    entries[1] = {kChunkUAST, 0, uastOffset, static_cast<std::uint64_t>(rawKtxDataSize)};

    const std::uint32_t totalSize = AlignUp32(uastOffset + static_cast<std::uint32_t>(rawKtxDataSize), header.alignment);
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
    std::memcpy(fileData.data() + uastOffset, rawKtxData, rawKtxDataSize);

    ktxTexture_Destroy(ktxTexture(ktx));

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
