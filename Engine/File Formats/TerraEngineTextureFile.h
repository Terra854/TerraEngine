#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

constexpr uint16_t kVersionMajor = 1;
constexpr uint16_t kVersionMinor = 0;
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
	uint16_t versionMajor;
	uint16_t versionMinor;
	uint16_t headerSize;
	uint16_t reserved;     // must be 0
	uint32_t flags;
	uint32_t alignment;
	uint32_t chunkCount;
	uint64_t tocOffset;
	uint64_t totalSize;
	uint32_t fileCrc32;    // computed with this field set to 0
	uint32_t reserved2;    // must be 0
};

struct TETFChunkEntry {
	uint32_t id;       // FourCC
	uint32_t reserved; // must be 0 for now
	uint64_t offset;   // absolute offset from start of file
	uint64_t size;     // payload size (unpadded)
};

struct TETFDesc {
	uint16_t width;
	uint16_t height;
	uint16_t depth;          // 1 for 2D/arrays/cubemap
	uint16_t layers;         // array layers (facesets for cubemap arrays)
	uint8_t mipCount;
	uint8_t faces;           // 1 or 6
	uint8_t format;          // see TETF format enum
	uint8_t channelType;     // 0=UNORM,1=SNORM,2=UINT,3=SINT,4=FLOAT
	uint16_t blockWidth;     // pixels per block in X (1 for uncompressed)
	uint16_t blockHeight;    // pixels per block in Y
	uint16_t blockBytes;     // bytes per block
	uint8_t semantic;        // optional usage hint
	uint8_t reserved;        // must be 0
	uint32_t mipOffsetChunk;     // index of first MIP chunk
	uint32_t userMetadataChunk;  // TETF_INVALID_CHUNK_INDEX if none
	uint32_t thumbnailChunk;     // TETF_INVALID_CHUNK_INDEX if none
	uint32_t reserved2;
};

struct TETFMipChunkHeader {
	uint8_t mipLevel;   // 0 = top level
	uint8_t sliceCount; // faces * layers, or depth slices for 3D
	uint16_t reserved;  // must be 0 (future per-mip flags)
	uint32_t sliceStride; // bytes per slice (raw)
};
#pragma pack(pop)

static_assert(sizeof(TETFHeader) == 48, "TETFHeader must be 48 bytes");
static_assert(sizeof(TETFChunkEntry) == 24, "TETFChunkEntry must be 24 bytes");
static_assert(sizeof(TETFDesc) == 36, "TETFDesc must be 36 bytes");
static_assert(sizeof(TETFMipChunkHeader) == 8, "TETFMipChunkHeader must be 8 bytes");

enum class TETFFormat : uint8_t {
	RGBA8 = 0,
	BGRA8 = 1,
	BC1 = 10,
	BC3 = 11,
	BC4 = 12,
	BC5 = 13,
	BC6H = 14,
	BC7 = 15,
	ASTC_4x4 = 20,
	ASTC_6x6 = 21,
	ASTC_8x8 = 22,
};

enum class TETFChannelType : uint8_t {
	UNORM = 0,
	SNORM = 1,
	UINT = 2,
	SINT = 3,
	FLOAT = 4
};

inline bool IsCompressedFormat(uint8_t fmt) {
	return fmt == static_cast<uint8_t>(TETFFormat::BC1) ||
		fmt == static_cast<uint8_t>(TETFFormat::BC3) ||
		fmt == static_cast<uint8_t>(TETFFormat::BC4) ||
		fmt == static_cast<uint8_t>(TETFFormat::BC5) ||
		fmt == static_cast<uint8_t>(TETFFormat::BC6H) ||
		fmt == static_cast<uint8_t>(TETFFormat::BC7) ||
		fmt == static_cast<uint8_t>(TETFFormat::ASTC_4x4) ||
		fmt == static_cast<uint8_t>(TETFFormat::ASTC_6x6) ||
		fmt == static_cast<uint8_t>(TETFFormat::ASTC_8x8);
}

uint32_t ComputeCrc32(const uint8_t* data, size_t size);

// Fully loaded representation of a .TETF blob. Storage owns the raw payloads that
// sit after the chunk table while spans provide typed, bounds-checked windows into
// that storage for easy slice/mip iteration without extra copies.
struct TETFMipPayload {
	TETFMipChunkHeader header{};
	std::span<const uint8_t> data; // Contains header.sliceCount slices; each sliceStride bytes.
};

struct Texture {
	TETFHeader header{};
	TETFDesc descriptor{};
	std::vector<TETFChunkEntry> chunkTable;
	std::vector<uint8_t> storage;
	std::vector<TETFMipPayload> mips;
	std::span<const uint8_t> userMetadata; // JSON metadata from META chunk (may be empty)
	std::span<const uint8_t> thumbnail;    // Raw thumbnail payload from THMB chunk (may be empty)
};
