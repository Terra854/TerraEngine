#include "MeshCompiler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
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
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "External/meshoptimizer/src/meshoptimizer.h"
#include "External/mikktspace/mikktspace.h"

namespace
{
	constexpr float kPi = 3.14159265358979323846f;
	constexpr std::array<std::string_view, 7> kSupportedMeshSourceExtensions = {
		".obj",
		".fbx",
		".gltf",
		".glb",
		".dae",
		".ply",
		".stl"};

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
		std::array<float, 3> tangent; // Final smoothed normal + tangents for sync
		std::array<float, 2> uv;
		float tangentW = 1.0f; // Sign for bitangent calculation
	};

	struct FaceInfo
	{
		std::array<uint32_t, 3> indices;
		std::array<float, 3> faceNormal;
		float area;
	};

	void CalculateFaceNormals(
		const std::vector<VertexRaw>& vertices,
		const std::vector<std::uint32_t>& indices,
		std::vector<FaceInfo>& outFaces)
	{
		outFaces.clear();
		outFaces.reserve(indices.size() / 3);

		for (size_t i = 0; i < indices.size(); i += 3)
		{
			FaceInfo face;
			face.indices = {indices[i + 0], indices[i + 1], indices[i + 2]};

			const auto& v0 = vertices[face.indices[0]].position;
			const auto& v1 = vertices[face.indices[1]].position;
			const auto& v2 = vertices[face.indices[2]].position;

			float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
			float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};

			// Cross product for face normal (length is 2 * area)
			float nx = e1[1] * e2[2] - e1[2] * e2[1];
			float ny = e1[2] * e2[0] - e1[0] * e2[2];
			float nz = e1[0] * e2[1] - e1[1] * e2[0];

			float length = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (length > 1e-9f)
			{
				face.faceNormal = {nx / length, ny / length, nz / length};
				face.area = length * 0.5f;
			}
			else
			{
				face.faceNormal = {0.0f, 0.0f, 0.0f};
				face.area = 0.0f;
			}

			outFaces.push_back(face);
		}
	}

	void SmoothNormals(std::vector<VertexRaw>& unindexedVertices, const std::vector<std::uint32_t>& unindexedIndices, float smoothingAngleDeg, float weldThreshold)
	{
		const float cosThreshold = std::cos(smoothingAngleDeg * (kPi / 180.0f));

		std::vector<FaceInfo> faces;
		CalculateFaceNormals(unindexedVertices, unindexedIndices, faces);

		// Quantize positions
		const float scale = (weldThreshold > 0.0f) ? (1.0f / weldThreshold) : 1000000.0f;

		struct CornerInfo {
			int32_t x, y, z;
			uint32_t vertexIdx;
			uint32_t faceIdx;

			bool operator<(const CornerInfo& other) const {
				if (x != other.x) return x < other.x;
				if (y != other.y) return y < other.y;
				return z < other.z;
			}
			bool operator==(const CornerInfo& other) const {
				return x == other.x && y == other.y && z == other.z;
			}
		};

		std::vector<CornerInfo> corners(unindexedVertices.size());
		for (uint32_t i = 0; i < unindexedVertices.size(); ++i) {
			const auto& pos = unindexedVertices[i].position;
			corners[i] = {
				static_cast<int32_t>(std::round(pos[0] * scale)),
				static_cast<int32_t>(std::round(pos[1] * scale)),
				static_cast<int32_t>(std::round(pos[2] * scale)),
				i,
				i / 3 // face index
			};
		}

		std::sort(corners.begin(), corners.end());

		// Process each run of identical positions
		for (size_t i = 0; i < corners.size(); )
		{
			size_t runEnd = i + 1;
			while (runEnd < corners.size() && corners[runEnd] == corners[i]) {
				runEnd++;
			}

			// All corners in [i, runEnd) share the same physical position
			size_t runLength = runEnd - i;
			if (runLength == 1) {
				unindexedVertices[corners[i].vertexIdx].normal = faces[corners[i].faceIdx].faceNormal;
				i = runEnd;
				continue;
			}

			// Group faces by connectivity and angle
			std::vector<uint32_t> assignedGroup(runLength, 0xFFFFFFFF);
			uint32_t groupCount = 0;

			for (size_t j = 0; j < runLength; ++j)
			{
				if (assignedGroup[j] != 0xFFFFFFFF) continue;

				uint32_t currentGroup = groupCount++;
				assignedGroup[j] = currentGroup;

				std::vector<size_t> stack = { j };
				while (!stack.empty())
				{
					size_t aIdx = stack.back();
					stack.pop_back();

					const auto& normalA = faces[corners[i + aIdx].faceIdx].faceNormal;

					for (size_t k = 0; k < runLength; ++k)
					{
						if (assignedGroup[k] != 0xFFFFFFFF) continue;

						const auto& normalB = faces[corners[i + k].faceIdx].faceNormal;
						float dot = normalA[0] * normalB[0] + normalA[1] * normalB[1] + normalA[2] * normalB[2];

						if (dot >= cosThreshold)
						{
							assignedGroup[k] = currentGroup;
							stack.push_back(k);
						}
					}
				}
			}

			// Calculate average normal for each group and assign
			for (uint32_t g = 0; g < groupCount; ++g)
			{
				float nx = 0, ny = 0, nz = 0;
				uint32_t firstFaceIdx = 0xFFFFFFFF;

				for (size_t j = 0; j < runLength; ++j)
				{
					if (assignedGroup[j] == g)
					{
						uint32_t fIdx = corners[i + j].faceIdx;
						if (firstFaceIdx == 0xFFFFFFFF) firstFaceIdx = fIdx;

						float area = faces[fIdx].area;
						nx += faces[fIdx].faceNormal[0] * area;
						ny += faces[fIdx].faceNormal[1] * area;
						nz += faces[fIdx].faceNormal[2] * area;
					}
				}

				float mag = std::sqrt(nx * nx + ny * ny + nz * nz);
				std::array<float, 3> finalNormal;
				if (mag > 1e-9f) {
					finalNormal = {nx / mag, ny / mag, nz / mag};
				} else {
					finalNormal = faces[firstFaceIdx].faceNormal;
				}

				// Assign to all vertices in this group
				for (size_t j = 0; j < runLength; ++j)
				{
					if (assignedGroup[j] == g)
					{
						unindexedVertices[corners[i + j].vertexIdx].normal = finalNormal;
					}
				}
			}

			i = runEnd;
		}
	}
	// MikkTSpace interface implementation
	struct MikkContext
	{
		std::vector<VertexRaw>* vertices;
		std::vector<std::uint32_t>* indices;
		std::vector<VertexRaw> outVertices;
		std::vector<uint32_t> outIndices;
	};

	int MikkGetNumFaces(const SMikkTSpaceContext* pContext)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		return static_cast<int>(ctx->indices->size() / 3);
	}

	int MikkGetNumVerticesOfFace(const SMikkTSpaceContext* pContext, const int iFace)
	{
		return 3;
	}

	void MikkGetPosition(const SMikkTSpaceContext* pContext, float fvPosOut[], const int iFace, const int iVert)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		uint32_t index = (*ctx->indices)[iFace * 3 + iVert];
		const auto& pos = (*ctx->vertices)[index].position;
		fvPosOut[0] = pos[0]; fvPosOut[1] = pos[1]; fvPosOut[2] = pos[2];
	}

	void MikkGetNormal(const SMikkTSpaceContext* pContext, float fvNormOut[], const int iFace, const int iVert)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		uint32_t index = (*ctx->indices)[iFace * 3 + iVert];
		const auto& norm = (*ctx->vertices)[index].normal;
		fvNormOut[0] = norm[0]; fvNormOut[1] = norm[1]; fvNormOut[2] = norm[2];
	}

	void MikkGetTexCoord(const SMikkTSpaceContext* pContext, float fvTexcOut[], const int iFace, const int iVert)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		uint32_t index = (*ctx->indices)[iFace * 3 + iVert];
		const auto& uv = (*ctx->vertices)[index].uv;
		fvTexcOut[0] = uv[0]; fvTexcOut[1] = uv[1];
	}

	void MikkSetTSpaceBasic(const SMikkTSpaceContext* pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		uint32_t index = (*ctx->indices)[iFace * 3 + iVert];
		VertexRaw& v = (*ctx->vertices)[index];
		v.tangent = {fvTangent[0], fvTangent[1], fvTangent[2]};
		v.tangentW = fSign;
	}

	void GenerateTangents(std::vector<VertexRaw>& vertices, std::vector<std::uint32_t>& indices)
	{
		MikkContext mikkCtx;
		mikkCtx.vertices = &vertices;
		mikkCtx.indices = &indices;

		SMikkTSpaceInterface mikkInterface{};
		mikkInterface.m_getNumFaces = MikkGetNumFaces;
		mikkInterface.m_getNumVerticesOfFace = MikkGetNumVerticesOfFace;
		mikkInterface.m_getPosition = MikkGetPosition;
		mikkInterface.m_getNormal = MikkGetNormal;
		mikkInterface.m_getTexCoord = MikkGetTexCoord;
		mikkInterface.m_setTSpaceBasic = MikkSetTSpaceBasic;

		SMikkTSpaceContext context{};
		context.m_pInterface = &mikkInterface;
		context.m_pUserData = &mikkCtx;

		// MikkTSpace expects per-face vertex data. It won't split vertices itself in this basic callback,
		// but we already split them during the normal smoothing phase to preserve hard edges.
		genTangSpaceDefault(&context);
	}

	struct MeshletBuildOutput
	{
		std::vector<std::uint32_t> vertexRemapIndices;
		std::vector<std::uint8_t> primitiveIndices;
		std::vector<MeshletRecord> meshletRecords;
		std::vector<MeshletSection> meshletSections;
		std::vector<MeshletBounds> meshletBounds;
	};

	std::string NormalizeExtension(const std::filesystem::path& filePath)
	{
		std::string ext = filePath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return ext;
	}

	bool ShouldFlipTexcoordV(const std::filesystem::path& inputPath)
	{
		// Preserve the legacy OBJ behavior exactly so the importer swap does not
		// change existing assets that were already authored against that path.
		return NormalizeExtension(inputPath) == ".obj";
	}

	std::array<float, 3> ExtractPosition(const aiVector3D& value)
	{
		return {
			value.x,
			value.y,
			value.z};
	}

	std::array<float, 3> ExtractNormal(const aiMesh& mesh, unsigned int vertexIndex)
	{
		if (!mesh.HasNormals())
		{
			return {0.0f, 0.0f, 1.0f};
		}

		const aiVector3D& normal = mesh.mNormals[vertexIndex];
		return {
			normal.x,
			normal.y,
			normal.z};
	}

	std::array<float, 2> ExtractTexcoord(const aiMesh& mesh, unsigned int vertexIndex, bool flipV)
	{
		if (!mesh.HasTextureCoords(0))
		{
			return {0.0f, 0.0f};
		}

		const aiVector3D& texcoord = mesh.mTextureCoords[0][vertexIndex];
		const float v = flipV ? (1.0f - texcoord.y) : texcoord.y;
		return {
			texcoord.x,
			v};
	}

	bool ImportSourceMesh(
		const std::filesystem::path& input,
		MeshCompileReport& report,
		std::vector<VertexRaw>& outRawVertices,
		std::vector<std::uint32_t>& outRawIndices,
		bool& outHasUv)
	{
		outRawVertices.clear();
		outRawIndices.clear();
		outHasUv = false;

		if (!IsSupportedMeshSourceExtension(input))
		{
			std::cerr << "Unsupported mesh source format '" << input.extension().string()
				<< "'. Supported formats: " << GetSupportedMeshSourceExtensionsText() << ".\n";
			report.status = "compile_fail";
			report.failureReason = "unsupported-source-format";
			return false;
		}

		Assimp::Importer importer;
		const unsigned int importFlags =
			aiProcess_Triangulate
			| aiProcess_SortByPType
			| aiProcess_PreTransformVertices
			| aiProcess_JoinIdenticalVertices
			| aiProcess_FindInvalidData;

		const aiScene* scene = importer.ReadFile(input.string(), importFlags);
		if (scene == nullptr || scene->mRootNode == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0u)
		{
			std::cerr << "Error: " << importer.GetErrorString() << "\n";
			report.status = "compile_fail";
			report.failureReason = "source-parse-failed";
			return false;
		}

		if (scene->HasAnimations())
		{
			std::cerr << "Warning: ignoring animation data while compiling static mesh source '" << input << "'.\n";
		}
		if (scene->HasLights())
		{
			std::cerr << "Warning: ignoring light data while compiling static mesh source '" << input << "'.\n";
		}
		if (scene->HasCameras())
		{
			std::cerr << "Warning: ignoring camera data while compiling static mesh source '" << input << "'.\n";
		}

		const bool flipTexcoordV = ShouldFlipTexcoordV(input);
		std::size_t skippedNonTriangleFaces = 0;
		bool warnedAboutBones = false;

		// Normalize every imported mesh into the same unindexed triangle soup that
		// the existing cleanup, tangent generation, and TEMF packing code already expects.
		for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
		{
			const aiMesh* mesh = scene->mMeshes[meshIndex];
			if (mesh == nullptr || mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
			{
				continue;
			}

			if (mesh->HasBones() && !warnedAboutBones)
			{
				std::cerr << "Warning: ignoring skinning weights and bones while compiling static mesh source '" << input << "'.\n";
				warnedAboutBones = true;
			}

			const bool meshHasUv = mesh->HasTextureCoords(0);
			outHasUv = outHasUv || meshHasUv;

			for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
			{
				const aiFace& face = mesh->mFaces[faceIndex];
				if (face.mNumIndices != 3u)
				{
					++skippedNonTriangleFaces;
					continue;
				}

				for (unsigned int localIndex = 0; localIndex < face.mNumIndices; ++localIndex)
				{
					const unsigned int vertexIndex = face.mIndices[localIndex];
					if (vertexIndex >= mesh->mNumVertices)
					{
						std::cerr << "Error: imported mesh face references a vertex outside the source vertex array.\n";
						report.status = "compile_fail";
						report.failureReason = "source-index-out-of-range";
						return false;
					}

					const std::array<float, 3> position = ExtractPosition(mesh->mVertices[vertexIndex]);
					const std::array<float, 3> normal = ExtractNormal(*mesh, vertexIndex);
					const std::array<float, 2> uv = ExtractTexcoord(*mesh, vertexIndex, flipTexcoordV);

					outRawIndices.push_back(static_cast<std::uint32_t>(outRawVertices.size()));
					outRawVertices.push_back({position, normal, {0.0f, 0.0f, 0.0f}, uv, 1.0f});
				}
			}
		}

		if (skippedNonTriangleFaces > 0u)
		{
			std::cerr << "Warning: skipped " << skippedNonTriangleFaces
				<< " non-triangle faces while normalizing source mesh '" << input << "'.\n";
		}

		if (outRawIndices.empty())
		{
			std::cerr << "No triangle geometry found in source mesh: " << input << "\n";
			report.status = "compile_fail";
			report.failureReason = "empty-geometry";
			return false;
		}

		return true;
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

	std::uint32_t PackR10G10B10A2Snorm(const std::array<float, 3>& value, float w = 0.0f)
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
		
		// Map -1..1 to 2-bit signed: -1 -> 0b10 (-2), 0 -> 0b00, 1 -> 0b01
		// For bitangent sign: -1.0 -> 0b11 (-1), 1.0 -> 0b01 (1)
		const int wInt = (w < 0.0f) ? 3 : 1;
		const std::uint32_t wBits = static_cast<std::uint32_t>(wInt & 0x3);
		
		return (wBits << 30) | (z << 20) | (y << 10) | x;
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
			const int16_t packed = static_cast<int16_t>(std::round(clamped * 32767.0f));
			return static_cast<uint32_t>(static_cast<uint16_t>(packed));
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
		outBytes.resize(remapIndices.size() * static_cast<std::size_t>(elementSize));
		std::uint8_t* writeCursor = outBytes.data();

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
				std::memcpy(writeCursor, &packed, sizeof(packed));
				writeCursor += sizeof(packed);
			}
			else
			{
				std::memcpy(writeCursor, &remapValue, sizeof(remapValue));
				writeCursor += sizeof(remapValue);
			}
		}

		return true;
	}

	template <typename T>
	void ReleaseVectorMemory(std::vector<T>& values)
	{
		// Swap with an empty vector so the allocator can release large transient
		// buffers as soon as the compile pipeline no longer needs them.
		std::vector<T>().swap(values);
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

		const std::array<char, 4096> zeroBytes{};
		std::uint32_t remaining = targetOffset - current;
		while (remaining > 0u)
		{
			const std::size_t chunkSize = std::min<std::size_t>(remaining, zeroBytes.size());
			out.write(zeroBytes.data(), static_cast<std::streamsize>(chunkSize));
			remaining -= static_cast<std::uint32_t>(chunkSize);
		}
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

		if (header.layout.attributeCount != 4u)
		{
			std::cerr << "Unexpected packed vertex attribute count. Expected 4, got "
				<< header.layout.attributeCount << ".\n";
			return false;
		}

		const VertexAttribute expectedLayout[4] = {
			{
				static_cast<std::uint32_t>(AttributeSemantic::Position),
				static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm),
				0u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::Normal),
				static_cast<std::uint32_t>(AttributeFormat::Oct32),
				4u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::Tangent),
				static_cast<std::uint32_t>(AttributeFormat::Oct32),
				8u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::TexCoord0),
				static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm),
				12u}};

		for (std::size_t attributeIndex = 0; attributeIndex < 4u; ++attributeIndex)
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
				bounds.coneApex[0] = computedBounds.cone_apex[0];
				bounds.coneApex[1] = computedBounds.cone_apex[1];
				bounds.coneApex[2] = computedBounds.cone_apex[2];
				bounds.coneCutoff = computedBounds.cone_cutoff;
				bounds.coneAxis[0] = computedBounds.cone_axis[0];
				bounds.coneAxis[1] = computedBounds.cone_axis[1];
				bounds.coneAxis[2] = computedBounds.cone_axis[2];
				bounds.coneApexOffset = 0.0f;
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

