# Terra Engine Mesh File (TEMF) Specification

TEMF packages meshlet-optimized vertex/index data in a high-performance layout designed for zero-copy GPU consumption and DirectStorage/GDS compatibility.

## Design Goals
- **DirectStorage Ready**: Every data stream is aligned to 4KB boundaries to support high-speed NVMe IO.
- **Zero-Copy Remap**: Support for on-disk `u16` or `u32` vertex indirection without CPU-side decompression.
- **Meshlet Native**: Built specifically for the programmable mesh shader pipeline.
- **Aggressive Packing**: Lean 12-byte vertex stride using quantized and octahedral attributes.

## Binary Layout (all little-endian)
```
Header                // fixed-size (220 bytes), padded to 4KB
VertexData            // raw bytes (12b stride), aligned to 4KB
VertexRemapData       // indirection pool (u16 or u32), aligned to 4KB
SkinningData          // optional (8b stride), aligned to 4KB
PrimitiveIndexData    // meshlet-local indices (u8), aligned to 4KB
MeshletRecordTable    // offsets/counts per meshlet, aligned to 4KB
MeshletSectionTable   // draw call ranges, aligned to 4KB
MeshletBoundsTable    // optional culling spheres, aligned to 4KB
```

### Header (220 bytes)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `TEMF` |
| 4 | 2 | version | Current version = 1 |
| 6 | 2 | reserved | Must be 0 |
| 8 | 4 | headerSize | Size of the header struct (220) |
| 12 | 4 | meshletVertexCount | Total vertices in the global packed pool |
| 16 | 4 | meshletVertexRemapCount | Number of entries in the indirection stream |
| 20 | 4 | meshletPrimitiveIndexCount | Number of `uint8` local indices |
| 24 | 4 | meshletCount | Total number of meshlets |
| 28 | 4 | meshletSectionCount | Number of draw call sections |
| 32 | 4 | meshletBoundsCount | Number of culling bounds entries |
| 36 | 4 | meshletVertexDataOffset | 4KB aligned offset to vertices |
| 40 | 4 | meshletVertexRemapDataOffset | 4KB aligned offset to remap stream |
| 44 | 4 | meshletPrimitiveIndexDataOffset | 4KB aligned offset to primitive stream |
| 48 | 4 | meshletTableOffset | 4KB aligned offset to meshlet records |
| 52 | 4 | meshletSectionOffset | 4KB aligned offset to sections |
| 56 | 4 | meshletVertexRemapEncoding | 0 = `uint32`, 1 = `uint16` |
| 60 | 4 | meshletFlags | bit 0: has bounds |
| 64 | 4 | meshletBoundsOffset | 4KB aligned offset to bounds |
| 68 | 4 | meshletSkinningDataOffset | 4KB aligned offset to skinning stream |
| 72 | 4 | meshletSkinningDataCount | Number of skinned vertices (matches vertex count if used) |
| 76 | 4 | layout.strideBytes | Byte stride of each vertex (standard = 12) |
| 80 | 4 | layout.attributeCount | Attributes used (<= 8) |
| 84 | 96 | layout.attributes[8] | Vertex declaration (semantic/format/offset) |
| 180 | 12 | quantization.positionOffset | Bounding box center |
| 192 | 12 | quantization.positionScale | Bounding box half-extents |
| 204 | 8 | quantization.uvOffset | Minimum UV |
| 212 | 8 | quantization.uvScale | UV Range |

### Key Formats
- **Vertex Attribute (12 bytes)**:
  - `Position`: `R10G10B10A2Snorm` (10-bit per axis).
  - `Normal`: `Oct32` (High-precision octahedral 2D mapping).
  - `TexCoord`: `R16G16Unorm` (16-bit precision).
- **Skinning Stream (8 bytes)**: 4x `uint8` Joint Indices + 4x `unorm8` Weights.
- **Meshlet Record (16 bytes)**: Stores `vertexOffset`, `vertexCount`, `primitiveIndexOffset`, `primitiveIndexCount`.

### Implementation Notes
- **Alignment**: Every payload MUST begin on a 4096-byte boundary to ensure compatibility with `O_DIRECT`, `io_uring`, and DirectStorage.
- **Shader Consumption**: Shaders should use `ByteAddressBuffer` to load remap indices and manually handle the `u16`/`u32` bit-shift logic based on the `meshletVertexRemapEncoding` flag.
- **Culling**: Pre-computed `MeshletBounds` provide a sphere-based visibility check for GPU-driven occlusion or frustum culling.
