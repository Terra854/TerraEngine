#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

constexpr uint16_t kVersion = 1;
constexpr uint32_t kMagic = 0x54455446; // "TETF" little-endian
constexpr uint32_t TETF_INVALID_CHUNK_INDEX = 0xFFFFFFFFu;

inline constexpr uint32_t MakeFourCC(const char(&c)[5]) {
	return (static_cast<uint32_t>(c[0]) << 0) |
		(static_cast<uint32_t>(c[1]) << 8) |
		(static_cast<uint32_t>(c[2]) << 16) |
		(static_cast<uint32_t>(c[3]) << 24);
}

inline constexpr uint32_t MakeFourCC(const char a, const char b, const char c, const char d) {
	return (static_cast<uint32_t>(a) << 0) |
		(static_cast<uint32_t>(b) << 8) |
		(static_cast<uint32_t>(c) << 16) |
		(static_cast<uint32_t>(d) << 24);
}

inline constexpr size_t AlignUp(size_t value, size_t alignment) {
	const size_t mask = alignment - 1;
	return (value + mask) & ~mask;
}

#pragma pack(push, 1)
struct TETFHeader {
	uint32_t magic;
	uint16_t version;
	uint16_t headerSize;
	uint32_t flags;        // Bit 0: sRGB, Bit 1: Premultiplied, Bit 2: Cubemap, Bit 3: Array, Bit 4: 3D
	uint32_t alignment;
	uint32_t chunkCount;
	uint64_t tocOffset;
	uint64_t totalSize;
	uint32_t fileCrc32;    // computed with this field set to 0
	uint32_t reserved;
	uint32_t reserved2;
};

struct TETFChunkEntry {
	uint32_t id;       // FourCC (e.g., 'DESC', 'UAST', 'THMB', 'META')
	uint32_t reserved; // must be 0
	uint64_t offset;   // absolute offset from start of file
	uint64_t size;     // payload size (unpadded)
};

constexpr uint32_t kChunkUAST = MakeFourCC('U', 'A', 'S', 'T');
constexpr uint32_t kChunkTHMB = MakeFourCC('T', 'H', 'M', 'B');
constexpr uint32_t kChunkMETA = MakeFourCC('M', 'E', 'T', 'A');
constexpr uint32_t kChunkDESC = MakeFourCC('D', 'E', 'S', 'C');

struct TETFLevelEntry {
	uint64_t offset;   // byte offset relative to the START of the 'UAST' chunk payload
	uint64_t size;     // size of the raw level data in bytes
};

struct TETFDesc {
	uint16_t width;
	uint16_t height;
	uint16_t depth;          // 1 for 2D/arrays/cubemap
	uint16_t layers;         // array layers (facesets for cubemap arrays)
	uint8_t mipCount;
	uint8_t faces;           // 1 or 6
	uint8_t reserved;        
	uint8_t reserved2;       
	uint16_t blockWidth;     // pixels per block in X (4 for UASTC)
	uint16_t blockHeight;    // pixels per block in Y
	uint16_t blockBytes;     // bytes per block (16 for UASTC)
	uint8_t semantic;        // 0=Unknown, 1=Color (sRGB), 2=Data (Linear)
	uint8_t reserved3;       // must be 0
	uint32_t payloadChunkIndex;  // index of the UAST payload chunk in the TOC
	uint32_t userMetadataChunk;  // TETF_INVALID_CHUNK_INDEX if none
	uint32_t thumbnailChunk;     // TETF_INVALID_CHUNK_INDEX if none
	uint32_t vkFormat;       // standard Vulkan VkFormat enum value
	// TETFLevelEntry levelTable[mipCount]; // Implicitly follows in DESC chunk
};
#pragma pack(pop)

static_assert(sizeof(TETFHeader) == 48, "TETFHeader must be 48 bytes");
static_assert(sizeof(TETFChunkEntry) == 24, "TETFChunkEntry must be 24 bytes");
static_assert(sizeof(TETFLevelEntry) == 16, "TETFLevelEntry must be 16 bytes");
static_assert(sizeof(TETFDesc) == 36, "TETFDesc (fixed part) must be 36 bytes");

enum class TextureFileSemantic : uint8_t {
	Unknown = 0,
	Color = 1,
	Data = 2
};

uint32_t ComputeCrc32(const uint8_t* data, size_t size);

// Fully loaded representation of a .TETF blob. 
struct Texture {
	TETFHeader header{};
	TETFDesc descriptor{};
	std::vector<TETFLevelEntry> levelTable; // Extracted from DESC chunk
	std::vector<TETFChunkEntry> chunkTable;
	std::vector<uint8_t> storage;           // Owns all chunk payloads
	std::vector<uint8_t> transcodedData;    // Dynamically generated from UASTC
	std::span<const uint8_t> uastcPayload;  // View into storage for the 'UAST' chunk
	std::span<const uint8_t> userMetadata; // JSON metadata from META chunk
	std::span<const uint8_t> thumbnail;    // Raw thumbnail payload from THMB chunk
};