bool IsSupportedMeshSourceExtension(const std::filesystem::path& filePath)
{
	const std::string ext = NormalizeExtension(filePath);
	return std::find(
		kSupportedMeshSourceExtensions.begin(),
		kSupportedMeshSourceExtensions.end(),
		std::string_view(ext)) != kSupportedMeshSourceExtensions.end();
}

std::string GetSupportedMeshSourceExtensionsText()
{
	std::string result;
	for (std::size_t index = 0; index < kSupportedMeshSourceExtensions.size(); ++index)
	{
		if (!result.empty())
		{
			result += ", ";
		}

		result += kSupportedMeshSourceExtensions[index];
	}

	return result;
}

bool CompileMesh(
	const std::filesystem::path& input,
	const std::filesystem::path& output,
	bool includeMeshletBounds,
	MeshCompileReport* outReport,
	float smoothingAngle,
	float weldThreshold)
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

	std::vector<VertexRaw> rawVertices;
	std::vector<std::uint32_t> rawIndices;
	bool hasUv = false;
	if (!ImportSourceMesh(input, report, rawVertices, rawIndices, hasUv))
	{
		return FinalizeReport(false);
	}

	// Phase 2: Foundational Cleanup (Welding & Smoothing)
	
	// 1. Smooth normals (Connectivity-aware) on unindexed vertices
	SmoothNormals(rawVertices, rawIndices, smoothingAngle, weldThreshold);
	
	// 2. Generate tangents on unindexed stream
	GenerateTangents(rawVertices, rawIndices);
	
	// 3. Final attribute-based weld to collapse identical vertices
	std::vector<unsigned int> finalRemap(rawVertices.size());
	size_t finalVertexCount = meshopt_generateVertexRemap(
		finalRemap.data(),
		rawIndices.data(),
		rawIndices.size(),
		rawVertices.data(),
		rawVertices.size(),
		sizeof(VertexRaw));

	std::vector<VertexRaw> vertices(finalVertexCount);
	std::vector<uint32_t> indices(rawIndices.size());

	meshopt_remapIndexBuffer(indices.data(), rawIndices.data(), rawIndices.size(), finalRemap.data());
	meshopt_remapVertexBuffer(vertices.data(), rawVertices.data(), rawVertices.size(), sizeof(VertexRaw), finalRemap.data());

	// The unindexed import buffers are no longer needed once the final indexed
	// mesh exists, so release them before the meshlet and packing stages begin.
	ReleaseVectorMemory(rawVertices);
	ReleaseVectorMemory(rawIndices);
	ReleaseVectorMemory(finalRemap);

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

	for (const auto& v : vertices)
	{
		for (int component = 0; component < 3; ++component)
		{
			minPos[component] = std::min(minPos[component], v.position[component]);
			maxPos[component] = std::max(maxPos[component], v.position[component]);
		}
		minUv[0] = std::min(minUv[0], v.uv[0]);
		minUv[1] = std::min(minUv[1], v.uv[1]);
		maxUv[0] = std::max(maxUv[0], v.uv[0]);
		maxUv[1] = std::max(maxUv[1], v.uv[1]);
	}

	if (indices.empty())
	{
		std::cerr << "No geometry found in source mesh: " << input << "\n";
		report.status = "compile_fail";
		report.failureReason = "empty-geometry";
		return FinalizeReport(false);
	}

	if ((indices.size() % 3u) != 0u)
	{
		std::cerr << "MeshCompiler generated non-triangulated primitive output for source mesh: " << input << "\n";
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

	// Meshlet generation is the last consumer of the index buffer.
	ReleaseVectorMemory(indices);

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
	header.layout.attributeCount = 4;
	header.layout.attributes[0] = {
		static_cast<std::uint32_t>(AttributeSemantic::Position),
		static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm),
		0};
	header.layout.attributes[1] = {
		static_cast<std::uint32_t>(AttributeSemantic::Normal),
		static_cast<std::uint32_t>(AttributeFormat::Oct32),
		4};
	header.layout.attributes[2] = {
		static_cast<std::uint32_t>(AttributeSemantic::Tangent),
		static_cast<std::uint32_t>(AttributeFormat::Oct32),
		8};
	header.layout.attributes[3] = {
		static_cast<std::uint32_t>(AttributeSemantic::TexCoord0),
		static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm),
		12};
	header.meshletVertexRemapEncoding = static_cast<std::uint32_t>(
		header.meshletVertexCount <= std::numeric_limits<std::uint16_t>::max()
			? MeshletRemapEncoding::Uint16
			: MeshletRemapEncoding::Uint32);
	header.meshletFlags = meshletPayload.meshletBounds.empty() ? 0u : kMeshletHeaderFlagHasBounds;
	header.meshletSkinningDataOffset = 0;
	header.meshletSkinningDataCount = 0;

	report.meshletGlobalVertexCount = header.meshletVertexCount;

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

	std::vector<std::uint8_t> vertexData(vertices.size() * static_cast<std::size_t>(header.layout.strideBytes));
	std::uint8_t* vertexWriteCursor = vertexData.data();

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

		const std::uint32_t packedPosition = PackR10G10B10A2Snorm(qPosition, vertex.tangentW);
		const std::uint32_t packedNormal   = PackOct32Normal(qNormal);

		
		std::array<float, 3> tangentVec = {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]};
		const std::uint32_t packedTangent = PackOct32Normal(tangentVec);
		
		const std::uint32_t packedUv = PackR16G16Unorm(qUv);

		std::memcpy(vertexWriteCursor, &packedPosition, sizeof(packedPosition));
		vertexWriteCursor += sizeof(packedPosition);
		std::memcpy(vertexWriteCursor, &packedNormal, sizeof(packedNormal));
		vertexWriteCursor += sizeof(packedNormal);
		std::memcpy(vertexWriteCursor, &packedTangent, sizeof(packedTangent));
		vertexWriteCursor += sizeof(packedTangent);
		std::memcpy(vertexWriteCursor, &packedUv, sizeof(packedUv));
		vertexWriteCursor += sizeof(packedUv);
	}

	// The packed stream now owns the vertex data required for serialization.
	ReleaseVectorMemory(vertices);

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

	ReleaseVectorMemory(meshletPayload.vertexRemapIndices);

	report.usedU16RemapEncoding = (remapEncoding == MeshletRemapEncoding::Uint16);

	PayloadLayout payloadLayout{};
	if (!TryBuildPayloadLayout(
		header,
		vertexData.size(),
		remapData.size(),
		0, // skinningBytes (static mesh source path only for now)
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
