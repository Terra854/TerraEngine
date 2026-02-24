#include "MeshCompiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
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

	struct QuantizedVertexKey
	{
		std::int32_t positionX;
		std::int32_t positionY;
		std::int32_t positionZ;
		std::int32_t normalX;
		std::int32_t normalY;
		std::int32_t normalZ;
		std::int32_t uvX;
		std::int32_t uvY;

		bool operator==(const QuantizedVertexKey& rhs) const
		{
			return positionX == rhs.positionX
				&& positionY == rhs.positionY
				&& positionZ == rhs.positionZ
				&& normalX == rhs.normalX
				&& normalY == rhs.normalY
				&& normalZ == rhs.normalZ
				&& uvX == rhs.uvX
				&& uvY == rhs.uvY;
		}
	};

	struct QuantizedVertexKeyHasher
	{
		std::size_t operator()(const QuantizedVertexKey& key) const noexcept
		{
			const auto HashCombine = [](std::size_t seed, std::int32_t value) -> std::size_t {
				return seed ^ (static_cast<std::size_t>(value) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
			};

			std::size_t seed = 0u;
			seed = HashCombine(seed, key.positionX);
			seed = HashCombine(seed, key.positionY);
			seed = HashCombine(seed, key.positionZ);
			seed = HashCombine(seed, key.normalX);
			seed = HashCombine(seed, key.normalY);
			seed = HashCombine(seed, key.normalZ);
			seed = HashCombine(seed, key.uvX);
			seed = HashCombine(seed, key.uvY);
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
		std::vector<std::uint32_t> vertexRemapIndices;
		std::vector<std::uint8_t> primitiveIndices;
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

	std::int32_t QuantizeFloat(float value, float scale)
	{
		return static_cast<std::int32_t>(std::round(value * scale));
	}

	QuantizedVertexKey BuildQuantizedVertexKey(
		const std::array<float, 3>& position,
		const std::array<float, 3>& normal,
		const std::array<float, 2>& uv)
	{
		// Quantized-key dedup allows near-identical vertices to collapse together,
		// reducing remap stream pressure without changing compile workflow inputs.
		return QuantizedVertexKey{
			QuantizeFloat(position[0], 4096.0f),
			QuantizeFloat(position[1], 4096.0f),
			QuantizeFloat(position[2], 4096.0f),
			QuantizeFloat(normal[0], 1024.0f),
			QuantizeFloat(normal[1], 1024.0f),
			QuantizeFloat(normal[2], 1024.0f),
			QuantizeFloat(uv[0], 4096.0f),
			QuantizeFloat(uv[1], 4096.0f)};
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

	std::uint32_t PackOct32Normal(const std::array<float, 3>& n)
	{
		// Octahedral encoding maps a unit vector to a 2D point in a [-1, 1] square.
		// Precision is significantly better than 10-bit SNORM for unit vectors.
		const float l1 = std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]);
		float x = n[0] / l1;
		float y = n[1] / l1;

		if (n[2] < 0.0f)
		{
			float oldX = x;
			x = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
			y = (1.0f - std::abs(oldX)) * (y >= 0.0f ? 1.0f : -1.0f);
		}

		const auto to16 = [](float val) {
			const float clamped = std::max(-1.0f, std::min(1.0f, val));
			const int packed = static_cast<int>(std::round(clamped * 32767.0f));
			return static_cast<std::uint32_t>(static_cast<std::uint16_t>(packed));
		};

		return (to16(y) << 16) | to16(x);
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

	bool BuildRemapStream(
		const std::vector<std::uint32_t>& remapIndices,
		MeshletRemapEncoding encoding,
		std::vector<std::uint8_t>& outBytes)
	{
		outBytes.clear();
		const std::uint32_t elementSize = GetRemapElementSizeBytes(encoding);
		outBytes.reserve(remapIndices.size() * static_cast<std::size_t>(elementSize));

		for (std::uint32_t remapValue : remapIndices)
		{
			if (encoding == MeshletRemapEncoding::Uint16)
			{
				if (remapValue > std::numeric_limits<std::uint16_t>::max())
				{
					std::cerr << "u16 remap encoding overflow for vertex index " << remapValue << ".\n";
					return false;
				}

				const std::uint16_t packed = static_cast<std::uint16_t>(remapValue);
				outBytes.insert(
					outBytes.end(),
					reinterpret_cast<const std::uint8_t*>(&packed),
					reinterpret_cast<const std::uint8_t*>(&packed) + sizeof(packed));
			}
			else
			{
				outBytes.insert(
					outBytes.end(),
					reinterpret_cast<const std::uint8_t*>(&remapValue),
					reinterpret_cast<const std::uint8_t*>(&remapValue) + sizeof(remapValue));
			}
		}

		return true;
	}

	std::string ComputeFileHashHex(const std::filesystem::path& filePath)
	{
		std::ifstream in(filePath, std::ios::binary);
		if (!in)
		{
			return {};
		}

		std::uint64_t hash = 1469598103934665603ull;
		const std::uint64_t prime = 1099511628211ull;
		std::array<char, 4096> buffer{};
		while (in)
		{
			in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize readCount = in.gcount();
			for (std::streamsize index = 0; index < readCount; ++index)
			{
				hash ^= static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(index)]);
				hash *= prime;
			}
		}

		std::ostringstream out;
		out << std::hex << std::setfill('0') << std::setw(16) << hash;
		return out.str();
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

	bool ValidateFixedGpuLayout(const MeshHeader& header)
	{
		// The direct-replacement format intentionally keeps one fixed layout so
		// runtime never has to branch or repack bytes for mesh shader uploads.
		if (header.layout.strideBytes != kMeshletPackedVertexStrideBytes)
		{
			std::cerr << "Unexpected packed vertex stride. Expected " << kMeshletPackedVertexStrideBytes
				<< ", got " << header.layout.strideBytes << ".\n";
			return false;
		}

		if (header.layout.attributeCount != 3u)
		{
			std::cerr << "Unexpected packed vertex attribute count. Expected 3, got "
				<< header.layout.attributeCount << ".\n";
			return false;
		}

		const VertexAttribute expectedLayout[3] = {
			{
				static_cast<std::uint32_t>(AttributeSemantic::Position),
				static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm),
				0u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::Normal),
				static_cast<std::uint32_t>(AttributeFormat::Oct32),
				4u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::TexCoord0),
				static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm),
				8u}};

		for (std::size_t attributeIndex = 0; attributeIndex < 3u; ++attributeIndex)
		{
			const VertexAttribute& current = header.layout.attributes[attributeIndex];
			const VertexAttribute& expected = expectedLayout[attributeIndex];
			if (current.semantic != expected.semantic
				|| current.format != expected.format
				|| current.offsetBytes != expected.offsetBytes)
			{
				std::cerr << "Packed vertex layout mismatch at attribute " << attributeIndex << ".\n";
				return false;
			}
		}

		if (kMeshletPrimitiveIndexSizeBytes != sizeof(std::uint8_t))
		{
			std::cerr << "Primitive stream element width must stay fixed at uint8.\n";
			return false;
		}

		if (kMeshletMaxVertices > std::numeric_limits<std::uint8_t>::max())
		{
			std::cerr << "Meshlet max vertices must fit uint8 primitive elements.\n";
			return false;
		}

		return true;
	}

	struct PayloadLayout
	{
		std::uint32_t vertexBytes = 0;
		std::uint32_t vertexRemapBytes = 0;
		std::uint32_t skinningBytes = 0;
		std::uint32_t primitiveBytes = 0;
		std::uint32_t meshletTableBytes = 0;
		std::uint32_t meshletSectionBytes = 0;
		std::uint32_t meshletBoundsBytes = 0;
		std::uint32_t vertexDataOffset = 0;
		std::uint32_t vertexRemapDataOffset = 0;
		std::uint32_t skinningDataOffset = 0;
		std::uint32_t primitiveDataOffset = 0;
		std::uint32_t meshletTableOffset = 0;
		std::uint32_t meshletSectionOffset = 0;
		std::uint32_t meshletBoundsOffset = 0;
	};

	bool TryBuildPayloadLayout(
		const MeshHeader& header,
		std::size_t vertexBytes,
		std::size_t vertexRemapBytes,
		std::size_t skinningBytes,
		std::size_t primitiveBytes,
		std::size_t meshletTableBytes,
		std::size_t meshletSectionBytes,
		std::size_t meshletBoundsBytes,
		PayloadLayout& outLayout)
	{
		const auto TryToU32 = [](std::size_t value, const char* label, std::uint32_t& outValue) -> bool {
			if (value > std::numeric_limits<std::uint32_t>::max())
			{
				std::cerr << "Payload size overflow for " << label << ": " << value << ".\n";
				return false;
			}

			outValue = static_cast<std::uint32_t>(value);
			return true;
		};

		if (!TryToU32(vertexBytes, "vertex stream", outLayout.vertexBytes)
			|| !TryToU32(vertexRemapBytes, "remap stream", outLayout.vertexRemapBytes)
			|| !TryToU32(skinningBytes, "skinning stream", outLayout.skinningBytes)
			|| !TryToU32(primitiveBytes, "primitive stream", outLayout.primitiveBytes)
			|| !TryToU32(meshletTableBytes, "meshlet table", outLayout.meshletTableBytes)
			|| !TryToU32(meshletSectionBytes, "meshlet section table", outLayout.meshletSectionBytes)
			|| !TryToU32(meshletBoundsBytes, "meshlet bounds table", outLayout.meshletBoundsBytes))
		{
			return false;
		}

		// Centralized offset generation keeps stream layout rules version-aware and deterministic.
		// Align to 4KB sector/page boundaries for DirectStorage and io_uring optimization.
		outLayout.vertexDataOffset = AlignUp(header.headerSize, kMeshletVertexDataAlignmentBytes);
		outLayout.vertexRemapDataOffset = AlignUp(outLayout.vertexDataOffset + AlignUp(outLayout.vertexBytes, 4096u), kMeshletIndexDataAlignmentBytes);
		outLayout.skinningDataOffset = outLayout.skinningBytes == 0u 
			? 0u 
			: AlignUp(outLayout.vertexRemapDataOffset + AlignUp(outLayout.vertexRemapBytes, 4096u), 4096u);
		
		const std::uint32_t lastOffset = outLayout.skinningDataOffset != 0u 
			? outLayout.skinningDataOffset + AlignUp(outLayout.skinningBytes, 4096u)
			: outLayout.vertexRemapDataOffset + AlignUp(outLayout.vertexRemapBytes, 4096u);

		outLayout.primitiveDataOffset = AlignUp(lastOffset, kMeshletIndexDataAlignmentBytes);
		outLayout.meshletTableOffset = AlignUp(outLayout.primitiveDataOffset + AlignUp(outLayout.primitiveBytes, 4096u), 4096u);
		outLayout.meshletSectionOffset = AlignUp(outLayout.meshletTableOffset + AlignUp(outLayout.meshletTableBytes, 4096u), 4096u);
		outLayout.meshletBoundsOffset = outLayout.meshletBoundsBytes == 0u
			? 0u
			: AlignUp(outLayout.meshletSectionOffset + AlignUp(outLayout.meshletSectionBytes, 4096u), 4096u);
		return true;
	}

	void WriteTemf(
		const std::filesystem::path& outputPath,
		const MeshHeader& header,
		const std::vector<std::uint8_t>& vertexData,
		const std::vector<std::uint8_t>& vertexRemapData,
		const std::vector<std::uint8_t>& skinningData,
		const std::vector<std::uint8_t>& primitiveIndices,
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

		WritePadding(out, header.meshletVertexRemapDataOffset);
		out.write(reinterpret_cast<const char*>(vertexRemapData.data()), static_cast<std::streamsize>(vertexRemapData.size()));

		if (!skinningData.empty())
		{
			WritePadding(out, header.meshletSkinningDataOffset);
			out.write(reinterpret_cast<const char*>(skinningData.data()), static_cast<std::streamsize>(skinningData.size()));
		}

		WritePadding(out, header.meshletPrimitiveIndexDataOffset);
		out.write(reinterpret_cast<const char*>(primitiveIndices.data()), static_cast<std::streamsize>(primitiveIndices.size() * sizeof(std::uint8_t)));

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

	bool BuildMeshletPayload(
		const std::vector<VertexRaw>& vertices,
		const std::vector<std::uint32_t>& indices,
		bool includeBounds,
		MeshletBuildOutput& output)
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
		if (includeBounds)
		{
			output.meshletBounds.reserve(meshletCount);
		}
		output.vertexRemapIndices.reserve(indices.size());
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
			if (!TryConvertCount(output.vertexRemapIndices.size(), "meshlet vertex remap offset", record.vertexOffset))
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

				output.vertexRemapIndices.push_back(sourceVertex);
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

				output.primitiveIndices.push_back(localVertex);
			}

			output.meshletRecords.push_back(record);

			if (includeBounds)
			{
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

bool CompileMesh(
	const std::filesystem::path& input,
	const std::filesystem::path& output,
	bool includeMeshletBounds,
	MeshCompileReport* outReport)
{
	MeshCompileReport report{};
	report.assetPath = input;
	report.formatVersion = kMeshCurrentVersion;
	report.sourceHashHex = ComputeFileHashHex(input);
	if (std::filesystem::exists(input))
	{
		report.sourceBytes = std::filesystem::file_size(input);
	}

	const auto FinalizeReport = [&](bool result) {
		if (outReport != nullptr)
		{
			*outReport = report;
		}
		return result;
	};

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
		report.status = "compile_fail";
		report.failureReason = "obj-parse-failed";
		return FinalizeReport(false);
	}

	const int positionCount = static_cast<int>(attrib.vertices.size() / 3u);
	const int normalCount = static_cast<int>(attrib.normals.size() / 3u);
	const int texcoordCount = static_cast<int>(attrib.texcoords.size() / 2u);

	std::unordered_map<QuantizedVertexKey, std::uint32_t, QuantizedVertexKeyHasher> vertexRemap;
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
				report.status = "compile_fail";
				report.failureReason = "face-not-triangulated";
				return FinalizeReport(false);
			}

			for (std::size_t faceVertex = 0; faceVertex < verticesPerFace; ++faceVertex)
			{
				const tinyobj::index_t idx = shape.mesh.indices[indexOffset + faceVertex];
				if (idx.vertex_index < 0 || idx.vertex_index >= positionCount)
				{
					std::cerr << "OBJ contains an out-of-range position index; aborting meshlet compile.\n";
					report.status = "compile_fail";
					report.failureReason = "position-index-out-of-range";
					return FinalizeReport(false);
				}

				if (idx.normal_index >= normalCount || idx.texcoord_index >= texcoordCount)
				{
					std::cerr << "OBJ contains an out-of-range normal/UV index; aborting meshlet compile.\n";
					report.status = "compile_fail";
					report.failureReason = "attribute-index-out-of-range";
					return FinalizeReport(false);
				}

				const std::array<float, 3> position = ExtractPosition(attrib, idx.vertex_index);
				const std::array<float, 3> normal = ExtractNormal(attrib, idx.normal_index);
				const std::array<float, 2> uv = ExtractTexcoord(attrib, idx.texcoord_index);
				const QuantizedVertexKey key = BuildQuantizedVertexKey(position, normal, uv);
				auto existing = vertexRemap.find(key);
				if (existing == vertexRemap.end())
				{
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
		report.status = "compile_fail";
		report.failureReason = "empty-geometry";
		return FinalizeReport(false);
	}

	if ((indices.size() % 3u) != 0u)
	{
		std::cerr << "MeshCompiler generated non-triangulated primitive output for OBJ: " << input << "\n";
		report.status = "compile_fail";
		report.failureReason = "non-triangle-index-buffer";
		return FinalizeReport(false);
	}

	MeshletBuildOutput meshletPayload{};
	if (!BuildMeshletPayload(vertices, indices, includeMeshletBounds, meshletPayload))
	{
		report.status = "compile_fail";
		report.failureReason = "meshlet-build-failed";
		return FinalizeReport(false);
	}

	MeshHeader header{};
	header.magic[0] = 'T';
	header.magic[1] = 'E';
	header.magic[2] = 'M';
	header.magic[3] = 'F';
	header.version = kMeshCurrentVersion;
	header.headerSize = static_cast<std::uint32_t>(sizeof(MeshHeader));

	if (!TryConvertCount(vertices.size(), "meshlet global vertex count", header.meshletVertexCount)
		|| !TryConvertCount(meshletPayload.vertexRemapIndices.size(), "meshlet vertex remap count", header.meshletVertexRemapCount)
		|| !TryConvertCount(meshletPayload.primitiveIndices.size(), "meshlet primitive index count", header.meshletPrimitiveIndexCount)
		|| !TryConvertCount(meshletPayload.meshletRecords.size(), "meshlet count", header.meshletCount)
		|| !TryConvertCount(meshletPayload.meshletSections.size(), "meshlet section count", header.meshletSectionCount)
		|| !TryConvertCount(meshletPayload.meshletBounds.size(), "meshlet bounds count", header.meshletBoundsCount))
	{
		report.status = "compile_fail";
		report.failureReason = "count-overflow";
		return FinalizeReport(false);
	}

	header.layout.strideBytes = kMeshletPackedVertexStrideBytes;
	header.layout.attributeCount = 3;
	header.layout.attributes[0] = {
		static_cast<std::uint32_t>(AttributeSemantic::Position),
		static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm),
		0};
	header.layout.attributes[1] = {
		static_cast<std::uint32_t>(AttributeSemantic::Normal),
		static_cast<std::uint32_t>(AttributeFormat::Oct32),
		4};
	header.layout.attributes[2] = {
		static_cast<std::uint32_t>(AttributeSemantic::TexCoord0),
		static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm),
		8};
	header.meshletVertexRemapEncoding = static_cast<std::uint32_t>(
		header.meshletVertexCount <= std::numeric_limits<std::uint16_t>::max()
			? MeshletRemapEncoding::Uint16
			: MeshletRemapEncoding::Uint32);
	header.meshletFlags = meshletPayload.meshletBounds.empty() ? 0u : kMeshletHeaderFlagHasBounds;
	header.meshletSkinningDataOffset = 0;
	header.meshletSkinningDataCount = 0;

	if (!ValidateFixedGpuLayout(header))
	{
		report.status = "compile_fail";
		report.failureReason = "fixed-layout-validation-failed";
		return FinalizeReport(false);
	}

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
	vertexData.reserve(vertices.size() * static_cast<std::size_t>(header.layout.strideBytes));

	// Keep packing deterministic so binary assets remain stable across repeated runs.
	for (const VertexRaw& vertex : vertices)
	{
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
		const std::uint32_t packedNormal = PackOct32Normal(qNormal);
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

	if ((meshletPayload.primitiveIndices.size() % 3u) != 0u)
	{
		std::cerr << "Primitive stream must remain triangle-aligned.\n";
		report.status = "compile_fail";
		report.failureReason = "primitive-stream-not-triangle-aligned";
		return FinalizeReport(false);
	}

	// The remap stream maps meshlet-local vertex indices to shared global vertices.
	for (std::size_t remapIndex = 0; remapIndex < meshletPayload.vertexRemapIndices.size(); ++remapIndex)
	{
		if (meshletPayload.vertexRemapIndices[remapIndex] >= header.meshletVertexCount)
		{
			std::cerr << "Vertex remap entry is out of global vertex range at index " << remapIndex << ".\n";
			report.status = "compile_fail";
			report.failureReason = "remap-out-of-range";
			return FinalizeReport(false);
		}
	}

	const MeshletRemapEncoding remapEncoding = static_cast<MeshletRemapEncoding>(header.meshletVertexRemapEncoding);
	std::vector<std::uint8_t> remapData;
	if (!BuildRemapStream(meshletPayload.vertexRemapIndices, remapEncoding, remapData))
	{
		report.status = "compile_fail";
		report.failureReason = "remap-encoding-failed";
		return FinalizeReport(false);
	}

	report.usedU16RemapEncoding = (remapEncoding == MeshletRemapEncoding::Uint16);

	PayloadLayout payloadLayout{};
	if (!TryBuildPayloadLayout(
		header,
		vertexData.size(),
		remapData.size(),
		0, // skinningBytes (unimplemented in OBJ path)
		meshletPayload.primitiveIndices.size() * sizeof(std::uint8_t),
		meshletPayload.meshletRecords.size() * sizeof(MeshletRecord),
		meshletPayload.meshletSections.size() * sizeof(MeshletSection),
		meshletPayload.meshletBounds.size() * sizeof(MeshletBounds),
		payloadLayout))
	{
		report.status = "compile_fail";
		report.failureReason = "payload-layout-overflow";
		return FinalizeReport(false);
	}

	// Offsets are aligned to direct GPU upload requirements so runtime only validates + uploads.
	header.meshletVertexDataOffset = payloadLayout.vertexDataOffset;
	header.meshletVertexRemapDataOffset = payloadLayout.vertexRemapDataOffset;
	header.meshletSkinningDataOffset = payloadLayout.skinningDataOffset;
	header.meshletPrimitiveIndexDataOffset = payloadLayout.primitiveDataOffset;
	header.meshletTableOffset = payloadLayout.meshletTableOffset;
	header.meshletSectionOffset = payloadLayout.meshletSectionOffset;
	header.meshletBoundsOffset = payloadLayout.meshletBoundsOffset;

	try
	{
		WriteTemf(
			output,
			header,
			vertexData,
			remapData,
			{}, // skinningData
			meshletPayload.primitiveIndices,
			meshletPayload.meshletRecords,
			meshletPayload.meshletSections,
			meshletPayload.meshletBounds);

		if (std::filesystem::exists(output))
		{
			report.outputBytes = std::filesystem::file_size(output);
		}

		if (report.sourceBytes > 0)
		{
			report.compressionRatio = static_cast<double>(report.outputBytes) / static_cast<double>(report.sourceBytes);
		}

		report.status = "success";

		return FinalizeReport(true);
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Failed to write meshlet-only TEMF: " << exception.what() << "\n";
		report.status = "compile_fail";
		report.failureReason = "write-failed";
		return FinalizeReport(false);
	}
}
