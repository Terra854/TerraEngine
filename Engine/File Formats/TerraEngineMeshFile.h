#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Versioned constants shared by the compressor and viewer when reading/writing the binary blob.
constexpr std::uint32_t kMagicValue = 0x464d4554u; // 'TEMF' little endian
constexpr std::size_t kMaxVertexAttributes = 8;
constexpr std::uint16_t kCurrentVersion = 1;

// Vertex attribute semantics mirror typical DCC exports and describe what the data represents.
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
enum class AttributeFormat : std::uint32_t
{
	R10G10B10A2Snorm = 1,
	R10G10B10A2Unorm = 2,
	R16G16Unorm = 3
};

// Describes index width on disk.
enum class IndexFormat : std::uint32_t
{
	UInt16 = 0,
	UInt32 = 1
};

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
struct VertexLayout
{
	std::uint32_t strideBytes;
	std::uint32_t attributeCount;
	VertexAttribute attributes[kMaxVertexAttributes];
};

// Parameters required to denormalize packed positions/UVs back into model space.
struct QuantizationInfo
{
	float positionOffset[3];  // Center of the bounding box
	float positionScale[3];   // Half-extent for each axis (max deviation from center)
	float uvOffset[2];        // Minimum UV
	float uvScale[2];         // Range of UV coordinates
};

// Final on-disk header that prefixes every TEMF file.
struct Header
{
	char magic[4];
	std::uint16_t version;
	std::uint16_t reserved;
	std::uint32_t headerSize;
	std::uint32_t vertexCount;
	std::uint32_t indexCount;
	std::uint32_t vertexDataOffset;
	std::uint32_t indexDataOffset;
	std::uint32_t indexFormat; // IndexFormat
	VertexLayout layout;
	QuantizationInfo quantization;
};
#pragma pack(pop)

static_assert(sizeof(Header) <= 512, "TEMF header should remain compact");

// In-memory representation of an entire .temf payload: contiguous storage plus typed views.
struct Mesh
{
	Header header{};
	std::vector<std::uint8_t> storage;
	std::span<const std::uint8_t> vertexData;
	std::span<const std::uint8_t> indexData;
};