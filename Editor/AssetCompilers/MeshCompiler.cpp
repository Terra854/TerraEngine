// MeshCompiler.cpp — Mesh compilation pipeline implementation
//
// Implements the mesh compilation logic, including source file importing via Assimp, normal smoothing, and tangent generation. It utilizes meshoptimizer for meshlet generation and packs the final data into a high-performance binary format suitable for GPU-driven rendering.

#include "MeshCompiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <assimp/Importer.hpp>
#include <assimp/ProgressHandler.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "External/meshoptimizer/src/meshoptimizer.h"
#include "External/mikktspace/mikktspace.h"

namespace
{
	constexpr float kPi = 3.14159265358979323846f;
	// meshoptimizer stops simplifying once this error budget is reached. The
	// adaptive cook then applies the user-facing maximumGeometricError gate so
	// bad candidates are rejected without changing the renderer-facing fallback.
	constexpr float kSimplificationTargetError = 0.01f;
	// Virtual geometry LODs are rendered directly at distance, so preserve open
	// borders during simplification. GLB/FBX hard-surface assets often split UVs,
	// materials, and thin parts into many seams; letting those borders collapse can
	// pass the numeric error gate while visibly shredding silhouettes.
	constexpr unsigned int kVirtualGeometrySimplifyOptions = meshopt_SimplifyLockBorder;
	constexpr std::array<std::string_view, 4> kSupportedMeshSourceExtensions = {
		".obj",
		".dae",
		".ply",
		".stl"};

	// AlignUp is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment)
	{
		const std::uint32_t mask = alignment - 1u;
		return (value + mask) & ~mask;
	}

	// struct QuantizedVertexKey groups related state and operations so callers have one documented contract.
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

	// struct QuantizedVertexKeyHasher groups related state and operations so callers have one documented contract.
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

	// struct VertexRaw groups related state and operations so callers have one documented contract.
	struct VertexRaw
	{
		std::array<float, 3> position;
		std::array<float, 3> normal;
		std::array<float, 3> tangent; // Final smoothed normal + tangents for sync
		std::array<float, 2> uv;
		float tangentW = 1.0f; // Sign for bitangent calculation
		// Offline deformable cooks use this non-rendered identity to prevent the
		// static weld from merging vertices whose skin or morph records differ.
		// Ordinary static imports leave it at zero and retain legacy welding.
		std::uint32_t weldIdentity = 0u;
	};

	// The VG simplifier must preserve more than position: hard-surface imports
	// usually duplicate vertices at normal/UV seams, and ignoring those
	// attributes lets a numerically "valid" LOD collapse visible panels or folds.
	// These eight floats are contiguous inside VertexRaw: normal.xyz,
	// tangent.xyz, and uv.xy. tangentW is intentionally omitted because a sign
	// flip alone should not force a separate simplification island.
	constexpr std::size_t kVirtualGeometrySimplifyAttributeCount = 8u;
	constexpr std::array<float, kVirtualGeometrySimplifyAttributeCount> kVirtualGeometrySimplifyAttributeWeights = {
		0.50f,
		0.50f,
		0.50f,
		0.20f,
		0.20f,
		0.20f,
		0.35f,
		0.35f,
	};
	static_assert(offsetof(VertexRaw, tangent) == offsetof(VertexRaw, normal) + sizeof(float) * 3u);
	static_assert(offsetof(VertexRaw, uv) == offsetof(VertexRaw, normal) + sizeof(float) * 6u);

