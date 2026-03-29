#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

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

constexpr uint32_t kVersionMajor = 1;
constexpr uint32_t kMagic = 0x54455446; // "TETF" little-endian
constexpr uint32_t TETF_INVALID_CHUNK_INDEX = 0xFFFFFFFFu;

constexpr uint32_t kDescChunkId = MakeFourCC("DESC");
constexpr uint32_t kKtx2ChunkId = MakeFourCC("KTX2");
constexpr uint32_t kMetadataChunkId = MakeFourCC("META");
constexpr uint32_t kThumbnailChunkId = MakeFourCC("THMB");

#pragma pack(push, 1)
struct TETFHeader {
	uint32_t magic;
	uint32_t versionMajor; // single packed version field for the current TETF contract
	uint16_t headerSize;
	uint16_t reserved;     // must be 0
	uint32_t flags;        // reserved for future direct-replacement contract metadata
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
	uint16_t depth;          // 1 for standard 2D textures
	uint16_t layers;         // array layers
	uint8_t mipCount;
	uint8_t faces;           // 1 for standard 2D textures, 6 for cubemaps
	uint8_t reservedFormat0; // reserved; legacy TerraEngine format enum was removed
	uint8_t reservedFormat1; // reserved; legacy TerraEngine channel type was removed
	uint16_t reservedFormat2; // reserved; legacy block width was removed
	uint16_t reservedFormat3; // reserved; legacy block height was removed
	uint16_t reservedFormat4; // reserved; legacy block byte size was removed
	uint8_t semantic;        // optional usage hint
	uint8_t reserved;        // must be 0
	uint32_t payloadChunk;       // index of the required KTX2 payload chunk
	uint32_t userMetadataChunk;  // TETF_INVALID_CHUNK_INDEX if none
	uint32_t thumbnailChunk;     // TETF_INVALID_CHUNK_INDEX if none
	uint32_t vkFormat;           // mirrored from the embedded KTX2 payload for quick inspection
};
#pragma pack(pop)

static_assert(sizeof(TETFHeader) == 48, "TETFHeader must be 48 bytes");
static_assert(sizeof(TETFChunkEntry) == 24, "TETFChunkEntry must be 24 bytes");
static_assert(sizeof(TETFDesc) == 36, "TETFDesc must be 36 bytes");

enum class TextureFileSemantic : uint8_t {
	Unknown = 0,
	Color = 1,
	Data = 2
};

// TETF now uses a single packed version field. Keeping it as uint32_t leaves
// room for future direct-replacement contract bumps without reworking the
// header layout again.
inline bool IsCurrentTetfVersion(uint32_t major) {
	return major == kVersionMajor;
}

inline bool IsCurrentTetfHeader(const TETFHeader& header) {
	return header.magic == kMagic && IsCurrentTetfVersion(header.versionMajor);
}

inline bool IsLegacyTetfHeader(const TETFHeader& header) {
	return header.magic == kMagic && header.versionMajor < kVersionMajor;
}

inline std::string_view GetTextureSemanticName(uint8_t semantic) {
	switch (static_cast<TextureFileSemantic>(semantic))
	{
	case TextureFileSemantic::Color:
		return "Color";
	case TextureFileSemantic::Data:
		return "Data";
	default:
		return "Unknown";
	}
}

uint32_t ComputeCrc32(const uint8_t* data, size_t size);

// Fully loaded representation of a current .tetf blob. containerBytes owns the
// raw wrapped file so metadata and the embedded KTX2 payload can be referenced
// safely, while imageData owns the byte-exact mip payloads the Vulkan upload
// path copies into GPU images.
struct TETFMipPayload {
	uint8_t mipLevel = 0;
	uint8_t sliceCount = 0;
	uint16_t reserved = 0;
	uint32_t sliceStride = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t depth = 0;
	std::span<const uint8_t> data;
};

struct Texture {
	TETFHeader header{};
	TETFDesc descriptor{};
	std::vector<TETFChunkEntry> chunkTable;
	std::vector<uint8_t> containerBytes;
	std::vector<uint8_t> imageData;
	std::vector<TETFMipPayload> mips;
	std::span<const uint8_t> ktx2Payload;
	std::span<const uint8_t> userMetadata; // JSON metadata from META chunk (may be empty)
	std::span<const uint8_t> thumbnail;    // Raw thumbnail payload from THMB chunk (may be empty)
	uint32_t supercompressionScheme = 0;
};
