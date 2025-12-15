#include "MeshCompiler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Engine/File Formats/TerraEngineMeshFile.h"
#define TINYOBJLOADER_IMPLEMENTATION
#include "External/tinyobjloader/tiny_obj_loader.h"

namespace {
    std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) {
        const std::uint32_t mask = alignment - 1;
        return (value + mask) & ~mask;
    }

    struct VertexKey {
        int positionIndex;
        int normalIndex;
        int texcoordIndex;

        bool operator==(const VertexKey& rhs) const {
            return positionIndex == rhs.positionIndex &&
                normalIndex == rhs.normalIndex &&
                texcoordIndex == rhs.texcoordIndex;
        }
    };

    struct VertexKeyHasher {
        std::size_t operator()(const VertexKey& key) const noexcept {
            std::size_t seed = static_cast<std::size_t>(key.positionIndex);
            seed ^= static_cast<std::size_t>(key.normalIndex + 0x9e3779b9 + (seed << 6) + (seed >> 2));
            seed ^= static_cast<std::size_t>(key.texcoordIndex + 0x9e3779b9 + (seed << 6) + (seed >> 2));
            return seed;
        }
    };

    std::array<float, 3> ExtractPosition(const tinyobj::attrib_t& attrib, int index) {
        const int base = index * 3;
        return {
            attrib.vertices[base + 0],
            attrib.vertices[base + 1],
            attrib.vertices[base + 2]};
    }

    std::array<float, 3> ExtractNormal(const tinyobj::attrib_t& attrib, int index) {
        if (index < 0) {
            return {0.0f, 0.0f, 1.0f};
        }
        const int base = index * 3;
        return {
            attrib.normals[base + 0],
            attrib.normals[base + 1],
            attrib.normals[base + 2]};
    }

    std::array<float, 2> ExtractTexcoord(const tinyobj::attrib_t& attrib, int index) {
        if (index < 0) {
            return {0.0f, 0.0f};
        }
        const int base = index * 2;
        return {
            attrib.texcoords[base + 0],
            1.0f - attrib.texcoords[base + 1]};
    }

    std::uint32_t PackR10G10B10A2Snorm(const std::array<float, 3>& v) {
        const auto clamp = [](float value) {
            return std::max(-1.0f, std::min(1.0f, value));
        };

        const auto to10 = [&](float value) {
            const float clamped = clamp(value);
            const int packed = static_cast<int>(std::round(clamped * 511.0f));
            return static_cast<std::uint32_t>(packed & 0x3FF);
        };

        const std::uint32_t x = to10(v[0]);
        const std::uint32_t y = to10(v[1]);
        const std::uint32_t z = to10(v[2]);
        const std::uint32_t w = 0;
        return (w << 30) | (z << 20) | (y << 10) | x;
    }

    std::uint32_t PackR16G16Unorm(const std::array<float, 2>& v) {
        const auto clamp = [](float value) {
            return std::max(0.0f, std::min(1.0f, value));
        };

        const auto to16 = [&](float value) {
            const float clamped = clamp(value);
            const int packed = static_cast<int>(std::round(clamped * 65535.0f));
            return static_cast<std::uint32_t>(packed & 0xFFFF);
        };

        const std::uint32_t x = to16(v[0]);
        const std::uint32_t y = to16(v[1]);
        return (y << 16) | x;
    }

    void WriteTemf(const std::filesystem::path& outputPath, MeshHeader header, const std::vector<std::uint8_t>& vertices, const std::vector<std::uint8_t>& indices) {
        std::ofstream out(outputPath, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Failed to open output file: " + outputPath.string());
        }

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));

        const std::uint32_t afterHeader = static_cast<std::uint32_t>(out.tellp());
        if (header.vertexDataOffset > afterHeader) {
            std::vector<char> padding(header.vertexDataOffset - afterHeader, 0);
            out.write(padding.data(), static_cast<std::streamsize>(padding.size()));
        }

        out.write(reinterpret_cast<const char*>(vertices.data()), static_cast<std::streamsize>(vertices.size()));

        const std::uint32_t afterVertices = header.vertexDataOffset + static_cast<std::uint32_t>(vertices.size());
        if (header.indexDataOffset > afterVertices) {
            const std::uint32_t padBytes = header.indexDataOffset - afterVertices;
            std::vector<char> padding(padBytes, 0);
            out.write(padding.data(), static_cast<std::streamsize>(padding.size()));
        }

        out.write(reinterpret_cast<const char*>(indices.data()), static_cast<std::streamsize>(indices.size()));
    }
}

