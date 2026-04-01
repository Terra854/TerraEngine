# Terra Engine Texture File (TETF) Specification

This document defines the TerraEngine Texture File (TETF) format, a hybrid container designed to provide fast, engine-specific metadata and thumbnails while leveraging the industry-standard **KTX2** format for high-performance, GPU-ready payloads.

## **Design Goals**
- **Fast Metadata Access**: Minimal header and descriptor chunk with embedded mip tables for instant resource management.
- **GPU-Ready Payloads**: Raw block-compressed data (BC7/ASTC) extracted from KTX2 for direct-to-VRAM uploads without container overhead.
- **Unified Tooling**: Leverages `libktx` for encoding while maintaining engine-specific semantics.
- **TEPAK Optimized**: Designed to avoid double compression; internal supercompression is disabled in favor of the outer **TEPAK** Zstandard+Dictionary compression.
- **Cross-API Compatibility**: Uses `VkFormat` as the universal identifier, allowing simple mapping to DXGI (DX12) or Metal.

---

## **Binary Layout (Little-Endian)**
```
TETFHeader
ChunkTable[chunkCount]   // Immediately follows header
ChunkPayloads...
```

### **1. TETFHeader (Fixed 48 bytes)**
| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | 4 | `magic` | ASCII `TETF` (0x54455446) |
| 4 | 2 | `version` | 1 |
| 6 | 2 | `headerSize` | Bytes of this header (48) |
| 8 | 4 | `flags` | Bit 0: sRGB, Bit 1: Premultiplied, Bit 2: Cubemap, Bit 3: Array, Bit 4: 3D |
| 12 | 4 | `alignment` | Payload alignment (Recommend 16 or 4096 for streaming) |
| 16 | 4 | `chunkCount` | Number of entries in the chunk table |
| 20 | 8 | `tocOffset` | Absolute offset to the first `ChunkTable` entry |
| 28 | 8 | `totalSize` | Total file size in bytes |
| 36 | 4 | `fileCrc32` | CRC-32/IEEE of the file (optional; 0 if unused) |
| 40 | 4 | `reserved` | Must be 0 |
| 44 | 4 | `reserved2` | Must be 0 |

### **2. ChunkTable Entry (24 bytes)**
| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | 4 | `id` | FourCC ID (`DESC`, `KTX2`, `THMB`, `META`) |
| 4 | 4 | `reserved` | Must be 0 |
| 8 | 8 | `offset` | Absolute byte offset to payload (aligned to `header.alignment`) |
| 16 | 8 | `size` | Payload size on disk (unpadded) |

---

## **Core Chunks**

### **`DESC` (Descriptor, Variable Length)**
Provides the "fast-path" summary of the texture. Authoritative for engine systems. Size is `36 + (mipCount * 16)` bytes.

| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | 2 | `width` | Texture width |
| 2 | 2 | `height` | Texture height |
| 4 | 2 | `depth` | Texture depth (1 for 2D/Arrays) |
| 6 | 2 | `layers` | Array layers (facesets for cubemap arrays) |
| 8 | 1 | `mipCount` | Number of mip levels |
| 9 | 1 | `faces` | 1 or 6 |
| 10 | 1 | `reserved` | - |
| 11 | 1 | `reserved2` | - |
| 12 | 2 | `blockWidth` | Pixels per block in X (1 for uncompressed) |
| 14 | 2 | `blockHeight` | Pixels per block in Y |
| 16 | 2 | `blockBytes` | Bytes per block |
| 18 | 1 | `semantic` | Usage hint: 0=Unknown, 1=Color (sRGB), 2=Data (Linear) |
| 19 | 1 | `reserved3` | - |
| 20 | 4 | `payloadChunkIndex` | Index in the ChunkTable for the `KTX2` payload |
| 24 | 4 | `userMetadataChunk` | Index for `META` chunk (0xFFFFFFFF if none) |
| 28 | 4 | `thumbnailChunk` | Index for `THMB` chunk (0xFFFFFFFF if none) |
| 32 | 4 | `vkFormat` | Standard Vulkan `VkFormat` enum value |
| 36 | 16*N | `levelTable` | Array of `mipCount` entries: `uint64 offset`, `uint64 size`. Offsets are relative to the start of the `KTX2` chunk payload. |

### **`KTX2` (GPU Payload)**
The payload contains the raw GPU-ready image data extracted from a KTX2 container.
- **Format**: Raw image levels as defined by the KTX2 specification, but without the KTX2 Header, Data Format Descriptor, or Key/Value metadata.
- **Internal Compression**: Must be `KTX_SS_NONE` (No supercompression).
- **GPU Formats**: Typically BC7 (Desktop) or ASTC (Mobile).
- **Rationale**: By stripping the KTX2 container and moving metadata to the `DESC` chunk, the loader can perform single-read metadata analysis and direct-to-VRAM uploads of specific mips.

---

## **Optional Chunks**
- **`THMB` (Thumbnail)**: A small (e.g., 128x128) raw RGBA8 image for editor previews. Allows instant browsing without decoding the high-res GPU data.
- **`META` (Metadata)**: UTF-8 JSON key-value pairs for authoring info (source file, artist, export date).

---

## **Cross-API Mapping (DX12)**
When using a DX12 RHI, the loader maps the `vkFormat` to a `DXGI_FORMAT`:
- `VK_FORMAT_BC7_UNORM_BLOCK` (145) → `DXGI_FORMAT_BC7_UNORM`
- `VK_FORMAT_R8G8B8A8_UNORM` (37) → `DXGI_FORMAT_R8G8B8A8_UNORM`

## **Authoring Pipeline**
1.  **Encode**: Use `libktx` to encode source images (PNG/TGA/HDR) into a temporary KTX2 blob with the desired GPU format.
2.  **Extract**: Extract the raw level data and metadata from the KTX2 blob.
3.  **Wrap**: Populate the TETF header and `DESC` chunk (including the `levelTable`) using the extracted metadata.
4.  **Pack**: Save the TETF file with the raw level data in the `KTX2` chunk.
5.  **Export**: **TEPAK** will compress the TETF file using Zstandard, providing final delivery compression.