	// struct FaceInfo groups related state and operations so callers have one documented contract.
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

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// SmoothNormals is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (x != other.x) return x < other.x;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (y != other.y) return y < other.y;
				return z < other.z;
			}
			bool operator==(const CornerInfo& other) const {
				return x == other.x && y == other.y && z == other.z;
			}
		};

		std::vector<CornerInfo> corners(unindexedVertices.size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t i = 0; i < corners.size(); )
		{
			size_t runEnd = i + 1;
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (size_t j = 0; j < runLength; ++j)
			{
				if (assignedGroup[j] != 0xFFFFFFFF) continue;

				uint32_t currentGroup = groupCount++;
				assignedGroup[j] = currentGroup;

				std::vector<size_t> stack = { j };
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				while (!stack.empty())
				{
					size_t aIdx = stack.back();
					stack.pop_back();

					const auto& normalA = faces[corners[i + aIdx].faceIdx].faceNormal;

					// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
					for (size_t k = 0; k < runLength; ++k)
					{
						if (assignedGroup[k] != 0xFFFFFFFF) continue;

						const auto& normalB = faces[corners[i + k].faceIdx].faceNormal;
						float dot = normalA[0] * normalB[0] + normalA[1] * normalB[1] + normalA[2] * normalB[2];

						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (dot >= cosThreshold)
						{
							assignedGroup[k] = currentGroup;
							stack.push_back(k);
						}
					}
				}
			}

			// Calculate average normal for each group and assign
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (uint32_t g = 0; g < groupCount; ++g)
			{
				float nx = 0, ny = 0, nz = 0;
				uint32_t firstFaceIdx = 0xFFFFFFFF;

				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t j = 0; j < runLength; ++j)
				{
					if (assignedGroup[j] == g)
					{
						uint32_t fIdx = corners[i + j].faceIdx;
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (firstFaceIdx == 0xFFFFFFFF) firstFaceIdx = fIdx;

						float area = faces[fIdx].area;
						nx += faces[fIdx].faceNormal[0] * area;
						ny += faces[fIdx].faceNormal[1] * area;
						nz += faces[fIdx].faceNormal[2] * area;
					}
				}

				float mag = std::sqrt(nx * nx + ny * ny + nz * nz);
				std::array<float, 3> finalNormal;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (mag > 1e-9f) {
					finalNormal = {nx / mag, ny / mag, nz / mag};
				} else {
					finalNormal = faces[firstFaceIdx].faceNormal;
				}

				// Assign to all vertices in this group
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
	// struct MikkContext groups related state and operations so callers have one documented contract.
	struct MikkContext
	{
		std::vector<VertexRaw>* vertices;
		std::vector<std::uint32_t>* indices;
		std::vector<VertexRaw> outVertices;
		std::vector<uint32_t> outIndices;
	};

	// MikkGetNumFaces is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	int MikkGetNumFaces(const SMikkTSpaceContext* pContext)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		return static_cast<int>(ctx->indices->size() / 3);
	}

	// MikkGetNumVerticesOfFace is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	int MikkGetNumVerticesOfFace(const SMikkTSpaceContext* pContext, const int iFace)
	{
		return 3;
	}

	// MikkGetPosition is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void MikkGetPosition(const SMikkTSpaceContext* pContext, float fvPosOut[], const int iFace, const int iVert)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		uint32_t index = (*ctx->indices)[iFace * 3 + iVert];
		const auto& pos = (*ctx->vertices)[index].position;
		fvPosOut[0] = pos[0]; fvPosOut[1] = pos[1]; fvPosOut[2] = pos[2];
	}

	// MikkGetNormal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void MikkGetNormal(const SMikkTSpaceContext* pContext, float fvNormOut[], const int iFace, const int iVert)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		uint32_t index = (*ctx->indices)[iFace * 3 + iVert];
		const auto& norm = (*ctx->vertices)[index].normal;
		fvNormOut[0] = norm[0]; fvNormOut[1] = norm[1]; fvNormOut[2] = norm[2];
	}

	// MikkGetTexCoord is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void MikkGetTexCoord(const SMikkTSpaceContext* pContext, float fvTexcOut[], const int iFace, const int iVert)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		uint32_t index = (*ctx->indices)[iFace * 3 + iVert];
		const auto& uv = (*ctx->vertices)[index].uv;
		fvTexcOut[0] = uv[0]; fvTexcOut[1] = uv[1];
	}

	// MikkSetTSpaceBasic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void MikkSetTSpaceBasic(const SMikkTSpaceContext* pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert)
	{
		auto* ctx = static_cast<MikkContext*>(pContext->m_pUserData);
		uint32_t index = (*ctx->indices)[iFace * 3 + iVert];
		VertexRaw& v = (*ctx->vertices)[index];
		v.tangent = {fvTangent[0], fvTangent[1], fvTangent[2]};
		v.tangentW = fSign;
	}

	// GenerateTangents is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// struct MeshletBuildOutput groups related state and operations so callers have one documented contract.
	struct MeshletBuildOutput
	{
		std::vector<std::uint32_t> vertexRemapIndices;
		std::vector<std::uint8_t> primitiveIndices;
		std::vector<MeshletRecord> meshletRecords;
		std::vector<MeshletSection> meshletSections;
		std::vector<MeshletBounds> meshletBounds;
	};

	struct VirtualGeometryLodSource
	{
		// indices is the requested whole-mesh simplification target used for
		// adaptive diagnostics. The final virtual-geometry payload rebuilds
		// each LOD from source-coverage regions so mixed cluster cuts can prove
		// that parent and child clusters represent the same source surface.
		std::vector<std::uint32_t> indices;
		float targetRatio = 1.0f;
		float geometricError = 0.0f;
		std::uint32_t requestedTargetIndex = 0u;
	};

	struct VirtualGeometrySourceRegion
	{
		// Source regions are the cook-time coverage truth. Each region owns a
		// disjoint set of source triangles, and every emitted cluster is built
		// from a contiguous range of these regions.
		std::uint32_t firstFineRegion = 0u;
		std::uint32_t fineRegionCount = 0u;
		std::vector<std::uint32_t> indices;
	};

	struct VirtualGeometryClusterCoverage
	{
		// This table is transient and intentionally not serialized in TEMF v1.
		// It lets the cooker validate exact source-region tiling before setting
		// the persisted ClusterCoverageValidated bit.
		std::uint32_t firstFineRegion = 0u;
		std::uint32_t fineRegionCount = 0u;
	};

	struct VirtualGeometryClusterCookPayload
	{
		MeshletBuildOutput meshlets;
		std::uint32_t firstFineRegion = 0u;
		std::uint32_t fineRegionCount = 0u;
		std::uint32_t primitiveCount = 0u;
		float geometricError = 0.0f;
		float targetRatio = 1.0f;
	};

	struct VirtualGeometryBuildOutput
	{
		// These arrays become the contiguous virtual-geometry payload after the
		// normal meshlet streams. Offsets written into VirtualGeometryHeader are
		// absolute file positions so the loader can validate every table before
		// publishing the mesh as usable runtime state.
		MeshletBuildOutput meshletPayload;
		VirtualGeometryHeader header{};
		std::vector<VirtualGeometryDetailLevelRecord> detailLevels;
		std::vector<VirtualGeometryMaterialSectionRecord> materialSections;
		std::vector<VirtualGeometrySectionRangeRecord> sectionRanges;
		std::vector<VirtualGeometryCullableGroupRecord> cullableGroups;
		std::vector<VirtualGeometryPageRecord> pages;
		std::vector<VirtualGeometryRequestedTargetRecord> requestedTargets;
		std::vector<VirtualGeometrySkippedDetailRecord> skippedDetails;
		std::vector<VirtualGeometryClusterRecord> clusters;
		std::vector<VirtualGeometryClusterCoverage> clusterCoverage;
		std::vector<VirtualGeometryClusterPageLinkRecord> clusterPageLinks;
		std::vector<VirtualGeometryConeSafetyRecord> coneSafety;
		std::vector<VirtualGeometryValidationFlagRecord> validationFlags;
		std::vector<VirtualGeometryDiagnosticRecord> diagnostics;
		std::string stringTable;
	};

	// NormalizeExtension is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string NormalizeExtension(const std::filesystem::path& filePath)
	{
		std::string ext = filePath.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return ext;
	}

	// ShouldFlipTexcoordV is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ShouldFlipTexcoordV(const std::filesystem::path& inputPath)
	{
		// Preserve the legacy OBJ behavior exactly so the importer swap does not
		// change existing assets that were already authored against that path.
		return NormalizeExtension(inputPath) == ".obj";
	}

	// ExtractPosition is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::array<float, 3> ExtractPosition(const aiVector3D& value)
	{
		return {
			value.x,
			value.y,
			value.z};
	}

	// ExtractNormal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// ExtractTexcoord is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	void ReportMeshProgress(const MeshCompileProgressCallback& callback, float progress, std::string_view stage, std::string_view message)
	{
		if (callback)
		{
			callback(std::clamp(progress, 0.0f, 1.0f), stage, message);
		}
	}

	class MeshAssimpProgressHandler final : public Assimp::ProgressHandler
	{
	public:
		explicit MeshAssimpProgressHandler(const MeshCompileProgressCallback& callback)
			: m_Callback(callback) {}

		bool Update(float percentage = -1.0f) override
		{
			try
			{
				const float normalized = percentage >= 0.0f ? std::clamp(percentage, 0.0f, 1.0f) : 0.0f;
				ReportMeshProgress(m_Callback, 0.05f + (0.18f - 0.05f) * normalized, "Mesh Import", "Reading source mesh geometry.");
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

	private:
		const MeshCompileProgressCallback& m_Callback;
	};

	bool ImportSourceMesh(
		const std::filesystem::path& input,
		MeshCompileReport& report,
		std::vector<VertexRaw>& outRawVertices,
		std::vector<std::uint32_t>& outRawIndices,
		bool& outHasUv,
		const MeshCompileProgressCallback& progressCallback)
	{
		outRawVertices.clear();
		outRawIndices.clear();
		outHasUv = false;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Assimp deletes the handler with the importer. The handler returns
		// false when cancellation is requested, allowing long source parsing to
		// unwind before the rest of the mesh compile runs.
		importer.SetProgressHandler(new MeshAssimpProgressHandler(progressCallback));
		const aiScene* scene = importer.ReadFile(input.string(), importFlags);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (scene == nullptr || scene->mRootNode == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0u)
		{
			std::cerr << "Error: " << importer.GetErrorString() << "\n";
			report.status = "compile_fail";
			report.failureReason = "source-parse-failed";
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (scene->HasAnimations())
		{
			std::cerr << "Warning: ignoring animation data while compiling static mesh source '" << input << "'.\n";
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (scene->HasLights())
		{
			std::cerr << "Warning: ignoring light data while compiling static mesh source '" << input << "'.\n";
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (mesh == nullptr || mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
			{
				continue;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (mesh->HasBones() && !warnedAboutBones)
			{
				std::cerr << "Warning: ignoring skinning weights and bones while compiling static mesh source '" << input << "'.\n";
				warnedAboutBones = true;
			}

			const bool meshHasUv = mesh->HasTextureCoords(0);
			outHasUv = outHasUv || meshHasUv;

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
			{
				const aiFace& face = mesh->mFaces[faceIndex];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (face.mNumIndices != 3u)
				{
					++skippedNonTriangleFaces;
					continue;
				}

				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (unsigned int localIndex = 0; localIndex < face.mNumIndices; ++localIndex)
				{
					const unsigned int vertexIndex = face.mIndices[localIndex];
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skippedNonTriangleFaces > 0u)
		{
			std::cerr << "Warning: skipped " << skippedNonTriangleFaces
				<< " non-triangle faces while normalizing source mesh '" << input << "'.\n";
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outRawIndices.empty())
		{
			std::cerr << "No triangle geometry found in source mesh: " << input << "\n";
			report.status = "compile_fail";
			report.failureReason = "empty-geometry";
			return false;
		}

		return true;
	}

	// QuantizeFloat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// PackR10G10B10A2Snorm is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// PackOct32Normal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::uint32_t PackOct32Normal(const std::array<float, 3>& n)
	{
		// Octahedral encoding maps a unit vector to a 2D point in a [-1, 1] square.
		// Precision is significantly better than 10-bit SNORM for unit vectors.
		const float l1 = std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]);
		float x = n[0] / l1;
		float y = n[1] / l1;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// PackR16G16Unorm is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// TryConvertCount is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::uint32_t remapValue : remapIndices)
		{
			if (encoding == MeshletRemapEncoding::Uint16)
			{
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// ReleaseVectorMemory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ReleaseVectorMemory(std::vector<T>& values)
	{
		// Swap with an empty vector so the allocator can release large transient
		// buffers as soon as the compile pipeline no longer needs them.
		std::vector<T>().swap(values);
	}

	// ComputeFileHashHex is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string ComputeFileHashHex(const std::filesystem::path& filePath)
	{
		std::ifstream in(filePath, std::ios::binary);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!in)
		{
			return {};
		}

		std::uint64_t hash = 1469598103934665603ull;
		const std::uint64_t prime = 1099511628211ull;
		std::array<char, 4096> buffer{};
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (in)
		{
			in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize readCount = in.gcount();
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

	// WritePadding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void WritePadding(std::ofstream& out, std::uint32_t targetOffset)
	{
		const std::streamoff currentOffset = static_cast<std::streamoff>(out.tellp());
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (currentOffset < 0)
		{
			throw std::runtime_error("Failed to query output stream position");
		}

		const std::uint32_t current = static_cast<std::uint32_t>(currentOffset);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (targetOffset <= current)
		{
			return;
		}

		const std::array<char, 4096> zeroBytes{};
		std::uint32_t remaining = targetOffset - current;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (remaining > 0u)
		{
			const std::size_t chunkSize = std::min<std::size_t>(remaining, zeroBytes.size());
			out.write(zeroBytes.data(), static_cast<std::streamsize>(chunkSize));
			remaining -= static_cast<std::uint32_t>(chunkSize);
		}
	}

	// ValidateFixedGpuLayout is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ValidateFixedGpuLayout(const MeshHeader& header)
	{
		// The direct-replacement format intentionally keeps one fixed layout so
		// runtime never has to branch or repack bytes for mesh shader uploads.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.layout.strideBytes != kMeshletPackedVertexStrideBytes)
		{
			std::cerr << "Unexpected packed vertex stride. Expected " << kMeshletPackedVertexStrideBytes
				<< ", got " << header.layout.strideBytes << ".\n";
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t attributeIndex = 0; attributeIndex < 4u; ++attributeIndex)
		{
			const VertexAttribute& current = header.layout.attributes[attributeIndex];
			const VertexAttribute& expected = expectedLayout[attributeIndex];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (current.semantic != expected.semantic
				|| current.format != expected.format
				|| current.offsetBytes != expected.offsetBytes)
			{
				std::cerr << "Packed vertex layout mismatch at attribute " << attributeIndex << ".\n";
				return false;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (kMeshletPrimitiveIndexSizeBytes != sizeof(std::uint8_t))
		{
			std::cerr << "Primitive stream element width must stay fixed at uint8.\n";
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (kMeshletMaxVertices > std::numeric_limits<std::uint8_t>::max())
		{
			std::cerr << "Meshlet max vertices must fit uint8 primitive elements.\n";
			return false;
		}

		return true;
	}

	// struct PayloadLayout groups related state and operations so callers have one documented contract.
	struct PayloadLayout
	{
		std::uint32_t vertexBytes = 0;
		std::uint32_t vertexRemapBytes = 0;
		std::uint32_t skinningBytes = 0;
		std::uint32_t primitiveBytes = 0;
		std::uint32_t meshletTableBytes = 0;
		std::uint32_t meshletSectionBytes = 0;
		std::uint32_t meshletBoundsBytes = 0;
		std::uint32_t virtualGeometryBytes = 0;
		std::uint32_t vertexDataOffset = 0;
		std::uint32_t vertexRemapDataOffset = 0;
		std::uint32_t skinningDataOffset = 0;
		std::uint32_t primitiveDataOffset = 0;
		std::uint32_t meshletTableOffset = 0;
		std::uint32_t meshletSectionOffset = 0;
		std::uint32_t meshletBoundsOffset = 0;
		std::uint32_t virtualGeometryHeaderOffset = 0;
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
		std::size_t virtualGeometryBytes,
		PayloadLayout& outLayout)
	{
		const auto TryToU32 = [](std::size_t value, const char* label, std::uint32_t& outValue) -> bool {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (value > std::numeric_limits<std::uint32_t>::max())
			{
				std::cerr << "Payload size overflow for " << label << ": " << value << ".\n";
				return false;
			}

			outValue = static_cast<std::uint32_t>(value);
			return true;
		};

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryToU32(vertexBytes, "vertex stream", outLayout.vertexBytes)
			|| !TryToU32(vertexRemapBytes, "remap stream", outLayout.vertexRemapBytes)
			|| !TryToU32(skinningBytes, "skinning stream", outLayout.skinningBytes)
			|| !TryToU32(primitiveBytes, "primitive stream", outLayout.primitiveBytes)
			|| !TryToU32(meshletTableBytes, "meshlet table", outLayout.meshletTableBytes)
			|| !TryToU32(meshletSectionBytes, "meshlet section table", outLayout.meshletSectionBytes)
			|| !TryToU32(meshletBoundsBytes, "meshlet bounds table", outLayout.meshletBoundsBytes)
			|| !TryToU32(virtualGeometryBytes, "virtual geometry payload", outLayout.virtualGeometryBytes))
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
		const std::uint32_t lastGeometryOffset = outLayout.meshletBoundsOffset != 0u
			? outLayout.meshletBoundsOffset + AlignUp(outLayout.meshletBoundsBytes, 4096u)
			: outLayout.meshletSectionOffset + AlignUp(outLayout.meshletSectionBytes, 4096u);
		outLayout.virtualGeometryHeaderOffset = outLayout.virtualGeometryBytes == 0u
			? 0u
			: AlignUp(lastGeometryOffset, 4096u);
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
		const std::vector<MeshletBounds>& meshletBounds,
		const VirtualGeometryBuildOutput* virtualGeometry)
	{
		std::ofstream out(outputPath, std::ios::binary);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!out)
		{
			throw std::runtime_error("Failed to open output file: " + outputPath.string());
		}

		out.write(reinterpret_cast<const char*>(&header), sizeof(header));

		WritePadding(out, header.meshletVertexDataOffset);
		out.write(reinterpret_cast<const char*>(vertexData.data()), static_cast<std::streamsize>(vertexData.size()));

		WritePadding(out, header.meshletVertexRemapDataOffset);
		out.write(reinterpret_cast<const char*>(vertexRemapData.data()), static_cast<std::streamsize>(vertexRemapData.size()));

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!meshletBounds.empty())
		{
			WritePadding(out, header.meshletBoundsOffset);
			out.write(reinterpret_cast<const char*>(meshletBounds.data()), static_cast<std::streamsize>(meshletBounds.size() * sizeof(MeshletBounds)));
		}

		if (virtualGeometry != nullptr && header.virtualGeometryHeaderOffset != 0u)
		{
			WritePadding(out, header.virtualGeometryHeaderOffset);
			out.write(reinterpret_cast<const char*>(&virtualGeometry->header), sizeof(VirtualGeometryHeader));

			if (!virtualGeometry->detailLevels.empty())
			{
				WritePadding(out, virtualGeometry->header.detailLevelTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->detailLevels.data()), static_cast<std::streamsize>(virtualGeometry->detailLevels.size() * sizeof(VirtualGeometryDetailLevelRecord)));
			}
			if (!virtualGeometry->materialSections.empty())
			{
				WritePadding(out, virtualGeometry->header.materialSectionTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->materialSections.data()), static_cast<std::streamsize>(virtualGeometry->materialSections.size() * sizeof(VirtualGeometryMaterialSectionRecord)));
			}
			if (!virtualGeometry->sectionRanges.empty())
			{
				WritePadding(out, virtualGeometry->header.sectionRangeTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->sectionRanges.data()), static_cast<std::streamsize>(virtualGeometry->sectionRanges.size() * sizeof(VirtualGeometrySectionRangeRecord)));
			}
			if (!virtualGeometry->cullableGroups.empty())
			{
				WritePadding(out, virtualGeometry->header.cullableGroupTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->cullableGroups.data()), static_cast<std::streamsize>(virtualGeometry->cullableGroups.size() * sizeof(VirtualGeometryCullableGroupRecord)));
			}
			if (!virtualGeometry->pages.empty())
			{
				WritePadding(out, virtualGeometry->header.pageTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->pages.data()), static_cast<std::streamsize>(virtualGeometry->pages.size() * sizeof(VirtualGeometryPageRecord)));
			}
			if (!virtualGeometry->requestedTargets.empty())
			{
				WritePadding(out, virtualGeometry->header.requestedTargetTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->requestedTargets.data()), static_cast<std::streamsize>(virtualGeometry->requestedTargets.size() * sizeof(VirtualGeometryRequestedTargetRecord)));
			}
			if (!virtualGeometry->skippedDetails.empty())
			{
				WritePadding(out, virtualGeometry->header.skippedDetailTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->skippedDetails.data()), static_cast<std::streamsize>(virtualGeometry->skippedDetails.size() * sizeof(VirtualGeometrySkippedDetailRecord)));
			}
			if (!virtualGeometry->clusters.empty())
			{
				WritePadding(out, virtualGeometry->header.clusterTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->clusters.data()), static_cast<std::streamsize>(virtualGeometry->clusters.size() * sizeof(VirtualGeometryClusterRecord)));
			}
			if (!virtualGeometry->clusterPageLinks.empty())
			{
				WritePadding(out, virtualGeometry->header.clusterPageLinkTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->clusterPageLinks.data()), static_cast<std::streamsize>(virtualGeometry->clusterPageLinks.size() * sizeof(VirtualGeometryClusterPageLinkRecord)));
			}
			if (!virtualGeometry->coneSafety.empty())
			{
				WritePadding(out, virtualGeometry->header.coneSafetyTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->coneSafety.data()), static_cast<std::streamsize>(virtualGeometry->coneSafety.size() * sizeof(VirtualGeometryConeSafetyRecord)));
			}
			if (!virtualGeometry->validationFlags.empty())
			{
				WritePadding(out, virtualGeometry->header.validationFlagTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->validationFlags.data()), static_cast<std::streamsize>(virtualGeometry->validationFlags.size() * sizeof(VirtualGeometryValidationFlagRecord)));
			}
			if (!virtualGeometry->diagnostics.empty())
			{
				WritePadding(out, virtualGeometry->header.diagnosticTableOffset);
				out.write(reinterpret_cast<const char*>(virtualGeometry->diagnostics.data()), static_cast<std::streamsize>(virtualGeometry->diagnostics.size() * sizeof(VirtualGeometryDiagnosticRecord)));
			}
			if (!virtualGeometry->stringTable.empty())
			{
				WritePadding(out, virtualGeometry->header.diagnosticStringOffset);
				out.write(virtualGeometry->stringTable.data(), static_cast<std::streamsize>(virtualGeometry->stringTable.size()));
			}
		}
	}

	bool BuildMeshletPayload(
		const std::vector<VertexRaw>& vertices,
		const std::vector<std::uint32_t>& indices,
		bool includeBounds,
		MeshletBuildOutput& output)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vertices.empty() || indices.empty())
		{
			std::cerr << "Cannot build meshlets from empty vertex/index payload.\n";
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (meshletCount == 0)
		{
			std::cerr << "meshoptimizer failed to generate any meshlets.\n";
			return false;
		}

		meshlets.resize(meshletCount);
		output.meshletRecords.reserve(meshletCount);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (includeBounds)
		{
			output.meshletBounds.reserve(meshletCount);
		}
		output.vertexRemapIndices.reserve(indices.size());
		output.primitiveIndices.reserve(indices.size());

		// Keep this loop sequential to preserve deterministic byte offsets in output artifacts.
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!TryConvertCount(output.vertexRemapIndices.size(), "meshlet vertex remap offset", record.vertexOffset))
			{
				return false;
			}

			record.vertexCount = sourceMeshlet.vertex_count;

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::size_t localVertex = 0; localVertex < sourceMeshlet.vertex_count; ++localVertex)
			{
				const std::uint32_t sourceVertex = meshletVertices[sourceMeshlet.vertex_offset + localVertex];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (sourceVertex >= vertices.size())
				{
					std::cerr << "meshoptimizer vertex indirection points outside source vertex array.\n";
					return false;
				}

				output.vertexRemapIndices.push_back(sourceVertex);
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!TryConvertCount(output.primitiveIndices.size(), "meshlet primitive offset", record.primitiveIndexOffset))
			{
				return false;
			}

			record.primitiveIndexCount = sourceMeshlet.triangle_count * 3u;

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::size_t localPrimitive = 0; localPrimitive < static_cast<std::size_t>(record.primitiveIndexCount); ++localPrimitive)
			{
				const std::uint8_t localVertex = meshletTriangles[sourceMeshlet.triangle_offset + localPrimitive];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (localVertex >= sourceMeshlet.vertex_count)
				{
					std::cerr << "meshoptimizer triangle payload references an out-of-range local vertex.\n";
					return false;
				}

				output.primitiveIndices.push_back(localVertex);
			}

			output.meshletRecords.push_back(record);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (output.primitiveIndices.empty() || (output.primitiveIndices.size() % 3u) != 0u)
		{
			std::cerr << "Meshlet primitive stream is empty or non-triangulated after meshlet build.\n";
			return false;
		}

		MeshletSection section{};
		section.meshletOffset = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryConvertCount(output.meshletRecords.size(), "meshlet section meshlet count", section.meshletCount))
		{
			return false;
		}
		output.meshletSections.push_back(section);

		return true;
	}

	std::uint32_t AppendVirtualGeometryString(VirtualGeometryBuildOutput& output, std::string_view value)
	{
		const std::uint32_t offset = static_cast<std::uint32_t>(output.stringTable.size());
		output.stringTable.append(value.data(), value.size());
		output.stringTable.push_back('\0');
		return offset;
	}

	bool HasValidVirtualGeometryConeBounds(
		const std::vector<VirtualGeometryCullableGroupRecord>& groups,
		std::uint32_t groupOffset,
		std::uint32_t groupCount)
	{
		if (groupCount == 0u || groupOffset >= groups.size())
		{
			return false;
		}

		const std::uint32_t end = std::min<std::uint32_t>(
			static_cast<std::uint32_t>(groups.size()),
			groupOffset + groupCount);
		for (std::uint32_t groupIndex = groupOffset; groupIndex < end; ++groupIndex)
		{
			const VirtualGeometryCullableGroupRecord& group = groups[groupIndex];
			const float axisLengthSq =
				group.coneAxis[0] * group.coneAxis[0]
				+ group.coneAxis[1] * group.coneAxis[1]
				+ group.coneAxis[2] * group.coneAxis[2];
			const bool finiteCone =
				std::isfinite(group.coneApex[0])
				&& std::isfinite(group.coneApex[1])
				&& std::isfinite(group.coneApex[2])
				&& std::isfinite(group.coneAxis[0])
				&& std::isfinite(group.coneAxis[1])
				&& std::isfinite(group.coneAxis[2])
				&& std::isfinite(group.coneCutoff)
				&& std::isfinite(group.coneApexOffset);
			if (!finiteCone
				|| axisLengthSq < 1e-6f
				|| group.coneCutoff < -1.0f
				|| group.coneCutoff > 1.0f)
			{
				return false;
			}
		}
		return end == groupOffset + groupCount;
	}

	VirtualGeometryConeSafetyRecord BuildVirtualGeometryConeSafetyRecord(
		VirtualGeometryBuildOutput& output,
		const VirtualGeometryClusterRecord& cluster,
		const VirtualGeometryConeSafetyCookSettings& settings)
	{
		// Keep these values numerically aligned with RenderTypes.h without
		// including renderer headers in the offline asset compiler.
		constexpr std::uint32_t kConeMaterialOneSided = 0u;
		constexpr std::uint32_t kConeMaterialUnknown = 2u;
		constexpr std::uint32_t kConeWindingConsistent = 0u;
		constexpr std::uint32_t kConeWindingUnknown = 2u;
		constexpr std::uint32_t kConeAlphaOpaque = 0u;
		constexpr std::uint32_t kConeAlphaUnknown = 3u;
		constexpr std::uint32_t kConeThinShellLow = 0u;
		constexpr std::uint32_t kConeThinShellUnknown = 2u;

		VirtualGeometryConeSafetyRecord record{};
		record.clusterId = cluster.clusterId;
		record.groupOffset = cluster.groupOffset;
		record.groupCount = cluster.groupCount;
		record.materialSidedness = kConeMaterialUnknown;
		record.windingState = kConeWindingUnknown;
		record.alphaRisk = kConeAlphaUnknown;
		record.thinShellRisk = kConeThinShellUnknown;

		const bool coneBoundsValid = HasValidVirtualGeometryConeBounds(output.cullableGroups, cluster.groupOffset, cluster.groupCount);
		if (coneBoundsValid)
		{
			record.flags |= kVirtualGeometryConeSafetyFlagConeValid;
		}

		std::string disabledReason;
		if (!coneBoundsValid)
		{
			disabledReason = "Cone bounds are missing or invalid.";
		}
		else if (settings.proveOneSidedOpaqueClusters)
		{
			record.materialSidedness = kConeMaterialOneSided;
			record.windingState = kConeWindingConsistent;
			record.alphaRisk = kConeAlphaOpaque;
			record.thinShellRisk = kConeThinShellLow;
			record.flags |= kVirtualGeometryConeSafetyFlagEligible;
		}
		else
		{
			disabledReason = settings.disabledReason.empty()
				? "Cone culling disabled until imported material safety metadata is proven."
				: settings.disabledReason;
		}

		if (!disabledReason.empty())
		{
			record.disabledReasonStringOffset = AppendVirtualGeometryString(output, disabledReason);
			record.disabledReasonStringSize = static_cast<std::uint32_t>(disabledReason.size());
		}
		return record;
	}

	void BuildPositionBounds(
		const std::vector<VertexRaw>& vertices,
		float (&outMin)[3],
		float (&outMax)[3])
	{
		outMin[0] = outMin[1] = outMin[2] = std::numeric_limits<float>::max();
		outMax[0] = outMax[1] = outMax[2] = std::numeric_limits<float>::lowest();
		for (const VertexRaw& vertex : vertices)
		{
			for (int component = 0; component < 3; ++component)
			{
				outMin[component] = std::min(outMin[component], vertex.position[component]);
				outMax[component] = std::max(outMax[component], vertex.position[component]);
			}
		}
	}

	const char* ToCompileTargetStatusText(VirtualGeometryDetailTargetRecordStatus status)
	{
		switch (status)
		{
		case VirtualGeometryDetailTargetRecordStatus::Requested: return "Requested";
		case VirtualGeometryDetailTargetRecordStatus::Generated: return "Generated";
		case VirtualGeometryDetailTargetRecordStatus::Merged: return "Merged";
		case VirtualGeometryDetailTargetRecordStatus::Skipped: return "Skipped";
		case VirtualGeometryDetailTargetRecordStatus::Rejected: return "Rejected";
		}
		return "Unknown";
	}

	const char* ToCompileSkippedReasonText(VirtualGeometrySkippedDetailRecordReason reason)
	{
		switch (reason)
		{
		case VirtualGeometrySkippedDetailRecordReason::None: return "None";
		case VirtualGeometrySkippedDetailRecordReason::MeshTooSmall: return "MeshTooSmall";
		case VirtualGeometrySkippedDetailRecordReason::TooFewTriangles: return "TooFewTriangles";
		case VirtualGeometrySkippedDetailRecordReason::TooFewMeshlets: return "TooFewMeshlets";
		case VirtualGeometrySkippedDetailRecordReason::InsufficientTriangleReduction: return "InsufficientTriangleReduction";
		case VirtualGeometrySkippedDetailRecordReason::InsufficientMeshletReduction: return "InsufficientMeshletReduction";
		case VirtualGeometrySkippedDetailRecordReason::InvalidSimplificationOutput: return "InvalidSimplificationOutput";
		case VirtualGeometrySkippedDetailRecordReason::MaterialSafetyRejected: return "MaterialSafetyRejected";
		case VirtualGeometrySkippedDetailRecordReason::GeometricErrorRejected: return "GeometricErrorRejected";
		case VirtualGeometrySkippedDetailRecordReason::DuplicateOfPreviousLevel: return "DuplicateOfPreviousLevel";
		}
		return "Unknown";
	}

	std::uint32_t ParseCompileTargetStatus(std::string_view status)
	{
		if (status == "Generated")
		{
			return static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Generated);
		}
		if (status == "Merged")
		{
			return static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Merged);
		}
		if (status == "Rejected")
		{
			return static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Rejected);
		}
		if (status == "Requested")
		{
			return static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Requested);
		}
		return static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Skipped);
	}

	std::uint32_t ParseCompileSkippedReason(std::string_view reason)
	{
		if (reason == "TooFewTriangles")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::TooFewTriangles);
		}
		if (reason == "TooFewMeshlets")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::TooFewMeshlets);
		}
		if (reason == "InsufficientTriangleReduction")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::InsufficientTriangleReduction);
		}
		if (reason == "InsufficientMeshletReduction")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::InsufficientMeshletReduction);
		}
		if (reason == "InvalidSimplificationOutput")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::InvalidSimplificationOutput);
		}
		if (reason == "MaterialSafetyRejected")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::MaterialSafetyRejected);
		}
		if (reason == "GeometricErrorRejected")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::GeometricErrorRejected);
		}
		if (reason == "DuplicateOfPreviousLevel")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::DuplicateOfPreviousLevel);
		}
		if (reason == "MeshTooSmall")
		{
			return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::MeshTooSmall);
		}
		return static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::None);
	}

	std::uint32_t EstimateMeshletCountForLod(
		const std::vector<VertexRaw>& vertices,
		const std::vector<std::uint32_t>& indices,
		std::uint32_t fallbackCount)
	{
		MeshletBuildOutput meshletOutput{};
		if (!BuildMeshletPayload(vertices, indices, false, meshletOutput))
		{
			return fallbackCount;
		}
		return static_cast<std::uint32_t>(meshletOutput.meshletRecords.size());
	}

	void AppendRequestedTargetReport(
		MeshCompileReport& report,
		std::uint32_t targetIndex,
		float requestedRatio,
		std::uint32_t desiredTriangleCount,
		std::uint32_t desiredMeshletCount,
		VirtualGeometryDetailTargetRecordStatus status)
	{
		VirtualGeometryRequestedTargetCompileReport target;
		target.targetIndex = targetIndex;
		target.requestedRatio = requestedRatio;
		target.desiredTriangleCount = desiredTriangleCount;
		target.desiredMeshletCount = desiredMeshletCount;
		target.status = ToCompileTargetStatusText(status);
		report.virtualGeometry.requestedTargets.push_back(target);
	}

	void AppendSkippedTargetReport(
		MeshCompileReport& report,
		std::uint32_t targetIndex,
		float requestedRatio,
		VirtualGeometryDetailTargetRecordStatus status,
		VirtualGeometrySkippedDetailRecordReason reason,
		std::uint32_t nearestGeneratedLevelIndex,
		std::string message,
		std::uint32_t candidateTriangleCount = 0u,
		std::uint32_t candidateMeshletCount = 0u,
		float actualCandidateRatio = 0.0f)
	{
		VirtualGeometrySkippedLevelCompileReport skipped;
		skipped.targetIndex = targetIndex;
		skipped.requestedRatio = requestedRatio;
		skipped.status = ToCompileTargetStatusText(status);
		skipped.reason = ToCompileSkippedReasonText(reason);
		skipped.nearestGeneratedLevelIndex = nearestGeneratedLevelIndex;
		// Store the attempted candidate separately from the requested target
		// and nearest fallback so Inspector tables can explain merged rows.
		skipped.candidateTriangleCount = candidateTriangleCount;
		skipped.candidateMeshletCount = candidateMeshletCount;
		skipped.actualCandidateRatio = actualCandidateRatio;
		skipped.message = std::move(message);
		report.virtualGeometry.skippedLevels.push_back(skipped);
	}

	std::string BuildSmallMeshSkipMessage(std::uint32_t sourceTriangleCount, std::uint32_t minimumSourceTriangles)
	{
		std::ostringstream message;
		message << "source mesh has " << sourceTriangleCount
			<< " triangles, below the minimum source threshold of " << minimumSourceTriangles
			<< "; extra adaptive detail levels would add switches without useful reduction";
		return message.str();
	}

	std::string BuildMinimumGeneratedTriangleMessage(std::uint32_t desiredTriangleCount, std::uint32_t minimumGeneratedTriangles)
	{
		std::ostringstream message;
		message << "target requested " << desiredTriangleCount
			<< " triangles, below the minimum generated detail threshold of " << minimumGeneratedTriangles;
		return message.str();
	}

	std::string BuildUsefulReductionMessage(
		std::string_view domain,
		float reduction,
		float threshold,
		std::uint32_t previousCount,
		std::uint32_t candidateCount)
	{
		std::ostringstream message;
		message << "candidate " << domain << " reduction is "
			<< std::fixed << std::setprecision(1) << (std::max(reduction, 0.0f) * 100.0f)
			<< "% from " << previousCount << " to " << candidateCount
			<< ", below the useful threshold of " << (threshold * 100.0f)
			<< "%; target was merged into the nearest generated detail";
		return message.str();
	}

	bool BuildVirtualGeometryLods(
		const std::vector<VertexRaw>& vertices,
		const std::vector<std::uint32_t>& sourceIndices,
		MeshCompileReport& report,
		std::vector<VirtualGeometryLodSource>& outLods)
	{
		outLods.clear();
		report.virtualGeometry.requestedTargets.clear();
		report.virtualGeometry.generatedLevels.clear();
		report.virtualGeometry.skippedLevels.clear();
		report.virtualGeometry.clusters.clear();
		report.virtualGeometry.pages.clear();
		report.virtualGeometry.adaptiveSettings = SanitizeVirtualGeometryAdaptiveCompileSettings(
			report.virtualGeometry.adaptiveSettings.targetRatios.empty()
				? MakeDefaultVirtualGeometryAdaptiveCompileSettings()
				: report.virtualGeometry.adaptiveSettings);
		const VirtualGeometryAdaptiveCompileSettings settings = report.virtualGeometry.adaptiveSettings;

		VirtualGeometryLodSource baseLod{};
		baseLod.indices = sourceIndices;
		baseLod.targetRatio = 1.0f;
		baseLod.geometricError = 0.0f;
		baseLod.requestedTargetIndex = 0u;
		outLods.push_back(std::move(baseLod));

		const std::uint32_t sourceTriangleCount = static_cast<std::uint32_t>(sourceIndices.size() / 3u);
		const std::uint32_t sourceMeshletCount = EstimateMeshletCountForLod(vertices, sourceIndices, 1u);
		report.virtualGeometry.sourceTriangleCount = sourceTriangleCount;
		report.virtualGeometry.selectedFallbackAvailable = 1u;
		AppendRequestedTargetReport(report, 0u, 1.0f, sourceTriangleCount, sourceMeshletCount, VirtualGeometryDetailTargetRecordStatus::Generated);
		report.virtualGeometry.generatedLevels.push_back({ 0u, 0u, 0u, 1.0f, sourceTriangleCount, sourceMeshletCount, 0.0f });

		// LOD0 is always accepted before optional simplification starts so the
		// virtual-geometry chain has a complete source-detail reference. If no
		// lower-detail level survives the adaptive gates, this same page becomes
		// the explicit fallback when fallback ids are assigned after cooking.
		// Very small meshes keep a one-detail virtual-geometry payload. That is
		// cheaper and safer than forcing simplification that cannot remove enough
		// triangles to pay for the extra LOD switch.
		if (sourceTriangleCount < settings.minimumSourceTriangles)
		{
			report.virtualGeometry.status = VirtualGeometryCompileStatus::Skipped;
			report.virtualGeometry.warnings.push_back("virtual geometry simplification skipped: source mesh is below the small-mesh threshold");
			for (std::uint32_t targetIndex = 1u; targetIndex < settings.targetRatios.size(); ++targetIndex)
			{
				const float ratio = settings.targetRatios[targetIndex];
				const std::uint32_t desiredTriangles = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(sourceTriangleCount) * ratio));
				AppendRequestedTargetReport(report, targetIndex, ratio, desiredTriangles, sourceMeshletCount, VirtualGeometryDetailTargetRecordStatus::Skipped);
				AppendSkippedTargetReport(
					report,
					targetIndex,
					ratio,
					VirtualGeometryDetailTargetRecordStatus::Skipped,
					VirtualGeometrySkippedDetailRecordReason::MeshTooSmall,
					0u,
					BuildSmallMeshSkipMessage(sourceTriangleCount, settings.minimumSourceTriangles));
			}
			return true;
		}

		std::vector<std::uint32_t> currentIndices = sourceIndices;
		const float* positions = vertices.front().position.data();
		const float* attributes = vertices.front().normal.data();
		float maxError = 0.0f;
		std::uint32_t previousTriangleCount = sourceTriangleCount;
		std::uint32_t previousMeshletCount = sourceMeshletCount;

		for (std::uint32_t targetIndex = 1u; targetIndex < settings.targetRatios.size(); ++targetIndex)
		{
			const float targetRatio = settings.targetRatios[targetIndex];
			const std::uint32_t desiredTriangleCount = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(sourceTriangleCount) * targetRatio));
			const std::uint32_t desiredMeshletCount = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(sourceMeshletCount) * targetRatio));
			std::size_t targetIndexCount = static_cast<std::size_t>(static_cast<float>(sourceIndices.size()) * targetRatio);
			targetIndexCount = std::max<std::size_t>(3u, (targetIndexCount / 3u) * 3u);
			// Ratios are requests, not obligations. Each gate below explains why
			// a target did or did not become a real level so artists can tune the
			// source mesh instead of guessing from a silent hardcoded LOD chain.
			if (desiredTriangleCount < settings.minimumGeneratedTriangles)
			{
				AppendRequestedTargetReport(report, targetIndex, targetRatio, desiredTriangleCount, desiredMeshletCount, VirtualGeometryDetailTargetRecordStatus::Skipped);
				AppendSkippedTargetReport(
					report,
					targetIndex,
					targetRatio,
					VirtualGeometryDetailTargetRecordStatus::Skipped,
					VirtualGeometrySkippedDetailRecordReason::TooFewTriangles,
					static_cast<std::uint32_t>(outLods.size() - 1u),
					BuildMinimumGeneratedTriangleMessage(desiredTriangleCount, settings.minimumGeneratedTriangles));
				continue;
			}

			if (targetIndexCount >= currentIndices.size())
			{
				AppendRequestedTargetReport(report, targetIndex, targetRatio, desiredTriangleCount, desiredMeshletCount, VirtualGeometryDetailTargetRecordStatus::Merged);
				AppendSkippedTargetReport(
					report,
					targetIndex,
					targetRatio,
					VirtualGeometryDetailTargetRecordStatus::Merged,
					VirtualGeometrySkippedDetailRecordReason::DuplicateOfPreviousLevel,
					static_cast<std::uint32_t>(outLods.size() - 1u),
					"target ratio would not reduce the previous generated detail",
					previousTriangleCount,
					previousMeshletCount,
					static_cast<float>(previousTriangleCount) / static_cast<float>(sourceTriangleCount));
				continue;
			}

			std::vector<std::uint32_t> simplified(currentIndices.size());
			float resultError = 0.0f;
			std::size_t simplifiedIndexCount = meshopt_simplifyWithAttributes(
				simplified.data(),
				currentIndices.data(),
				currentIndices.size(),
				positions,
				vertices.size(),
				sizeof(VertexRaw),
				attributes,
				sizeof(VertexRaw),
				kVirtualGeometrySimplifyAttributeWeights.data(),
				kVirtualGeometrySimplifyAttributeCount,
				nullptr,
				targetIndexCount,
				kSimplificationTargetError,
				kVirtualGeometrySimplifyOptions,
				&resultError);
			simplifiedIndexCount = (simplifiedIndexCount / 3u) * 3u;

			if (simplifiedIndexCount < 3u || simplifiedIndexCount >= currentIndices.size())
			{
				AppendRequestedTargetReport(report, targetIndex, targetRatio, desiredTriangleCount, desiredMeshletCount, VirtualGeometryDetailTargetRecordStatus::Rejected);
				AppendSkippedTargetReport(
					report,
					targetIndex,
					targetRatio,
					VirtualGeometryDetailTargetRecordStatus::Rejected,
					VirtualGeometrySkippedDetailRecordReason::InvalidSimplificationOutput,
					static_cast<std::uint32_t>(outLods.size() - 1u),
					"meshoptimizer did not return a smaller valid triangle list");
				continue;
			}

			const std::uint32_t candidateTriangleCount = static_cast<std::uint32_t>(simplifiedIndexCount / 3u);
			resultError = std::max(resultError, maxError);
			if (!std::isfinite(resultError) || resultError > settings.maximumGeometricError)
			{
				// Rejecting one candidate does not poison the entire asset. The
				// previous detail remains the fallback so the renderer never sees
				// a lower-detail mesh with unbounded visual error.
				report.virtualGeometry.warnings.push_back("virtual geometry simplification skipped a candidate LOD: geometric error exceeded the US2 tolerance");
				AppendRequestedTargetReport(report, targetIndex, targetRatio, desiredTriangleCount, desiredMeshletCount, VirtualGeometryDetailTargetRecordStatus::Rejected);
				AppendSkippedTargetReport(
					report,
					targetIndex,
					targetRatio,
					VirtualGeometryDetailTargetRecordStatus::Rejected,
					VirtualGeometrySkippedDetailRecordReason::GeometricErrorRejected,
					static_cast<std::uint32_t>(outLods.size() - 1u),
					"simplified detail exceeded the configured geometric error limit",
					candidateTriangleCount,
					0u,
					static_cast<float>(candidateTriangleCount) / static_cast<float>(sourceTriangleCount));
				continue;
			}

			simplified.resize(simplifiedIndexCount);
			const std::uint32_t candidateMeshletCount = EstimateMeshletCountForLod(vertices, simplified, previousMeshletCount);
			const float actualCandidateRatio = static_cast<float>(candidateTriangleCount) / static_cast<float>(sourceTriangleCount);
			if (candidateMeshletCount < settings.minimumMeshletsPerGeneratedLevel)
			{
				AppendRequestedTargetReport(report, targetIndex, targetRatio, desiredTriangleCount, desiredMeshletCount, VirtualGeometryDetailTargetRecordStatus::Skipped);
				AppendSkippedTargetReport(
					report,
					targetIndex,
					targetRatio,
					VirtualGeometryDetailTargetRecordStatus::Skipped,
					VirtualGeometrySkippedDetailRecordReason::TooFewMeshlets,
					static_cast<std::uint32_t>(outLods.size() - 1u),
					"candidate would produce too few meshlets to justify a separate detail level",
					candidateTriangleCount,
					candidateMeshletCount,
					actualCandidateRatio);
				continue;
			}

			const float triangleReduction = previousTriangleCount > 0u
				? 1.0f - (static_cast<float>(candidateTriangleCount) / static_cast<float>(previousTriangleCount))
				: 0.0f;
			const float meshletReduction = previousMeshletCount > 0u
				? 1.0f - (static_cast<float>(candidateMeshletCount) / static_cast<float>(previousMeshletCount))
				: 0.0f;
			if (triangleReduction < settings.minimumUsefulTriangleReduction)
			{
				AppendRequestedTargetReport(report, targetIndex, targetRatio, desiredTriangleCount, desiredMeshletCount, VirtualGeometryDetailTargetRecordStatus::Merged);
				AppendSkippedTargetReport(
					report,
					targetIndex,
					targetRatio,
					VirtualGeometryDetailTargetRecordStatus::Merged,
					VirtualGeometrySkippedDetailRecordReason::InsufficientTriangleReduction,
					static_cast<std::uint32_t>(outLods.size() - 1u),
					BuildUsefulReductionMessage(
						"triangle",
						triangleReduction,
						settings.minimumUsefulTriangleReduction,
						previousTriangleCount,
						candidateTriangleCount),
					candidateTriangleCount,
					candidateMeshletCount,
					actualCandidateRatio);
				continue;
			}
			if (meshletReduction < settings.minimumUsefulMeshletReduction)
			{
				AppendRequestedTargetReport(report, targetIndex, targetRatio, desiredTriangleCount, desiredMeshletCount, VirtualGeometryDetailTargetRecordStatus::Merged);
				AppendSkippedTargetReport(
					report,
					targetIndex,
					targetRatio,
					VirtualGeometryDetailTargetRecordStatus::Merged,
					VirtualGeometrySkippedDetailRecordReason::InsufficientMeshletReduction,
					static_cast<std::uint32_t>(outLods.size() - 1u),
					BuildUsefulReductionMessage(
						"meshlet",
						meshletReduction,
						settings.minimumUsefulMeshletReduction,
						previousMeshletCount,
						candidateMeshletCount),
					candidateTriangleCount,
					candidateMeshletCount,
					actualCandidateRatio);
				continue;
			}

			VirtualGeometryLodSource lod{};
			lod.targetRatio = static_cast<float>(simplifiedIndexCount) / static_cast<float>(sourceIndices.size());
			lod.geometricError = resultError;
			lod.indices = simplified;
			lod.requestedTargetIndex = targetIndex;
			maxError = std::max(maxError, resultError);
			currentIndices = std::move(simplified);
			outLods.push_back(std::move(lod));
			AppendRequestedTargetReport(report, targetIndex, targetRatio, desiredTriangleCount, desiredMeshletCount, VirtualGeometryDetailTargetRecordStatus::Generated);
			report.virtualGeometry.generatedLevels.push_back({
				static_cast<std::uint32_t>(report.virtualGeometry.generatedLevels.size()),
				targetIndex,
				static_cast<std::uint32_t>(outLods.size() - 1u),
				actualCandidateRatio,
				candidateTriangleCount,
				candidateMeshletCount,
				resultError
			});
			previousTriangleCount = candidateTriangleCount;
			previousMeshletCount = candidateMeshletCount;

			if ((currentIndices.size() / 3u) <= settings.minimumGeneratedTriangles)
			{
				break;
			}
		}

		if (outLods.size() == 1u)
		{
			report.virtualGeometry.status = VirtualGeometryCompileStatus::Skipped;
			report.virtualGeometry.warnings.push_back("virtual geometry simplification skipped: meshoptimizer did not produce a smaller valid detail level");
			return true;
		}

		report.virtualGeometry.status = VirtualGeometryCompileStatus::Cooked;
		report.virtualGeometry.maxGeometricError = maxError;
		return true;
	}

	std::uint32_t ComputeVirtualGeometrySpatialClusterCount(std::uint32_t groupCount)
	{
		// Spatial clusters are the dispatch unit for mixed-detail cuts. Tiny
		// LODs stay whole, while dense LODs are split enough for nearby and far
		// regions of the same object to make different refinement decisions.
		constexpr std::uint32_t kTargetGroupsPerCluster = 8u;
		constexpr std::uint32_t kMaxSpatialClustersPerLod = 64u;
		if (groupCount == 0u)
		{
			return 0u;
		}
		const std::uint32_t desiredClusters = (groupCount + kTargetGroupsPerCluster - 1u) / kTargetGroupsPerCluster;
		return std::clamp(desiredClusters, 1u, std::min(groupCount, kMaxSpatialClustersPerLod));
	}

	std::uint32_t FloorPowerOfTwo(std::uint32_t value)
	{
		std::uint32_t result = 1u;
		while (result <= (std::numeric_limits<std::uint32_t>::max() / 2u) && (result * 2u) <= value)
		{
			result *= 2u;
		}
		return result;
	}

	std::uint32_t ComputeVirtualGeometryFineRegionCount(std::uint32_t sourceTriangleCount, std::uint32_t sourceMeshletCount)
	{
		if (sourceTriangleCount == 0u || sourceMeshletCount == 0u)
		{
			return 0u;
		}

		const std::uint32_t desiredFromMeshlets = ComputeVirtualGeometrySpatialClusterCount(sourceMeshletCount);
		const std::uint32_t desiredFromTriangles = std::max(1u, sourceTriangleCount / 16u);
		const std::uint32_t desired = std::max(1u, std::min(desiredFromMeshlets, desiredFromTriangles));
		return std::min(FloorPowerOfTwo(desired), sourceTriangleCount);
	}

	std::uint32_t ComputeVirtualGeometryCoverageClusterCount(std::uint32_t fineRegionCount, float targetRatio)
	{
		if (fineRegionCount == 0u)
		{
			return 0u;
		}
		if (!std::isfinite(targetRatio) || targetRatio >= 0.999f)
		{
			return fineRegionCount;
		}

		const float clampedRatio = std::clamp(targetRatio, 0.001f, 1.0f);
		const std::uint32_t desired = std::max(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(fineRegionCount) * clampedRatio)));
		return std::min(fineRegionCount, FloorPowerOfTwo(desired));
	}

	std::uint32_t ExpandMortonBits(std::uint32_t value)
	{
		value &= 0x000003ffu;
		value = (value | (value << 16u)) & 0x030000ffu;
		value = (value | (value << 8u)) & 0x0300f00fu;
		value = (value | (value << 4u)) & 0x030c30c3u;
		value = (value | (value << 2u)) & 0x09249249u;
		return value;
	}

	std::uint32_t QuantizeMortonAxis(float value, float minimum, float maximum)
	{
		const float extent = maximum - minimum;
		if (!std::isfinite(value) || !std::isfinite(extent) || extent <= 0.0f)
		{
			return 0u;
		}

		const float normalized = std::clamp((value - minimum) / extent, 0.0f, 1.0f);
		return static_cast<std::uint32_t>(std::round(normalized * 1023.0f));
	}

	std::uint32_t EncodeMorton3D(float x, float y, float z, const float boundsMin[3], const float boundsMax[3])
	{
		const std::uint32_t qx = QuantizeMortonAxis(x, boundsMin[0], boundsMax[0]);
		const std::uint32_t qy = QuantizeMortonAxis(y, boundsMin[1], boundsMax[1]);
		const std::uint32_t qz = QuantizeMortonAxis(z, boundsMin[2], boundsMax[2]);
		return ExpandMortonBits(qx) | (ExpandMortonBits(qy) << 1u) | (ExpandMortonBits(qz) << 2u);
	}

	std::vector<VirtualGeometrySourceRegion> BuildVirtualGeometrySourceRegions(
		const std::vector<VertexRaw>& vertices,
		const std::vector<std::uint32_t>& sourceIndices,
		std::uint32_t desiredFineRegionCount)
	{
		struct TriangleSortKey
		{
			std::uint32_t triangleIndex = 0u;
			std::uint32_t mortonKey = 0u;
		};

		std::vector<VirtualGeometrySourceRegion> regions;
		const std::uint32_t sourceTriangleCount = static_cast<std::uint32_t>(sourceIndices.size() / 3u);
		if (vertices.empty() || sourceTriangleCount == 0u || desiredFineRegionCount == 0u)
		{
			return regions;
		}

		float boundsMin[3];
		float boundsMax[3];
		BuildPositionBounds(vertices, boundsMin, boundsMax);

		std::vector<TriangleSortKey> sortedTriangles;
		sortedTriangles.reserve(sourceTriangleCount);
		for (std::uint32_t triangleIndex = 0u; triangleIndex < sourceTriangleCount; ++triangleIndex)
		{
			const std::uint32_t i0 = sourceIndices[(static_cast<std::size_t>(triangleIndex) * 3u) + 0u];
			const std::uint32_t i1 = sourceIndices[(static_cast<std::size_t>(triangleIndex) * 3u) + 1u];
			const std::uint32_t i2 = sourceIndices[(static_cast<std::size_t>(triangleIndex) * 3u) + 2u];
			if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
			{
				continue;
			}

			const float cx = (vertices[i0].position[0] + vertices[i1].position[0] + vertices[i2].position[0]) / 3.0f;
			const float cy = (vertices[i0].position[1] + vertices[i1].position[1] + vertices[i2].position[1]) / 3.0f;
			const float cz = (vertices[i0].position[2] + vertices[i1].position[2] + vertices[i2].position[2]) / 3.0f;
			sortedTriangles.push_back({ triangleIndex, EncodeMorton3D(cx, cy, cz, boundsMin, boundsMax) });
		}

		if (sortedTriangles.empty())
		{
			return regions;
		}

		std::sort(sortedTriangles.begin(), sortedTriangles.end(), [](const TriangleSortKey& left, const TriangleSortKey& right) {
			if (left.mortonKey != right.mortonKey)
			{
				return left.mortonKey < right.mortonKey;
			}
			return left.triangleIndex < right.triangleIndex;
		});

		const std::uint32_t fineRegionCount = std::min<std::uint32_t>(desiredFineRegionCount, static_cast<std::uint32_t>(sortedTriangles.size()));
		regions.reserve(fineRegionCount);
		for (std::uint32_t regionIndex = 0u; regionIndex < fineRegionCount; ++regionIndex)
		{
			const std::uint32_t firstTriangle = static_cast<std::uint32_t>((static_cast<std::uint64_t>(sortedTriangles.size()) * regionIndex) / fineRegionCount);
			const std::uint32_t lastTriangle = static_cast<std::uint32_t>((static_cast<std::uint64_t>(sortedTriangles.size()) * (regionIndex + 1u)) / fineRegionCount);
			VirtualGeometrySourceRegion region{};
			region.firstFineRegion = regionIndex;
			region.fineRegionCount = 1u;
			region.indices.reserve(static_cast<std::size_t>(lastTriangle - firstTriangle) * 3u);
			for (std::uint32_t sortedIndex = firstTriangle; sortedIndex < lastTriangle; ++sortedIndex)
			{
				const std::uint32_t triangleIndex = sortedTriangles[sortedIndex].triangleIndex;
				region.indices.push_back(sourceIndices[(static_cast<std::size_t>(triangleIndex) * 3u) + 0u]);
				region.indices.push_back(sourceIndices[(static_cast<std::size_t>(triangleIndex) * 3u) + 1u]);
				region.indices.push_back(sourceIndices[(static_cast<std::size_t>(triangleIndex) * 3u) + 2u]);
			}
			if (!region.indices.empty())
			{
				regions.push_back(std::move(region));
			}
		}
		return regions;
	}

	std::vector<std::uint32_t> CollectVirtualGeometryCoverageIndices(
		const std::vector<VirtualGeometrySourceRegion>& fineRegions,
		std::uint32_t firstFineRegion,
		std::uint32_t fineRegionCount)
	{
		std::vector<std::uint32_t> indices;
		const std::uint32_t lastFineRegion = std::min<std::uint32_t>(
			static_cast<std::uint32_t>(fineRegions.size()),
			firstFineRegion + fineRegionCount);
		std::size_t indexCount = 0u;
		for (std::uint32_t regionIndex = firstFineRegion; regionIndex < lastFineRegion; ++regionIndex)
		{
			indexCount += fineRegions[regionIndex].indices.size();
		}
		indices.reserve(indexCount);
		for (std::uint32_t regionIndex = firstFineRegion; regionIndex < lastFineRegion; ++regionIndex)
		{
			const std::vector<std::uint32_t>& regionIndices = fineRegions[regionIndex].indices;
			indices.insert(indices.end(), regionIndices.begin(), regionIndices.end());
		}
		return indices;
	}

	float ComputeVirtualGeometrySelectionErrorFloor(float targetRatio)
	{
		if (!std::isfinite(targetRatio))
		{
			return 0.0f;
		}

		const float retainedRatio = std::clamp(targetRatio, 0.001f, 1.0f);
		const float removedToRetainedRatio = (1.0f - retainedRatio) / retainedRatio;
		// A plain (1 - ratio) floor converges toward one, so deep LODs receive
		// nearly identical screen-error values and can all transition within a
		// tiny camera movement. Scaling by removed geometry relative to what
		// remains keeps the first half-octave transition near 480 pixels while
		// progressively spacing coarser levels farther apart.
		return std::clamp(removedToRetainedRatio * 0.0075f, 0.0f, 0.1f);
	}

	std::vector<std::uint32_t> SimplifyVirtualGeometryCoverageCluster(
		const std::vector<VertexRaw>& vertices,
		const std::vector<std::uint32_t>& sourceIndices,
		float targetRatio,
		float& outGeometricError)
	{
		outGeometricError = 0.0f;
		if (sourceIndices.size() < 6u || !std::isfinite(targetRatio) || targetRatio >= 0.999f)
		{
			return sourceIndices;
		}

		std::size_t targetIndexCount = static_cast<std::size_t>(static_cast<float>(sourceIndices.size()) * std::clamp(targetRatio, 0.001f, 1.0f));
		targetIndexCount = std::max<std::size_t>(3u, (targetIndexCount / 3u) * 3u);
		if (targetIndexCount >= sourceIndices.size())
		{
			return sourceIndices;
		}

		std::vector<std::uint32_t> simplified(sourceIndices.size());
		float resultError = 0.0f;
		const float* positions = vertices.front().position.data();
		const float* attributes = vertices.front().normal.data();
		std::size_t simplifiedIndexCount = meshopt_simplifyWithAttributes(
			simplified.data(),
			sourceIndices.data(),
			sourceIndices.size(),
			positions,
			vertices.size(),
			sizeof(VertexRaw),
			attributes,
			sizeof(VertexRaw),
			kVirtualGeometrySimplifyAttributeWeights.data(),
			kVirtualGeometrySimplifyAttributeCount,
			nullptr,
			targetIndexCount,
			kSimplificationTargetError,
			kVirtualGeometrySimplifyOptions,
			&resultError);
		simplifiedIndexCount = (simplifiedIndexCount / 3u) * 3u;

		if (simplifiedIndexCount < 3u || simplifiedIndexCount >= sourceIndices.size() || !std::isfinite(resultError))
		{
			return sourceIndices;
		}

		simplified.resize(simplifiedIndexCount);
		outGeometricError = resultError;
		return simplified;
	}

	void ComputeVirtualGeometryClusterBounds(
		const std::vector<VirtualGeometryCullableGroupRecord>& groups,
		std::uint32_t groupOffset,
		std::uint32_t groupCount,
		float outCenter[3],
		float& outRadius)
	{
		// Build a conservative sphere around the meshlet bounds owned by this
		// cluster. Render preparation relies on this worker-safe metadata instead
		// of reading individual meshlet records while choosing a cluster cut.
		std::array<float, 3> minBounds = {
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()
		};
		std::array<float, 3> maxBounds = {
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max()
		};
		for (std::uint32_t groupIndex = groupOffset; groupIndex < groupOffset + groupCount && groupIndex < groups.size(); ++groupIndex)
		{
			const VirtualGeometryCullableGroupRecord& group = groups[groupIndex];
			const float radius = std::isfinite(group.boundsRadius) ? std::max(group.boundsRadius, 0.0f) : 1.0f;
			for (int axis = 0; axis < 3; ++axis)
			{
				const float center = std::isfinite(group.boundsCenter[axis]) ? group.boundsCenter[axis] : 0.0f;
				minBounds[axis] = std::min(minBounds[axis], center - radius);
				maxBounds[axis] = std::max(maxBounds[axis], center + radius);
			}
		}

		for (int axis = 0; axis < 3; ++axis)
		{
			if (minBounds[axis] > maxBounds[axis])
			{
				minBounds[axis] = -0.5f;
				maxBounds[axis] = 0.5f;
			}
			outCenter[axis] = (minBounds[axis] + maxBounds[axis]) * 0.5f;
		}

		outRadius = 0.0f;
		for (std::uint32_t groupIndex = groupOffset; groupIndex < groupOffset + groupCount && groupIndex < groups.size(); ++groupIndex)
		{
			const VirtualGeometryCullableGroupRecord& group = groups[groupIndex];
			const float radius = std::isfinite(group.boundsRadius) ? std::max(group.boundsRadius, 0.0f) : 1.0f;
			const float dx = group.boundsCenter[0] - outCenter[0];
			const float dy = group.boundsCenter[1] - outCenter[1];
			const float dz = group.boundsCenter[2] - outCenter[2];
			outRadius = std::max(outRadius, std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) + radius);
		}
		if (!std::isfinite(outRadius) || outRadius <= 0.0f)
		{
			outRadius = 1.0f;
		}
	}

	bool AppendRegionalLodMeshlets(
		const std::vector<VirtualGeometrySourceRegion>& fineRegions,
		std::uint32_t lodIndex,
		std::uint32_t sectionId,
		const VirtualGeometryLodSource& lod,
		const std::vector<VertexRaw>& vertices,
		bool includeBounds,
		float previousDetailGeometricError,
		bool& outDetailEmitted,
		std::uint32_t& outCandidatePrimitiveCount,
		std::uint32_t& outCandidateMeshletCount,
		VirtualGeometryBuildOutput& output)
	{
		outDetailEmitted = false;
		outCandidatePrimitiveCount = 0u;
		outCandidateMeshletCount = 0u;
		if (fineRegions.empty())
		{
			std::cerr << "Virtual geometry source coverage regions are empty.\n";
			return false;
		}

		const std::uint32_t coverageClusterCount = ComputeVirtualGeometryCoverageClusterCount(
			static_cast<std::uint32_t>(fineRegions.size()),
			lod.targetRatio);
		if (coverageClusterCount == 0u)
		{
			std::cerr << "Virtual geometry coverage cluster count resolved to zero.\n";
			return false;
		}

		std::vector<VirtualGeometryClusterCookPayload> clusterPayloads;
		clusterPayloads.reserve(coverageClusterCount);
		std::uint32_t totalPrimitiveCount = 0u;
		std::uint32_t totalMeshletCount = 0u;
		float maxClusterError = 0.0f;

		for (std::uint32_t coverageClusterIndex = 0u; coverageClusterIndex < coverageClusterCount; ++coverageClusterIndex)
		{
			const std::uint32_t firstFineRegion = (static_cast<std::uint32_t>(fineRegions.size()) * coverageClusterIndex) / coverageClusterCount;
			const std::uint32_t lastFineRegion = (static_cast<std::uint32_t>(fineRegions.size()) * (coverageClusterIndex + 1u)) / coverageClusterCount;
			const std::uint32_t fineRegionCount = lastFineRegion > firstFineRegion ? lastFineRegion - firstFineRegion : 1u;
			std::vector<std::uint32_t> sourceCoverageIndices = CollectVirtualGeometryCoverageIndices(
				fineRegions,
				firstFineRegion,
				fineRegionCount);
			if (sourceCoverageIndices.empty())
			{
				std::cerr << "Virtual geometry coverage cluster has no source triangles.\n";
				return false;
			}

			float simplificationError = 0.0f;
			std::vector<std::uint32_t> clusterIndices = SimplifyVirtualGeometryCoverageCluster(
				vertices,
				sourceCoverageIndices,
				lod.targetRatio,
				simplificationError);

			VirtualGeometryClusterCookPayload payload{};
			payload.firstFineRegion = firstFineRegion;
			payload.fineRegionCount = fineRegionCount;
			payload.primitiveCount = static_cast<std::uint32_t>(clusterIndices.size() / 3u);
			payload.targetRatio = sourceCoverageIndices.empty()
				? 1.0f
				: static_cast<float>(clusterIndices.size()) / static_cast<float>(sourceCoverageIndices.size());
			const float ratioSpacingFloor = ComputeVirtualGeometrySelectionErrorFloor(lod.targetRatio);
			// Different simplification requests can produce the same topology
			// and error estimate. Give every coarser emitted detail a meaningfully
			// larger screen-error threshold so several LODs cannot switch over a
			// few centimeters of camera travel. Cap the synthetic spacing input
			// at the renderer's conservative visual-error tolerance; the actual
			// simplifier error remains authoritative when it is larger.
			const float previousDetailSpacingFloor = lodIndex == 0u
				? 0.0f
				: std::min(previousDetailGeometricError * 1.5f, 0.05f);
			payload.geometricError = std::max({
				simplificationError,
				ratioSpacingFloor,
				previousDetailSpacingFloor
			});
			if (!BuildMeshletPayload(vertices, clusterIndices, includeBounds, payload.meshlets))
			{
				std::cerr << "Virtual geometry coverage cluster meshlet generation failed.\n";
				return false;
			}

			if (payload.meshlets.meshletRecords.empty())
			{
				std::cerr << "Virtual geometry coverage cluster generated no meshlets.\n";
				return false;
			}

			totalPrimitiveCount += payload.primitiveCount;
			totalMeshletCount += static_cast<std::uint32_t>(payload.meshlets.meshletRecords.size());
			maxClusterError = std::max(maxClusterError, payload.geometricError);
			clusterPayloads.push_back(std::move(payload));
		}

		outCandidatePrimitiveCount = totalPrimitiveCount;
		outCandidateMeshletCount = totalMeshletCount;
		if (lodIndex > 0u && !output.detailLevels.empty())
		{
			const VirtualGeometryDetailLevelRecord& previousDetail = output.detailLevels.back();
			// A zero percentage threshold means every real reduction is valid;
			// it does not make an identical regional result a useful LOD. Merge
			// candidates that reduce neither triangles nor meshlets so the
			// Inspector and runtime cannot expose duplicate transition colors.
			if (totalPrimitiveCount >= previousDetail.primitiveCount
				&& totalMeshletCount >= previousDetail.meshletCount)
			{
				return true;
			}
		}

		std::uint32_t meshletOffset = 0u;
		std::uint32_t primitiveOffset = 0u;
		std::uint32_t groupOffset = 0u;
		if (!TryConvertCount(output.meshletPayload.meshletRecords.size(), "virtual geometry meshlet offset", meshletOffset)
			|| !TryConvertCount(output.meshletPayload.primitiveIndices.size(), "virtual geometry primitive offset", primitiveOffset)
			|| !TryConvertCount(output.cullableGroups.size(), "virtual geometry group offset", groupOffset))
		{
			return false;
		}

		const std::uint32_t sectionRangeIndex = static_cast<std::uint32_t>(output.sectionRanges.size());
		const std::uint32_t pageRangeOffset = static_cast<std::uint32_t>(output.pages.size());

		MeshletSection meshletSection{};
		meshletSection.meshletOffset = meshletOffset;
		meshletSection.meshletCount = totalMeshletCount;
		output.meshletPayload.meshletSections.push_back(meshletSection);

		VirtualGeometrySectionRangeRecord sectionRange{};
		sectionRange.lodIndex = lodIndex;
		sectionRange.sectionId = sectionId;
		sectionRange.meshletOffset = meshletOffset;
		sectionRange.meshletCount = totalMeshletCount;
		sectionRange.primitiveOffset = primitiveOffset;
		sectionRange.primitiveCount = totalPrimitiveCount;
		output.sectionRanges.push_back(sectionRange);

		VirtualGeometryDetailLevelRecord detail{};
		detail.lodIndex = lodIndex;
		detail.requestedTargetIndex = lod.requestedTargetIndex;
		detail.targetStatus = static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Generated);
		detail.meshletOffset = meshletOffset;
		detail.meshletCount = totalMeshletCount;
		detail.vertexCount = static_cast<std::uint32_t>(vertices.size());
		detail.primitiveCount = totalPrimitiveCount;
		detail.sectionRangeOffset = sectionRangeIndex;
		detail.sectionRangeCount = 1u;
		detail.pageRangeOffset = pageRangeOffset;
		detail.pageRangeCount = static_cast<std::uint32_t>(clusterPayloads.size());
		detail.geometricError = maxClusterError;
		detail.targetRatio = lod.targetRatio;
		BuildPositionBounds(vertices, detail.boundsMin, detail.boundsMax);
		output.detailLevels.push_back(detail);

		for (VirtualGeometryClusterCookPayload& payload : clusterPayloads)
		{
			// Each spatial coverage cluster owns one independently addressable page.
			// Keeping page boundaries aligned with hierarchy nodes lets streaming
			// select a compatible parent without loading an unrelated whole LOD.
			const std::uint32_t clusterPageId =
				static_cast<std::uint32_t>(output.pages.size());
			const std::uint32_t clusterLinkOffset =
				static_cast<std::uint32_t>(output.clusterPageLinks.size());
			std::uint32_t clusterMeshletOffset = 0u;
			std::uint32_t clusterRemapOffset = 0u;
			std::uint32_t clusterPrimitiveOffset = 0u;
			std::uint32_t clusterGroupOffset = 0u;
			if (!TryConvertCount(output.meshletPayload.meshletRecords.size(), "virtual geometry cluster meshlet offset", clusterMeshletOffset)
				|| !TryConvertCount(output.meshletPayload.vertexRemapIndices.size(), "virtual geometry cluster remap offset", clusterRemapOffset)
				|| !TryConvertCount(output.meshletPayload.primitiveIndices.size(), "virtual geometry cluster primitive offset", clusterPrimitiveOffset)
				|| !TryConvertCount(output.cullableGroups.size(), "virtual geometry cluster group offset", clusterGroupOffset))
			{
				return false;
			}

			output.meshletPayload.vertexRemapIndices.insert(
				output.meshletPayload.vertexRemapIndices.end(),
				payload.meshlets.vertexRemapIndices.begin(),
				payload.meshlets.vertexRemapIndices.end());
			output.meshletPayload.primitiveIndices.insert(
				output.meshletPayload.primitiveIndices.end(),
				payload.meshlets.primitiveIndices.begin(),
				payload.meshlets.primitiveIndices.end());

			for (MeshletRecord record : payload.meshlets.meshletRecords)
			{
				record.vertexOffset += clusterRemapOffset;
				record.primitiveIndexOffset += clusterPrimitiveOffset;
				output.meshletPayload.meshletRecords.push_back(record);
			}

			const std::uint32_t boundsOffset = static_cast<std::uint32_t>(output.meshletPayload.meshletBounds.size());
			output.meshletPayload.meshletBounds.insert(
				output.meshletPayload.meshletBounds.end(),
				payload.meshlets.meshletBounds.begin(),
				payload.meshlets.meshletBounds.end());

			for (std::size_t localMeshlet = 0u; localMeshlet < payload.meshlets.meshletRecords.size(); ++localMeshlet)
			{
				const std::size_t globalMeshlet = static_cast<std::size_t>(clusterMeshletOffset) + localMeshlet;
				const MeshletRecord& record = output.meshletPayload.meshletRecords[globalMeshlet];
				VirtualGeometryCullableGroupRecord group{};
				group.groupIndex = static_cast<std::uint32_t>(output.cullableGroups.size());
				group.lodIndex = lodIndex;
				group.vertexOffset = record.vertexOffset;
				group.vertexCount = record.vertexCount;
				group.primitiveOffset = record.primitiveIndexOffset;
				group.primitiveCount = record.primitiveIndexCount / 3u;
				group.sectionId = sectionId;
				group.pageId = clusterPageId;

				const std::size_t localBoundsIndex = static_cast<std::size_t>(boundsOffset) + localMeshlet;
				if (localBoundsIndex < output.meshletPayload.meshletBounds.size())
				{
					const MeshletBounds& bounds = output.meshletPayload.meshletBounds[localBoundsIndex];
					group.boundsCenter[0] = bounds.center[0];
					group.boundsCenter[1] = bounds.center[1];
					group.boundsCenter[2] = bounds.center[2];
					group.boundsRadius = bounds.radius;
					group.coneApex[0] = bounds.coneApex[0];
					group.coneApex[1] = bounds.coneApex[1];
					group.coneApex[2] = bounds.coneApex[2];
					group.coneCutoff = bounds.coneCutoff;
					group.coneAxis[0] = bounds.coneAxis[0];
					group.coneAxis[1] = bounds.coneAxis[1];
					group.coneAxis[2] = bounds.coneAxis[2];
					group.coneApexOffset = bounds.coneApexOffset;
				}
				else
				{
					group.boundsRadius = 1.0f;
					group.coneCutoff = 2.0f;
				}

				output.cullableGroups.push_back(group);
			}

			VirtualGeometryClusterRecord cluster{};
			cluster.clusterId = static_cast<std::uint32_t>(output.clusters.size());
			cluster.parentClusterId = kVirtualGeometryInvalidIndex;
			cluster.firstChildClusterId = kVirtualGeometryInvalidIndex;
			cluster.childCount = 0u;
			cluster.lodIndex = lodIndex;
			cluster.groupOffset = clusterGroupOffset;
			cluster.groupCount = static_cast<std::uint32_t>(output.cullableGroups.size()) - clusterGroupOffset;
			cluster.materialSectionId = sectionId;
			cluster.pageId = clusterPageId;
			cluster.fallbackClusterId = cluster.clusterId;
			cluster.geometricError = payload.geometricError;
			ComputeVirtualGeometryClusterBounds(output.cullableGroups, cluster.groupOffset, cluster.groupCount, cluster.boundsCenter, cluster.boundsRadius);
			output.clusters.push_back(cluster);
			output.clusterCoverage.push_back({ payload.firstFineRegion, payload.fineRegionCount });

			VirtualGeometryClusterPageLinkRecord link{};
			link.clusterId = cluster.clusterId;
			link.pageId = clusterPageId;
			link.firstGroup = cluster.groupOffset;
			link.groupCount = cluster.groupCount;
			link.flags = lodIndex == 0u ? 0x3u : 0x1u;
			output.clusterPageLinks.push_back(link);

			VirtualGeometryPageRecord page{};
			page.pageId = clusterPageId;
			page.firstLodIndex = lodIndex;
			page.lodCount = 1u;
			page.groupOffset = clusterGroupOffset;
			page.groupCount = cluster.groupCount;
			// Hierarchy linking resolves this temporary self-reference to the
			// matching coarser spatial ancestor after all LODs are available.
			page.fallbackPageId = page.pageId;
			page.clusterPageLinkOffset = clusterLinkOffset;
			page.clusterPageLinkCount = 1u;
			page.priority = lodIndex == 0u
				? 1000u
				: std::max<std::uint32_t>(1u, 1000u - (lodIndex * 100u));
			page.byteOffset =
				static_cast<std::uint64_t>(clusterMeshletOffset)
				* sizeof(MeshletRecord);
			page.byteSize =
				static_cast<std::uint64_t>(payload.meshlets.meshletRecords.size())
					* (sizeof(MeshletRecord) + sizeof(MeshletBounds))
				+ static_cast<std::uint64_t>(payload.meshlets.primitiveIndices.size())
					* sizeof(std::uint8_t)
				+ static_cast<std::uint64_t>(payload.meshlets.vertexRemapIndices.size())
					* sizeof(std::uint32_t)
				+ static_cast<std::uint64_t>(payload.meshlets.vertexRemapIndices.size())
					* kMeshletPackedVertexStrideBytes;
			output.pages.push_back(page);
		}
		outDetailEmitted = true;
		return true;
	}

	bool LinkVirtualGeometryClusterHierarchy(
		std::vector<VirtualGeometryClusterRecord>& clusters,
		const std::vector<VirtualGeometryClusterCoverage>& clusterCoverage)
	{
		if (clusters.empty() || clusters.size() != clusterCoverage.size())
		{
			return false;
		}

		for (std::size_t index = 0u; index < clusters.size(); ++index)
		{
			VirtualGeometryClusterRecord& cluster = clusters[index];
			cluster.clusterId = static_cast<std::uint32_t>(index);
			cluster.parentClusterId = kVirtualGeometryInvalidIndex;
			cluster.firstChildClusterId = kVirtualGeometryInvalidIndex;
			cluster.childCount = 0u;
			cluster.fallbackClusterId = cluster.clusterId;
		}

		std::uint32_t maxLodIndex = 0u;
		for (const VirtualGeometryClusterRecord& cluster : clusters)
		{
			maxLodIndex = std::max(maxLodIndex, cluster.lodIndex);
		}

		const auto collectLodClusters = [&clusters](std::uint32_t lodIndex) {
			std::vector<std::uint32_t> indices;
			for (std::uint32_t index = 0u; index < static_cast<std::uint32_t>(clusters.size()); ++index)
			{
				if (clusters[index].lodIndex == lodIndex)
				{
					indices.push_back(index);
				}
			}
			return indices;
		};

		const auto sortedLodClusters = [&clusters, &clusterCoverage, &collectLodClusters](std::uint32_t lodIndex) {
			std::vector<std::uint32_t> indices = collectLodClusters(lodIndex);
			std::sort(indices.begin(), indices.end(), [&clusters, &clusterCoverage](std::uint32_t left, std::uint32_t right) {
				if (clusterCoverage[left].firstFineRegion != clusterCoverage[right].firstFineRegion)
				{
					return clusterCoverage[left].firstFineRegion < clusterCoverage[right].firstFineRegion;
				}
				if (clusterCoverage[left].fineRegionCount != clusterCoverage[right].fineRegionCount)
				{
					return clusterCoverage[left].fineRegionCount < clusterCoverage[right].fineRegionCount;
				}
				return clusters[left].clusterId < clusters[right].clusterId;
			});
			return indices;
		};

		for (std::uint32_t parentLod = 1u; parentLod <= maxLodIndex; ++parentLod)
		{
			const std::vector<std::uint32_t> parentIndices = sortedLodClusters(parentLod);
			const std::vector<std::uint32_t> childIndices = sortedLodClusters(parentLod - 1u);
			if (parentIndices.empty() || childIndices.empty())
			{
				continue;
			}

			for (std::uint32_t parentIndex : parentIndices)
			{
				const VirtualGeometryClusterCoverage& parentCoverage = clusterCoverage[parentIndex];
				const std::uint32_t parentBegin = parentCoverage.firstFineRegion;
				const std::uint32_t parentEnd = parentCoverage.firstFineRegion + parentCoverage.fineRegionCount;
				std::uint32_t firstChildId = kVirtualGeometryInvalidIndex;
				std::uint32_t lastChildId = 0u;
				std::uint32_t coveredBegin = parentBegin;
				bool hasChild = false;
				bool coverageValid = true;

				for (std::uint32_t childIndex : childIndices)
				{
					const VirtualGeometryClusterCoverage& childCoverage = clusterCoverage[childIndex];
					const std::uint32_t childBegin = childCoverage.firstFineRegion;
					const std::uint32_t childEnd = childCoverage.firstFineRegion + childCoverage.fineRegionCount;
					if (childEnd <= parentBegin)
					{
						continue;
					}
					if (childBegin >= parentEnd)
					{
						break;
					}
					if (childBegin != coveredBegin || childEnd > parentEnd)
					{
						coverageValid = false;
						break;
					}

					VirtualGeometryClusterRecord& child = clusters[childIndex];
					VirtualGeometryClusterRecord& parent = clusters[parentIndex];
					child.parentClusterId = parent.clusterId;
					child.fallbackClusterId = parent.clusterId;
					firstChildId = std::min(firstChildId, child.clusterId);
					lastChildId = std::max(lastChildId, child.clusterId);
					coveredBegin = childEnd;
					hasChild = true;
				}

				if (!hasChild || !coverageValid || coveredBegin != parentEnd)
				{
					return false;
				}

				VirtualGeometryClusterRecord& parent = clusters[parentIndex];
				parent.firstChildClusterId = firstChildId;
				parent.childCount = (lastChildId - firstChildId) + 1u;
			}
		}
		return true;
	}

	bool ValidateVirtualGeometryClusterCoverage(
		const std::vector<VirtualGeometryClusterRecord>& clusters,
		const std::vector<VirtualGeometryClusterCoverage>& clusterCoverage)
	{
		if (clusters.empty() || clusters.size() != clusterCoverage.size())
		{
			return false;
		}

		std::uint32_t maxLodIndex = 0u;
		std::uint32_t fineRegionCount = 0u;
		for (std::size_t index = 0u; index < clusters.size(); ++index)
		{
			maxLodIndex = std::max(maxLodIndex, clusters[index].lodIndex);
			fineRegionCount = std::max(fineRegionCount, clusterCoverage[index].firstFineRegion + clusterCoverage[index].fineRegionCount);
			if (clusterCoverage[index].fineRegionCount == 0u)
			{
				return false;
			}
		}

		for (std::uint32_t lodIndex = 0u; lodIndex <= maxLodIndex; ++lodIndex)
		{
			std::vector<VirtualGeometryClusterCoverage> lodCoverage;
			for (std::size_t clusterIndex = 0u; clusterIndex < clusters.size(); ++clusterIndex)
			{
				if (clusters[clusterIndex].lodIndex == lodIndex)
				{
					lodCoverage.push_back(clusterCoverage[clusterIndex]);
				}
			}
			if (lodCoverage.empty())
			{
				return false;
			}
			std::sort(lodCoverage.begin(), lodCoverage.end(), [](const VirtualGeometryClusterCoverage& left, const VirtualGeometryClusterCoverage& right) {
				return left.firstFineRegion < right.firstFineRegion;
			});

			std::uint32_t cursor = 0u;
			for (const VirtualGeometryClusterCoverage& coverage : lodCoverage)
			{
				if (coverage.firstFineRegion != cursor)
				{
					return false;
				}
				cursor += coverage.fineRegionCount;
			}
			if (cursor != fineRegionCount)
			{
				return false;
			}
		}

		for (const VirtualGeometryClusterRecord& parent : clusters)
		{
			if (parent.childCount == 0u)
			{
				continue;
			}
			if (parent.firstChildClusterId >= clusters.size()
				|| (static_cast<std::uint64_t>(parent.firstChildClusterId) + parent.childCount) > clusters.size())
			{
				return false;
			}

			const VirtualGeometryClusterCoverage& parentCoverage = clusterCoverage[parent.clusterId];
			std::uint32_t cursor = parentCoverage.firstFineRegion;
			const std::uint32_t parentEnd = parentCoverage.firstFineRegion + parentCoverage.fineRegionCount;
			for (std::uint32_t childOffset = 0u; childOffset < parent.childCount; ++childOffset)
			{
				const VirtualGeometryClusterRecord& child = clusters[parent.firstChildClusterId + childOffset];
				const VirtualGeometryClusterCoverage& childCoverage = clusterCoverage[child.clusterId];
				if (child.parentClusterId != parent.clusterId
					|| childCoverage.firstFineRegion != cursor
					|| (childCoverage.firstFineRegion + childCoverage.fineRegionCount) > parentEnd)
				{
					return false;
				}
				cursor += childCoverage.fineRegionCount;
			}
			if (cursor != parentEnd)
			{
				return false;
			}
		}

		return true;
	}

	void SynchronizeGeneratedLevelReportWithEmittedDetails(
		MeshCompileReport& report,
		const std::vector<VirtualGeometryDetailLevelRecord>& emittedDetails)
	{
		for (const VirtualGeometryDetailLevelRecord& detail : emittedDetails)
		{
			auto generated = std::find_if(
				report.virtualGeometry.generatedLevels.begin(),
				report.virtualGeometry.generatedLevels.end(),
				[&detail](const VirtualGeometryGeneratedLevelCompileReport& candidate) {
					// Regional cooking can merge an earlier candidate and compact
					// every later emitted LOD index. The requested target remains
					// stable across both stages and is therefore the safe identity.
					return candidate.requestedTargetIndex
						== detail.requestedTargetIndex;
				});
			if (generated == report.virtualGeometry.generatedLevels.end())
			{
				continue;
			}

			// The simplification stage estimates a candidate LOD before the
			// coverage-safe regional cook splits and rebuilds meshlets. The
			// emitted detail record is the renderer's source of truth, so keep
			// Inspector/package diagnostics aligned with the final TEMF payload.
			generated->generatedLevelIndex = detail.lodIndex;
			generated->lodIndex = detail.lodIndex;
			generated->requestedTargetIndex = detail.requestedTargetIndex;
			generated->actualRatio = report.virtualGeometry.sourceTriangleCount > 0u
				? static_cast<float>(detail.primitiveCount) / static_cast<float>(report.virtualGeometry.sourceTriangleCount)
				: detail.targetRatio;
			generated->triangleCount = detail.primitiveCount;
			generated->meshletCount = detail.meshletCount;
			generated->geometricError = detail.geometricError;
		}

		for(VirtualGeometrySkippedLevelCompileReport& skipped
			: report.virtualGeometry.skippedLevels) {
			if(emittedDetails.empty()) {
				continue;
			}
			std::size_t nearestDetailIndex = 0U;
			for(std::size_t detailIndex = 0U;
				detailIndex < emittedDetails.size(); ++detailIndex) {
				const VirtualGeometryDetailLevelRecord& detail =
					emittedDetails[detailIndex];
				if(detail.requestedTargetIndex > skipped.targetIndex) {
					break;
				}
				nearestDetailIndex = detailIndex;
			}
			// The simplification stage records fallback indices before regional
			// meshlet cooking. If that later stage merges a duplicate candidate,
			// remap every skipped target to the compact emitted table so the TEMF
			// loader never observes an index for a detail that was removed.
			skipped.nearestGeneratedLevelIndex =
				static_cast<std::uint32_t>(nearestDetailIndex);
		}
	}

	bool BuildVirtualGeometryPayload(
		const std::vector<VertexRaw>& vertices,
		const std::vector<VirtualGeometryLodSource>& lods,
		bool includeBounds,
		const VirtualGeometryConeSafetyCookSettings& coneSafetySettings,
		MeshCompileReport& report,
		VirtualGeometryBuildOutput& output)
	{
		output = {};
		const std::uint32_t sectionId = 0u;
		const std::uint32_t sourcePrimitiveCount = lods.empty() ? 0u : static_cast<std::uint32_t>(lods.front().indices.size() / 3u);
		const std::uint32_t sourceMeshletCount = lods.empty() ? 0u : EstimateMeshletCountForLod(vertices, lods.front().indices, 1u);
		const std::uint32_t fineRegionCount = ComputeVirtualGeometryFineRegionCount(sourcePrimitiveCount, sourceMeshletCount);
		const std::vector<VirtualGeometrySourceRegion> fineRegions = BuildVirtualGeometrySourceRegions(
			vertices,
			lods.empty() ? std::vector<std::uint32_t>{} : lods.front().indices,
			fineRegionCount);
		if (fineRegions.empty())
		{
			report.virtualGeometry.status = VirtualGeometryCompileStatus::Failed;
			report.virtualGeometry.errors.push_back("virtual geometry source coverage partition failed");
			return false;
		}

		for (std::size_t lodIndex = 0; lodIndex < lods.size(); ++lodIndex)
		{
			const float previousDetailGeometricError = output.detailLevels.empty()
				? 0.0f
				: output.detailLevels.back().geometricError;
			bool detailEmitted = false;
			std::uint32_t candidatePrimitiveCount = 0u;
			std::uint32_t candidateMeshletCount = 0u;
			if (!AppendRegionalLodMeshlets(
				fineRegions,
				static_cast<std::uint32_t>(output.detailLevels.size()),
				sectionId,
				lods[lodIndex],
				vertices,
				includeBounds,
				previousDetailGeometricError,
				detailEmitted,
				candidatePrimitiveCount,
				candidateMeshletCount,
				output))
			{
				report.virtualGeometry.status = VirtualGeometryCompileStatus::Failed;
				report.virtualGeometry.errors.push_back("virtual geometry regional meshlet generation failed for LOD " + std::to_string(lodIndex));
				return false;
			}
			if (!detailEmitted)
			{
				const std::uint32_t requestedTargetIndex = lods[lodIndex].requestedTargetIndex;
				auto requestedTarget = std::find_if(
					report.virtualGeometry.requestedTargets.begin(),
					report.virtualGeometry.requestedTargets.end(),
					[requestedTargetIndex](const VirtualGeometryRequestedTargetCompileReport& target) {
						return target.targetIndex == requestedTargetIndex;
					});
				if (requestedTarget != report.virtualGeometry.requestedTargets.end())
				{
					requestedTarget->status = ToCompileTargetStatusText(VirtualGeometryDetailTargetRecordStatus::Merged);
				}
				report.virtualGeometry.generatedLevels.erase(
					std::remove_if(
						report.virtualGeometry.generatedLevels.begin(),
						report.virtualGeometry.generatedLevels.end(),
						[requestedTargetIndex](const VirtualGeometryGeneratedLevelCompileReport& generated) {
							return generated.requestedTargetIndex == requestedTargetIndex;
						}),
					report.virtualGeometry.generatedLevels.end());
				const std::uint32_t nearestGeneratedLevelIndex = output.detailLevels.empty()
					? 0u
					: static_cast<std::uint32_t>(output.detailLevels.size() - 1u);
				const float actualCandidateRatio = sourcePrimitiveCount > 0u
					? static_cast<float>(candidatePrimitiveCount) / static_cast<float>(sourcePrimitiveCount)
					: 1.0f;
				AppendSkippedTargetReport(
					report,
					requestedTargetIndex,
					lods[lodIndex].targetRatio,
					VirtualGeometryDetailTargetRecordStatus::Merged,
					VirtualGeometrySkippedDetailRecordReason::DuplicateOfPreviousLevel,
					nearestGeneratedLevelIndex,
					"regional virtual-geometry cook produced no triangle or meshlet reduction; target was merged into the previous detail",
					candidatePrimitiveCount,
					candidateMeshletCount,
					actualCandidateRatio);
			}
		}

		const bool hierarchyLinked = LinkVirtualGeometryClusterHierarchy(output.clusters, output.clusterCoverage);
		const bool coverageValidated = hierarchyLinked && ValidateVirtualGeometryClusterCoverage(output.clusters, output.clusterCoverage);
		if (hierarchyLinked)
		{
			// A physical page follows the same spatial parent chain as its cluster.
			// Coarsest roots remain self-fallback and are pinned by the runtime;
			// every finer page names the compatible ancestor that covers the same
			// source region, avoiding a whole-owner fallback dependency.
			for (const VirtualGeometryClusterRecord& cluster : output.clusters)
			{
				if (cluster.pageId >= output.pages.size()
					|| cluster.fallbackClusterId >= output.clusters.size())
				{
					report.virtualGeometry.status =
						VirtualGeometryCompileStatus::Failed;
					report.virtualGeometry.errors.push_back(
						"virtual geometry cluster fallback page link is out of range");
					return false;
				}
				output.pages[cluster.pageId].fallbackPageId =
					output.clusters[cluster.fallbackClusterId].pageId;
			}
		}
		if (!coverageValidated)
		{
			report.virtualGeometry.warnings.push_back("virtual geometry cluster coverage validation failed; renderer will use conservative complete-detail fallback");
		}
		SynchronizeGeneratedLevelReportWithEmittedDetails(report, output.detailLevels);

		output.coneSafety.reserve(output.clusters.size());
		for (const VirtualGeometryClusterRecord& cluster : output.clusters)
		{
			output.coneSafety.push_back(BuildVirtualGeometryConeSafetyRecord(output, cluster, coneSafetySettings));
		}

		const std::uint32_t sectionNameOffset = AppendVirtualGeometryString(output, "DefaultSection");
		VirtualGeometryMaterialSectionRecord section{};
		section.sectionId = sectionId;
		section.materialSlot = 0u;
		section.sourceTriangleCount = sourcePrimitiveCount;
		section.perLodRangeOffset = 0u;
		section.perLodRangeCount = static_cast<std::uint32_t>(output.sectionRanges.size());
		section.sourceNameStringOffset = sectionNameOffset;
		section.sourceNameStringSize = static_cast<std::uint32_t>(std::string_view("DefaultSection").size());
		output.materialSections.push_back(section);

		for (const VirtualGeometryRequestedTargetCompileReport& target : report.virtualGeometry.requestedTargets)
		{
			VirtualGeometryRequestedTargetRecord record{};
			record.targetIndex = target.targetIndex;
			record.status = ParseCompileTargetStatus(target.status);
			record.desiredTriangleCount = target.desiredTriangleCount;
			record.desiredMeshletCount = target.desiredMeshletCount;
			record.requestedRatio = target.requestedRatio;
			output.requestedTargets.push_back(record);
		}

		for (const VirtualGeometrySkippedLevelCompileReport& skipped : report.virtualGeometry.skippedLevels)
		{
			const std::uint32_t messageOffset = skipped.message.empty()
				? 0u
				: AppendVirtualGeometryString(output, skipped.message);
			VirtualGeometrySkippedDetailRecord record{};
			record.targetIndex = skipped.targetIndex;
			record.status = ParseCompileTargetStatus(skipped.status);
			record.reason = ParseCompileSkippedReason(skipped.reason);
			record.nearestGeneratedLodIndex = skipped.nearestGeneratedLevelIndex;
			record.messageStringOffset = messageOffset;
			record.messageStringSize = static_cast<std::uint32_t>(skipped.message.size());
			record.candidateTriangleCount = skipped.candidateTriangleCount;
			record.candidateMeshletCount = skipped.candidateMeshletCount;
			record.requestedRatio = skipped.requestedRatio;
			record.actualCandidateRatio = skipped.actualCandidateRatio;
			record.actualNearestRatio = skipped.nearestGeneratedLevelIndex < output.detailLevels.size()
				? output.detailLevels[skipped.nearestGeneratedLevelIndex].targetRatio
				: 1.0f;
			output.skippedDetails.push_back(record);
		}

		for (const std::string& warning : report.virtualGeometry.warnings)
		{
			const std::uint32_t codeOffset = AppendVirtualGeometryString(output, "VirtualGeometryCookWarning");
			const std::uint32_t messageOffset = AppendVirtualGeometryString(output, warning);
			VirtualGeometryDiagnosticRecord diagnostic{};
			diagnostic.severity = 1u;
			diagnostic.codeStringOffset = codeOffset;
			diagnostic.codeStringSize = static_cast<std::uint32_t>(std::string_view("VirtualGeometryCookWarning").size());
			diagnostic.messageStringOffset = messageOffset;
			diagnostic.messageStringSize = static_cast<std::uint32_t>(warning.size());
			output.diagnostics.push_back(diagnostic);
		}

		const std::string_view validationMessage = coverageValidated
			? "fallback coverage, cluster hierarchy, cluster coverage, and page links validated"
			: "fallback coverage, cluster hierarchy, and page links validated";
		const std::uint32_t validationMessageOffset = AppendVirtualGeometryString(output, validationMessage);
		VirtualGeometryValidationFlagRecord validation{};
		validation.flags = kVirtualGeometryValidationFlagAdaptiveTargetsPresent
			| kVirtualGeometryValidationFlagFallbackCoverageValidated
			| kVirtualGeometryValidationFlagClusterHierarchyValidated
			| kVirtualGeometryValidationFlagPageLinksValidated
			| kVirtualGeometryValidationFlagConeSafetyMetadataValidated;
		if (coverageValidated)
		{
			validation.flags |= kVirtualGeometryValidationFlagClusterCoverageValidated;
		}
		validation.messageStringOffset = validationMessageOffset;
		validation.messageStringSize = static_cast<std::uint32_t>(validationMessage.size());
		output.validationFlags.push_back(validation);

		report.virtualGeometry.detailLevelCount = static_cast<std::uint32_t>(output.detailLevels.size());
		report.virtualGeometry.cullableGroupCount = static_cast<std::uint32_t>(output.cullableGroups.size());
		report.virtualGeometry.pageCount = static_cast<std::uint32_t>(output.pages.size());
		report.virtualGeometry.coneSafetyRecordCount = static_cast<std::uint32_t>(output.coneSafety.size());
		report.virtualGeometry.coneEligibleClusterCount = static_cast<std::uint32_t>(std::count_if(
			output.coneSafety.begin(),
			output.coneSafety.end(),
			[](const VirtualGeometryConeSafetyRecord& record) {
				return (record.flags & kVirtualGeometryConeSafetyFlagEligible) != 0u;
			}));
		report.virtualGeometry.clusters.clear();
		for (const VirtualGeometryClusterRecord& cluster : output.clusters)
		{
			report.virtualGeometry.clusters.push_back({
				cluster.clusterId,
				cluster.parentClusterId,
				cluster.lodIndex,
				cluster.groupCount,
				cluster.materialSectionId,
				cluster.pageId,
				cluster.fallbackClusterId
			});
		}
		report.virtualGeometry.pages.clear();
		for (const VirtualGeometryPageRecord& page : output.pages)
		{
			report.virtualGeometry.pages.push_back({
				page.pageId,
				page.firstLodIndex,
				page.lodCount,
				page.fallbackPageId,
				page.byteSize,
				page.pageId == page.fallbackPageId
			});
		}
		return true;
	}

	bool TryComputeVirtualGeometryPayloadBytes(const VirtualGeometryBuildOutput& output, std::uint32_t& outBytes)
	{
		std::uint32_t cursor = static_cast<std::uint32_t>(sizeof(VirtualGeometryHeader));
		const auto AddTable = [&](std::size_t count, std::size_t stride, const char* label) -> bool {
			cursor = AlignUp(cursor, 4u);
			std::size_t bytes = 0u;
			if (count != 0u && stride > (std::numeric_limits<std::size_t>::max() / count))
			{
				std::cerr << "Virtual geometry table size overflow for " << label << ".\n";
				return false;
			}
			bytes = count * stride;
			if (bytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - cursor))
			{
				std::cerr << "Virtual geometry payload size overflow for " << label << ".\n";
				return false;
			}
			cursor += static_cast<std::uint32_t>(bytes);
			return true;
		};

		if (!AddTable(output.detailLevels.size(), sizeof(VirtualGeometryDetailLevelRecord), "detail levels")
			|| !AddTable(output.materialSections.size(), sizeof(VirtualGeometryMaterialSectionRecord), "material sections")
			|| !AddTable(output.sectionRanges.size(), sizeof(VirtualGeometrySectionRangeRecord), "section ranges")
			|| !AddTable(output.cullableGroups.size(), sizeof(VirtualGeometryCullableGroupRecord), "cullable groups")
			|| !AddTable(output.pages.size(), sizeof(VirtualGeometryPageRecord), "pages")
			|| !AddTable(output.requestedTargets.size(), sizeof(VirtualGeometryRequestedTargetRecord), "requested targets")
			|| !AddTable(output.skippedDetails.size(), sizeof(VirtualGeometrySkippedDetailRecord), "skipped details")
			|| !AddTable(output.clusters.size(), sizeof(VirtualGeometryClusterRecord), "clusters")
			|| !AddTable(output.clusterPageLinks.size(), sizeof(VirtualGeometryClusterPageLinkRecord), "cluster page links")
			|| !AddTable(output.coneSafety.size(), sizeof(VirtualGeometryConeSafetyRecord), "cone safety")
			|| !AddTable(output.validationFlags.size(), sizeof(VirtualGeometryValidationFlagRecord), "validation flags")
			|| !AddTable(output.diagnostics.size(), sizeof(VirtualGeometryDiagnosticRecord), "diagnostics")
			|| !AddTable(output.stringTable.size(), sizeof(char), "string table"))
		{
			return false;
		}

		outBytes = cursor;
		return true;
	}

	void PopulateVirtualGeometryHeader(VirtualGeometryBuildOutput& output, std::uint32_t rootOffset)
	{
		VirtualGeometryHeader header{};
		header.payloadGeneration = kVirtualGeometryPayloadGeneration;
		header.headerSize = static_cast<std::uint32_t>(sizeof(VirtualGeometryHeader));
		header.detailLevelCount = static_cast<std::uint32_t>(output.detailLevels.size());
		header.materialSectionCount = static_cast<std::uint32_t>(output.materialSections.size());
		header.sectionRangeCount = static_cast<std::uint32_t>(output.sectionRanges.size());
		header.cullableGroupCount = static_cast<std::uint32_t>(output.cullableGroups.size());
		header.pageCount = static_cast<std::uint32_t>(output.pages.size());
		header.diagnosticRecordCount = static_cast<std::uint32_t>(output.diagnostics.size());
		header.requestedTargetCount = static_cast<std::uint32_t>(output.requestedTargets.size());
		header.skippedDetailCount = static_cast<std::uint32_t>(output.skippedDetails.size());
		header.clusterCount = static_cast<std::uint32_t>(output.clusters.size());
		header.clusterPageLinkCount = static_cast<std::uint32_t>(output.clusterPageLinks.size());
		header.validationFlagCount = static_cast<std::uint32_t>(output.validationFlags.size());
		header.coneSafetyCount = static_cast<std::uint32_t>(output.coneSafety.size());
		header.sourceTriangleCount = output.detailLevels.empty() ? 0u : output.detailLevels.front().primitiveCount;
		header.sourceVertexCount = output.detailLevels.empty() ? 0u : output.detailLevels.front().vertexCount;
		header.flags = output.validationFlags.empty()
			? (kVirtualGeometryValidationFlagAdaptiveTargetsPresent
				| kVirtualGeometryValidationFlagFallbackCoverageValidated
				| kVirtualGeometryValidationFlagClusterHierarchyValidated
				| kVirtualGeometryValidationFlagPageLinksValidated
				| kVirtualGeometryValidationFlagConeSafetyMetadataValidated)
			: output.validationFlags.front().flags;

		std::uint32_t cursor = rootOffset + static_cast<std::uint32_t>(sizeof(VirtualGeometryHeader));
		const auto AssignTable = [&](std::uint32_t& offset, std::size_t count, std::size_t stride) {
			cursor = AlignUp(cursor, 4u);
			offset = count == 0u ? 0u : cursor;
			cursor += static_cast<std::uint32_t>(count * stride);
		};

		AssignTable(header.detailLevelTableOffset, output.detailLevels.size(), sizeof(VirtualGeometryDetailLevelRecord));
		AssignTable(header.materialSectionTableOffset, output.materialSections.size(), sizeof(VirtualGeometryMaterialSectionRecord));
		AssignTable(header.sectionRangeTableOffset, output.sectionRanges.size(), sizeof(VirtualGeometrySectionRangeRecord));
		AssignTable(header.cullableGroupTableOffset, output.cullableGroups.size(), sizeof(VirtualGeometryCullableGroupRecord));
		AssignTable(header.pageTableOffset, output.pages.size(), sizeof(VirtualGeometryPageRecord));
		AssignTable(header.requestedTargetTableOffset, output.requestedTargets.size(), sizeof(VirtualGeometryRequestedTargetRecord));
		AssignTable(header.skippedDetailTableOffset, output.skippedDetails.size(), sizeof(VirtualGeometrySkippedDetailRecord));
		AssignTable(header.clusterTableOffset, output.clusters.size(), sizeof(VirtualGeometryClusterRecord));
		AssignTable(header.clusterPageLinkTableOffset, output.clusterPageLinks.size(), sizeof(VirtualGeometryClusterPageLinkRecord));
		AssignTable(header.coneSafetyTableOffset, output.coneSafety.size(), sizeof(VirtualGeometryConeSafetyRecord));
		AssignTable(header.validationFlagTableOffset, output.validationFlags.size(), sizeof(VirtualGeometryValidationFlagRecord));
		AssignTable(header.diagnosticTableOffset, output.diagnostics.size(), sizeof(VirtualGeometryDiagnosticRecord));
		AssignTable(header.diagnosticStringOffset, output.stringTable.size(), sizeof(char));
		header.diagnosticStringSize = static_cast<std::uint32_t>(output.stringTable.size());
		output.header = header;
	}
}

