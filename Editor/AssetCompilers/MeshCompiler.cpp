#include "MeshCompiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "External/meshoptimizer/src/meshoptimizer.h"
#define TINYOBJLOADER_IMPLEMENTATION
#include "External/tinyobjloader/tiny_obj_loader.h"

namespace
{
	std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment)
	{
		const std::uint32_t mask = alignment - 1u;
		return (value + mask) & ~mask;
	}

	struct VertexKey
	{
		int positionIndex;
		int normalIndex;
		int texcoordIndex;

		bool operator==(const VertexKey& rhs) const
		{
			return positionIndex == rhs.positionIndex
				&& normalIndex == rhs.normalIndex
				&& texcoordIndex == rhs.texcoordIndex;
		}
	};

	struct VertexKeyHasher
	{
		std::size_t operator()(const VertexKey& key) const noexcept
		{
			std::size_t seed = static_cast<std::size_t>(key.positionIndex);
			seed ^= static_cast<std::size_t>(key.normalIndex + 0x9e3779b9 + (seed << 6) + (seed >> 2));
			seed ^= static_cast<std::size_t>(key.texcoordIndex + 0x9e3779b9 + (seed << 6) + (seed >> 2));
			return seed;
		}
	};

	struct VertexRaw
	{
		std::array<float, 3> position;
		std::array<float, 3> normal;
		std::array<float, 2> uv;
	};

	struct MeshletBuildOutput
	{
		std::vector<std::uint32_t> flattenedVertexIndices;
		std::vector<std::uint32_t> primitiveIndices;
		std::vector<MeshletRecord> meshletRecords;
		std::vector<MeshletSection> meshletSections;
		std::vector<MeshletBounds> meshletBounds;
	};

	std::array<float, 3> ExtractPosition(const tinyobj::attrib_t& attrib, int index)
	{
		const int base = index * 3;
		return {
			attrib.vertices[base + 0],
			attrib.vertices[base + 1],
			attrib.vertices[base + 2]};
	}

	std::array<float, 3> ExtractNormal(const tinyobj::attrib_t& attrib, int index)
	{
		if (index < 0)
		{
			return {0.0f, 0.0f, 1.0f};
		}

		const int base = index * 3;
		return {
			attrib.normals[base + 0],
			attrib.normals[base + 1],
			attrib.normals[base + 2]};
	}

	std::array<float, 2> ExtractTexcoord(const tinyobj::attrib_t& attrib, int index)
	{
		if (index < 0)
		{
			return {0.0f, 0.0f};
		}

		const int base = index * 2;
		return {
			attrib.texcoords[base + 0],
			1.0f - attrib.texcoords[base + 1]};
	}

	std::uint32_t PackR10G10B10A2Snorm(const std::array<float, 3>& value)
	{
		const auto clamp = [](float input) {
			return std::max(-1.0f, std::min(1.0f, input));
		};

		const auto to10 = [&](float input) {
			const float clamped = clamp(input);
			const int packed = static_cast<int>(std::round(clamped * 511.0f));
			return static_cast<std::uint32_t>(packed & 0x3FF);
		};

		const std::uint32_t x = to10(value[0]);
		const std::uint32_t y = to10(value[1]);
		const std::uint32_t z = to10(value[2]);
		const std::uint32_t w = 0;
		return (w << 30) | (z << 20) | (y << 10) | x;
	}

	std::uint32_t PackR16G16Unorm(const std::array<float, 2>& value)
	{
		const auto clamp = [](float input) {
			return std::max(0.0f, std::min(1.0f, input));
		};

		const auto to16 = [&](float input) {
			const float clamped = clamp(input);
			const int packed = static_cast<int>(std::round(clamped * 65535.0f));
			return static_cast<std::uint32_t>(packed & 0xFFFF);
		};

		const std::uint32_t x = to16(value[0]);
		const std::uint32_t y = to16(value[1]);
		return (y << 16) | x;
	}

	bool TryConvertCount(std::size_t value, const char* label, std::uint32_t& outValue)
	{
		if (value > std::numeric_limits<std::uint32_t>::max())
		{
			std::cerr << "Count overflow for " << label << ": " << value << "\n";
			return false;
		}

		outValue = static_cast<std::uint32_t>(value);
		return true;
	}

	void WritePadding(std::ofstream& out, std::uint32_t targetOffset)
	{
		const std::streamoff currentOffset = static_cast<std::streamoff>(out.tellp());
		if (currentOffset < 0)
		{
			throw std::runtime_error("Failed to query output stream position");
		}

		const std::uint32_t current = static_cast<std::uint32_t>(currentOffset);
		if (targetOffset <= current)
		{
			return;
		}

		std::vector<char> padding(static_cast<std::size_t>(targetOffset - current), 0);
		out.write(padding.data(), static_cast<std::streamsize>(padding.size()));
	}

	void WriteTemf(
		const std::filesystem::path& outputPath,
		const MeshHeader& header,
		const std::vector<std::uint8_t>& vertexData,
		const std::vector<std::uint32_t>& primitiveIndices,
		const std::vector<MeshletRecord>& meshletRecords,
		const std::vector<MeshletSection>& meshletSections,
		const std::vector<MeshletBounds>& meshletBounds)
	{
		std::ofstream out(outputPath, std::ios::binary);
		if (!out)
		{
			throw std::runtime_error("Failed to open output file: " + outputPath.string());
		}

		out.write(reinterpret_cast<const char*>(&header), sizeof(header));

		WritePadding(out, header.meshletVertexDataOffset);
		out.write(reinterpret_cast<const char*>(vertexData.data()), static_cast<std::streamsize>(vertexData.size()));

		WritePadding(out, header.meshletPrimitiveIndexDataOffset);
		out.write(reinterpret_cast<const char*>(primitiveIndices.data()), static_cast<std::streamsize>(primitiveIndices.size() * sizeof(std::uint32_t)));

		WritePadding(out, header.meshletTableOffset);
		out.write(reinterpret_cast<const char*>(meshletRecords.data()), static_cast<std::streamsize>(meshletRecords.size() * sizeof(MeshletRecord)));

		WritePadding(out, header.meshletSectionOffset);
		out.write(reinterpret_cast<const char*>(meshletSections.data()), static_cast<std::streamsize>(meshletSections.size() * sizeof(MeshletSection)));

		if (!meshletBounds.empty())
		{
			WritePadding(out, header.meshletBoundsOffset);
			out.write(reinterpret_cast<const char*>(meshletBounds.data()), static_cast<std::streamsize>(meshletBounds.size() * sizeof(MeshletBounds)));
		}
	}

	bool BuildMeshletPayload(const std::vector<VertexRaw>& vertices, const std::vector<std::uint32_t>& indices, MeshletBuildOutput& output)
	{
		if (vertices.empty() || indices.empty())
		{
			std::cerr << "Cannot build meshlets from empty vertex/index payload.\n";
			return false;
		}

		if ((indices.size() % 3u) != 0u)
		{
			std::cerr << "Non-triangulated primitive output detected before meshlet build.\n";
			return false;
		}

		const std::size_t maxMeshletCount = meshopt_buildMeshletsBound(indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);
		std::vector<meshopt_Meshlet> meshlets(maxMeshletCount);
		std::vector<unsigned int> meshletVertices(indices.size());
		std::vector<unsigned char> meshletTriangles(indices.size());

		const float* positions = vertices.front().position.data();
		const std::size_t meshletCount = meshopt_buildMeshlets(
			meshlets.data(),
			meshletVertices.data(),
			meshletTriangles.data(),
			indices.data(),
			indices.size(),
			positions,
			vertices.size(),
			sizeof(VertexRaw),
			kMeshletMaxVertices,
			kMeshletMaxTriangles,
			0.0f);

		if (meshletCount == 0)
		{
			std::cerr << "meshoptimizer failed to generate any meshlets.\n";
			return false;
		}

		meshlets.resize(meshletCount);
		output.meshletRecords.reserve(meshletCount);
		output.meshletBounds.reserve(meshletCount);
		output.flattenedVertexIndices.reserve(indices.size());
		output.primitiveIndices.reserve(indices.size());

		// Keep this loop sequential to preserve deterministic byte offsets in output artifacts.
		for (const meshopt_Meshlet& sourceMeshlet : meshlets)
		{
			if (sourceMeshlet.vertex_count == 0 || sourceMeshlet.triangle_count == 0)
			{
				std::cerr << "meshoptimizer generated an empty meshlet, refusing to emit invalid payload.\n";
				return false;
			}

			meshopt_optimizeMeshlet(
				meshletVertices.data() + sourceMeshlet.vertex_offset,
				meshletTriangles.data() + sourceMeshlet.triangle_offset,
				sourceMeshlet.triangle_count,
				sourceMeshlet.vertex_count);

			MeshletRecord record{};
			if (!TryConvertCount(output.flattenedVertexIndices.size(), "meshlet vertex offset", record.vertexOffset))
			{
				return false;
			}

			record.vertexCount = sourceMeshlet.vertex_count;

			for (std::size_t localVertex = 0; localVertex < sourceMeshlet.vertex_count; ++localVertex)
			{
				const std::uint32_t sourceVertex = meshletVertices[sourceMeshlet.vertex_offset + localVertex];
				if (sourceVertex >= vertices.size())
				{
					std::cerr << "meshoptimizer vertex indirection points outside source vertex array.\n";
					return false;
				}

				output.flattenedVertexIndices.push_back(sourceVertex);
			}

			if (!TryConvertCount(output.primitiveIndices.size(), "meshlet primitive offset", record.primitiveIndexOffset))
			{
				return false;
			}

			record.primitiveIndexCount = sourceMeshlet.triangle_count * 3u;

			for (std::size_t localPrimitive = 0; localPrimitive < static_cast<std::size_t>(record.primitiveIndexCount); ++localPrimitive)
			{
				const std::uint8_t localVertex = meshletTriangles[sourceMeshlet.triangle_offset + localPrimitive];
				if (localVertex >= sourceMeshlet.vertex_count)
				{
					std::cerr << "meshoptimizer triangle payload references an out-of-range local vertex.\n";
					return false;
				}

				output.primitiveIndices.push_back(record.vertexOffset + localVertex);
			}

			output.meshletRecords.push_back(record);

			const meshopt_Bounds computedBounds = meshopt_computeMeshletBounds(
				meshletVertices.data() + sourceMeshlet.vertex_offset,
				meshletTriangles.data() + sourceMeshlet.triangle_offset,
				sourceMeshlet.triangle_count,
				positions,
				vertices.size(),
				sizeof(VertexRaw));

			MeshletBounds bounds{};
			bounds.center[0] = computedBounds.center[0];
			bounds.center[1] = computedBounds.center[1];
			bounds.center[2] = computedBounds.center[2];
			bounds.radius = computedBounds.radius;
			output.meshletBounds.push_back(bounds);
		}

		if (output.primitiveIndices.empty() || (output.primitiveIndices.size() % 3u) != 0u)
		{
			std::cerr << "Meshlet primitive stream is empty or non-triangulated after meshlet build.\n";
			return false;
		}

		MeshletSection section{};
		section.meshletOffset = 0;
		if (!TryConvertCount(output.meshletRecords.size(), "meshlet section meshlet count", section.meshletCount))
		{
			return false;
		}
		output.meshletSections.push_back(section);

		return true;
	}
}

