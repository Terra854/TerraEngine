# Terra Engine Texture File (TETF) Proposal

This spec defines a compact, load-friendly container for GPU-ready textures. The header always starts with the ASCII magic `TETF`. The format favors direct uploads (block-compressed surfaces, aligned mip offsets) and intentionally avoids per-chunk compression because the outer game package already compresses all assets together.

## Design goals
- Load quickly: fixed-size header, predictable alignment, table of contents near the front.
- GPU-ready payloads: store already-compressed GPU formats (BCn/ASTC) without recompressing during load.
- Avoid double compression: no internal compression; rely on the outer game package for compression and dedup.
- Scalable: supports 2D/2D-array/3D/cubemap, mip chains, per-face slices, and future extension chunks.
- Mobile scope: ASTC-capable devices only; ETC2 is intentionally not supported.

## Binary layout (all little-endian)
```
TETFHeader
ChunkTable[chunkCount]   // immediately follows header
ChunkPayloads...
```

### TETFHeader (fixed 48 bytes)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `TETF` |
| 4 | 2 | versionMajor | Start at 1 |
| 6 | 2 | versionMinor | Start at 0 |
| 8 | 2 | headerSize | Bytes of this header (>=48) for forward compat |
| 10 | 2 | reserved | Must be 0 |
| 12 | 4 | flags | Bit 0: sRGB (hint), Bit 1: premultiplied alpha, Bit 2: cubemap, Bit 3: array, Bit 4: is3D, others reserved |
| 16 | 4 | alignment | Payload alignment in bytes (power of two, recommend 16 or 4K) |
| 20 | 4 | chunkCount | Entries in the chunk table |
| 24 | 8 | tocOffset | Offset to first `ChunkTable` entry (usually headerSize) |
| 32 | 8 | totalSize | Total file bytes (integrity/early reject) |
| 40 | 4 | fileCrc32 | CRC of entire file with this field zeroed (optional; set 0 if unused) |
| 44 | 4 | reserved2 | Must be 0 |

### ChunkTable entry (24 bytes per entry)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | id | FourCC chunk id (`DESC`, `MIP `, `META`, `THMB`, etc.) |
| 4 | 4 | reserved | Must be 0 for now (future use) |
| 8 | 8 | offset | Byte offset to payload (aligned to header.alignment) |
| 16 | 8 | size | Bytes stored on disk (uncompressed) |

Chunk order is arbitrary; the descriptor chunk must exist and should be first for cache locality. Unknown chunk ids are skipped.

`flags` are fast-path hints; the DESC chunk is authoritative for geometry/layout. Writers should keep them consistent; loaders may reject files when a mismatch is detected.
Loaders may ignore `flags` and rely solely on `DESC`; if `flags` and `DESC` disagree, loaders should warn or reject.
`semantic` is an optional content hint to aid tooling and debug pipelines; render backends should not rely on it for correctness.

## Core chunks

### `DESC` (descriptor, uncompressed)
Defines the texture layout. Single instance.

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 2 | width |
| 2 | 2 | height |
| 4 | 2 | depth | For 3D; set 1 for 2D/arrays/cubemaps |
| 6 | 2 | layers | Array layers (for cubemap arrays: count of facesets) |
| 8 | 1 | mipCount |
| 9 | 1 | faces | 1 for 2D/3D; 6 for cubemap |
| 10 | 1 | format | See format enum below |
| 11 | 1 | channelType | 0=UNORM,1=SNORM,2=UINT,3=SINT,4=FLOAT |
| 12 | 2 | blockWidth | Pixels per block in X (1 for uncompressed) |
| 14 | 2 | blockHeight | Pixels per block in Y |
| 16 | 2 | blockBytes | Bytes per block |
| 18 | 1 | semantic | Optional usage hint: 0=Unknown,1=Albedo,2=Normal,3=ORM,4=UI,5=Height,6=Emission |
| 19 | 1 | reserved | Must be 0 |
| 20 | 4 | mipOffsetChunk | Chunk table index of first `MIP ` entry (for quick jump) |
| 24 | 4 | userMetadataChunk | Chunk table index of `META` (`0xFFFFFFFF` if none) |
| 28 | 4 | thumbnailChunk | Chunk table index of `THMB` (`0xFFFFFFFF` if none) |
| 32 | 4 | reserved2 |