VirtualGeometryAdaptiveCompileSettings MakeDefaultVirtualGeometryAdaptiveCompileSettings()
{
	VirtualGeometryAdaptiveCompileSettings settings;
	settings.targetRatios = {
		1.0f,
		0.70710677f,
		0.5f,
		0.35355338f,
		0.25f,
		0.17677669f,
		0.125f,
		0.08838835f
	};
	// Accept every valid simplification by default. Per-mesh overrides remain
	// available for projects that want to merge small percentage reductions.
	settings.minimumSourceTriangles = 1u;
	settings.minimumGeneratedTriangles = 1u;
	settings.minimumMeshletsPerGeneratedLevel = 1u;
	settings.minimumUsefulTriangleReduction = 0.0f;
	settings.minimumUsefulMeshletReduction = 0.0f;
	settings.maximumGeometricError = 0.1f;
	settings.settingsSource = "ProjectDefaults";
	return settings;
}

VirtualGeometryAdaptiveCompileSettings SanitizeVirtualGeometryAdaptiveCompileSettings(const VirtualGeometryAdaptiveCompileSettings& settings)
{
	VirtualGeometryAdaptiveCompileSettings sanitized = settings;
	if (sanitized.targetRatios.empty())
	{
		sanitized.targetRatios = MakeDefaultVirtualGeometryAdaptiveCompileSettings().targetRatios;
	}

	std::vector<float> normalizedRatios;
	normalizedRatios.reserve(sanitized.targetRatios.size());
	float previousRatio = 2.0f;
	for (float ratio : sanitized.targetRatios)
	{
		if (!std::isfinite(ratio) || ratio <= 0.0f)
		{
			continue;
		}
		ratio = std::clamp(ratio, 0.01f, 1.0f);
		if (!normalizedRatios.empty() && ratio >= previousRatio)
		{
			continue;
		}
		normalizedRatios.push_back(ratio);
		previousRatio = ratio;
		if (normalizedRatios.size() >= 8u)
		{
			break;
		}
	}
	if (normalizedRatios.empty() || normalizedRatios.front() != 1.0f)
	{
		normalizedRatios.insert(normalizedRatios.begin(), 1.0f);
	}
	sanitized.targetRatios = std::move(normalizedRatios);
	sanitized.minimumSourceTriangles = std::max(1u, sanitized.minimumSourceTriangles);
	sanitized.minimumGeneratedTriangles = std::max(1u, sanitized.minimumGeneratedTriangles);
	sanitized.minimumMeshletsPerGeneratedLevel = std::max(1u, sanitized.minimumMeshletsPerGeneratedLevel);
	sanitized.minimumUsefulTriangleReduction = std::isfinite(sanitized.minimumUsefulTriangleReduction)
		? std::clamp(sanitized.minimumUsefulTriangleReduction, 0.0f, 0.95f)
		: 0.0f;
	sanitized.minimumUsefulMeshletReduction = std::isfinite(sanitized.minimumUsefulMeshletReduction)
		? std::clamp(sanitized.minimumUsefulMeshletReduction, 0.0f, 0.95f)
		: 0.0f;
	sanitized.maximumGeometricError = std::isfinite(sanitized.maximumGeometricError)
		? std::clamp(sanitized.maximumGeometricError, 0.0f, 1000.0f)
		: 0.1f;
	if (sanitized.settingsSource.empty())
	{
		sanitized.settingsSource = "ProjectDefaults";
	}
	return sanitized;
}

