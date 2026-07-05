# Terra Engine Mesh File (TEMF) Specification

TEMF packages meshlet-optimized vertex/index data in a high-performance layout designed for zero-copy GPU consumption and DirectStorage/GDS compatibility.

## Design Goals
- **DirectStorage Ready**: Every data stream is aligned to 4KB boundaries to support high-speed NVMe IO.
- **Zero-Copy Remap**: Support for on-disk `u16` or `u32` vertex indirection without CPU-side decompression.
- **Meshlet Native**: Built specifically for the programmable mesh shader pipeline.
- **Aggressive Packing**: Lean 16-byte vertex stride using quantized and octahedral attributes.

## Virtual Geometry Compatibility Note

The virtual geometry feature keeps TEMF at version `1` while the engine format
is still unstable. The payload layout may break compatibility with older
compiled mesh files in place. Users should rebuild static mesh and model package
outputs from source after the virtual geometry payload lands; no legacy
compatibility shim is planned for stale compiled payloads.

Virtual geometry payloads add advisory requested targets, accepted/generated
detail-level tables, skipped-detail diagnostics, preserved material section
ranges, cullable group metadata, streamable page metadata, cluster hierarchy
records, cluster-page links, validation flags, and cook diagnostics. Loaders
must reject stale or invalid virtual geometry payloads atomically so failed
rebuilds do not leave partially visible runtime state.

These additions are still allowed to break older compiled mesh files while the
engine format is unstable. When `virtualGeometryPayloadGeneration` changes, or
when validation reports missing requested targets, fallback coverage, cluster
hierarchy, page metadata, or cluster-page links, rebuild the source mesh or
source package instead of trying to mix old whole-object LOD data with new
cluster/page selection data.

## Binary Layout (all little-endian)
```
Header                // fixed-size MeshHeader (236 bytes), padded to 4KB
VertexData            // raw bytes (16b stride), aligned to 4KB
VertexRemapData       // indirection pool (u16 or u32), aligned to 4KB
SkinningData          // optional (8b stride), aligned to 4KB
PrimitiveIndexData    // meshlet-local indices (u8), aligned to 4KB
MeshletRecordTable    // offsets/counts per meshlet, aligned to 4KB
MeshletSectionTable   // draw call ranges, aligned to 4KB
MeshletBoundsTable    // optional culling spheres + cones, aligned to 4KB
VirtualGeometryRoot   // optional VirtualGeometryHeader + tables, 4-byte table alignment
```

### Header (236 bytes)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `TEMF` |
| 4 | 2 | version | Current version = 1 |
| 6 | 2 | reserved | Must be 0 |
| 8 | 4 | headerSize | Size of the header struct (236) |
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
| 60 | 4 | meshletFlags | bit 0: has bounds, bit 1: has virtual geometry |
| 64 | 4 | meshletBoundsOffset | 4KB aligned offset to bounds |
| 68 | 4 | meshletSkinningDataOffset | 4KB aligned offset to skinning stream |
| 72 | 4 | meshletSkinningDataCount | Number of skinned vertices (matches vertex count if used) |
| 76 | 4 | virtualGeometryHeaderOffset | Absolute offset to `VirtualGeometryHeader` when bit 1 is set |
| 80 | 4 | virtualGeometryHeaderSize | Size of `VirtualGeometryHeader` (116) |
| 84 | 4 | virtualGeometryPayloadGeneration | Current in-place payload generation |
| 88 | 4 | virtualGeometryReserved | Reserved, must be 0 |
| 92 | 4 | layout.strideBytes | Byte stride of each vertex (standard = 16) |
| 96 | 4 | layout.attributeCount | Attributes used (<= 8) |
| 100 | 96 | layout.attributes[8] | Vertex declaration (semantic/format/offset) |
| 196 | 12 | quantization.positionOffset | Bounding box center |
| 208 | 12 | quantization.positionScale | Bounding box half-extents |
| 220 | 8 | quantization.uvOffset | Minimum UV |
| 228 | 8 | quantization.uvScale | UV Range |

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
- **Culling**: Pre-computed `MeshletBounds` provide conservative frustum sphere checks. Cone data is still stored for diagnostics/future validation, but runtime cone rejection is disabled until imported mesh sidedness and winding safety are carried into the task shader.

