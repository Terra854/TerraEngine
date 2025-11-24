# Terra Engine Mesh File (TEMF) Specification

TEMF packages mesh vertex/index data in a GPU-ready layout with minimal parsing. Headers are fixed-size and little-endian so tools can validate quickly before uploading to GPU buffers.

## Design goals
- Keep headers compact and deterministic for bundle deduplication.
- Describe vertex layout explicitly (stride + attributes) so no runtime reflection is required.
- Support quantized positions/UVs with decode parameters in the header.
- Avoid external dependencies; everything needed to stream a mesh is contained in the blob.

## Binary layout (all little-endian)
```
Header                // fixed-size, packed
VertexData            // raw bytes; matches Header.vertexDataOffset
IndexData             // raw bytes; matches Header.indexDataOffset
```

### Header (176 bytes)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `TEMF` |
| 4 | 2 | version | Current version = 1 |
| 6 | 2 | reserved | Must be 0 |
| 8 | 4 | headerSize | Always `sizeof(Header)` (176) |
| 12 | 4 | vertexCount | Number of vertices |
| 16 | 4 | indexCount | Number of indices |
| 20 | 4 | vertexDataOffset | Byte offset to first vertex byte |
| 24 | 4 | indexDataOffset | Byte offset to first index byte |
| 28 | 4 | indexFormat | 0 = UInt16, 1 = UInt32 |
| 32 | 4 | layout.strideBytes | Byte stride of each vertex |
| 36 | 4 | layout.attributeCount | Attributes actually used (<= 8) |
| 40 | 96 | layout.attributes[8] | Eight `VertexAttribute` entries (see below) |
| 136 | 12 | quantization.positionOffset | Center of bounding box (float[3]) |
| 148 | 12 | quantization.positionScale | Half-extent per axis (float[3]) |
| 160 | 8 | quantization.uvOffset | Minimum UV (float[2]) |
| 168 | 8 | quantization.uvScale | UV range (float[2]) |

`VertexAttribute` layout (12 bytes each, tightly packed):
- `semantic` (uint32): 0=Position, 1=Normal, 2=Tangent, 3=Bitangent, 4=TexCoord0, 5=Color0
- `format` (uint32): 1=R10G10B10A2Snorm, 2=R10G10B10A2Unorm, 3=R16G16Unorm
- `offsetBytes` (uint32): Byte offset within a vertex

`VertexLayout` always reserves space for 8 attributes; unused entries are ignored past `attributeCount`.

`QuantizationInfo` lets loaders reconstruct normalized vertex data (positions/UVs) without external metadata.

### Payloads
- **VertexData**: `vertexCount * layout.strideBytes` bytes beginning at `vertexDataOffset`.
- **IndexData**: `indexCount * (2 or 4)` bytes beginning at `indexDataOffset`; width chosen by `indexFormat`.

### Loader checklist
- Validate magic/version/headerSize before trusting offsets.
- Confirm `vertexDataOffset` and `indexDataOffset` are within the file and non-overlapping.
- Apply `QuantizationInfo` when expanding packed vertex attributes in tooling; runtime paths can upload as-is if the pipeline expects the encoded formats.