// IsSupportedMeshSourceExtension is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool IsSupportedMeshSourceExtension(const std::filesystem::path& filePath)
{
	const std::string ext = NormalizeExtension(filePath);
	return std::find(
		kSupportedMeshSourceExtensions.begin(),
		kSupportedMeshSourceExtensions.end(),
		std::string_view(ext)) != kSupportedMeshSourceExtensions.end();
}

// GetSupportedMeshSourceExtensionsText is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string GetSupportedMeshSourceExtensionsText()
{
	std::string result;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

static bool CompileMeshInternal(
	const std::filesystem::path& input,
	const std::filesystem::path& output,
	bool includeMeshletBounds,
	MeshCompileReport* outReport,
	float smoothingAngle,
	float weldThreshold,
	const VirtualGeometryAdaptiveCompileSettings* adaptiveSettings,
	MeshCompileProgressCallback progressCallback,
	const VirtualGeometryConeSafetyCookSettings* coneSafetySettings,
	const std::vector<std::uint32_t>* rawVertexWeldIdentities,
	std::vector<std::uint32_t>* outFinalWeldIdentities)
{
	MeshCompileReport report{};
	report.assetPath = input;
	report.formatVersion = kMeshCurrentVersion;
	if (adaptiveSettings != nullptr)
	{
		// The AssetDatabase/Inspector path resolves project defaults plus a
		// per-mesh override before cooking. Store that resolved profile on the
		// report so BuildVirtualGeometryLods uses it instead of the defaults.
		report.virtualGeometry.adaptiveSettings = *adaptiveSettings;
	}
	report.sourceHashHex = ComputeFileHashHex(input);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(input))
	{
		report.sourceBytes = std::filesystem::file_size(input);
	}

	const auto FinalizeReport = [&](bool result) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outReport != nullptr)
		{
			*outReport = report;
		}
		return result;
	};

	std::vector<VertexRaw> rawVertices;
	std::vector<std::uint32_t> rawIndices;
	bool hasUv = false;
	ReportMeshProgress(progressCallback, 0.05f, "Mesh Import", "Reading source mesh geometry.");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ImportSourceMesh(input, report, rawVertices, rawIndices, hasUv, progressCallback))
	{
		return FinalizeReport(false);
	}
	if(rawVertexWeldIdentities != nullptr)
	{
		// CompileMeshFromRaw emits OBJ faces in source-index order. The importer
		// returns one triangle-corner record in that same order; fail closed if a
		// future importer change breaks the correspondence needed by deformation.
		if(rawVertexWeldIdentities->size() != rawVertices.size())
		{
			report.status = "compile_fail";
			report.failureReason = "raw-weld-identity-mismatch";
			return FinalizeReport(false);
		}
		for(std::size_t index = 0u; index < rawVertices.size(); ++index)
		{
			rawVertices[index].weldIdentity = (*rawVertexWeldIdentities)[index];
		}
	}

	// Phase 2: Foundational Cleanup (Welding & Smoothing)
	
	ReportMeshProgress(progressCallback, 0.18f, "Mesh Cleanup", "Smoothing normals and generating tangents.");
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

	ReportMeshProgress(progressCallback, 0.28f, "Mesh Remap", "Building optimized indexed vertex buffers.");
	meshopt_remapIndexBuffer(indices.data(), rawIndices.data(), rawIndices.size(), finalRemap.data());
	meshopt_remapVertexBuffer(vertices.data(), rawVertices.data(), rawVertices.size(), sizeof(VertexRaw), finalRemap.data());
	if (outFinalWeldIdentities != nullptr)
	{
		// Deformable cooks need the final cleanup order because generated normals
		// or tangents may split one safely canonicalized source vertex into several
		// static vertices. The caller duplicates the same skin/morph record onto
		// every resulting vertex before appending the TDEF/TDFV payload.
		outFinalWeldIdentities->clear();
		outFinalWeldIdentities->reserve(vertices.size());
		for (const VertexRaw& vertex : vertices)
		{
			outFinalWeldIdentities->push_back(vertex.weldIdentity);
		}
	}

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

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (indices.empty())
	{
		std::cerr << "No geometry found in source mesh: " << input << "\n";
		report.status = "compile_fail";
		report.failureReason = "empty-geometry";
		return FinalizeReport(false);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if ((indices.size() % 3u) != 0u)
	{
		std::cerr << "MeshCompiler generated non-triangulated primitive output for source mesh: " << input << "\n";
		report.status = "compile_fail";
		report.failureReason = "non-triangle-index-buffer";
		return FinalizeReport(false);
	}

	std::vector<VirtualGeometryLodSource> virtualGeometryLods;
	ReportMeshProgress(progressCallback, 0.42f, "Virtual Geometry", "Building adaptive virtual geometry LODs.");
	if (!BuildVirtualGeometryLods(vertices, indices, report, virtualGeometryLods))
	{
		report.status = "compile_fail";
		report.failureReason = "virtual-geometry-lod-build-failed";
		return FinalizeReport(false);
	}

	VirtualGeometryBuildOutput virtualGeometryPayload{};
	const VirtualGeometryConeSafetyCookSettings resolvedConeSafetySettings =
		coneSafetySettings != nullptr ? *coneSafetySettings : VirtualGeometryConeSafetyCookSettings{};
	ReportMeshProgress(progressCallback, 0.58f, "Virtual Geometry", "Building virtual geometry clusters and pages.");
	if (!BuildVirtualGeometryPayload(vertices, virtualGeometryLods, includeMeshletBounds, resolvedConeSafetySettings, report, virtualGeometryPayload))
	{
		report.status = "compile_fail";
		report.failureReason = "virtual-geometry-payload-build-failed";
		return FinalizeReport(false);
	}

	MeshletBuildOutput& meshletPayload = virtualGeometryPayload.meshletPayload;

	// Meshlet generation is the last consumer of the index buffer.
	ReleaseVectorMemory(indices);
	ReleaseVectorMemory(virtualGeometryLods);

	MeshHeader header{};
	header.magic[0] = 'T';
	header.magic[1] = 'E';
	header.magic[2] = 'M';
	header.magic[3] = 'F';
	header.version = kMeshCurrentVersion;
	header.headerSize = static_cast<std::uint32_t>(sizeof(MeshHeader));

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	header.meshletFlags = (meshletPayload.meshletBounds.empty() ? 0u : kMeshletHeaderFlagHasBounds) | kMeshletHeaderFlagHasVirtualGeometry;
	header.meshletSkinningDataOffset = 0;
	header.meshletSkinningDataCount = 0;

	report.meshletGlobalVertexCount = header.meshletVertexCount;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ValidateFixedGpuLayout(header))
	{
		report.status = "compile_fail";
		report.failureReason = "fixed-layout-validation-failed";
		return FinalizeReport(false);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

	ReportMeshProgress(progressCallback, 0.74f, "Mesh Pack", "Packing GPU vertex streams.");
	// Keep packing deterministic so binary assets remain stable across repeated runs.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const VertexRaw& vertex : vertices)
	{
		std::array<float, 3> qPosition{
			(vertex.position[0] - header.quantization.positionOffset[0]) / header.quantization.positionScale[0],
			(vertex.position[1] - header.quantization.positionOffset[1]) / header.quantization.positionScale[1],
			(vertex.position[2] - header.quantization.positionOffset[2]) / header.quantization.positionScale[2]};

		std::array<float, 3> qNormal = vertex.normal;
		const float normalLengthSq = qNormal[0] * qNormal[0] + qNormal[1] * qNormal[1] + qNormal[2] * qNormal[2];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
	ReportMeshProgress(progressCallback, 0.82f, "Mesh Remap", "Encoding meshlet vertex remap data.");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!BuildRemapStream(meshletPayload.vertexRemapIndices, remapEncoding, remapData))
	{
		report.status = "compile_fail";
		report.failureReason = "remap-encoding-failed";
		return FinalizeReport(false);
	}

	ReleaseVectorMemory(meshletPayload.vertexRemapIndices);

	report.usedU16RemapEncoding = (remapEncoding == MeshletRemapEncoding::Uint16);

	std::uint32_t virtualGeometryPayloadBytes = 0u;
	if (!TryComputeVirtualGeometryPayloadBytes(virtualGeometryPayload, virtualGeometryPayloadBytes))
	{
		report.status = "compile_fail";
		report.failureReason = "virtual-geometry-layout-overflow";
		return FinalizeReport(false);
	}

	PayloadLayout payloadLayout{};
	ReportMeshProgress(progressCallback, 0.88f, "Mesh Layout", "Building TEMF payload layout.");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!TryBuildPayloadLayout(
		header,
		vertexData.size(),
		remapData.size(),
		0, // skinningBytes (static mesh source path only for now)
		meshletPayload.primitiveIndices.size() * sizeof(std::uint8_t),
		meshletPayload.meshletRecords.size() * sizeof(MeshletRecord),
		meshletPayload.meshletSections.size() * sizeof(MeshletSection),
		meshletPayload.meshletBounds.size() * sizeof(MeshletBounds),
		virtualGeometryPayloadBytes,
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
	header.virtualGeometryHeaderOffset = payloadLayout.virtualGeometryHeaderOffset;
	header.virtualGeometryHeaderSize = static_cast<std::uint32_t>(sizeof(VirtualGeometryHeader));
	header.virtualGeometryPayloadGeneration = kVirtualGeometryPayloadGeneration;
	PopulateVirtualGeometryHeader(virtualGeometryPayload, header.virtualGeometryHeaderOffset);

	try
	{
		ReportMeshProgress(progressCallback, 0.94f, "Mesh Write", "Writing Terra mesh file.");
		std::filesystem::create_directories(output.parent_path());
		WriteTemf(
			output,
			header,
			vertexData,
			remapData,
			{}, // skinningData
			meshletPayload.primitiveIndices,
			meshletPayload.meshletRecords,
			meshletPayload.meshletSections,
			meshletPayload.meshletBounds,
			&virtualGeometryPayload);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::filesystem::exists(output))
		{
			report.outputBytes = std::filesystem::file_size(output);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (report.sourceBytes > 0)
		{
			report.compressionRatio = static_cast<double>(report.outputBytes) / static_cast<double>(report.sourceBytes);
		}

		report.status = "success";

		ReportMeshProgress(progressCallback, 0.99f, "Mesh Done", "Mesh compile output is ready.");
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

bool CompileMesh(
	const std::filesystem::path& input,
	const std::filesystem::path& output,
	bool includeMeshletBounds,
	MeshCompileReport* outReport,
	float smoothingAngle,
	float weldThreshold,
	const VirtualGeometryAdaptiveCompileSettings* adaptiveSettings,
	MeshCompileProgressCallback progressCallback,
	const VirtualGeometryConeSafetyCookSettings* coneSafetySettings)
{
	return CompileMeshInternal(
		input, output, includeMeshletBounds, outReport, smoothingAngle, weldThreshold,
		adaptiveSettings, std::move(progressCallback), coneSafetySettings, nullptr, nullptr);
}

static bool CompileMeshFromRawInternal(
	const std::filesystem::path& logicalSource,
	const std::filesystem::path& output,
	const std::vector<RawMeshVertex>& vertices,
	const std::vector<std::uint32_t>& indices,
	bool preserveSourceVertexIdentity,
	bool includeMeshletBounds,
	MeshCompileReport* outReport,
	float smoothingAngle,
	float weldThreshold,
	const VirtualGeometryAdaptiveCompileSettings* adaptiveSettings,
	MeshCompileProgressCallback progressCallback,
	const VirtualGeometryConeSafetyCookSettings* coneSafetySettings,
	std::vector<std::uint32_t>* outFinalSourceVertexIndices)
{
	if (vertices.empty() || indices.empty() || (indices.size() % 3u) != 0u)
	{
		if (outReport != nullptr)
		{
			outReport->assetPath = logicalSource;
			outReport->formatVersion = kMeshCurrentVersion;
			outReport->status = "compile_fail";
			outReport->failureReason = "invalid-raw-geometry";
		}
		return false;
	}

	const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineRawMeshCompile";
	std::filesystem::create_directories(tempRoot);
	const std::string stableName = output.filename().replace_extension(".obj").string();
	const std::filesystem::path stagedObj = tempRoot / stableName;

	ReportMeshProgress(progressCallback, 0.02f, "Mesh Stage", "Staging decoded package geometry for mesh compile.");
	{
		std::ofstream out(stagedObj, std::ios::trunc);
		if (!out)
		{
			if (outReport != nullptr)
			{
				outReport->assetPath = logicalSource;
				outReport->formatVersion = kMeshCurrentVersion;
				outReport->status = "compile_fail";
				outReport->failureReason = "raw-stage-open-failed";
			}
			return false;
		}

		// OBJ is used here only as a short-lived interchange stream into the
		// existing static mesh cleanup path. The glTF package parser has already
		// resolved buffers, accessors, materials, and diagnostics before this point.
		out << "# TerraEngine transient raw mesh stage from " << logicalSource.generic_string() << '\n';
		for (const RawMeshVertex& vertex : vertices)
		{
			out << "v " << vertex.position[0] << ' ' << vertex.position[1] << ' ' << vertex.position[2] << '\n';
		}
		for (const RawMeshVertex& vertex : vertices)
		{
			// The transient handoff uses OBJ so it can reuse the established
			// cleanup path. CompileMesh preserves legacy OBJ behavior by flipping
			// V on import, so pre-flip package UVs here to keep decoded glTF UVs
			// unchanged in the final TEMF.
			out << "vt " << vertex.uv[0] << ' ' << (1.0f - vertex.uv[1]) << '\n';
		}
		for (const RawMeshVertex& vertex : vertices)
		{
			out << "vn " << vertex.normal[0] << ' ' << vertex.normal[1] << ' ' << vertex.normal[2] << '\n';
		}
		for (std::size_t index = 0; index < indices.size(); index += 3)
		{
			const std::uint32_t a = indices[index + 0] + 1u;
			const std::uint32_t b = indices[index + 1] + 1u;
			const std::uint32_t c = indices[index + 2] + 1u;
			if (a == 0u || b == 0u || c == 0u || a > vertices.size() || b > vertices.size() || c > vertices.size())
			{
				if (outReport != nullptr)
				{
					outReport->assetPath = logicalSource;
					outReport->formatVersion = kMeshCurrentVersion;
					outReport->status = "compile_fail";
					outReport->failureReason = "raw-index-out-of-range";
				}
				return false;
			}
			out << "f " << a << '/' << a << '/' << a
				<< ' ' << b << '/' << b << '/' << b
				<< ' ' << c << '/' << c << '/' << c << '\n';
		}
	}

	std::vector<std::uint32_t> rawVertexWeldIdentities;
	if(preserveSourceVertexIdentity)
	{
		// The transient OBJ importer expands faces into triangle corners. Repeating
		// each original index here preserves legitimate static sharing while keeping
		// coincident vertices with different deformation records separate.
		rawVertexWeldIdentities.assign(indices.begin(), indices.end());
	}
	const bool compiled = CompileMeshInternal(
		stagedObj, output, includeMeshletBounds, outReport, smoothingAngle, weldThreshold,
		adaptiveSettings, std::move(progressCallback), coneSafetySettings,
		preserveSourceVertexIdentity ? &rawVertexWeldIdentities : nullptr,
		outFinalSourceVertexIndices);
	std::error_code removeError;
	std::filesystem::remove(stagedObj, removeError);
	if (outReport != nullptr)
	{
		outReport->assetPath = logicalSource;
	}
	return compiled;
}

bool CompileMeshFromRaw(
	const std::filesystem::path& logicalSource,
	const std::filesystem::path& output,
	const std::vector<RawMeshVertex>& vertices,
	const std::vector<std::uint32_t>& indices,
	bool includeMeshletBounds,
	MeshCompileReport* outReport,
	float smoothingAngle,
	float weldThreshold,
	const VirtualGeometryAdaptiveCompileSettings* adaptiveSettings,
	MeshCompileProgressCallback progressCallback,
	const VirtualGeometryConeSafetyCookSettings* coneSafetySettings)
{
	return CompileMeshFromRawInternal(
		logicalSource, output, vertices, indices, false, includeMeshletBounds, outReport,
		smoothingAngle, weldThreshold, adaptiveSettings, std::move(progressCallback), coneSafetySettings, nullptr);
}

namespace
{
	constexpr std::uint32_t kInvalidPalettePartition = std::numeric_limits<std::uint32_t>::max();
	std::atomic<std::uint64_t> gSkinnedMeshStageSequence{0u};

	bool TryConvertSkinnedCount(std::size_t value, std::uint32_t& outValue)
	{
		// The TEMF extension stores offsets and counts as uint32 values. Check
		// before narrowing so hostile package data cannot wrap file ranges.
		if (value > std::numeric_limits<std::uint32_t>::max())
		{
			return false;
		}
		outValue = static_cast<std::uint32_t>(value);
		return true;
	}

	bool TryMultiplySkinnedSize(std::size_t left, std::size_t right, std::size_t& outValue)
	{
		// Package-controlled counts must never wrap a host-size allocation before
		// the stricter uint32 on-disk checks run.
		if (left != 0u && right > (std::numeric_limits<std::size_t>::max() / left))
		{
			return false;
		}
		outValue = left * right;
		return true;
	}

	bool TryAlignSkinnedOffset(std::uint64_t value, std::uint32_t alignment, std::uint32_t& outValue)
	{
		// Alignment is a power of two for every TEMF stream. Work in uint64 so the
		// addition cannot silently wrap before narrowing to the v1 uint32 offset.
		if (alignment == 0u || (alignment & (alignment - 1u)) != 0u)
		{
			return false;
		}
		const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1u);
		if (value > std::numeric_limits<std::uint64_t>::max() - mask)
		{
			return false;
		}
		const std::uint64_t aligned = (value + mask) & ~mask;
		if (aligned > std::numeric_limits<std::uint32_t>::max())
		{
			return false;
		}
		outValue = static_cast<std::uint32_t>(aligned);
		return true;
	}

	bool HasTemfMagic(const MeshHeader& header)
	{
		return header.magic[0] == 'T'
			&& header.magic[1] == 'E'
			&& header.magic[2] == 'M'
			&& header.magic[3] == 'F';
	}

	template <typename Record>
	bool TryReadStagedRecord(std::fstream& stream, std::uint32_t offset, Record& outRecord)
	{
		// The static TEMF is still private staging data here. Read typed records
		// only after the caller has checked the complete byte range, preventing a
		// corrupt intermediate file from producing partially published metadata.
		stream.clear();
		stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		stream.read(reinterpret_cast<char*>(&outRecord), sizeof(Record));
		return static_cast<bool>(stream);
	}

	bool TryBuildDeformableVirtualGeometrySidecar(
		std::fstream& stream,
		std::uint32_t stagedSize,
		const MeshHeader& meshHeader,
		const SkinnedMeshCookInput& input,
		const std::vector<const SkinnedMeshMorphTarget*>& orderedMorphs,
		const std::vector<std::uint32_t>& vertexPaletteIndices,
		const std::vector<std::uint8_t>& skinningBytes,
		std::uint32_t sidecarOffset,
		DeformableVirtualGeometrySidecarHeader& outHeader,
		std::vector<DeformableVirtualGeometryClusterRecord>& outClusters,
		std::vector<std::uint32_t>& outPaletteIndices,
		std::vector<DeformableVirtualGeometryMorphRecord>& outMorphs,
		std::vector<DeformableVirtualGeometryMorphVertex>& outMorphVertices)
	{
		const auto RangeWithinStagedFile = [stagedSize](std::uint32_t offset, std::uint64_t byteCount) {
			return offset <= stagedSize && byteCount <= static_cast<std::uint64_t>(stagedSize - offset);
		};
		const auto BuildEnvelope = [&](const std::vector<std::uint32_t>& vertices,
			float (&outMinimum)[3], float (&outMaximum)[3], float& outDeformationError) {
			std::array<float, 3> largestDisplacement{};
			for (std::size_t axis = 0u; axis < 3u; ++axis)
			{
				outMinimum[axis] = std::numeric_limits<float>::max();
				outMaximum[axis] = std::numeric_limits<float>::lowest();
			}
			for (std::uint32_t vertexIndex : vertices)
			{
				for (std::size_t axis = 0u; axis < 3u; ++axis)
				{
					float minimum = input.vertices[vertexIndex].baseVertex.position[axis];
					float maximum = minimum;
					for (const SkinnedMeshMorphTarget* morph : orderedMorphs)
					{
						const float delta = morph->positionDeltas[vertexIndex][axis];
						minimum += std::min(delta, 0.0f);
						maximum += std::max(delta, 0.0f);
					}
					outMinimum[axis] = std::min(outMinimum[axis], minimum);
					outMaximum[axis] = std::max(outMaximum[axis], maximum);
					largestDisplacement[axis] = std::max(
						largestDisplacement[axis],
						std::max(std::abs(minimum - input.vertices[vertexIndex].baseVertex.position[axis]),
							std::abs(maximum - input.vertices[vertexIndex].baseVertex.position[axis])));
				}
			}
			outDeformationError = std::sqrt(
				largestDisplacement[0] * largestDisplacement[0]
				+ largestDisplacement[1] * largestDisplacement[1]
				+ largestDisplacement[2] * largestDisplacement[2]);
			return std::isfinite(outDeformationError);
		};

		if (vertexPaletteIndices.size() != input.vertices.size()
			|| skinningBytes.size() != input.vertices.size() * kDeformableMeshSkinningStrideBytes)
		{
			return false;
		}
		const bool hasBoundSkinning = std::any_of(
			skinningBytes.begin(), skinningBytes.end(), [](std::uint8_t value) { return value != 0u; });
		std::vector<VirtualGeometryClusterRecord> sourceClusters;
		std::vector<VirtualGeometryCullableGroupRecord> sourceGroups;
		bool hasVirtualGeometryClusters = false;
		if ((meshHeader.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0u)
		{
			VirtualGeometryHeader virtualHeader{};
			if (meshHeader.virtualGeometryHeaderOffset == 0u
				|| meshHeader.virtualGeometryHeaderSize != sizeof(VirtualGeometryHeader)
				|| !RangeWithinStagedFile(meshHeader.virtualGeometryHeaderOffset, sizeof(VirtualGeometryHeader))
				|| !TryReadStagedRecord(stream, meshHeader.virtualGeometryHeaderOffset, virtualHeader)
				|| virtualHeader.payloadGeneration != kVirtualGeometryPayloadGeneration
				|| virtualHeader.headerSize != sizeof(VirtualGeometryHeader))
			{
				return false;
			}
			std::uint64_t clusterBytes = static_cast<std::uint64_t>(virtualHeader.clusterCount)
				* sizeof(VirtualGeometryClusterRecord);
			std::uint64_t groupBytes = static_cast<std::uint64_t>(virtualHeader.cullableGroupCount)
				* sizeof(VirtualGeometryCullableGroupRecord);
			if ((virtualHeader.clusterCount != 0u && !RangeWithinStagedFile(virtualHeader.clusterTableOffset, clusterBytes))
				|| (virtualHeader.cullableGroupCount != 0u && !RangeWithinStagedFile(virtualHeader.cullableGroupTableOffset, groupBytes)))
			{
				return false;
			}
			sourceClusters.resize(virtualHeader.clusterCount);
			sourceGroups.resize(virtualHeader.cullableGroupCount);
			if (!sourceClusters.empty())
			{
				stream.clear();
				stream.seekg(static_cast<std::streamoff>(virtualHeader.clusterTableOffset), std::ios::beg);
				stream.read(reinterpret_cast<char*>(sourceClusters.data()), static_cast<std::streamsize>(clusterBytes));
				if (!stream)
				{
					return false;
				}
			}
			if (!sourceGroups.empty())
			{
				stream.clear();
				stream.seekg(static_cast<std::streamoff>(virtualHeader.cullableGroupTableOffset), std::ios::beg);
				stream.read(reinterpret_cast<char*>(sourceGroups.data()), static_cast<std::streamsize>(groupBytes));
				if (!stream)
				{
					return false;
				}
			}
			hasVirtualGeometryClusters = !sourceClusters.empty();
		}

		const MeshletRemapEncoding remapEncoding = static_cast<MeshletRemapEncoding>(meshHeader.meshletVertexRemapEncoding);
		const std::uint32_t remapStride = GetRemapElementSizeBytes(remapEncoding);
		const std::uint64_t remapBytes = static_cast<std::uint64_t>(meshHeader.meshletVertexRemapCount) * remapStride;
		if ((remapEncoding != MeshletRemapEncoding::Uint16 && remapEncoding != MeshletRemapEncoding::Uint32)
			|| !RangeWithinStagedFile(meshHeader.meshletVertexRemapDataOffset, remapBytes))
		{
			return false;
		}
		std::vector<std::uint8_t> remapBytesBuffer(static_cast<std::size_t>(remapBytes));
		stream.clear();
		stream.seekg(static_cast<std::streamoff>(meshHeader.meshletVertexRemapDataOffset), std::ios::beg);
		stream.read(reinterpret_cast<char*>(remapBytesBuffer.data()), static_cast<std::streamsize>(remapBytesBuffer.size()));
		if (!stream)
		{
			return false;
		}
		const auto ReadRemappedVertex = [&](std::uint32_t remapIndex, std::uint32_t& outVertex) {
			if (remapIndex >= meshHeader.meshletVertexRemapCount)
			{
				return false;
			}
			const std::size_t offset = static_cast<std::size_t>(remapIndex) * remapStride;
			if (remapEncoding == MeshletRemapEncoding::Uint16)
			{
				std::uint16_t compact = 0u;
				std::memcpy(&compact, remapBytesBuffer.data() + offset, sizeof(compact));
				outVertex = compact;
			}
			else
			{
				std::memcpy(&outVertex, remapBytesBuffer.data() + offset, sizeof(outVertex));
			}
			return outVertex < vertexPaletteIndices.size();
		};

		const auto AppendCluster = [&](const VirtualGeometryClusterRecord* sourceCluster, bool fallbackOnly) {
			std::vector<std::uint32_t> clusterVertices;
			if (sourceCluster == nullptr)
			{
				clusterVertices.resize(static_cast<std::uint32_t>(input.vertices.size()));
				std::iota(clusterVertices.begin(), clusterVertices.end(), 0u);
			}
			else
			{
				if (sourceCluster->groupOffset > sourceGroups.size()
					|| sourceCluster->groupCount > sourceGroups.size() - sourceCluster->groupOffset)
				{
					return false;
				}
				for (std::uint32_t groupIndex = sourceCluster->groupOffset;
					groupIndex < sourceCluster->groupOffset + sourceCluster->groupCount; ++groupIndex)
				{
					const VirtualGeometryCullableGroupRecord& group = sourceGroups[groupIndex];
					if (group.vertexOffset > meshHeader.meshletVertexRemapCount
						|| group.vertexCount > meshHeader.meshletVertexRemapCount - group.vertexOffset)
					{
						return false;
					}
					for (std::uint32_t remapIndex = group.vertexOffset;
						remapIndex < group.vertexOffset + group.vertexCount; ++remapIndex)
					{
						std::uint32_t vertex = 0u;
						if (!ReadRemappedVertex(remapIndex, vertex))
						{
							return false;
						}
						clusterVertices.push_back(vertex);
					}
				}
				std::sort(clusterVertices.begin(), clusterVertices.end());
				clusterVertices.erase(std::unique(clusterVertices.begin(), clusterVertices.end()), clusterVertices.end());
				if (clusterVertices.empty())
				{
					return false;
				}
			}
			std::vector<std::uint32_t> palettes;
			palettes.reserve(clusterVertices.size());
			for (std::uint32_t vertex : clusterVertices)
			{
				palettes.push_back(vertexPaletteIndices[vertex]);
			}
			std::sort(palettes.begin(), palettes.end());
			palettes.erase(std::unique(palettes.begin(), palettes.end()), palettes.end());
			if (palettes.empty())
			{
				return false;
			}
			DeformableVirtualGeometryClusterRecord record{};
			record.clusterId = sourceCluster == nullptr ? 0u : sourceCluster->clusterId;
			record.fallbackClusterId = sourceCluster == nullptr ? 0u : sourceCluster->fallbackClusterId;
			record.parentClusterId = sourceCluster == nullptr ? kVirtualGeometryInvalidIndex : sourceCluster->parentClusterId;
			record.firstChildClusterId = sourceCluster == nullptr ? kVirtualGeometryInvalidIndex : sourceCluster->firstChildClusterId;
			record.childCount = sourceCluster == nullptr ? 0u : sourceCluster->childCount;
			record.geometricError = sourceCluster == nullptr ? 0.0f : sourceCluster->geometricError;
			if (!std::isfinite(record.geometricError) || record.geometricError < 0.0f
				|| !BuildEnvelope(clusterVertices, record.deformationBoundsMin, record.deformationBoundsMax, record.deformationError))
			{
				return false;
			}
			record.flags = kDeformableVirtualGeometryClusterFlagActive
				| kDeformableVirtualGeometryClusterFlagFallbackCoverageComplete;
			record.morphTargetCount = static_cast<std::uint32_t>(orderedMorphs.size());
			if (fallbackOnly)
			{
				record.flags |= kDeformableVirtualGeometryClusterFlagFallbackActive;
			}
			if (!hasBoundSkinning)
			{
				record.flags |= kDeformableVirtualGeometryClusterFlagConservativeEnvelopeValid
					| kDeformableVirtualGeometryClusterFlagFrustumCullingSafe;
				if (record.childCount != 0u)
				{
					record.flags |= kDeformableVirtualGeometryClusterFlagRefinementSafe;
				}
			}
			record.paletteIndexCount = static_cast<std::uint32_t>(palettes.size());
			outPaletteIndices.insert(outPaletteIndices.end(), palettes.begin(), palettes.end());
			outClusters.push_back(record);
			return true;
		};

		if (hasVirtualGeometryClusters)
		{
			for (const VirtualGeometryClusterRecord& cluster : sourceClusters)
			{
				if (cluster.clusterId != outClusters.size() || !AppendCluster(&cluster, cluster.fallbackClusterId == cluster.clusterId))
				{
					return false;
				}
			}
		}
		else if (!AppendCluster(nullptr, true))
		{
			return false;
		}

		std::uint64_t paletteCursor = static_cast<std::uint64_t>(sidecarOffset)
			+ sizeof(DeformableVirtualGeometrySidecarHeader)
			+ static_cast<std::uint64_t>(outClusters.size()) * sizeof(DeformableVirtualGeometryClusterRecord);
		for (DeformableVirtualGeometryClusterRecord& cluster : outClusters)
		{
			cluster.paletteIndexOffset = static_cast<std::uint32_t>(paletteCursor);
			paletteCursor += static_cast<std::uint64_t>(cluster.paletteIndexCount) * sizeof(std::uint32_t);
		}
		const std::uint64_t morphTableOffset = paletteCursor;
		paletteCursor += static_cast<std::uint64_t>(orderedMorphs.size()) * sizeof(DeformableVirtualGeometryMorphRecord);
		const std::uint64_t morphVertexDataOffset = paletteCursor;
		for (const SkinnedMeshMorphTarget* morph : orderedMorphs)
		{
			DeformableVirtualGeometryMorphRecord morphRecord{};
			morphRecord.stableTargetId = morph->stableTargetId;
			morphRecord.vertexDataOffset = static_cast<std::uint32_t>(paletteCursor);
			morphRecord.vertexCount = static_cast<std::uint32_t>(input.vertices.size());
			morphRecord.vertexStrideBytes = sizeof(DeformableVirtualGeometryMorphVertex);
			morphRecord.flags = kDeformableVirtualGeometryMorphFlagPosition;
			if (!morph->normalDeltas.empty())
			{
				morphRecord.flags |= kDeformableVirtualGeometryMorphFlagNormal;
			}
			if (!morph->tangentDeltas.empty())
			{
				morphRecord.flags |= kDeformableVirtualGeometryMorphFlagTangent;
			}
			outMorphs.push_back(morphRecord);
			for (std::size_t vertexIndex = 0u; vertexIndex < input.vertices.size(); ++vertexIndex)
			{
				DeformableVirtualGeometryMorphVertex gpuRecord{};
				std::copy_n(morph->positionDeltas[vertexIndex].begin(), 3u, gpuRecord.positionDelta);
				if (!morph->normalDeltas.empty())
				{
					std::copy_n(morph->normalDeltas[vertexIndex].begin(), 3u, gpuRecord.normalDelta);
				}
				if (!morph->tangentDeltas.empty())
				{
					std::copy_n(morph->tangentDeltas[vertexIndex].begin(), 3u, gpuRecord.tangentDelta);
				}
				outMorphVertices.push_back(gpuRecord);
			}
			paletteCursor += static_cast<std::uint64_t>(input.vertices.size()) * sizeof(DeformableVirtualGeometryMorphVertex);
		}
		if (paletteCursor > std::numeric_limits<std::uint32_t>::max()
			|| outMorphVertices.size() != orderedMorphs.size() * input.vertices.size())
		{
			return false;
		}
		outHeader.magic = kDeformableVirtualGeometrySidecarMagicValue;
		outHeader.version = kDeformableVirtualGeometrySidecarVersion;
		outHeader.headerSize = static_cast<std::uint16_t>(sizeof(DeformableVirtualGeometrySidecarHeader));
		outHeader.payloadSize = static_cast<std::uint32_t>(paletteCursor - sidecarOffset);
		outHeader.flags = kDeformableVirtualGeometrySidecarFlagDerivedFromClusters;
		outHeader.clusterCount = static_cast<std::uint32_t>(outClusters.size());
		outHeader.clusterTableOffset = sidecarOffset + static_cast<std::uint32_t>(sizeof(DeformableVirtualGeometrySidecarHeader));
		outHeader.paletteIndexCount = static_cast<std::uint32_t>(outPaletteIndices.size());
		outHeader.paletteIndexTableOffset = outPaletteIndices.empty() ? 0u : outClusters.front().paletteIndexOffset;
		outHeader.morphTargetCount = static_cast<std::uint32_t>(outMorphs.size());
		outHeader.morphTableOffset = outMorphs.empty() ? 0u : static_cast<std::uint32_t>(morphTableOffset);
		outHeader.morphVertexDataOffset = outMorphs.empty() ? 0u : static_cast<std::uint32_t>(morphVertexDataOffset);
		outHeader.morphVertexStrideBytes = sizeof(DeformableVirtualGeometryMorphVertex);
		return outHeader.clusterCount != 0u && outHeader.paletteIndexCount != 0u;
	}

	struct AnimatedVirtualPageCookedBlob
	{
		AnimatedVirtualPageDirectoryEntry entry{};
		std::vector<std::uint8_t> bytes;
	};

	struct AnimatedVirtualPageCookedSection
	{
		AnimatedVirtualPageSectionType type =
			AnimatedVirtualPageSectionType::PackedVertices;
		std::uint32_t flags = 1u;
		std::uint32_t elementCount = 0u;
		std::uint32_t elementStride = 0u;
		std::vector<std::uint8_t> bytes;
	};

	std::uint64_t HashAnimatedVirtualCookBytes(
		const std::uint8_t* bytes,
		std::size_t byteCount,
		std::uint64_t seed = 1469598103934665603ull)
	{
		std::uint64_t hash = seed;
		for (std::size_t index = 0u; index < byteCount; ++index)
		{
			hash ^= bytes[index];
			hash *= 1099511628211ull;
		}
		return hash == 0u ? 1u : hash;
	}

	template <typename Record>
	std::vector<std::uint8_t> CopyAnimatedVirtualPageRecords(
		const std::vector<Record>& records)
	{
		std::vector<std::uint8_t> bytes(
			records.size() * sizeof(Record));
		if (!bytes.empty())
		{
			std::memcpy(bytes.data(), records.data(), bytes.size());
		}
		return bytes;
	}

	template <typename Record>
	bool TryReadAnimatedVirtualPageTable(
		std::fstream& stream,
		std::uint32_t offset,
		std::uint32_t count,
		std::vector<Record>& output)
	{
		output.resize(count);
		if (output.empty())
		{
			return true;
		}
		stream.clear();
		stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		stream.read(
			reinterpret_cast<char*>(output.data()),
			static_cast<std::streamsize>(output.size() * sizeof(Record)));
		return static_cast<bool>(stream);
	}

	bool TryBuildAnimatedVirtualPageBlobs(
		std::fstream& stream,
		const MeshHeader& meshHeader,
		const SkinnedMeshCookInput& input,
		const SkinnedMeshCookSettings& settings,
		const std::vector<SkinnedMeshPalettePartition>& palettes,
		const std::vector<std::uint32_t>& vertexPaletteIndices,
		const std::vector<std::uint8_t>& skinningBytes,
		const std::vector<const SkinnedMeshMorphTarget*>& orderedMorphs,
		const std::vector<DeformableVirtualGeometryClusterRecord>&
			deformableClusters,
		std::uint64_t meshPayloadGeneration,
		std::vector<AnimatedVirtualPageCookedBlob>& output,
		std::vector<std::uint32_t>& rootPageIds)
	{
		if (meshPayloadGeneration == 0u
			|| meshHeader.virtualGeometryHeaderOffset == 0u
			|| meshHeader.layout.strideBytes != kMeshletPackedVertexStrideBytes
			|| vertexPaletteIndices.size() != input.vertices.size()
			|| skinningBytes.size()
				!= input.vertices.size() * sizeof(AnimatedVirtualPageSkinRecord))
		{
			return false;
		}

		VirtualGeometryHeader virtualHeader{};
		if (!TryReadStagedRecord(
				stream, meshHeader.virtualGeometryHeaderOffset, virtualHeader)
			|| virtualHeader.payloadGeneration
				!= kVirtualGeometryPayloadGeneration)
		{
			return false;
		}
		std::vector<VirtualGeometryPageRecord> pages;
		std::vector<VirtualGeometryClusterRecord> clusters;
		std::vector<VirtualGeometryCullableGroupRecord> groups;
		std::vector<MeshletRecord> meshlets;
		std::vector<MeshletBounds> bounds;
		if (!TryReadAnimatedVirtualPageTable(
				stream, virtualHeader.pageTableOffset,
				virtualHeader.pageCount, pages)
			|| !TryReadAnimatedVirtualPageTable(
				stream, virtualHeader.clusterTableOffset,
				virtualHeader.clusterCount, clusters)
			|| !TryReadAnimatedVirtualPageTable(
				stream, virtualHeader.cullableGroupTableOffset,
				virtualHeader.cullableGroupCount, groups)
			|| !TryReadAnimatedVirtualPageTable(
				stream, meshHeader.meshletTableOffset,
				meshHeader.meshletCount, meshlets)
			|| !TryReadAnimatedVirtualPageTable(
				stream, meshHeader.meshletBoundsOffset,
				meshHeader.meshletBoundsCount, bounds)
			|| pages.empty()
			|| clusters.size() != deformableClusters.size())
		{
			return false;
		}

		const std::uint32_t remapStride = GetRemapElementSizeBytes(
			static_cast<MeshletRemapEncoding>(
				meshHeader.meshletVertexRemapEncoding));
		if (remapStride == 0u)
		{
			return false;
		}
		std::vector<std::uint8_t> remapBytes(
			static_cast<std::size_t>(meshHeader.meshletVertexRemapCount)
				* remapStride);
		std::vector<std::uint8_t> primitiveBytes(
			meshHeader.meshletPrimitiveIndexCount);
		std::vector<std::uint8_t> packedVertices(
			static_cast<std::size_t>(meshHeader.meshletVertexCount)
				* meshHeader.layout.strideBytes);
		const auto ReadBytes = [&stream](
			std::uint32_t offset, std::vector<std::uint8_t>& bytes)
		{
			if (bytes.empty())
			{
				return true;
			}
			stream.clear();
			stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
			stream.read(
				reinterpret_cast<char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			return static_cast<bool>(stream);
		};
		if (!ReadBytes(meshHeader.meshletVertexRemapDataOffset, remapBytes)
			|| !ReadBytes(
				meshHeader.meshletPrimitiveIndexDataOffset, primitiveBytes)
			|| !ReadBytes(meshHeader.meshletVertexDataOffset, packedVertices))
		{
			return false;
		}
		const auto DecodeRemap = [&](std::uint32_t index,
			std::uint32_t& sourceVertex)
		{
			if (index >= meshHeader.meshletVertexRemapCount)
			{
				return false;
			}
			const std::size_t byteOffset =
				static_cast<std::size_t>(index) * remapStride;
			if (remapStride == sizeof(std::uint16_t))
			{
				std::uint16_t compact = 0u;
				std::memcpy(
					&compact, remapBytes.data() + byteOffset,
					sizeof(compact));
				sourceVertex = compact;
			}
			else
			{
				std::memcpy(
					&sourceVertex, remapBytes.data() + byteOffset,
					sizeof(sourceVertex));
			}
			return sourceVertex < meshHeader.meshletVertexCount;
		};

		output.clear();
		rootPageIds.clear();
		output.reserve(pages.size());
		for (const VirtualGeometryPageRecord& page : pages)
		{
			if (page.pageId >= pages.size()
				|| page.groupOffset > groups.size()
				|| page.groupCount == 0u
				|| page.groupCount > groups.size() - page.groupOffset)
			{
				return false;
			}
			const auto clusterIt = std::find_if(
				clusters.begin(), clusters.end(),
				[&page](const VirtualGeometryClusterRecord& cluster)
				{
					return cluster.pageId == page.pageId;
				});
			if (clusterIt == clusters.end()
				|| clusterIt->clusterId >= deformableClusters.size())
			{
				return false;
			}

			std::vector<std::uint32_t> sourceVertices;
			std::unordered_map<std::uint32_t, std::uint32_t> localVertexBySource;
			std::vector<std::uint32_t> localRemap;
			std::vector<MeshletRecord> localMeshlets;
			std::vector<MeshletBounds> localBounds;
			std::vector<std::uint8_t> localPrimitives;
			for (std::uint32_t groupIndex = page.groupOffset;
				groupIndex < page.groupOffset + page.groupCount; ++groupIndex)
			{
				const VirtualGeometryCullableGroupRecord& group =
					groups[groupIndex];
				if (group.groupIndex >= meshlets.size())
				{
					return false;
				}
				MeshletRecord localMeshlet = meshlets[group.groupIndex];
				if (localMeshlet.vertexOffset
						> meshHeader.meshletVertexRemapCount
					|| localMeshlet.vertexCount
						> meshHeader.meshletVertexRemapCount
							- localMeshlet.vertexOffset
					|| localMeshlet.primitiveIndexOffset
						> primitiveBytes.size()
					|| localMeshlet.primitiveIndexCount
						> primitiveBytes.size()
							- localMeshlet.primitiveIndexOffset)
				{
					return false;
				}
				localMeshlet.vertexOffset =
					static_cast<std::uint32_t>(localRemap.size());
				for (std::uint32_t vertex = 0u;
					vertex < meshlets[group.groupIndex].vertexCount; ++vertex)
				{
					std::uint32_t sourceVertex = 0u;
					if (!DecodeRemap(
						meshlets[group.groupIndex].vertexOffset + vertex,
						sourceVertex))
					{
						return false;
					}
					auto [iterator, inserted] = localVertexBySource.emplace(
						sourceVertex,
						static_cast<std::uint32_t>(sourceVertices.size()));
					if (inserted)
					{
						sourceVertices.push_back(sourceVertex);
					}
					localRemap.push_back(iterator->second);
				}
				localMeshlet.primitiveIndexOffset =
					static_cast<std::uint32_t>(localPrimitives.size());
				const std::size_t primitiveBegin =
					meshlets[group.groupIndex].primitiveIndexOffset;
				localPrimitives.insert(
					localPrimitives.end(),
					primitiveBytes.begin()
						+ static_cast<std::ptrdiff_t>(primitiveBegin),
					primitiveBytes.begin()
						+ static_cast<std::ptrdiff_t>(
							primitiveBegin
							+ meshlets[group.groupIndex]
								.primitiveIndexCount));
				localMeshlets.push_back(localMeshlet);
				if (group.groupIndex < bounds.size())
				{
					localBounds.push_back(bounds[group.groupIndex]);
				}
			}
			if (sourceVertices.empty() || localMeshlets.empty()
				|| localPrimitives.empty())
			{
				return false;
			}

			std::vector<std::uint8_t> localPackedVertices;
			std::vector<AnimatedVirtualPageSourceVertexRecord> sourceIds;
			localPackedVertices.reserve(
				sourceVertices.size() * meshHeader.layout.strideBytes);
			sourceIds.reserve(sourceVertices.size());
			for (std::uint32_t sourceVertex : sourceVertices)
			{
				const std::size_t begin =
					static_cast<std::size_t>(sourceVertex)
						* meshHeader.layout.strideBytes;
				localPackedVertices.insert(
					localPackedVertices.end(),
					packedVertices.begin()
						+ static_cast<std::ptrdiff_t>(begin),
					packedVertices.begin()
						+ static_cast<std::ptrdiff_t>(
							begin + meshHeader.layout.strideBytes));
				sourceIds.push_back({ sourceVertex });
			}

			// Rewrite partition-local influences into one page-local joint palette.
			// This makes the blob independent of the owner TDEF palette tables.
			std::vector<std::uint32_t> pageJointPalette;
			std::unordered_map<std::uint32_t, std::uint8_t>
				localJointByPaletteEntry;
			std::vector<AnimatedVirtualPageSkinRecord> localSkin;
			std::uint32_t pagePalettePartition =
				kVirtualGeometryInvalidIndex;
			localSkin.reserve(sourceVertices.size());
			for (std::uint32_t sourceVertex : sourceVertices)
			{
				const std::uint32_t partition =
					vertexPaletteIndices[sourceVertex];
				if (partition >= palettes.size())
				{
					return false;
				}
				if (pagePalettePartition
					== kVirtualGeometryInvalidIndex)
				{
					pagePalettePartition = partition;
				}
				else if (pagePalettePartition != partition)
				{
					// Version one animation palette dispatches one cooked
					// partition per work packet. A physical page must therefore
					// be split by the cooker before it crosses that boundary.
					return false;
				}
				AnimatedVirtualPageSkinRecord record{};
				const std::uint8_t* sourceSkin =
					skinningBytes.data()
					+ static_cast<std::size_t>(sourceVertex)
						* sizeof(AnimatedVirtualPageSkinRecord);
				for (std::uint32_t influence = 0u; influence < 4u;
					++influence)
				{
					record.normalizedWeights[influence] =
						sourceSkin[4u + influence];
					if (record.normalizedWeights[influence] == 0u)
					{
						continue;
					}
					const std::uint32_t partitionJoint =
						sourceSkin[influence];
					if (partitionJoint
						>= palettes[partition].sourceJointIds.size())
					{
						return false;
					}
					auto found =
						localJointByPaletteEntry.find(partitionJoint);
					if (found
						== localJointByPaletteEntry.end())
					{
						if (pageJointPalette.size()
							>= kAnimatedVirtualPageMaximumLocalJoints)
						{
							return false;
						}
						const std::uint8_t localJoint =
							static_cast<std::uint8_t>(
								pageJointPalette.size());
						// Store the evaluator palette entry, not the skeleton
						// joint id. Compute can then gather the correct 3x4
						// matrix without owner TDEF tables or CPU readback.
						pageJointPalette.push_back(partitionJoint);
						found =
							localJointByPaletteEntry.emplace(
								partitionJoint, localJoint).first;
					}
					record.localJointIndices[influence] = found->second;
				}
				localSkin.push_back(record);
			}

			std::vector<AnimatedVirtualPageMorphRecord> localMorphs;
			std::vector<DeformableVirtualGeometryMorphVertex> localMorphDeltas;
			for (const SkinnedMeshMorphTarget* morph : orderedMorphs)
			{
				const std::size_t deltaStart = localMorphDeltas.size();
				bool affected = false;
				for (std::uint32_t sourceVertex : sourceVertices)
				{
					DeformableVirtualGeometryMorphVertex delta{};
					std::copy_n(
						morph->positionDeltas[sourceVertex].begin(),
						3u, delta.positionDelta);
					if (!morph->normalDeltas.empty())
					{
						std::copy_n(
							morph->normalDeltas[sourceVertex].begin(),
							3u, delta.normalDelta);
					}
					if (!morph->tangentDeltas.empty())
					{
						std::copy_n(
							morph->tangentDeltas[sourceVertex].begin(),
							3u, delta.tangentDelta);
					}
					const std::uint8_t* deltaBytes =
						reinterpret_cast<const std::uint8_t*>(&delta);
					affected = affected || std::any_of(
						deltaBytes,
						deltaBytes + sizeof(delta),
						[](std::uint8_t value) { return value != 0u; });
					localMorphDeltas.push_back(delta);
				}
				if (!affected)
				{
					localMorphDeltas.resize(deltaStart);
					continue;
				}
				std::uint32_t flags =
					kDeformableVirtualGeometryMorphFlagPosition;
				if (!morph->normalDeltas.empty())
				{
					flags |=
						kDeformableVirtualGeometryMorphFlagNormal;
				}
				if (!morph->tangentDeltas.empty())
				{
					flags |=
						kDeformableVirtualGeometryMorphFlagTangent;
				}
				localMorphs.push_back({
					morph->stableTargetId,
					static_cast<std::uint32_t>(
						deltaStart
						* sizeof(
							DeformableVirtualGeometryMorphVertex)),
					static_cast<std::uint32_t>(sourceVertices.size()),
					static_cast<std::uint32_t>(
						sizeof(
							DeformableVirtualGeometryMorphVertex)),
					flags });
			}

			std::vector<AnimatedVirtualPageCookedSection> sections;
			const auto AddSection = [&sections](
				AnimatedVirtualPageSectionType type,
				std::uint32_t count,
				std::uint32_t stride,
				std::vector<std::uint8_t> bytes)
			{
				if (count == 0u || stride == 0u || bytes.empty())
				{
					return;
				}
				sections.push_back({
					type, 1u, count, stride, std::move(bytes) });
			};
			AddSection(
				AnimatedVirtualPageSectionType::PackedVertices,
				static_cast<std::uint32_t>(sourceVertices.size()),
				meshHeader.layout.strideBytes,
				std::move(localPackedVertices));
			// Capture the count before moving the byte vector. C++ does not
			// guarantee function-argument evaluation order, so reading size()
			// in the same call as a by-value move can otherwise observe zero.
			const std::uint32_t localPrimitiveByteCount =
				static_cast<std::uint32_t>(localPrimitives.size());
			AddSection(
				AnimatedVirtualPageSectionType::PrimitiveBytes,
				localPrimitiveByteCount,
				sizeof(std::uint8_t), std::move(localPrimitives));
			AddSection(
				AnimatedVirtualPageSectionType::VertexRemap,
				static_cast<std::uint32_t>(localRemap.size()),
				sizeof(std::uint32_t),
				CopyAnimatedVirtualPageRecords(localRemap));
			AddSection(
				AnimatedVirtualPageSectionType::Meshlets,
				static_cast<std::uint32_t>(localMeshlets.size()),
				sizeof(MeshletRecord),
				CopyAnimatedVirtualPageRecords(localMeshlets));
			AddSection(
				AnimatedVirtualPageSectionType::MeshletBounds,
				static_cast<std::uint32_t>(localBounds.size()),
				sizeof(MeshletBounds),
				CopyAnimatedVirtualPageRecords(localBounds));
			std::vector<QuantizationInfo> decodeConstants{
				meshHeader.quantization };
			AddSection(
				AnimatedVirtualPageSectionType::DecodeConstants,
				1u, sizeof(QuantizationInfo),
				CopyAnimatedVirtualPageRecords(decodeConstants));
			std::vector<std::uint32_t> materialIds{
				clusterIt->materialSectionId };
			AddSection(
				AnimatedVirtualPageSectionType::Materials,
				1u, sizeof(std::uint32_t),
				CopyAnimatedVirtualPageRecords(materialIds));
			AddSection(
				AnimatedVirtualPageSectionType::SkinRecords,
				static_cast<std::uint32_t>(localSkin.size()),
				sizeof(AnimatedVirtualPageSkinRecord),
				CopyAnimatedVirtualPageRecords(localSkin));
			AddSection(
				AnimatedVirtualPageSectionType::JointPalette,
				static_cast<std::uint32_t>(pageJointPalette.size()),
				sizeof(std::uint32_t),
				CopyAnimatedVirtualPageRecords(pageJointPalette));
			AddSection(
				AnimatedVirtualPageSectionType::MorphTargets,
				static_cast<std::uint32_t>(localMorphs.size()),
				sizeof(AnimatedVirtualPageMorphRecord),
				CopyAnimatedVirtualPageRecords(localMorphs));
			AddSection(
				AnimatedVirtualPageSectionType::MorphDeltas,
				static_cast<std::uint32_t>(localMorphDeltas.size()),
				sizeof(DeformableVirtualGeometryMorphVertex),
				CopyAnimatedVirtualPageRecords(localMorphDeltas));
			AddSection(
				AnimatedVirtualPageSectionType::SourceVertexIds,
				static_cast<std::uint32_t>(sourceIds.size()),
				sizeof(AnimatedVirtualPageSourceVertexRecord),
				CopyAnimatedVirtualPageRecords(sourceIds));
			std::vector<VirtualGeometryClusterRecord> localClusters{
				*clusterIt };
			AddSection(
				AnimatedVirtualPageSectionType::Clusters,
				1u, sizeof(VirtualGeometryClusterRecord),
				CopyAnimatedVirtualPageRecords(localClusters));
			std::vector<std::array<float, 2>> errorSamples{ {
				clusterIt->geometricError,
				deformableClusters[clusterIt->clusterId]
					.deformationError } };
			AddSection(
				AnimatedVirtualPageSectionType::ErrorSamples,
				1u, sizeof(std::array<float, 2>),
				CopyAnimatedVirtualPageRecords(errorSamples));
			if (sections.size()
				> kAnimatedVirtualPageMaximumSections)
			{
				return false;
			}

			AnimatedVirtualPageCookedBlob cooked;
			AnimatedVirtualPageBlobHeader blobHeader{};
			blobHeader.magic =
				kAnimatedVirtualPageBlobMagicValue;
			blobHeader.version =
				kAnimatedVirtualPageFormatVersion;
			blobHeader.headerSize =
				sizeof(AnimatedVirtualPageBlobHeader);
			blobHeader.flags =
				kAnimatedVirtualPageFlagAnimated
				| kAnimatedVirtualPageFlagHasSkinning
				| (localMorphs.empty()
					? 0u
					: kAnimatedVirtualPageFlagHasMorphs)
				| kAnimatedVirtualPageFlagHasErrorSamples;
			if (page.fallbackPageId == page.pageId)
			{
				blobHeader.flags |=
					kAnimatedVirtualPageFlagPinnedFallback;
				rootPageIds.push_back(page.pageId);
			}
			blobHeader.pageId = page.pageId;
			blobHeader.sectionCount =
				static_cast<std::uint32_t>(sections.size());
			blobHeader.sectionTableOffset =
				sizeof(AnimatedVirtualPageBlobHeader);
			blobHeader.meshPayloadGeneration =
				meshPayloadGeneration;
			std::uint64_t contentGeneration =
				HashAnimatedVirtualCookBytes(
					reinterpret_cast<const std::uint8_t*>(
						&page.pageId),
					sizeof(page.pageId),
					meshPayloadGeneration);
			for (const AnimatedVirtualPageCookedSection& section :
				sections)
			{
				contentGeneration = HashAnimatedVirtualCookBytes(
					section.bytes.data(), section.bytes.size(),
					contentGeneration);
			}
			blobHeader.contentGeneration = contentGeneration;
			blobHeader.skeletonCompatibilityFingerprint =
				settings.skeletonCompatibilityFingerprint;
			blobHeader.contentHash = contentGeneration;

			std::uint32_t blobCursor = AlignUp(
				static_cast<std::uint32_t>(
					sizeof(AnimatedVirtualPageBlobHeader)
					+ sections.size()
						* sizeof(AnimatedVirtualPageSectionRecord)),
				16u);
			std::vector<AnimatedVirtualPageSectionRecord>
				sectionRecords;
			sectionRecords.reserve(sections.size());
			for (const AnimatedVirtualPageCookedSection& section :
				sections)
			{
				blobCursor = AlignUp(blobCursor, 16u);
				sectionRecords.push_back({
					static_cast<std::uint32_t>(section.type),
					section.flags,
					blobCursor,
					section.bytes.size(),
					section.elementCount,
					section.elementStride });
				if (section.bytes.size()
					> std::numeric_limits<std::uint32_t>::max()
						- blobCursor)
				{
					return false;
				}
				blobCursor +=
					static_cast<std::uint32_t>(section.bytes.size());
			}
			blobHeader.byteSize = blobCursor;
			cooked.bytes.resize(blobCursor, 0u);
			std::memcpy(
				cooked.bytes.data(), &blobHeader,
				sizeof(blobHeader));
			std::memcpy(
				cooked.bytes.data()
					+ blobHeader.sectionTableOffset,
				sectionRecords.data(),
				sectionRecords.size()
					* sizeof(AnimatedVirtualPageSectionRecord));
			for (std::size_t index = 0u;
				index < sections.size(); ++index)
			{
				std::memcpy(
					cooked.bytes.data()
						+ sectionRecords[index].byteOffset,
					sections[index].bytes.data(),
					sections[index].bytes.size());
			}

			const DeformableVirtualGeometryClusterRecord&
				deformable =
					deformableClusters[clusterIt->clusterId];
			cooked.entry.pageId = page.pageId;
			cooked.entry.parentPageId =
				clusterIt->parentClusterId
					== kVirtualGeometryInvalidIndex
				? kVirtualGeometryInvalidIndex
				: clusters[clusterIt->parentClusterId].pageId;
			cooked.entry.fallbackPageId =
				page.fallbackPageId;
			cooked.entry.flags = blobHeader.flags;
			cooked.entry.blobSize = cooked.bytes.size();
			cooked.entry.blobHash =
				HashAnimatedVirtualCookBytes(
					cooked.bytes.data(), cooked.bytes.size());
			cooked.entry.contentGeneration =
				contentGeneration;
			cooked.entry.clusterOffset =
				clusterIt->clusterId;
			cooked.entry.clusterCount = 1u;
			cooked.entry.materialSectionId =
				clusterIt->materialSectionId;
			cooked.entry.skeletonCompatibilityFingerprint =
				settings.skeletonCompatibilityFingerprint;
			std::copy_n(
				deformable.deformationBoundsMin, 3u,
				cooked.entry.conservativeBoundsMin);
			std::copy_n(
				deformable.deformationBoundsMax, 3u,
				cooked.entry.conservativeBoundsMax);
			cooked.entry.geometricError =
				clusterIt->geometricError;
			cooked.entry.deformationError =
				deformable.deformationError;
			output.push_back(std::move(cooked));
		}
		return !output.empty() && !rootPageIds.empty();
	}

	bool TryWriteAnimatedVirtualPageDirectory(
		std::fstream& stream,
		std::uint64_t extensionEnd,
		std::uint64_t meshPayloadGeneration,
		std::vector<AnimatedVirtualPageCookedBlob>& pages,
		const std::vector<std::uint32_t>& rootPageIds,
		std::uint64_t& outputEnd)
	{
		std::uint32_t directoryOffset = 0u;
		if (!TryAlignSkinnedOffset(
			extensionEnd, 16u, directoryOffset)
			|| pages.empty() || rootPageIds.empty())
		{
			return false;
		}
		const std::uint64_t pageTableOffset =
			static_cast<std::uint64_t>(directoryOffset)
			+ sizeof(AnimatedVirtualPageDirectoryHeader);
		const std::uint64_t rootTableOffset =
			pageTableOffset
			+ pages.size()
				* sizeof(AnimatedVirtualPageDirectoryEntry);
		std::uint32_t blobCursor = 0u;
		if (!TryAlignSkinnedOffset(
			rootTableOffset
				+ rootPageIds.size()
					* sizeof(std::uint32_t),
			16u, blobCursor))
		{
			return false;
		}
		for (AnimatedVirtualPageCookedBlob& page : pages)
		{
			page.entry.blobOffset = blobCursor;
			if (page.bytes.size()
				> std::numeric_limits<std::uint32_t>::max()
					- blobCursor)
			{
				return false;
			}
			blobCursor = AlignUp(
				blobCursor
					+ static_cast<std::uint32_t>(
						page.bytes.size()),
				16u);
		}

		AnimatedVirtualPageDirectoryHeader directory{};
		directory.magic =
			kAnimatedVirtualPageDirectoryMagicValue;
		directory.version =
			kAnimatedVirtualPageFormatVersion;
		directory.headerSize =
			sizeof(AnimatedVirtualPageDirectoryHeader);
		directory.payloadSize =
			static_cast<std::uint32_t>(
				rootTableOffset
				+ rootPageIds.size()
					* sizeof(std::uint32_t)
				- directoryOffset);
		directory.pageCount =
			static_cast<std::uint32_t>(pages.size());
		directory.pageTableOffset =
			static_cast<std::uint32_t>(pageTableOffset);
		directory.rootPageCount =
			static_cast<std::uint32_t>(rootPageIds.size());
		directory.rootPageTableOffset =
			static_cast<std::uint32_t>(rootTableOffset);
		directory.flags =
			kAnimatedVirtualPageFlagAnimated
			| kAnimatedVirtualPageFlagHasSkinning;
		directory.meshPayloadGeneration =
			meshPayloadGeneration;
		std::vector<AnimatedVirtualPageDirectoryEntry> entries;
		entries.reserve(pages.size());
		for (const AnimatedVirtualPageCookedBlob& page : pages)
		{
			entries.push_back(page.entry);
		}
		directory.directoryHash =
			HashAnimatedVirtualCookBytes(
				reinterpret_cast<const std::uint8_t*>(
					entries.data()),
				entries.size()
					* sizeof(
						AnimatedVirtualPageDirectoryEntry));
		directory.directoryHash =
			HashAnimatedVirtualCookBytes(
				reinterpret_cast<const std::uint8_t*>(
					rootPageIds.data()),
				rootPageIds.size() * sizeof(std::uint32_t),
				directory.directoryHash);

		stream.clear();
		stream.seekp(0, std::ios::end);
		const std::streamoff current = stream.tellp();
		if (current < 0
			|| static_cast<std::uint64_t>(current)
				> directoryOffset)
		{
			return false;
		}
		std::vector<char> padding(
			static_cast<std::size_t>(
				directoryOffset
				- static_cast<std::uint64_t>(current)),
			0);
		stream.write(
			padding.data(),
			static_cast<std::streamsize>(padding.size()));
		stream.write(
			reinterpret_cast<const char*>(&directory),
			sizeof(directory));
		stream.write(
			reinterpret_cast<const char*>(entries.data()),
			static_cast<std::streamsize>(
				entries.size()
				* sizeof(
					AnimatedVirtualPageDirectoryEntry)));
		stream.write(
			reinterpret_cast<const char*>(rootPageIds.data()),
			static_cast<std::streamsize>(
				rootPageIds.size()
					* sizeof(std::uint32_t)));
		for (const AnimatedVirtualPageCookedBlob& page : pages)
		{
			const std::streamoff beforePage = stream.tellp();
			if (beforePage < 0
				|| static_cast<std::uint64_t>(beforePage)
					> page.entry.blobOffset)
			{
				return false;
			}
			padding.assign(
				static_cast<std::size_t>(
					page.entry.blobOffset
						- static_cast<std::uint64_t>(
							beforePage)),
				0);
			stream.write(
				padding.data(),
				static_cast<std::streamsize>(padding.size()));
			stream.write(
				reinterpret_cast<const char*>(
					page.bytes.data()),
				static_cast<std::streamsize>(
					page.bytes.size()));
		}
		stream.flush();
		outputEnd = static_cast<std::uint64_t>(
			stream.tellp());
		return static_cast<bool>(stream)
			&& outputEnd
				<= std::numeric_limits<std::uint32_t>::max();
	}

	bool TryAppendDeformablePayload(
		const std::filesystem::path& stagedPath,
		const SkinnedMeshCookInput& input,
		const SkinnedMeshCookSettings& settings,
		const std::vector<SkinnedMeshPalettePartition>& palettes,
		const std::vector<std::uint32_t>& vertexPaletteIndices,
		const std::vector<std::uint8_t>& skinningBytes,
		SkinnedMeshDeformableTemfSummary& outSummary,
		SkinnedMeshCookFailure& outFailure)
	{
		outFailure = SkinnedMeshCookFailure::StagedPayloadWriteFailed;
		std::fstream stream(stagedPath, std::ios::binary | std::ios::in | std::ios::out);
		if (!stream)
		{
			return false;
		}

		MeshHeader header{};
		stream.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!stream || !HasTemfMagic(header) || header.version != kMeshCurrentVersion
			|| header.headerSize != sizeof(MeshHeader))
		{
			outFailure = SkinnedMeshCookFailure::FormatCompatibilityMismatch;
			return false;
		}

		std::error_code sizeError;
		const std::uintmax_t stagedSize = std::filesystem::file_size(stagedPath, sizeError);
		if (sizeError || stagedSize < sizeof(MeshHeader)
			|| stagedSize > static_cast<std::uintmax_t>(std::numeric_limits<std::uint32_t>::max()))
		{
			outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
			return false;
		}

		std::size_t expectedSkinningBytes = 0u;
		if (!TryMultiplySkinnedSize(input.vertices.size(), kDeformableMeshSkinningStrideBytes, expectedSkinningBytes)
			|| expectedSkinningBytes > std::numeric_limits<std::uint32_t>::max()
			|| skinningBytes.size() != expectedSkinningBytes
			|| vertexPaletteIndices.size() != input.vertices.size()
			|| header.meshletVertexCount != input.vertices.size()
			|| header.meshletSkinningDataOffset != 0u
			|| header.meshletSkinningDataCount != 0u
			|| header.virtualGeometryReserved != 0u)
		{
			outFailure = SkinnedMeshCookFailure::StaticVertexLayoutIncompatible;
			return false;
		}

		std::uint32_t skinningOffset = 0u;
		std::uint32_t metadataOffset = 0u;
		if (!TryAlignSkinnedOffset(stagedSize, kMeshletVertexDataAlignmentBytes, skinningOffset)
			|| !TryAlignSkinnedOffset(
				static_cast<std::uint64_t>(skinningOffset) + skinningBytes.size(),
				kMeshletVertexDataAlignmentBytes,
				metadataOffset))
		{
			outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
			return false;
		}

		std::uint32_t vertexCount = 0u;
		std::uint32_t paletteCount = 0u;
		std::uint32_t morphCount = 0u;
		if (!TryConvertSkinnedCount(input.vertices.size(), vertexCount)
			|| !TryConvertSkinnedCount(palettes.size(), paletteCount)
			|| !TryConvertSkinnedCount(input.morphTargets.size(), morphCount))
		{
			outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
			return false;
		}
		for (std::uint32_t paletteIndex : vertexPaletteIndices)
		{
			if (paletteIndex >= paletteCount)
			{
				outFailure = SkinnedMeshCookFailure::PalettePartitionLimitExceeded;
				return false;
			}
		}

		std::vector<const SkinnedMeshMorphTarget*> orderedMorphs;
		orderedMorphs.reserve(input.morphTargets.size());
		for (const SkinnedMeshMorphTarget& morph : input.morphTargets)
		{
			orderedMorphs.push_back(&morph);
		}
		std::sort(orderedMorphs.begin(), orderedMorphs.end(), [](const SkinnedMeshMorphTarget* left, const SkinnedMeshMorphTarget* right) {
			return left->stableTargetId < right->stableTargetId;
		});
		for (std::size_t index = 1u; index < orderedMorphs.size(); ++index)
		{
			if (orderedMorphs[index - 1u]->stableTargetId == orderedMorphs[index]->stableTargetId)
			{
				outFailure = SkinnedMeshCookFailure::DuplicateMorphTarget;
				return false;
			}
		}

		std::vector<DeformableMeshPaletteRecord> paletteRecords(palettes.size());
		std::vector<DeformableMeshMorphRecord> morphRecords(orderedMorphs.size());
		std::uint64_t tableCursor = static_cast<std::uint64_t>(metadataOffset) + sizeof(DeformableMeshPayloadHeader)
			+ static_cast<std::uint64_t>(paletteRecords.size()) * sizeof(DeformableMeshPaletteRecord)
			+ static_cast<std::uint64_t>(morphRecords.size()) * sizeof(DeformableMeshMorphRecord);
		if (tableCursor > std::numeric_limits<std::uint32_t>::max())
		{
			outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
			return false;
		}

		for (std::size_t index = 0u; index < palettes.size(); ++index)
		{
			std::uint32_t jointCount = 0u;
			if (palettes[index].sourceJointIds.size() > kDeformableMeshMaximumPaletteJoints
				|| !TryConvertSkinnedCount(palettes[index].sourceJointIds.size(), jointCount)
				|| tableCursor + static_cast<std::uint64_t>(jointCount) * sizeof(std::uint32_t) > std::numeric_limits<std::uint32_t>::max())
			{
				outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
				return false;
			}
			paletteRecords[index] = { static_cast<std::uint32_t>(tableCursor), jointCount };
			tableCursor += static_cast<std::uint64_t>(jointCount) * sizeof(std::uint32_t);
		}

		const std::uint32_t vertexPaletteIndexOffset = static_cast<std::uint32_t>(tableCursor);
		const std::uint64_t vertexPaletteBytes = static_cast<std::uint64_t>(vertexPaletteIndices.size()) * sizeof(std::uint32_t);
		if (tableCursor + vertexPaletteBytes > std::numeric_limits<std::uint32_t>::max())
		{
			outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
			return false;
		}
		tableCursor += vertexPaletteBytes;

		for (std::size_t index = 0u; index < orderedMorphs.size(); ++index)
		{
			const SkinnedMeshMorphTarget& morph = *orderedMorphs[index];
			std::uint32_t deltaCount = 0u;
			if (!TryConvertSkinnedCount(morph.positionDeltas.size(), deltaCount)
				|| tableCursor + static_cast<std::uint64_t>(deltaCount) * sizeof(std::array<float, 3>) > std::numeric_limits<std::uint32_t>::max())
			{
				outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
				return false;
			}
			morphRecords[index] = {
				morph.stableTargetId,
				static_cast<std::uint32_t>(tableCursor),
				deltaCount,
				static_cast<std::uint32_t>(sizeof(std::array<float, 3>)),
				kDeformableMeshPayloadFlagDenseMorphPositionDeltas
			};
			tableCursor += static_cast<std::uint64_t>(deltaCount) * sizeof(std::array<float, 3>);
		}
		if (tableCursor - metadataOffset > std::numeric_limits<std::uint32_t>::max())
		{
			outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
			return false;
		}
		// Keep TDEF v1's payloadSize frozen at tableCursor. TDFV starts immediately
		// afterward, allowing historical readers to validate and consume the base
		// skin/morph data unchanged while current loaders opt into cluster facts.
		const std::uint32_t sidecarOffset = static_cast<std::uint32_t>(tableCursor);
		DeformableVirtualGeometrySidecarHeader virtualGeometrySidecar{};
		std::vector<DeformableVirtualGeometryClusterRecord> virtualGeometryClusters;
		std::vector<std::uint32_t> virtualGeometryPaletteIndices;
		std::vector<DeformableVirtualGeometryMorphRecord> virtualGeometryMorphs;
		std::vector<DeformableVirtualGeometryMorphVertex> virtualGeometryMorphVertices;
		if (!TryBuildDeformableVirtualGeometrySidecar(
			stream,
			static_cast<std::uint32_t>(stagedSize),
			header,
			input,
			orderedMorphs,
			vertexPaletteIndices,
			skinningBytes,
			sidecarOffset,
			virtualGeometrySidecar,
			virtualGeometryClusters,
			virtualGeometryPaletteIndices,
			virtualGeometryMorphs,
			virtualGeometryMorphVertices))
		{
			outFailure = SkinnedMeshCookFailure::StagedPayloadWriteFailed;
			return false;
		}
		const std::uint64_t stagedPayloadEnd = static_cast<std::uint64_t>(sidecarOffset)
			+ virtualGeometrySidecar.payloadSize;
		if (stagedPayloadEnd > std::numeric_limits<std::uint32_t>::max())
		{
			outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
			return false;
		}

		DeformableMeshPayloadHeader metadata{};
		metadata.magic = kDeformableMeshPayloadMagicValue;
		metadata.version = kDeformableMeshPayloadVersion;
		metadata.headerSize = static_cast<std::uint16_t>(sizeof(DeformableMeshPayloadHeader));
		metadata.payloadSize = static_cast<std::uint32_t>(tableCursor - metadataOffset);
		metadata.compatibleMeshVersion = kMeshCurrentVersion;
		metadata.skinningVertexCount = vertexCount;
		metadata.palettePartitionCount = paletteCount;
		metadata.morphTargetCount = morphCount;
		metadata.paletteTableOffset = metadataOffset + static_cast<std::uint32_t>(sizeof(DeformableMeshPayloadHeader));
		metadata.morphTableOffset = metadata.paletteTableOffset
			+ static_cast<std::uint32_t>(paletteRecords.size() * sizeof(DeformableMeshPaletteRecord));
		metadata.vertexPaletteIndexOffset = vertexPaletteIndexOffset;
		metadata.vertexPaletteIndexCount = vertexCount;
		metadata.skinningDataOffset = skinningOffset;
		metadata.skinningDataSize = static_cast<std::uint32_t>(skinningBytes.size());
		metadata.skinningStrideBytes = kDeformableMeshSkinningStrideBytes;
		metadata.maximumInfluences = kDeformableMeshMaximumInfluences;
		metadata.maximumPaletteJoints = kDeformableMeshMaximumPaletteJoints;
		metadata.sourceSkeletonJointCount = settings.sourceSkeletonJointCount;
		metadata.skeletonCompatibilityFingerprint = settings.skeletonCompatibilityFingerprint;
		metadata.flags = kDeformableMeshPayloadFlagConservativeMorphEnvelope
			| kDeformableMeshPayloadFlagZeroWeightMeansIdentity
			| kDeformableMeshPayloadFlagVertexPaletteIndices
			| kDeformableMeshPayloadFlagDenseMorphPositionDeltas;
		metadata.virtualGeometryDeformationFlags = kDeformableMeshVirtualGeometryFlagWholeMeshEnvelope
			| kDeformableMeshVirtualGeometryFlagDisableUnprovenConeCulling
			| kDeformableMeshVirtualGeometryFlagRequireCompleteFallback;
		for (std::size_t component = 0u; component < 3u; ++component)
		{
			metadata.deformationBoundsMin[component] = std::numeric_limits<float>::max();
			metadata.deformationBoundsMax[component] = std::numeric_limits<float>::lowest();
		}
		for (std::size_t vertexIndex = 0u; vertexIndex < input.vertices.size(); ++vertexIndex)
		{
			const SkinnedMeshCookVertex& vertex = input.vertices[vertexIndex];
			for (std::size_t component = 0u; component < 3u; ++component)
			{
				// Multiple morph targets can be active together. Summing all negative
				// and positive deltas produces a conservative [0,1]-weight envelope;
				// sampling each target independently would underestimate combinations.
				float minimum = vertex.baseVertex.position[component];
				float maximum = vertex.baseVertex.position[component];
				for (const SkinnedMeshMorphTarget* morph : orderedMorphs)
				{
					const float delta = morph->positionDeltas[vertexIndex][component];
					minimum += std::min(delta, 0.0f);
					maximum += std::max(delta, 0.0f);
				}
				metadata.deformationBoundsMin[component] = std::min(metadata.deformationBoundsMin[component], minimum);
				metadata.deformationBoundsMax[component] = std::max(metadata.deformationBoundsMax[component], maximum);
			}
		}

		// Match ResourceManager's accepted deformation generation byte-for-byte:
		// skin stream first, followed by the complete frozen TDEF v1 payload.
		std::uint64_t meshPayloadGeneration = 14695981039346656037ull;
		const auto HashGenerationBytes = [&meshPayloadGeneration](
			const void* bytes, std::size_t byteCount)
		{
			const std::uint8_t* values =
				static_cast<const std::uint8_t*>(bytes);
			for (std::size_t index = 0u; index < byteCount; ++index)
			{
				meshPayloadGeneration ^= values[index];
				meshPayloadGeneration *= 1099511628211ull;
			}
		};
		HashGenerationBytes(skinningBytes.data(), skinningBytes.size());
		HashGenerationBytes(&metadata, sizeof(metadata));
		HashGenerationBytes(
			paletteRecords.data(),
			paletteRecords.size() * sizeof(DeformableMeshPaletteRecord));
		HashGenerationBytes(
			morphRecords.data(),
			morphRecords.size() * sizeof(DeformableMeshMorphRecord));
		for (const SkinnedMeshPalettePartition& palette : palettes)
		{
			HashGenerationBytes(
				palette.sourceJointIds.data(),
				palette.sourceJointIds.size() * sizeof(std::uint32_t));
		}
		HashGenerationBytes(
			vertexPaletteIndices.data(),
			vertexPaletteIndices.size() * sizeof(std::uint32_t));
		for (const SkinnedMeshMorphTarget* morph : orderedMorphs)
		{
			HashGenerationBytes(
				morph->positionDeltas.data(),
				morph->positionDeltas.size()
					* sizeof(std::array<float, 3>));
		}
		if (meshPayloadGeneration == 0u)
		{
			meshPayloadGeneration = 1u;
		}
		std::vector<AnimatedVirtualPageCookedBlob>
			animatedPageBlobs;
		std::vector<std::uint32_t> animatedRootPageIds;
		const bool hasAnimatedPhysicalPages =
			TryBuildAnimatedVirtualPageBlobs(
			stream,
			header,
			input,
			settings,
			palettes,
			vertexPaletteIndices,
			skinningBytes,
			orderedMorphs,
			virtualGeometryClusters,
			meshPayloadGeneration,
			animatedPageBlobs,
			animatedRootPageIds);

		// Append all extension data before rewriting the small root header. A crash
		// can only damage this private staged file; the published TEMF is untouched.
		stream.clear();
		stream.seekp(0, std::ios::end);
		const std::streamoff currentEnd = stream.tellp();
		if (currentEnd < 0 || static_cast<std::uint64_t>(currentEnd) > skinningOffset)
		{
			return false;
		}
		std::vector<char> padding(static_cast<std::size_t>(skinningOffset - static_cast<std::uint64_t>(currentEnd)), 0);
		stream.write(padding.data(), static_cast<std::streamsize>(padding.size()));
		stream.write(reinterpret_cast<const char*>(skinningBytes.data()), static_cast<std::streamsize>(skinningBytes.size()));
		const std::uint64_t afterSkinning = static_cast<std::uint64_t>(skinningOffset) + skinningBytes.size();
		padding.assign(static_cast<std::size_t>(metadataOffset - afterSkinning), 0);
		stream.write(padding.data(), static_cast<std::streamsize>(padding.size()));
		stream.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
		stream.write(reinterpret_cast<const char*>(paletteRecords.data()), static_cast<std::streamsize>(paletteRecords.size() * sizeof(DeformableMeshPaletteRecord)));
		stream.write(reinterpret_cast<const char*>(morphRecords.data()), static_cast<std::streamsize>(morphRecords.size() * sizeof(DeformableMeshMorphRecord)));
		for (const SkinnedMeshPalettePartition& palette : palettes)
		{
			stream.write(reinterpret_cast<const char*>(palette.sourceJointIds.data()), static_cast<std::streamsize>(palette.sourceJointIds.size() * sizeof(std::uint32_t)));
		}
		stream.write(reinterpret_cast<const char*>(vertexPaletteIndices.data()), static_cast<std::streamsize>(vertexPaletteIndices.size() * sizeof(std::uint32_t)));
		for (const SkinnedMeshMorphTarget* morph : orderedMorphs)
		{
			stream.write(reinterpret_cast<const char*>(morph->positionDeltas.data()), static_cast<std::streamsize>(morph->positionDeltas.size() * sizeof(std::array<float, 3>)));
		}
		stream.write(reinterpret_cast<const char*>(&virtualGeometrySidecar), sizeof(virtualGeometrySidecar));
		stream.write(
			reinterpret_cast<const char*>(virtualGeometryClusters.data()),
			static_cast<std::streamsize>(virtualGeometryClusters.size() * sizeof(DeformableVirtualGeometryClusterRecord)));
		stream.write(
			reinterpret_cast<const char*>(virtualGeometryPaletteIndices.data()),
			static_cast<std::streamsize>(virtualGeometryPaletteIndices.size() * sizeof(std::uint32_t)));
		stream.write(
			reinterpret_cast<const char*>(virtualGeometryMorphs.data()),
			static_cast<std::streamsize>(virtualGeometryMorphs.size() * sizeof(DeformableVirtualGeometryMorphRecord)));
		stream.write(
			reinterpret_cast<const char*>(virtualGeometryMorphVertices.data()),
			static_cast<std::streamsize>(virtualGeometryMorphVertices.size() * sizeof(DeformableVirtualGeometryMorphVertex)));
		const std::streamoff writtenEnd = stream.tellp();
		stream.flush();
		if (!stream || writtenEnd < 0 || static_cast<std::uint64_t>(writtenEnd) != stagedPayloadEnd)
		{
			return false;
		}
		if (hasAnimatedPhysicalPages)
		{
			std::uint64_t animatedPayloadEnd = 0u;
			if (!TryWriteAnimatedVirtualPageDirectory(
				stream,
				stagedPayloadEnd,
				meshPayloadGeneration,
				animatedPageBlobs,
				animatedRootPageIds,
				animatedPayloadEnd)
				|| animatedPayloadEnd <= stagedPayloadEnd)
			{
				outFailure =
					SkinnedMeshCookFailure::StagedPayloadWriteFailed;
				return false;
			}
		}

		// Read the staged header back from disk before publishing its root pointer.
		// This catches short writes, stale layouts, and—critically—a skeleton
		// fingerprint that differs from the generation supplied by the importer.
		DeformableMeshPayloadHeader stagedMetadata{};
		stream.clear();
		stream.seekg(static_cast<std::streamoff>(metadataOffset), std::ios::beg);
		stream.read(reinterpret_cast<char*>(&stagedMetadata), sizeof(stagedMetadata));
		if (!stream
			|| stagedMetadata.magic != kDeformableMeshPayloadMagicValue
			|| stagedMetadata.version != kDeformableMeshPayloadVersion
			|| stagedMetadata.headerSize != sizeof(DeformableMeshPayloadHeader)
			|| stagedMetadata.compatibleMeshVersion != kMeshCurrentVersion
			|| stagedMetadata.skeletonCompatibilityFingerprint != settings.skeletonCompatibilityFingerprint
			|| stagedMetadata.maximumInfluences != kDeformableMeshMaximumInfluences
			|| stagedMetadata.maximumPaletteJoints != kDeformableMeshMaximumPaletteJoints
			|| stagedMetadata.skinningStrideBytes != kDeformableMeshSkinningStrideBytes)
		{
			outFailure = SkinnedMeshCookFailure::FormatCompatibilityMismatch;
			return false;
		}

		const auto RangeWithin = [](std::uint32_t offset, std::uint64_t byteCount, std::uint64_t rangeBegin, std::uint64_t rangeEnd) {
			const std::uint64_t begin = offset;
			return begin >= rangeBegin && begin <= rangeEnd && byteCount <= (rangeEnd - begin);
		};
		const std::uint64_t payloadEnd = static_cast<std::uint64_t>(metadataOffset) + stagedMetadata.payloadSize;
		const std::uint64_t paletteTableBytes = static_cast<std::uint64_t>(stagedMetadata.palettePartitionCount)
			* sizeof(DeformableMeshPaletteRecord);
		const std::uint64_t morphTableBytes = static_cast<std::uint64_t>(stagedMetadata.morphTargetCount)
			* sizeof(DeformableMeshMorphRecord);
		const std::uint64_t vertexPaletteTableBytes = static_cast<std::uint64_t>(stagedMetadata.vertexPaletteIndexCount)
			* sizeof(std::uint32_t);
		if (payloadEnd != tableCursor
			|| stagedMetadata.skinningVertexCount != vertexCount
			|| stagedMetadata.sourceSkeletonJointCount != settings.sourceSkeletonJointCount
			|| stagedMetadata.skinningDataOffset != skinningOffset
			|| stagedMetadata.skinningDataSize != skinningBytes.size()
			|| !RangeWithin(stagedMetadata.skinningDataOffset, stagedMetadata.skinningDataSize, skinningOffset, metadataOffset)
			|| !RangeWithin(stagedMetadata.paletteTableOffset, paletteTableBytes, metadataOffset, payloadEnd)
			|| !RangeWithin(stagedMetadata.morphTableOffset, morphTableBytes, metadataOffset, payloadEnd)
			|| !RangeWithin(stagedMetadata.vertexPaletteIndexOffset, vertexPaletteTableBytes, metadataOffset, payloadEnd))
		{
			outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
			return false;
		}
		for (const DeformableMeshPaletteRecord& record : paletteRecords)
		{
			if (record.jointCount == 0u || record.jointCount > kDeformableMeshMaximumPaletteJoints
				|| !RangeWithin(
					record.jointTableOffset,
					static_cast<std::uint64_t>(record.jointCount) * sizeof(std::uint32_t),
					metadataOffset,
					payloadEnd))
			{
				outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
				return false;
			}
		}
		for (const DeformableMeshMorphRecord& record : morphRecords)
		{
			if (record.positionDeltaStrideBytes != sizeof(std::array<float, 3>)
				|| record.positionDeltaCount != vertexCount
				|| !RangeWithin(
					record.positionDeltaOffset,
					static_cast<std::uint64_t>(record.positionDeltaCount) * record.positionDeltaStrideBytes,
					metadataOffset,
					payloadEnd))
			{
				outFailure = SkinnedMeshCookFailure::PayloadRangeOverflow;
				return false;
			}
		}

		header.meshletSkinningDataOffset = skinningOffset;
		header.meshletSkinningDataCount = vertexCount;
		header.virtualGeometryReserved = metadataOffset;
		stream.seekp(0, std::ios::beg);
		stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
		stream.flush();
		if (!stream)
		{
			return false;
		}

		outSummary.hasSkinning = true;
		outSummary.morphTargetCount = morphCount;
		outSummary.palettePartitionCount = paletteCount;
		outSummary.deformationEnvelopeValid = true;
		outSummary.zeroWeightIdentityEncoded = true;
		outSummary.virtualGeometryFailsOpen = true;
		outSummary.virtualGeometryClusterCount = virtualGeometrySidecar.clusterCount;
		outSummary.skeletonCompatibilityValidated = true;
		outSummary.skeletonCompatibilityFingerprint = metadata.skeletonCompatibilityFingerprint;
		outFailure = SkinnedMeshCookFailure::None;
		return true;
	}

	bool ReplaceStagedTemf(const std::filesystem::path& stagedPath, const std::filesystem::path& outputPath)
	{
#ifdef _WIN32
		// MoveFileEx performs the protected replace in one filesystem operation;
		// callers never delete the known-good output before this succeeds.
		return MoveFileExW(stagedPath.c_str(), outputPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
		std::error_code error;
		std::filesystem::rename(stagedPath, outputPath, error);
		return !error;
#endif
	}
}

bool CompileSkinnedMeshFromRaw(
	const std::filesystem::path& logicalSource,
	const std::filesystem::path& output,
	const SkinnedMeshCookInput& input,
	SkinnedMeshCookReport* outReport,
	const SkinnedMeshCookSettings& settings)
{
	SkinnedMeshCookReport report{};
	report.skeletonCompatibilityFingerprint = settings.skeletonCompatibilityFingerprint;
	std::error_code existenceError;
	const bool hadLastKnownGood = std::filesystem::exists(output, existenceError) && !existenceError;
	const auto SetDiagnostic = [&](std::string code, std::string message, std::string correction) {
		report.diagnosticCode = std::move(code);
		report.diagnosticMessage = std::move(message);
		report.recommendedCorrection = std::move(correction);
	};
	const auto Finish = [&](bool success, SkinnedMeshCookFailure failure) {
		report.failure = failure;
		report.published = success;
		report.preservedLastKnownGood = !success && hadLastKnownGood;
		if (outReport != nullptr)
		{
			*outReport = report;
		}
		return success;
	};

	if (input.vertices.empty() || input.vertices.size() > std::numeric_limits<std::uint32_t>::max()
		|| input.indices.empty() || (input.indices.size() % 3u) != 0u
		|| settings.sourceSkeletonJointCount == 0u || settings.maximumPalettePartitions == 0u)
	{
		SetDiagnostic(
			"SkinnedMeshInvalidInput",
			"Skinned mesh cooking requires used vertices, triangle indices, a nonzero identity-joint domain, and at least one permitted palette.",
			"Triangulate the mesh, remove empty primitives, and verify the skeleton and palette cook settings.");
		return Finish(false, SkinnedMeshCookFailure::InvalidInput);
	}
	for (std::uint32_t index : input.indices)
	{
		if (index >= input.vertices.size())
		{
			SetDiagnostic(
				"SkinnedMeshIndexOutOfRange",
				"A triangle references a vertex outside the decoded vertex array.",
				"Re-export the source mesh or repair the primitive index buffer before importing.");
			return Finish(false, SkinnedMeshCookFailure::InvalidInput);
		}
	}

	report.cookedVertices.reserve(input.vertices.size());
	for (std::size_t sourceVertexIndex = 0u; sourceVertexIndex < input.vertices.size(); ++sourceVertexIndex)
	{
		const SkinnedMeshCookVertex& sourceVertex = input.vertices[sourceVertexIndex];
		const RawMeshVertex& baseVertex = sourceVertex.baseVertex;
		const bool finiteBaseVertex = std::all_of(baseVertex.position.begin(), baseVertex.position.end(), [](float value) { return std::isfinite(value); })
			&& std::all_of(baseVertex.normal.begin(), baseVertex.normal.end(), [](float value) { return std::isfinite(value); })
			&& std::all_of(baseVertex.uv.begin(), baseVertex.uv.end(), [](float value) { return std::isfinite(value); });
		if (!finiteBaseVertex)
		{
			report.failingVertexIndex = static_cast<std::uint32_t>(sourceVertexIndex);
			SetDiagnostic(
				"SkinnedMeshNonFiniteVertex",
				"A skinned vertex contains a non-finite position, normal, or texture coordinate.",
				"Repair or re-export the source geometry so every vertex attribute is finite.");
			return Finish(false, SkinnedMeshCookFailure::InvalidInput);
		}

		std::vector<SkinnedMeshInfluence> merged = sourceVertex.influences;
		for (const SkinnedMeshInfluence& influence : merged)
		{
			if (!std::isfinite(influence.weight) || influence.weight < 0.0f)
			{
				report.failingVertexIndex = static_cast<std::uint32_t>(sourceVertexIndex);
				report.failingJointIndex = influence.jointIndex;
				SetDiagnostic(
					"SkinnedMeshInvalidInfluenceWeight",
					"A skin influence has a negative or non-finite weight.",
					"Normalize the source skin weights and remove NaN, infinity, and negative values before reimporting.");
				return Finish(false, SkinnedMeshCookFailure::InvalidInfluenceWeight);
			}
			if (influence.weight > 0.0f && influence.jointIndex >= settings.sourceSkeletonJointCount)
			{
				report.failingVertexIndex = static_cast<std::uint32_t>(sourceVertexIndex);
				report.failingJointIndex = influence.jointIndex;
				SetDiagnostic(
					"SkinnedMeshMissingJoint",
					"Vertex " + std::to_string(sourceVertexIndex) + " references joint "
						+ std::to_string(influence.jointIndex) + ", but the source skeleton has only "
						+ std::to_string(settings.sourceSkeletonJointCount) + " joints.",
					"Restore the missing joint, remove the stale influence, or bind the mesh to the matching skeleton generation.");
				return Finish(false, SkinnedMeshCookFailure::MissingJoint);
			}
		}
		std::sort(merged.begin(), merged.end(), [](const SkinnedMeshInfluence& left, const SkinnedMeshInfluence& right) {
			return left.jointIndex < right.jointIndex;
		});
		std::vector<SkinnedMeshInfluence> uniqueInfluences;
		for (const SkinnedMeshInfluence& influence : merged)
		{
			if (!uniqueInfluences.empty() && uniqueInfluences.back().jointIndex == influence.jointIndex)
			{
				uniqueInfluences.back().weight += influence.weight;
				if (!std::isfinite(uniqueInfluences.back().weight))
				{
					report.failingVertexIndex = static_cast<std::uint32_t>(sourceVertexIndex);
					report.failingJointIndex = influence.jointIndex;
					SetDiagnostic(
						"SkinnedMeshInfluenceWeightOverflow",
						"Merged duplicate influences overflowed the supported finite weight range.",
						"Normalize duplicate joint weights in the source skin before reimporting.");
					return Finish(false, SkinnedMeshCookFailure::InvalidInfluenceWeight);
				}
			}
			else
			{
				uniqueInfluences.push_back(influence);
			}
		}
		uniqueInfluences.erase(
			std::remove_if(uniqueInfluences.begin(), uniqueInfluences.end(), [](const SkinnedMeshInfluence& influence) {
				return influence.weight == 0.0f;
			}),
			uniqueInfluences.end());

		float totalWeight = 0.0f;
		for (const SkinnedMeshInfluence& influence : uniqueInfluences)
		{
			totalWeight += influence.weight;
		}
		if (!std::isfinite(totalWeight))
		{
			report.failingVertexIndex = static_cast<std::uint32_t>(sourceVertexIndex);
			SetDiagnostic(
				"SkinnedMeshInfluenceWeightOverflow",
				"The combined influence weight is outside the supported finite range.",
				"Normalize skin weights in the source asset before reimporting.");
			return Finish(false, SkinnedMeshCookFailure::InvalidInfluenceWeight);
		}
		SkinnedMeshCookedVertex cooked{};
		if (!(totalWeight > 0.0f))
		{
			cooked.usesIdentityDeformation = true;
			report.zeroWeightVertexCount += 1u;
			report.cookedVertices.push_back(std::move(cooked));
			continue;
		}

		std::sort(uniqueInfluences.begin(), uniqueInfluences.end(), [](const SkinnedMeshInfluence& left, const SkinnedMeshInfluence& right) {
			return left.weight != right.weight ? left.weight > right.weight : left.jointIndex < right.jointIndex;
		});
		const std::size_t retainedCount = std::min<std::size_t>(kDeformableMeshMaximumInfluences, uniqueInfluences.size());
		float retainedWeight = 0.0f;
		for (std::size_t index = 0u; index < retainedCount; ++index)
		{
			retainedWeight += uniqueInfluences[index].weight;
		}
		if (!(retainedWeight > 0.0f) || !std::isfinite(retainedWeight))
		{
			report.failingVertexIndex = static_cast<std::uint32_t>(sourceVertexIndex);
			SetDiagnostic(
				"SkinnedMeshInfluenceWeightOverflow",
				"The retained influence sum is outside the supported finite range.",
				"Normalize skin weights in the source asset before reimporting.");
			return Finish(false, SkinnedMeshCookFailure::InvalidInfluenceWeight);
		}
		cooked.discardedWeight = totalWeight - retainedWeight;
		cooked.influences.assign(uniqueInfluences.begin(), uniqueInfluences.begin() + static_cast<std::ptrdiff_t>(retainedCount));
		std::sort(cooked.influences.begin(), cooked.influences.end(), [](const SkinnedMeshInfluence& left, const SkinnedMeshInfluence& right) {
			return left.jointIndex < right.jointIndex;
		});
		for (SkinnedMeshInfluence& influence : cooked.influences)
		{
			influence.weight /= retainedWeight;
			cooked.normalizedWeightSum += influence.weight;
			cooked.sourceJointIds.push_back(influence.jointIndex);
		}
		report.cookedVertices.push_back(std::move(cooked));
	}

	const bool hasBoundSkinning = std::any_of(
		report.cookedVertices.begin(),
		report.cookedVertices.end(),
		[](const SkinnedMeshCookedVertex& vertex) { return !vertex.usesIdentityDeformation; });
	if (hasBoundSkinning && settings.skeletonCompatibilityFingerprint == 0u)
	{
		SetDiagnostic(
			"SkinnedMeshMissingSkeletonCompatibilityFingerprint",
			"A mesh with retained skin influences does not identify the compatible cooked skeleton generation.",
			"Pass the nonzero compatibility fingerprint emitted by the package's SkeletonAsset cook.");
		return Finish(false, SkinnedMeshCookFailure::MissingSkeletonCompatibilityFingerprint);
	}

	std::vector<std::uint32_t> morphTargetIds;
	morphTargetIds.reserve(input.morphTargets.size());
	for (const SkinnedMeshMorphTarget& morph : input.morphTargets)
	{
		if (morph.positionDeltas.size() != input.vertices.size())
		{
			SetDiagnostic(
				"SkinnedMeshMorphVertexCountMismatch",
				"A morph target does not contain exactly one position delta per cooked vertex.",
				"Re-export the morph target with topology identical to the base mesh.");
			return Finish(false, SkinnedMeshCookFailure::InvalidMorphTarget);
		}
		if ((!morph.normalDeltas.empty() && morph.normalDeltas.size() != input.vertices.size())
			|| (!morph.tangentDeltas.empty() && morph.tangentDeltas.size() != input.vertices.size()))
		{
			SetDiagnostic(
				"SkinnedMeshMorphAttributeVertexCountMismatch",
				"An optional morph normal or tangent channel does not contain one delta per cooked vertex.",
				"Re-export every supplied morph attribute with topology identical to the base mesh.");
			return Finish(false, SkinnedMeshCookFailure::InvalidMorphTarget);
		}
		for (const std::array<float, 3>& delta : morph.positionDeltas)
		{
			if (!std::isfinite(delta[0]) || !std::isfinite(delta[1]) || !std::isfinite(delta[2]))
			{
				SetDiagnostic(
					"SkinnedMeshNonFiniteMorphDelta",
					"A morph target contains a non-finite position delta.",
					"Repair or re-export the morph so every delta is finite.");
				return Finish(false, SkinnedMeshCookFailure::InvalidMorphTarget);
			}
		}
		const auto HasNonFiniteDelta = [](const std::vector<std::array<float, 3>>& deltas) {
			return std::any_of(deltas.begin(), deltas.end(), [](const std::array<float, 3>& delta) {
				return !std::isfinite(delta[0]) || !std::isfinite(delta[1]) || !std::isfinite(delta[2]);
			});
		};
		if (HasNonFiniteDelta(morph.normalDeltas) || HasNonFiniteDelta(morph.tangentDeltas))
		{
			SetDiagnostic(
				"SkinnedMeshNonFiniteMorphAttributeDelta",
				"A morph normal or tangent delta contains a non-finite component.",
				"Repair or re-export optional morph attributes so every supplied delta is finite.");
			return Finish(false, SkinnedMeshCookFailure::InvalidMorphTarget);
		}
		morphTargetIds.push_back(morph.stableTargetId);
	}
	std::sort(morphTargetIds.begin(), morphTargetIds.end());
	if (std::adjacent_find(morphTargetIds.begin(), morphTargetIds.end()) != morphTargetIds.end())
	{
		SetDiagnostic(
			"SkinnedMeshDuplicateMorphTarget",
			"Two morph streams use the same stable target identifier.",
			"Assign unique semantic morph target identifiers before cooking the package.");
		return Finish(false, SkinnedMeshCookFailure::DuplicateMorphTarget);
	}

	// Vertices shared by two triangles cannot use two unrelated palette-local
	// index spaces. Build topology-connected components first, then pack whole
	// components into deterministic palettes. This avoids silently corrupting a
	// shared vertex when the 256-joint boundary falls between adjacent triangles.
	std::vector<std::uint32_t> componentParent(input.vertices.size());
	std::iota(componentParent.begin(), componentParent.end(), 0u);
	const auto FindComponent = [&](std::uint32_t vertex) {
		std::uint32_t root = vertex;
		while (componentParent[root] != root)
		{
			root = componentParent[root];
		}
		while (componentParent[vertex] != vertex)
		{
			const std::uint32_t next = componentParent[vertex];
			componentParent[vertex] = root;
			vertex = next;
		}
		return root;
	};
	const auto UnionComponents = [&](std::uint32_t left, std::uint32_t right) {
		const std::uint32_t leftRoot = FindComponent(left);
		const std::uint32_t rightRoot = FindComponent(right);
		if (leftRoot != rightRoot)
		{
			// Always attach the larger root to the smaller root so source traversal
			// and thread scheduling cannot change component identity.
			componentParent[std::max(leftRoot, rightRoot)] = std::min(leftRoot, rightRoot);
		}
	};
	std::vector<bool> referencedVertices(input.vertices.size(), false);
	for (std::size_t triangle = 0u; triangle < input.indices.size(); triangle += 3u)
	{
		const std::uint32_t first = input.indices[triangle];
		const std::uint32_t second = input.indices[triangle + 1u];
		const std::uint32_t third = input.indices[triangle + 2u];
		UnionComponents(first, second);
		UnionComponents(first, third);
		referencedVertices[first] = true;
		referencedVertices[second] = true;
		referencedVertices[third] = true;
	}

	struct PaletteComponent
	{
		std::uint32_t firstVertex = 0u;
		std::vector<std::uint32_t> vertices;
		std::vector<std::uint32_t> sourceJointIds;
	};
	std::vector<PaletteComponent> components;
	std::vector<std::uint32_t> componentLookup(input.vertices.size(), kInvalidPalettePartition);
	for (std::uint32_t vertexIndex = 0u; vertexIndex < input.vertices.size(); ++vertexIndex)
	{
		if (!referencedVertices[vertexIndex])
		{
			continue;
		}
		const std::uint32_t root = FindComponent(vertexIndex);
		if (componentLookup[root] == kInvalidPalettePartition)
		{
			componentLookup[root] = static_cast<std::uint32_t>(components.size());
			components.push_back({ vertexIndex, {}, {} });
		}
		PaletteComponent& component = components[componentLookup[root]];
		component.vertices.push_back(vertexIndex);
		const std::vector<std::uint32_t>& joints = report.cookedVertices[vertexIndex].sourceJointIds;
		component.sourceJointIds.insert(component.sourceJointIds.end(), joints.begin(), joints.end());
	}
	for (PaletteComponent& component : components)
	{
		std::sort(component.sourceJointIds.begin(), component.sourceJointIds.end());
		component.sourceJointIds.erase(
			std::unique(component.sourceJointIds.begin(), component.sourceJointIds.end()),
			component.sourceJointIds.end());
		if (component.sourceJointIds.empty())
		{
			// TDEF keeps palette records structurally nonempty. Joint zero is a
			// deterministic placeholder for an unbound component; all four weights
			// remain zero and the identity flag forbids runtime palette sampling.
			component.sourceJointIds.push_back(0u);
		}
		if (component.sourceJointIds.size() > kDeformableMeshMaximumPaletteJoints)
		{
			SetDiagnostic(
				"SkinnedMeshConnectedPaletteTooLarge",
				"A topology-connected deformable region references more than 256 retained joints and cannot share one palette-local index space.",
				"Split the mesh at a safe primitive boundary or reduce the skeleton influences for that region.");
			return Finish(false, SkinnedMeshCookFailure::PalettePartitionLimitExceeded);
		}
	}
	std::sort(components.begin(), components.end(), [](const PaletteComponent& left, const PaletteComponent& right) {
		if (left.sourceJointIds != right.sourceJointIds)
		{
			return std::lexicographical_compare(
				left.sourceJointIds.begin(), left.sourceJointIds.end(),
				right.sourceJointIds.begin(), right.sourceJointIds.end());
		}
		return left.firstVertex < right.firstVertex;
	});

	std::vector<std::uint32_t> vertexPalette(input.vertices.size(), kInvalidPalettePartition);
	for (const PaletteComponent& component : components)
	{
		std::uint32_t selectedPalette = kInvalidPalettePartition;
		std::vector<std::uint32_t> selectedJointUnion;
		for (std::uint32_t paletteIndex = 0u; paletteIndex < report.palettePartitions.size(); ++paletteIndex)
		{
			const std::vector<std::uint32_t>& existing = report.palettePartitions[paletteIndex].sourceJointIds;
			std::vector<std::uint32_t> combined;
			combined.reserve(existing.size() + component.sourceJointIds.size());
			std::set_union(
				existing.begin(), existing.end(),
				component.sourceJointIds.begin(), component.sourceJointIds.end(),
				std::back_inserter(combined));
			if (combined.size() <= kDeformableMeshMaximumPaletteJoints)
			{
				selectedPalette = paletteIndex;
				selectedJointUnion = std::move(combined);
				break;
			}
		}
		if (selectedPalette == kInvalidPalettePartition)
		{
			if (report.palettePartitions.size() >= settings.maximumPalettePartitions)
			{
				SetDiagnostic(
					"SkinnedMeshPalettePartitionLimitExceeded",
					"The deterministic cook requires more palette partitions than the active operational limit permits.",
					"Increase maximumPalettePartitions or split the mesh into smaller deformable sections.");
				return Finish(false, SkinnedMeshCookFailure::PalettePartitionLimitExceeded);
			}
			selectedPalette = static_cast<std::uint32_t>(report.palettePartitions.size());
			report.palettePartitions.push_back({ component.sourceJointIds });
		}
		else
		{
			report.palettePartitions[selectedPalette].sourceJointIds = std::move(selectedJointUnion);
		}
		for (std::uint32_t vertexIndex : component.vertices)
		{
			vertexPalette[vertexIndex] = selectedPalette;
			report.cookedVertices[vertexIndex].palettePartitionIndex = selectedPalette;
		}
	}
	if (report.palettePartitions.empty())
	{
		SetDiagnostic(
			"SkinnedMeshNoPalettePartition",
			"No referenced deformable vertices were available to form a palette partition.",
			"Remove empty primitives or re-export the skinned mesh with valid triangle references.");
		return Finish(false, SkinnedMeshCookFailure::PalettePartitionLimitExceeded);
	}

	std::size_t sourceSkinningByteCount = 0u;
	if (!TryMultiplySkinnedSize(input.vertices.size(), kDeformableMeshSkinningStrideBytes, sourceSkinningByteCount)
		|| sourceSkinningByteCount > std::numeric_limits<std::uint32_t>::max())
	{
		SetDiagnostic(
			"SkinnedMeshSkinningStreamOverflow",
			"The 8-byte-per-vertex skinning stream exceeds the TEMF v1 address range.",
			"Split the source mesh into smaller assets before cooking.");
		return Finish(false, SkinnedMeshCookFailure::PayloadRangeOverflow);
	}
	std::vector<std::uint8_t> sourceSkinningBytes(sourceSkinningByteCount, 0u);
	for (std::size_t vertexIndex = 0u; vertexIndex < report.cookedVertices.size(); ++vertexIndex)
	{
		SkinnedMeshCookedVertex& cooked = report.cookedVertices[vertexIndex];
		std::uint8_t* encoded = sourceSkinningBytes.data() + vertexIndex * kDeformableMeshSkinningStrideBytes;
		if (!referencedVertices[vertexIndex])
		{
			continue;
		}
		if (cooked.usesIdentityDeformation)
		{
			// Four zero weights are a deliberate identity record, not an invalid
			// normalization. The payload flag requires runtime decoders to bypass
			// palette sampling for this exact representation.
			continue;
		}
		const std::uint32_t paletteIndex = vertexPalette[vertexIndex];
		if (paletteIndex == kInvalidPalettePartition || paletteIndex >= report.palettePartitions.size())
		{
			return Finish(false, SkinnedMeshCookFailure::PalettePartitionLimitExceeded);
		}
		const std::vector<std::uint32_t>& palette = report.palettePartitions[paletteIndex].sourceJointIds;
		std::uint32_t quantizedSum = 0u;
		std::array<float, kDeformableMeshMaximumInfluences> fractionalRemainders{};
		for (std::size_t influenceIndex = 0u; influenceIndex < cooked.influences.size(); ++influenceIndex)
		{
			const auto joint = std::lower_bound(palette.begin(), palette.end(), cooked.influences[influenceIndex].jointIndex);
			if (joint == palette.end() || *joint != cooked.influences[influenceIndex].jointIndex)
			{
				return Finish(false, SkinnedMeshCookFailure::PalettePartitionLimitExceeded);
			}
			const std::uint8_t localJoint = static_cast<std::uint8_t>(std::distance(palette.begin(), joint));
			const float scaledWeight = cooked.influences[influenceIndex].weight * 255.0f;
			const std::uint32_t quantized = static_cast<std::uint32_t>(std::floor(scaledWeight));
			encoded[influenceIndex] = localJoint;
			encoded[4u + influenceIndex] = static_cast<std::uint8_t>(quantized);
			cooked.paletteJointIndices[influenceIndex] = localJoint;
			cooked.quantizedWeights[influenceIndex] = static_cast<std::uint8_t>(quantized);
			fractionalRemainders[influenceIndex] = scaledWeight - static_cast<float>(quantized);
			quantizedSum += quantized;
		}
		if (quantizedSum > 255u)
		{
			SetDiagnostic(
				"SkinnedMeshWeightQuantizationOverflow",
				"Normalized retained weights produced an invalid UNORM8 sum.",
				"Normalize the source skin weights before reimporting.");
			return Finish(false, SkinnedMeshCookFailure::InvalidInfluenceWeight);
		}
		std::array<std::size_t, kDeformableMeshMaximumInfluences> remainderOrder{ 0u, 1u, 2u, 3u };
		std::sort(remainderOrder.begin(), remainderOrder.begin() + static_cast<std::ptrdiff_t>(cooked.influences.size()),
			[&](std::size_t left, std::size_t right) {
				if (fractionalRemainders[left] != fractionalRemainders[right])
				{
					return fractionalRemainders[left] > fractionalRemainders[right];
				}
				return cooked.influences[left].jointIndex < cooked.influences[right].jointIndex;
			});
		std::uint32_t remainder = 255u - quantizedSum;
		for (std::uint32_t unit = 0u; unit < remainder; ++unit)
		{
			const std::size_t influenceIndex = remainderOrder[unit % cooked.influences.size()];
			encoded[4u + influenceIndex] += 1u;
			cooked.quantizedWeights[influenceIndex] += 1u;
		}
		// A very small positive source weight can legitimately round to zero after
		// deterministic UNORM8 apportionment. Zero-weight slots must use joint zero:
		// runtime validation treats any other palette index as non-canonical because
		// that index must never be sampled. Keep the retained source influence in the
		// report, but canonicalize its encoded and reported palette-local value.
		for (std::size_t influenceIndex = 0u; influenceIndex < cooked.influences.size(); ++influenceIndex)
		{
			if (cooked.quantizedWeights[influenceIndex] == 0u)
			{
				encoded[influenceIndex] = 0u;
				cooked.paletteJointIndices[influenceIndex] = 0u;
			}
		}
	}

	// Canonicalize used vertices by first primitive occurrence. Some glTF assets,
	// including the Khronos Fox sample, store every triangle corner as a separate
	// unindexed vertex. Preserving those source indices verbatim makes every edge
	// look like a locked border and prevents meshoptimizer from generating any LOD.
	//
	// Merge only records that are byte-identical in every renderer-visible input:
	// base geometry, palette-local skinning, and all morph channels. This rebuilds
	// safe topology without ever borrowing a different vertex's deformation data.
	// The loop remains sequential because first occurrence defines the stable TEMF
	// vertex order consumed by the skin, morph, and animated-page tables.
	SkinnedMeshCookInput stagedInput;
	stagedInput.indices.reserve(input.indices.size());
	std::vector<std::uint32_t> sourceToStaged(input.vertices.size(), kInvalidPalettePartition);
	std::vector<std::uint32_t> stagedToSource;
	std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> stagedVerticesBySignature;
	const auto HashSourceVertex = [&](std::uint32_t sourceIndex) {
		const RawMeshVertex& base = input.vertices[sourceIndex].baseVertex;
		std::uint64_t hash = HashAnimatedVirtualCookBytes(
			reinterpret_cast<const std::uint8_t*>(base.position.data()),
			sizeof(base.position));
		hash = HashAnimatedVirtualCookBytes(
			reinterpret_cast<const std::uint8_t*>(base.normal.data()),
			sizeof(base.normal),
			hash);
		hash = HashAnimatedVirtualCookBytes(
			reinterpret_cast<const std::uint8_t*>(base.uv.data()),
			sizeof(base.uv),
			hash);
		hash = HashAnimatedVirtualCookBytes(
			reinterpret_cast<const std::uint8_t*>(&vertexPalette[sourceIndex]),
			sizeof(vertexPalette[sourceIndex]),
			hash);
		hash = HashAnimatedVirtualCookBytes(
			sourceSkinningBytes.data()
				+ static_cast<std::size_t>(sourceIndex) * kDeformableMeshSkinningStrideBytes,
			kDeformableMeshSkinningStrideBytes,
			hash);
		for (const SkinnedMeshMorphTarget& morph : input.morphTargets)
		{
			hash = HashAnimatedVirtualCookBytes(
				reinterpret_cast<const std::uint8_t*>(morph.positionDeltas[sourceIndex].data()),
				sizeof(morph.positionDeltas[sourceIndex]),
				hash);
			if (!morph.normalDeltas.empty())
			{
				hash = HashAnimatedVirtualCookBytes(
					reinterpret_cast<const std::uint8_t*>(morph.normalDeltas[sourceIndex].data()),
					sizeof(morph.normalDeltas[sourceIndex]),
					hash);
			}
			if (!morph.tangentDeltas.empty())
			{
				hash = HashAnimatedVirtualCookBytes(
					reinterpret_cast<const std::uint8_t*>(morph.tangentDeltas[sourceIndex].data()),
					sizeof(morph.tangentDeltas[sourceIndex]),
					hash);
			}
		}
		return hash;
	};
	const auto SourceVerticesMatch = [&](std::uint32_t left, std::uint32_t right) {
		const RawMeshVertex& leftBase = input.vertices[left].baseVertex;
		const RawMeshVertex& rightBase = input.vertices[right].baseVertex;
		if (std::memcmp(leftBase.position.data(), rightBase.position.data(), sizeof(leftBase.position)) != 0
			|| std::memcmp(leftBase.normal.data(), rightBase.normal.data(), sizeof(leftBase.normal)) != 0
			|| std::memcmp(leftBase.uv.data(), rightBase.uv.data(), sizeof(leftBase.uv)) != 0
			|| vertexPalette[left] != vertexPalette[right]
			|| std::memcmp(
				sourceSkinningBytes.data() + static_cast<std::size_t>(left) * kDeformableMeshSkinningStrideBytes,
				sourceSkinningBytes.data() + static_cast<std::size_t>(right) * kDeformableMeshSkinningStrideBytes,
				kDeformableMeshSkinningStrideBytes) != 0)
		{
			return false;
		}
		for (const SkinnedMeshMorphTarget& morph : input.morphTargets)
		{
			if (std::memcmp(
					morph.positionDeltas[left].data(),
					morph.positionDeltas[right].data(),
					sizeof(morph.positionDeltas[left])) != 0
				|| (!morph.normalDeltas.empty()
					&& std::memcmp(
						morph.normalDeltas[left].data(),
						morph.normalDeltas[right].data(),
						sizeof(morph.normalDeltas[left])) != 0)
				|| (!morph.tangentDeltas.empty()
					&& std::memcmp(
						morph.tangentDeltas[left].data(),
						morph.tangentDeltas[right].data(),
						sizeof(morph.tangentDeltas[left])) != 0))
			{
				return false;
			}
		}
		return true;
	};
	for (std::uint32_t sourceIndex : input.indices)
	{
		if (sourceToStaged[sourceIndex] == kInvalidPalettePartition)
		{
			const std::uint64_t signature = HashSourceVertex(sourceIndex);
			std::vector<std::uint32_t>& candidates = stagedVerticesBySignature[signature];
			for (std::uint32_t candidate : candidates)
			{
				if (SourceVerticesMatch(sourceIndex, stagedToSource[candidate]))
				{
					sourceToStaged[sourceIndex] = candidate;
					break;
				}
			}
			if (sourceToStaged[sourceIndex] == kInvalidPalettePartition)
			{
				sourceToStaged[sourceIndex] = static_cast<std::uint32_t>(stagedInput.vertices.size());
				stagedInput.vertices.push_back(input.vertices[sourceIndex]);
				stagedToSource.push_back(sourceIndex);
				candidates.push_back(sourceToStaged[sourceIndex]);
			}
		}
		stagedInput.indices.push_back(sourceToStaged[sourceIndex]);
	}
	std::vector<const SkinnedMeshMorphTarget*> orderedSourceMorphs;
	orderedSourceMorphs.reserve(input.morphTargets.size());
	for (const SkinnedMeshMorphTarget& morph : input.morphTargets)
	{
		orderedSourceMorphs.push_back(&morph);
	}
	std::sort(orderedSourceMorphs.begin(), orderedSourceMorphs.end(), [](const SkinnedMeshMorphTarget* left, const SkinnedMeshMorphTarget* right) {
		return left->stableTargetId < right->stableTargetId;
	});
	for (const SkinnedMeshMorphTarget* sourceMorph : orderedSourceMorphs)
	{
		SkinnedMeshMorphTarget stagedMorph;
		stagedMorph.stableTargetId = sourceMorph->stableTargetId;
		stagedMorph.positionDeltas.reserve(stagedToSource.size());
		for (std::uint32_t sourceIndex : stagedToSource)
		{
			stagedMorph.positionDeltas.push_back(sourceMorph->positionDeltas[sourceIndex]);
			if (!sourceMorph->normalDeltas.empty())
			{
				stagedMorph.normalDeltas.push_back(sourceMorph->normalDeltas[sourceIndex]);
			}
			if (!sourceMorph->tangentDeltas.empty())
			{
				stagedMorph.tangentDeltas.push_back(sourceMorph->tangentDeltas[sourceIndex]);
			}
		}
		stagedInput.morphTargets.push_back(std::move(stagedMorph));
	}
	std::size_t stagedSkinningByteCount = 0u;
	if (!TryMultiplySkinnedSize(stagedToSource.size(), kDeformableMeshSkinningStrideBytes, stagedSkinningByteCount))
	{
		SetDiagnostic(
			"SkinnedMeshSkinningStreamOverflow",
			"The canonical skinning stream size overflowed the host address range.",
			"Split the source mesh into smaller assets before cooking.");
		return Finish(false, SkinnedMeshCookFailure::PayloadRangeOverflow);
	}
	std::vector<std::uint8_t> stagedSkinningBytes(stagedSkinningByteCount, 0u);
	std::vector<std::uint32_t> stagedVertexPalettes;
	stagedVertexPalettes.reserve(stagedToSource.size());
	for (std::size_t stagedIndex = 0u; stagedIndex < stagedToSource.size(); ++stagedIndex)
	{
		const std::uint32_t sourceIndex = stagedToSource[stagedIndex];
		std::memcpy(
			stagedSkinningBytes.data() + stagedIndex * kDeformableMeshSkinningStrideBytes,
			sourceSkinningBytes.data() + static_cast<std::size_t>(sourceIndex) * kDeformableMeshSkinningStrideBytes,
			kDeformableMeshSkinningStrideBytes);
		stagedVertexPalettes.push_back(vertexPalette[sourceIndex]);
	}

	std::vector<RawMeshVertex> staticVertices;
	staticVertices.reserve(stagedInput.vertices.size());
	for (const SkinnedMeshCookVertex& vertex : stagedInput.vertices)
	{
		staticVertices.push_back(vertex.baseVertex);
	}
	const std::uint64_t stageSequence = gSkinnedMeshStageSequence.fetch_add(1u, std::memory_order_relaxed);
	const std::uint64_t stageNonce = static_cast<std::uint64_t>(
		std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
	const std::uint32_t processId = static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
	const std::uint32_t processId = 0u;
#endif
	// A process-local counter alone is insufficient: two editor processes can
	// simultaneously recook the same asset after a shared-cache invalidation.
	// Hash the full process/counter/clock tuple into a fixed-width token. This
	// retains cross-process isolation without allowing decimal clock text to push
	// an otherwise valid nested Windows project beyond the legacy path limit.
	std::uint64_t stageToken = 1469598103934665603ULL;
	const auto mixStageValue = [&stageToken](auto value) {
		for(std::size_t byteIndex = 0U; byteIndex < sizeof(value); ++byteIndex)
		{
			stageToken ^= static_cast<std::uint8_t>(value >> (byteIndex * 8U));
			stageToken *= 1099511628211ULL;
		}
	};
	mixStageValue(processId);
	mixStageValue(stageSequence);
	mixStageValue(stageNonce);
	std::ostringstream stageTokenText;
	stageTokenText << std::hex << std::setfill('0') << std::setw(16) << stageToken;
	const std::filesystem::path stagePath = output.parent_path()
		/ (output.filename().string() + ".s-" + stageTokenText.str());
	std::error_code ignored;
	std::filesystem::remove(stagePath, ignored);
	MeshCompileReport staticReport{};
	std::vector<std::uint32_t> finalVertexToStagedVertex;
	if (!CompileMeshFromRawInternal(
		logicalSource,
		stagePath,
		staticVertices,
		stagedInput.indices,
		true,
		true,
		&staticReport,
		60.0f,
		1e-6f,
		&settings.virtualGeometrySettings,
		{},
		nullptr,
		&finalVertexToStagedVertex))
	{
		std::filesystem::remove(stagePath, ignored);
		SetDiagnostic(
			"SkinnedMeshStaticCookFailed",
			"The shared TEMF geometry stage failed: " + staticReport.failureReason,
			"Resolve the reported static geometry error and retry the package cook.");
		return Finish(false, SkinnedMeshCookFailure::StaticMeshCookFailed);
	}
	SkinnedMeshCookInput payloadInput;
	payloadInput.indices = stagedInput.indices;
	payloadInput.vertices.reserve(finalVertexToStagedVertex.size());
	std::vector<std::uint8_t> payloadSkinningBytes;
	payloadSkinningBytes.reserve(
		finalVertexToStagedVertex.size()
			* kDeformableMeshSkinningStrideBytes);
	std::vector<std::uint32_t> payloadVertexPalettes;
	payloadVertexPalettes.reserve(finalVertexToStagedVertex.size());
	for (const SkinnedMeshMorphTarget& stagedMorph : stagedInput.morphTargets)
	{
		SkinnedMeshMorphTarget payloadMorph;
		payloadMorph.stableTargetId = stagedMorph.stableTargetId;
		payloadMorph.positionDeltas.reserve(finalVertexToStagedVertex.size());
		payloadMorph.normalDeltas.reserve(
			stagedMorph.normalDeltas.empty() ? 0u : finalVertexToStagedVertex.size());
		payloadMorph.tangentDeltas.reserve(
			stagedMorph.tangentDeltas.empty() ? 0u : finalVertexToStagedVertex.size());
		payloadInput.morphTargets.push_back(std::move(payloadMorph));
	}
	for (const std::uint32_t stagedVertex : finalVertexToStagedVertex)
	{
		if (stagedVertex >= stagedInput.vertices.size()
			|| stagedVertex >= stagedVertexPalettes.size())
		{
			std::filesystem::remove(stagePath, ignored);
			SetDiagnostic(
				"SkinnedMeshStaticVertexLayoutIncompatible",
				"Static cleanup emitted a vertex without a valid deformation-source mapping.",
				"Reimport the source mesh after rebuilding the mesh compiler and runtime from the same revision.");
			return Finish(false, SkinnedMeshCookFailure::StaticVertexLayoutIncompatible);
		}
		payloadInput.vertices.push_back(stagedInput.vertices[stagedVertex]);
		payloadVertexPalettes.push_back(stagedVertexPalettes[stagedVertex]);
		const std::uint8_t* skinningRecord = stagedSkinningBytes.data()
			+ static_cast<std::size_t>(stagedVertex)
				* kDeformableMeshSkinningStrideBytes;
		payloadSkinningBytes.insert(
			payloadSkinningBytes.end(),
			skinningRecord,
			skinningRecord + kDeformableMeshSkinningStrideBytes);
		for (std::size_t morphIndex = 0u;
			morphIndex < stagedInput.morphTargets.size();
			++morphIndex)
		{
			const SkinnedMeshMorphTarget& stagedMorph =
				stagedInput.morphTargets[morphIndex];
			SkinnedMeshMorphTarget& payloadMorph =
				payloadInput.morphTargets[morphIndex];
			payloadMorph.positionDeltas.push_back(
				stagedMorph.positionDeltas[stagedVertex]);
			if (!stagedMorph.normalDeltas.empty())
			{
				payloadMorph.normalDeltas.push_back(
					stagedMorph.normalDeltas[stagedVertex]);
			}
			if (!stagedMorph.tangentDeltas.empty())
			{
				payloadMorph.tangentDeltas.push_back(
					stagedMorph.tangentDeltas[stagedVertex]);
			}
		}
	}
	SkinnedMeshCookFailure appendFailure = SkinnedMeshCookFailure::StagedPayloadWriteFailed;
	if (!TryAppendDeformablePayload(
		stagePath,
		payloadInput,
		settings,
		report.palettePartitions,
		payloadVertexPalettes,
		payloadSkinningBytes,
		report.deformableTemf,
		appendFailure))
	{
		std::filesystem::remove(stagePath, ignored);
		if (appendFailure == SkinnedMeshCookFailure::StaticVertexLayoutIncompatible)
		{
			SetDiagnostic(
				"SkinnedMeshStaticVertexLayoutIncompatible",
				"Static cleanup merged or split vertices in a way that would misalign skin or morph streams.",
				"Preserve deformable seams in the source mesh or split vertices whose skin/morph data differs.");
		}
		else if (appendFailure == SkinnedMeshCookFailure::FormatCompatibilityMismatch)
		{
			SetDiagnostic(
				"SkinnedMeshTemfCompatibilityMismatch",
				"The staged static TEMF version is incompatible with the deformable extension writer.",
				"Rebuild the mesh compiler and runtime from the same TerraEngine revision.");
		}
		else if (appendFailure == SkinnedMeshCookFailure::PayloadRangeOverflow)
		{
			SetDiagnostic(
				"SkinnedMeshPayloadRangeOverflow",
				"A deformable TEMF count or byte range exceeds the checked v1 address space.",
				"Split the mesh or morph set into smaller runtime assets.");
		}
		else
		{
			SetDiagnostic(
				"SkinnedMeshStagedPayloadWriteFailed",
				"The private staged deformable payload could not be validated or written.",
				"Check available disk space and retry; the published generation was not modified.");
		}
		return Finish(false, appendFailure);
	}
	if (!ReplaceStagedTemf(stagePath, output))
	{
		std::filesystem::remove(stagePath, ignored);
		SetDiagnostic(
			"SkinnedMeshPublicationFailed",
			"The validated staged TEMF could not atomically replace the published path.",
			"Close processes locking the destination and retry; the last-known-good generation remains intact.");
		return Finish(false, SkinnedMeshCookFailure::PublicationFailed);
	}
	SetDiagnostic(
		"SkinnedMeshCookSucceeded",
		"The deformable TEMF generation was validated and published atomically.",
		{});
	return Finish(true, SkinnedMeshCookFailure::None);
}