Format enum suggestion (extend as needed): 0=RGBA8, 1=BGRA8, 10=BC1, 11=BC3, 12=BC4, 13=BC5, 14=BC6H, 15=BC7, 20=ASTC_4x4, 21=ASTC_6x6, 22=ASTC_8x8. (No ETC2; mobile targets must support ASTC.)

`0xFFFFFFFF` is an invalid chunk index constant (`TETF_INVALID_CHUNK_INDEX`). Loaders must treat any index >= chunkCount as invalid.

### `MIP ` (per-mip payloads)
One chunk per mip level; chunk id is literally `"MIP "` (with space). Payload layout:

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 1 | mipLevel | 0 is top level |
| 1 | 1 | sliceCount | slices = faces * layers (or depth slices for 3D) |
| 2 | 2 | reserved | Must be 0 for now (future use, e.g., per-mip flags) |
| 4 | 4 | sliceStride | Bytes per slice payload (always uncompressed) |
| 8 | ... | slices | `sliceCount` slices back-to-back, each `sliceStride` bytes |

`sliceStride` is the stored size per slice (raw). For block-compressed formats, ensure each slice size is padded up to a full block; honor header.alignment to keep offsets GPU-friendly when using memory mapping.

Expected mip payload size = `sliceCount * sliceStride` and must match block math for `(width,height,depth,mipLevel,blockWidth,blockHeight,blockBytes)`. For 3D textures, `sliceCount` should equal `depth` (for that mip) * `faces` * `layers`; each slice is one Z-slice at that mip.
Mip dimensions derive as `w_m = max(1, width >> mipLevel)`, `h_m = max(1, height >> mipLevel)`, `d_m = max(1, depth >> mipLevel)` before block math.

### Optional chunks
- `META`: UTF-8 JSON key-value metadata (author, color space hints, packing notes). Keep small; outer package compression will dedupe.
- `THMB`: Small preview image, e.g., 128x128 RGBA8, usually raw to avoid nested compression.
- `HIST`: Histograms or per-channel ranges; useful for runtime debug/tonemapping decisions.
- `ANNO`: Tool annotations (UV charts, packing tool version). Ignored at runtime.

## Compression guidance
- No internal compression. Keep payloads raw and let the outer asset bundle handle compression and dedup across files. Use block-compressed GPU formats (BCn/ASTC) for runtime efficiency.

## Alignment and padding
- `header.alignment` defines payload alignment; recommend 16 bytes (minimum) and 4096 when streaming from disk to GPU upload buffers.
- Pad chunk payloads with zeroes to the next alignment boundary; `size` excludes padding.
- All offsets are absolute from start of file.
- If embedded in a larger pack, interpret offsets relative to the start of the TETF blob; keep that base when memory-mapping.

## Integrity
- `fileCrc32` lets you detect corruption pre-decompress. If unused, set to 0. Use CRC-32/IEEE (poly 0x04C11DB7, init 0xFFFFFFFF, reflected).
- Loaders should sanity-check: magic, version, headerSize, chunkCount, tocOffset within file.

## Loader outline (pseudocode)
```cpp
Read header -> validate magic/version/chunk table bounds
Seek tocOffset, read chunkCount entries
Find DESC chunk, parse descriptor
Validate flag/DESC consistency (faces/layers/3D bits) and chunk indices < chunkCount
For each MIP chunk:
  verify mipLevel ordering and expected size math
  iterate slices (faces * layers or depth slices) in order
  upload slices per mip; honor block dimensions and sliceStride
```

## Authoring pipeline notes
- Build GPU-native payloads offline (e.g., BC7 via texconv, ASTC via astcenc). Skip runtime compression.
- Enforce deterministic chunk ordering to maximize outer-archive dedup (DESC, MIP0..N, THMB, META, etc.).
- Consider writing a simple validator that checks block alignment, slice counts, and that `sliceStride` matches expected block math.
- If you need third-party codecs, clone them under `external/` (none are required by this spec).

## Versioning
- Use semantic increments: bump `versionMinor` for additive chunks/fields; bump `versionMajor` if breaking changes happen (field meaning changes, endianness change, chunk table format change).

## Example minimal file layout
```
0000: TETF 01 00 00 00 ... header (48 bytes)
0030: chunk table (DESC, MIP0, MIP1)
00A0: DESC chunk payload (raw)
0100: MIP0 chunk payload (raw blocks, aligned)
0x...? padding to alignment boundary
....: MIP1 chunk payload
```
