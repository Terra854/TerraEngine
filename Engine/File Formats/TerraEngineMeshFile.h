// TerraEngineMeshFile.h — Binary format definition for meshlet-based geometry (TEMF).
//
// This file defines the custom mesh format used by TerraEngine, optimized for
// modern meshlet-based rendering pipelines. It includes vertex attribute
// definitions, quantization parameters, and meshlet record structures used for
// efficient culling and GPU-driven rendering.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Versioned constants shared by the compressor and viewer when reading/writing the binary blob.
constexpr std::uint32_t kMeshMagicValue = 0x464d4554u; // 'TEMF' little endian
constexpr std::size_t kMaxVertexAttributes = 8;
constexpr std::uint16_t kMeshCurrentVersion = 1;
constexpr std::uint32_t kMeshletPackedVertexStrideBytes = 16u;
constexpr std::uint32_t kMeshletPrimitiveIndexSizeBytes = sizeof(std::uint8_t);
constexpr std::uint32_t kMeshletVertexRemapDecodedElementSizeBytes = sizeof(std::uint32_t);
constexpr std::uint32_t kMeshletVertexDataAlignmentBytes = 4096u;
constexpr std::uint32_t kMeshletIndexDataAlignmentBytes = 4096u;
constexpr std::uint32_t kMeshletMaxVertices = 64;
constexpr std::uint32_t kMeshletMaxTriangles = 124;
constexpr std::uint32_t kMeshletHeaderFlagHasBounds = 1u << 0u;

// Vertex attribute semantics mirror typical DCC exports and describe what the data represents.
// AttributeSemantic names the supported states explicitly so serialization and switches stay readable.
enum class AttributeSemantic : std::uint32_t
{
	Position = 0,
	Normal = 1,
	Tangent = 2,
	Bitangent = 3,
	TexCoord0 = 4,
	Color0 = 5
};

// Encoded formats supported by the toolkit; extend when additional precision is required.
// AttributeFormat names the supported states explicitly so serialization and switches stay readable.
enum class AttributeFormat : std::uint32_t
{
	R10G10B10A2Snorm = 1,
	R10G10B10A2Unorm = 2,
	R16G16Unorm = 3,
	Oct32 = 4
};

// Remap stream encoding tells loaders whether on-disk remap elements are 16-bit
// or 32-bit before they are expanded to a normalized uint32 runtime view.
enum class MeshletRemapEncoding : std::uint32_t
{
	Uint32 = 0,
	Uint16 = 1
};

// GetRemapElementSizeBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
constexpr std::uint32_t GetRemapElementSizeBytes(MeshletRemapEncoding encoding)
{
	return encoding == MeshletRemapEncoding::Uint16 ? static_cast<std::uint32_t>(sizeof(std::uint16_t)) : static_cast<std::uint32_t>(sizeof(std::uint32_t));
}

#pragma pack(push, 1)
// Structures below need a predictable layout so the header matches exactly on disk.
// Packed descriptions of each attribute inside a vertex.
struct VertexAttribute
{
	std::uint32_t semantic;   // AttributeSemantic
	std::uint32_t format;     // AttributeFormat
	std::uint32_t offsetBytes;
};

// Full vertex declaration stored in the header so viewers know how to decode the stream.
// struct VertexLayout groups related state and operations so callers have one documented contract.
struct VertexLayout
{
	std::uint32_t strideBytes;
	std::uint32_t attributeCount;
	VertexAttribute attributes[kMaxVertexAttributes];
};

// Parameters required to denormalize packed positions/UVs back into model space.
// struct QuantizationInfo groups related state and operations so callers have one documented contract.
struct QuantizationInfo
{
	float positionOffset[3];  // Center of the bounding box
	float positionScale[3];   // Half-extent for each axis (max deviation from center)
	float uvOffset[2];        // Minimum UV
	float uvScale[2];         // Range of UV coordinates
};

// Per-meshlet table entry. Offsets/counts point into packed meshlet streams.
// struct MeshletRecord groups related state and operations so callers have one documented contract.
struct MeshletRecord
{
	std::uint32_t vertexOffset;          // Offset into meshlet vertex remap stream.
	std::uint32_t vertexCount;           // Number of local vertices (max 64).
	std::uint32_t primitiveIndexOffset;  // Offset into uint8 meshlet-local primitive stream.
	std::uint32_t primitiveIndexCount;   // Primitive element count (triangle-aligned).
};

// Maps a render section to a contiguous range of meshlets.
// struct MeshletSection groups related state and operations so callers have one documented contract.
struct MeshletSection
{
	std::uint32_t meshletOffset;
	std::uint32_t meshletCount;
};

// Optional per-meshlet sphere + cone bounds for visibility/culling work.
// struct MeshletBounds groups related state and operations so callers have one documented contract.
struct MeshletBounds
{
	float center[3];
	float radius;
	float coneApex[3];
	float coneCutoff;
	float coneAxis[3];
	float coneApexOffset;
};

// Final on-disk header that prefixes every meshlet-only TEMF file.
// struct MeshHeader groups related state and operations so callers have one documented contract.
struct MeshHeader
{
	char magic[4];
	std::uint16_t version;
	std::uint16_t reserved;
	std::uint32_t headerSize;
	std::uint32_t meshletVertexCount;         // Global packed-vertex stream count.
	std::uint32_t meshletVertexRemapCount;    // Number of remap entries (decoded count).
	std::uint32_t meshletPrimitiveIndexCount; // Number of uint8 entries in primitive stream.
	std::uint32_t meshletCount;
	std::uint32_t meshletSectionCount;
	std::uint32_t meshletBoundsCount;
	std::uint32_t meshletVertexDataOffset;
	std::uint32_t meshletVertexRemapDataOffset;
	std::uint32_t meshletPrimitiveIndexDataOffset;
	std::uint32_t meshletTableOffset;
	std::uint32_t meshletSectionOffset;
	std::uint32_t meshletVertexRemapEncoding; // MeshletRemapEncoding
	std::uint32_t meshletFlags;               // kMeshletHeaderFlag*
	std::uint32_t meshletBoundsOffset;
	std::uint32_t meshletSkinningDataOffset; // Byte offset to 8-byte skinning stream (Joints/Weights)
	std::uint32_t meshletSkinningDataCount;  // Number of vertices with skinning data
	VertexLayout layout;
	QuantizationInfo quantization;
};
#pragma pack(pop)

static_assert(sizeof(MeshHeader) <= 640, "TEMF header should remain compact");

// In-memory representation of an entire .temf payload.
// Raw byte storage keeps payload ownership in one place while spans/vectors expose typed views.
struct Mesh
{
	MeshHeader header{};
	std::vector<std::uint8_t> storage;
	std::span<const std::uint8_t> meshletVertexData;
	// Runtime always works with uint32 remap entries even if on-disk storage used uint16.
	std::vector<std::uint32_t> meshletVertexRemapDecoded;
	std::span<const std::uint8_t> meshletVertexRemapData;
	std::span<const std::uint8_t> meshletSkinningData;
	std::span<const std::uint8_t> meshletPrimitiveIndexData;
	std::vector<MeshletRecord> meshletRecords;
	std::vector<MeshletSection> meshletSections;
	std::vector<MeshletBounds> meshletBounds;
	// Object-level culling needs one conservative mesh bound before the GPU
	// sees individual meshlets. It is derived during load so render systems can
	// cheaply transform it into world space without walking every meshlet.
	bool hasLocalBounds = false;
	float localBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
	float localBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
};