### Meshlet Bounds Payload (48 bytes per meshlet)

| Field | Type | Description |
| --- | --- | --- |
| `center` | `float3` | Bounding sphere center in object space. |
| `radius` | `float` | Bounding sphere radius. |
| `coneApex` | `float3` | Cone apex for perspective backface checks. |
| `coneCutoff` | `float` | Cone cosine threshold. |
| `coneAxis` | `float3` | Cone axis (normalized by loader). |
| `coneApexOffset` | `float` | Reserved for future tuning, currently `0`. |

Loader rules:
- Invalid sphere values are hard errors.
- Invalid cone values are sanitized to conservative "do not backface cull" behavior.

### Virtual Geometry Header (116 bytes)

The root is present only when `meshletFlags & kMeshletHeaderFlagHasVirtualGeometry`
is set. All offsets are absolute file offsets. Table records are packed and
4-byte aligned relative to the root.

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | payloadGeneration | Must match the loader's current `kVirtualGeometryPayloadGeneration` |
| 4 | 4 | headerSize | Size of `VirtualGeometryHeader` (76) |
| 8 | 4 | detailLevelCount | Number of whole-mesh selectable LOD records |
| 12 | 4 | materialSectionCount | Number of material section records |
| 16 | 4 | sectionRangeCount | Number of per-LOD section ranges |
| 20 | 4 | cullableGroupCount | Number of cullable group records |
| 24 | 4 | pageCount | Number of virtual geometry page records |
| 28 | 4 | diagnosticRecordCount | Number of diagnostic table records |
| 32 | 4 | flags | Reserved for payload-level feature flags |
| 36 | 4 | sourceTriangleCount | Source triangle count before simplification |
| 40 | 4 | sourceVertexCount | Source vertex count before simplification |
| 44 | 4 | requestedTargetCount | Number of advisory requested target records |
| 48 | 4 | skippedDetailCount | Number of skipped/merged/rejected detail diagnostics |
| 52 | 4 | clusterCount | Number of cluster hierarchy records |
| 56 | 4 | clusterPageLinkCount | Number of cluster-page link records |
| 60 | 4 | validationFlagCount | Number of validation flag records |
| 64 | 4 | detailLevelTableOffset | Absolute offset to `VirtualGeometryDetailLevelRecord[]` |
| 68 | 4 | materialSectionTableOffset | Absolute offset to `VirtualGeometryMaterialSectionRecord[]` |
| 72 | 4 | sectionRangeTableOffset | Absolute offset to `VirtualGeometrySectionRangeRecord[]` |
| 76 | 4 | cullableGroupTableOffset | Absolute offset to `VirtualGeometryCullableGroupRecord[]` |
| 80 | 4 | pageTableOffset | Absolute offset to `VirtualGeometryPageRecord[]` |
| 84 | 4 | diagnosticTableOffset | Absolute offset to `VirtualGeometryDiagnosticRecord[]` |
| 88 | 4 | requestedTargetTableOffset | Absolute offset to `VirtualGeometryRequestedTargetRecord[]` |
| 92 | 4 | skippedDetailTableOffset | Absolute offset to `VirtualGeometrySkippedDetailRecord[]` |
| 96 | 4 | clusterTableOffset | Absolute offset to `VirtualGeometryClusterRecord[]` |
| 100 | 4 | clusterPageLinkTableOffset | Absolute offset to `VirtualGeometryClusterPageLinkRecord[]` |
| 104 | 4 | validationFlagTableOffset | Absolute offset to `VirtualGeometryValidationFlagRecord[]` |
| 108 | 4 | diagnosticStringOffset | Absolute offset to UTF-8/NUL diagnostic strings |
| 112 | 4 | diagnosticStringSize | Byte size of the diagnostic string table |

### Virtual Geometry Tables

- **Requested target record (20 bytes)**: `targetIndex`, target status, desired
  triangle count, desired meshlet count, and advisory requested ratio.
- **Detail level record (76 bytes)**: `lodIndex`, requested target index, target
  status, meshlet offset/count, vertex count, primitive count, section range
  offset/count, page range offset/count, geometric error, actual target ratio,
  and conservative local bounds.
- **Skipped detail record (44 bytes)**: requested target index, status, reason,
  nearest generated LOD, message string range, attempted candidate triangle and
  meshlet counts, requested ratio, candidate ratio, and nearest accepted ratio.
