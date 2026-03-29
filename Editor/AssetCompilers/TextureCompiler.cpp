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

bool CompileTexture(const std::filesystem::path& input, const std::filesystem::path& output, TextureFileSemantic semantic) {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::uint8_t* pixels = stbi_load(input.string().c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        std::cerr << "Failed to load image: " << input << " (" << stbi_failure_reason() << ")\n";
        return false;
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
    header.chunkCount = 2; 
    header.fileCrc32 = 0;
    header.totalSize = 0;
    header.tocOffset = AlignUp32(sizeof(TETFHeader), header.alignment);

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
    desc.semantic = static_cast<std::uint8_t>(semantic);
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

    std::ofstream out(output, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open output path: " << output << "\n";
        return false;
    }

    out.write(reinterpret_cast<const char*>(fileData.data()), static_cast<std::streamsize>(fileData.size()));
    return true;
}