bool CompileMesh(const std::filesystem::path& input, const std::filesystem::path& output)
{
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn;
	std::string err;

	const bool ok = tinyobj::LoadObj(
		&attrib,
		&shapes,
		&materials,
		&warn,
		&err,
		input.string().c_str(),
		input.parent_path().string().c_str(),
		true);

	if (!warn.empty())
	{
		std::cerr << "Warning: " << warn << "\n";
	}

	if (!ok || !err.empty())
	{
		std::cerr << "Error: " << err << "\n";
		return false;
	}

	const int positionCount = static_cast<int>(attrib.vertices.size() / 3u);
	const int normalCount = static_cast<int>(attrib.normals.size() / 3u);
	const int texcoordCount = static_cast<int>(attrib.texcoords.size() / 2u);

	std::unordered_map<VertexKey, std::uint32_t, VertexKeyHasher> vertexRemap;
	std::vector<VertexRaw> vertices;
	std::vector<std::uint32_t> indices;

	std::array<float, 3> minPos{
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max()};
	std::array<float, 3> maxPos{
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest()};
	std::array<float, 2> minUv{
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max()};
	std::array<float, 2> maxUv{
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest()};
	bool hasUv = false;

	for (const auto& shape : shapes)
	{
		std::size_t indexOffset = 0;
		for (std::size_t faceIndex = 0; faceIndex < shape.mesh.num_face_vertices.size(); ++faceIndex)
		{
			const std::size_t verticesPerFace = static_cast<std::size_t>(shape.mesh.num_face_vertices[faceIndex]);
			if (verticesPerFace != 3u)
			{
				std::cerr << "MeshCompiler only accepts triangulated topology; found face with " << verticesPerFace << " vertices.\n";
				return false;
			}

			for (std::size_t faceVertex = 0; faceVertex < verticesPerFace; ++faceVertex)
			{
				const tinyobj::index_t idx = shape.mesh.indices[indexOffset + faceVertex];
				if (idx.vertex_index < 0 || idx.vertex_index >= positionCount)
				{
					std::cerr << "OBJ contains an out-of-range position index; aborting meshlet compile.\n";
					return false;
				}

				if (idx.normal_index >= normalCount || idx.texcoord_index >= texcoordCount)
				{
					std::cerr << "OBJ contains an out-of-range normal/UV index; aborting meshlet compile.\n";
					return false;
				}

				VertexKey key{idx.vertex_index, idx.normal_index, idx.texcoord_index};
				auto existing = vertexRemap.find(key);
				if (existing == vertexRemap.end())
				{
					const std::array<float, 3> position = ExtractPosition(attrib, idx.vertex_index);
					const std::array<float, 3> normal = ExtractNormal(attrib, idx.normal_index);
					const std::array<float, 2> uv = ExtractTexcoord(attrib, idx.texcoord_index);

					for (int component = 0; component < 3; ++component)
					{
						minPos[component] = std::min(minPos[component], position[component]);
						maxPos[component] = std::max(maxPos[component], position[component]);
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
				else
				{
					indices.push_back(existing->second);
				}
			}

			indexOffset += verticesPerFace;
		}
	}

	if (indices.empty())
	{
		std::cerr << "No geometry found in OBJ: " << input << "\n";
		return false;
	}

	if ((indices.size() % 3u) != 0u)
	{
		std::cerr << "MeshCompiler generated non-triangulated primitive output for OBJ: " << input << "\n";
		return false;
	}

	MeshletBuildOutput meshletPayload{};
	if (!BuildMeshletPayload(vertices, indices, meshletPayload))
	{
		return false;
	}

	MeshHeader header{};
	header.magic[0] = 'T';
	header.magic[1] = 'E';
	header.magic[2] = 'M';
	header.magic[3] = 'F';
	header.version = kMeshCurrentVersion;
	header.headerSize = static_cast<std::uint32_t>(sizeof(MeshHeader));

	if (!TryConvertCount(meshletPayload.flattenedVertexIndices.size(), "meshlet vertex count", header.meshletVertexCount)
		|| !TryConvertCount(meshletPayload.primitiveIndices.size(), "meshlet primitive index count", header.meshletPrimitiveIndexCount)
		|| !TryConvertCount(meshletPayload.meshletRecords.size(), "meshlet count", header.meshletCount)
		|| !TryConvertCount(meshletPayload.meshletSections.size(), "meshlet section count", header.meshletSectionCount)
		|| !TryConvertCount(meshletPayload.meshletBounds.size(), "meshlet bounds count", header.meshletBoundsCount))
	{
		return false;
	}

	header.layout.strideBytes = 12;
	header.layout.attributeCount = 3;
	header.layout.attributes[0] = {
		static_cast<std::uint32_t>(AttributeSemantic::Position),
		static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm),
		0};
	header.layout.attributes[1] = {
		static_cast<std::uint32_t>(AttributeSemantic::Normal),
		static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm),
		4};
	header.layout.attributes[2] = {
		static_cast<std::uint32_t>(AttributeSemantic::TexCoord0),
		static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm),
		8};

	for (int component = 0; component < 3; ++component)
	{
		header.quantization.positionOffset[component] = (minPos[component] + maxPos[component]) * 0.5f;
		const float extent = (maxPos[component] - minPos[component]) * 0.5f;
		header.quantization.positionScale[component] = extent > 0.0f ? extent : 1.0f;
	}

	header.quantization.uvOffset[0] = hasUv ? minUv[0] : 0.0f;
	header.quantization.uvOffset[1] = hasUv ? minUv[1] : 0.0f;
	header.quantization.uvScale[0] = hasUv ? std::max(1e-6f, maxUv[0] - minUv[0]) : 1.0f;
	header.quantization.uvScale[1] = hasUv ? std::max(1e-6f, maxUv[1] - minUv[1]) : 1.0f;

	std::vector<std::uint8_t> vertexData;
	vertexData.reserve(meshletPayload.flattenedVertexIndices.size() * static_cast<std::size_t>(header.layout.strideBytes));

	// Keep packing deterministic so binary assets remain stable across repeated runs.
	for (std::uint32_t flattenedVertex : meshletPayload.flattenedVertexIndices)
	{
		if (flattenedVertex >= vertices.size())
		{
			std::cerr << "Meshlet vertex stream references missing source vertex data.\n";
			return false;
		}

		const VertexRaw& vertex = vertices[flattenedVertex];
		std::array<float, 3> qPosition{
			(vertex.position[0] - header.quantization.positionOffset[0]) / header.quantization.positionScale[0],
			(vertex.position[1] - header.quantization.positionOffset[1]) / header.quantization.positionScale[1],
			(vertex.position[2] - header.quantization.positionOffset[2]) / header.quantization.positionScale[2]};

		std::array<float, 3> qNormal = vertex.normal;
		const float normalLengthSq = qNormal[0] * qNormal[0] + qNormal[1] * qNormal[1] + qNormal[2] * qNormal[2];
		if (normalLengthSq > 0.0f)
		{
			const float invNormalLength = 1.0f / std::sqrt(normalLengthSq);
			qNormal[0] *= invNormalLength;
			qNormal[1] *= invNormalLength;
			qNormal[2] *= invNormalLength;
		}

		std::array<float, 2> qUv{
			(vertex.uv[0] - header.quantization.uvOffset[0]) / header.quantization.uvScale[0],
			(vertex.uv[1] - header.quantization.uvOffset[1]) / header.quantization.uvScale[1]};

		const std::uint32_t packedPosition = PackR10G10B10A2Snorm(qPosition);
		const std::uint32_t packedNormal = PackR10G10B10A2Snorm(qNormal);
		const std::uint32_t packedUv = PackR16G16Unorm(qUv);

		vertexData.insert(
			vertexData.end(),
			reinterpret_cast<const std::uint8_t*>(&packedPosition),
			reinterpret_cast<const std::uint8_t*>(&packedPosition) + sizeof(packedPosition));
		vertexData.insert(
			vertexData.end(),
			reinterpret_cast<const std::uint8_t*>(&packedNormal),
			reinterpret_cast<const std::uint8_t*>(&packedNormal) + sizeof(packedNormal));
		vertexData.insert(
			vertexData.end(),
			reinterpret_cast<const std::uint8_t*>(&packedUv),
			reinterpret_cast<const std::uint8_t*>(&packedUv) + sizeof(packedUv));
	}

	const std::uint32_t vertexBytes = static_cast<std::uint32_t>(vertexData.size());
	const std::uint32_t primitiveBytes = static_cast<std::uint32_t>(meshletPayload.primitiveIndices.size() * sizeof(std::uint32_t));
	const std::uint32_t meshletTableBytes = static_cast<std::uint32_t>(meshletPayload.meshletRecords.size() * sizeof(MeshletRecord));
	const std::uint32_t meshletSectionBytes = static_cast<std::uint32_t>(meshletPayload.meshletSections.size() * sizeof(MeshletSection));
	const std::uint32_t meshletBoundsBytes = static_cast<std::uint32_t>(meshletPayload.meshletBounds.size() * sizeof(MeshletBounds));

	header.meshletVertexDataOffset = AlignUp(header.headerSize, 16);
	header.meshletPrimitiveIndexDataOffset = AlignUp(header.meshletVertexDataOffset + vertexBytes, 4);
	header.meshletTableOffset = AlignUp(header.meshletPrimitiveIndexDataOffset + primitiveBytes, 4);
	header.meshletSectionOffset = AlignUp(header.meshletTableOffset + meshletTableBytes, 4);
	header.meshletBoundsOffset = meshletPayload.meshletBounds.empty()
		? 0u
		: AlignUp(header.meshletSectionOffset + meshletSectionBytes, 4);

	try
	{
		WriteTemf(
			output,
			header,
			vertexData,
			meshletPayload.primitiveIndices,
			meshletPayload.meshletRecords,
			meshletPayload.meshletSections,
			meshletPayload.meshletBounds);
		return true;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Failed to write meshlet-only TEMF: " << exception.what() << "\n";
		return false;
	}
}