- **Material section record (28 bytes)**: section id, material slot, source
  triangle count, per-LOD range offset/count, and source-name string range.
- **Section range record (24 bytes)**: LOD, section id, meshlet offset/count,
  primitive offset, and primitive count.
- **Cullable group record (80 bytes)**: group id, LOD, vertex/primitive ranges,
  section id, page id, sphere bounds, and cone-culling data. The MVP mirrors one
  group per meshlet.
- **Page record (52 bytes)**: page id, covered LOD range, group range, fallback
  page id, cluster-page link range, priority, byte offset, and byte size. The
  MVP emits one page per LOD and keeps page `0` as the resident fallback.
- **Cluster record (60 bytes)**: cluster id, parent/child range, LOD, cullable
  group range, material section, page id, fallback cluster id, geometric error,
  and conservative local sphere bounds.
- **Cluster-page link record (20 bytes)**: cluster id, page id, first group,
  group count, and link flags for primary/fallback coverage.
- **Validation flag record (12 bytes)**: validation bit mask and optional
  message string range. Current bits are `AdaptiveTargetsPresent`,
  `FallbackCoverageValidated`, `ClusterHierarchyValidated`,
  `PageLinksValidated`, and `ClusterCoverageValidated` when the cooker proves
  regional source coverage.
- **Diagnostic record (20 bytes)**: severity plus string-table ranges for code
  and message text.

Validation rules:

- `meshletFlags` bit 1 requires a non-zero `virtualGeometryHeaderOffset`,
  `virtualGeometryHeaderSize == sizeof(VirtualGeometryHeader)`, and matching
  `virtualGeometryPayloadGeneration`.
- Table counts and offsets must stay inside the file and must not overlap
  earlier meshlet streams.
- Detail levels must contain non-zero meshlet and primitive counts, monotonic
  finite geometric error, valid section/page ranges, and conservative finite
  bounds.
- Requested target records must have finite positive ratios and statuses in the
  known requested/generated/merged/skipped/rejected range.
- Skipped detail records must reference a requested target, a valid nearest
  generated fallback, a known skip reason, and a message range inside the
  diagnostic string table.
- Section ranges must stay inside the meshlet table and may not overlap within a
  detail level.
- Cullable group bounds must be finite. Invalid cone data is allowed only when
  converted to conservative "do not cull" behavior.
- Cluster hierarchy records must be acyclic, reference valid pages and material
  sections, and provide finite bounds. A missing high-detail cluster must have a
  fallback path to a resident coarser cluster.
- `ClusterCoverageValidated` is written only when the cooker can prove that
  every detail level tiles the same source-region set and that every parent
  cluster's children exactly cover the parent's source-region range. The source
  region table is cook-time metadata in TEMF v1; runtime consumes the persisted
  validation bit and structural cluster records.
- Page records must reference valid cullable group ranges, valid fallback pages,
  non-zero byte sizes, and valid cluster-page link ranges.
- Validation flags must include adaptive targets, fallback coverage, cluster
  hierarchy, and page links before export packaging can accept the mesh.
- Page-streamed rendering is considered ready only when page records, page
  byte sizes, cluster-page links, material sections, finite bounds, fallback
  links, and the fallback/hierarchy/page-link validation bits are all present.
  Missing or corrupt high-detail page metadata is rejected before render
  publication; fallback coverage remains the required visible substitute.
- Tiny or simple meshes may intentionally skip page streaming when the cooker
  writes a skipped-detail reason such as `MeshTooSmall`, `TooFewTriangles`, or
  `TooFewMeshlets`. These meshes remain renderable, but inspectors and
  automation must report `smallMeshStreamingSkipped` and the skipped reason
  instead of exposing page-streaming controls as if real pages existed.
- `ClusterHierarchyValidated` only proves structural parent/child metadata.
  Runtime mixed-detail cluster rendering additionally requires
  `ClusterCoverageValidated`; without it the renderer must fail open to a
  complete highest-detail range instead of dispatching sparse mixed LOD slices.
- Failed validation marks the asset as `NeedsRebuild` or rejects the reload
  before publishing new runtime state. Existing usable meshes remain cached, and
  editor/package rebuilds publish compiled outputs through temporary files.