bool CompileMesh(const std::filesystem::path& input, const std::filesystem::path& output) {
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn;
    std::string err;

    const bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, input.string().c_str(), input.parent_path().string().c_str(), true);
    if (!warn.empty()) {
        std::cerr << "Warning: " << warn << "\n";
    }
    if (!ok || !err.empty()) {
        std::cerr << "Error: " << err << "\n";
        return false;
    }

    struct VertexRaw {
        std::array<float, 3> position;
        std::array<float, 3> normal;
        std::array<float, 2> uv;
    };

    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHasher> vertexRemap;
    std::vector<VertexRaw> vertices;
    std::vector<std::uint32_t> indices;

    std::array<float, 3> minPos{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    std::array<float, 3> maxPos{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    std::array<float, 2> minUv{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    std::array<float, 2> maxUv{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    bool hasUv = false;

    for (const auto& shape : shapes) {
        size_t indexOffset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            const size_t fv = static_cast<size_t>(shape.mesh.num_face_vertices[f]);
            for (size_t v = 0; v < fv; ++v) {
                const tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                VertexKey key{idx.vertex_index, idx.normal_index, idx.texcoord_index};

                auto it = vertexRemap.find(key);
                if (it == vertexRemap.end()) {
                    const std::array<float, 3> position = ExtractPosition(attrib, idx.vertex_index);
                    const std::array<float, 3> normal = ExtractNormal(attrib, idx.normal_index);
                    const std::array<float, 2> uv = ExtractTexcoord(attrib, idx.texcoord_index);

                    for (int c = 0; c < 3; ++c) {
                        minPos[c] = std::min(minPos[c], position[c]);
                        maxPos[c] = std::max(maxPos[c], position[c]);
                    }
                    minUv[0] = std::min(minUv[0], uv[0]);
                    minUv[1] = std::min(minUv[1], uv[1]);
                    maxUv[0] = std::max(maxUv[0], uv[0]);
                    maxUv[1] = std::max(maxUv[1], uv[1]);
                    hasUv = hasUv || idx.texcoord_index >= 0;

                    const std::uint32_t vertexIndex = static_cast<std::uint32_t>(vertices.size());
                    vertexRemap.emplace(key, vertexIndex);
                    vertices.push_back(VertexRaw{position, normal, uv});
                    indices.push_back(vertexIndex);
                }
                else {
                    indices.push_back(it->second);
                }
            }
            indexOffset += fv;
        }
    }

    if (indices.empty()) {
        std::cerr << "No geometry found in OBJ: " << input << "\n";
        return false;
    }

    MeshHeader header{};
    header.magic[0] = 'T';
    header.magic[1] = 'E';
    header.magic[2] = 'M';
    header.magic[3] = 'F';
    header.version = kMeshCurrentVersion;
    header.headerSize = static_cast<std::uint32_t>(sizeof(MeshHeader));
    header.vertexCount = static_cast<std::uint32_t>(vertexRemap.size());
    header.indexCount = static_cast<std::uint32_t>(indices.size());
    header.indexFormat = header.vertexCount <= std::numeric_limits<std::uint16_t>::max() ? static_cast<std::uint32_t>(IndexFormat::UInt16) : static_cast<std::uint32_t>(IndexFormat::UInt32);
    header.layout.strideBytes = 12;
    header.layout.attributeCount = 3;
    header.layout.attributes[0] = {static_cast<std::uint32_t>(AttributeSemantic::Position), static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm), 0};
    header.layout.attributes[1] = {static_cast<std::uint32_t>(AttributeSemantic::Normal), static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm), 4};
    header.layout.attributes[2] = {static_cast<std::uint32_t>(AttributeSemantic::TexCoord0), static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm), 8};

    for (int c = 0; c < 3; ++c) {
        header.quantization.positionOffset[c] = (minPos[c] + maxPos[c]) * 0.5f;
        const float extent = (maxPos[c] - minPos[c]) * 0.5f;
        header.quantization.positionScale[c] = extent > 0.0f ? extent : 1.0f;
    }
    header.quantization.uvOffset[0] = hasUv ? minUv[0] : 0.0f;
    header.quantization.uvOffset[1] = hasUv ? minUv[1] : 0.0f;
    header.quantization.uvScale[0] = hasUv ? std::max(1e-6f, maxUv[0] - minUv[0]) : 1.0f;
    header.quantization.uvScale[1] = hasUv ? std::max(1e-6f, maxUv[1] - minUv[1]) : 1.0f;

    std::vector<std::uint8_t> vertexData;
    vertexData.reserve(vertices.size() * header.layout.strideBytes);
    for (const VertexRaw& v : vertices) {
        std::array<float, 3> qPosition{
            (v.position[0] - header.quantization.positionOffset[0]) / header.quantization.positionScale[0],
            (v.position[1] - header.quantization.positionOffset[1]) / header.quantization.positionScale[1],
            (v.position[2] - header.quantization.positionOffset[2]) / header.quantization.positionScale[2]};
        std::array<float, 3> qNormal = v.normal;
        const float lengthSq = qNormal[0] * qNormal[0] + qNormal[1] * qNormal[1] + qNormal[2] * qNormal[2];
        if (lengthSq > 0.0f) {
            const float invLen = 1.0f / std::sqrt(lengthSq);
            qNormal[0] *= invLen;
            qNormal[1] *= invLen;
            qNormal[2] *= invLen;
        }
        std::array<float, 2> qUv{
            (v.uv[0] - header.quantization.uvOffset[0]) / header.quantization.uvScale[0],
            (v.uv[1] - header.quantization.uvOffset[1]) / header.quantization.uvScale[1]};

        const std::uint32_t positionPacked = PackR10G10B10A2Snorm(qPosition);
        const std::uint32_t normalPacked = PackR10G10B10A2Snorm(qNormal);
        const std::uint32_t uvPacked = PackR16G16Unorm(qUv);

        vertexData.insert(vertexData.end(), reinterpret_cast<const std::uint8_t*>(&positionPacked), reinterpret_cast<const std::uint8_t*>(&positionPacked) + sizeof(positionPacked));
        vertexData.insert(vertexData.end(), reinterpret_cast<const std::uint8_t*>(&normalPacked), reinterpret_cast<const std::uint8_t*>(&normalPacked) + sizeof(normalPacked));
        vertexData.insert(vertexData.end(), reinterpret_cast<const std::uint8_t*>(&uvPacked), reinterpret_cast<const std::uint8_t*>(&uvPacked) + sizeof(uvPacked));
    }

    header.vertexDataOffset = AlignUp(header.headerSize, 16);
    const std::uint32_t vertexBytes = static_cast<std::uint32_t>(vertexData.size());
    header.indexDataOffset = AlignUp(header.vertexDataOffset + vertexBytes, 4);

    std::vector<std::uint8_t> indexBytes;
    if (header.indexFormat == static_cast<std::uint32_t>(IndexFormat::UInt16)) {
        indexBytes.reserve(indices.size() * sizeof(std::uint16_t));
        for (std::uint32_t idx : indices) {
            const std::uint16_t packed = static_cast<std::uint16_t>(idx);
            const auto* begin = reinterpret_cast<const std::uint8_t*>(&packed);
            indexBytes.insert(indexBytes.end(), begin, begin + sizeof(packed));
        }
    }
    else {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(indices.data());
        indexBytes.assign(begin, begin + indices.size() * sizeof(std::uint32_t));
    }

    try {
        WriteTemf(output, header, vertexData, indexBytes);
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to write TEMF: " << e.what() << "\n";
        return false;
    }
}
