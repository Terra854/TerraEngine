# Terra Engine Mesh File (TEMF) Specification

TEMF stores a versioned production directory and encoded mesh blocks. The
canonical ABI is [`MeshHeader` (`TemfProductionHeader`)](../Engine/File%20Formats/TerraEngineMeshFile.h),
not the decoded `MeshletRuntimeLayout`. All root fields are packed little-endian.
Codecs and quantization profiles determine how payloads are reconstructed; a
raw, zero-copy or universally 4KB-aligned stream layout must not be assumed.

## Production contract

The current format is major `1`, minor `0`, with `headerBytes = 192` and semantic
contract `0x31445250464d4554` (`TEMFPRD1`). The loader requires the production
header/contract; historical meshlet root layouts are not supported compatibility
formats. Rebuild stale compiled assets from source rather than mixing layouts.
The unchanged major version alone does not establish compatibility.

## Binary layout (all little-endian)

```text
MeshHeader                 // 192 bytes, alignment 1
TemfPageDescriptor[]        // 64-byte records; pageTableOffset/pageCount
TemfBlockDescriptor[]       // 112-byte records; blockTableOffset/blockCount
TemfQuantizationDomain[]    // 128-byte records; quantizationTableOffset/count
TemfBlockDependency[]       // 16-byte records; dependencyTableOffset/count
Encoded block payloads     // payloadOffset/payloadBytes; per-block codec metadata
```

Follow the recorded 64-bit file-relative offsets and bounded ranges; tables and
payloads need not start at a universal 4KB boundary. The production writer uses
`kTemfDirectoryAlignment = 16` for directory-table and payload-base placement.
A paged mesh starts its page table after the 192-byte root; a direct-resident
mesh has no page table. Individual encoded blocks follow their recorded offsets,
lengths and alignment requirements, rather than an assumed 4KB or 16-byte stride.

### Header (192 bytes)

| Offset | Size | Field |
|---|---:|---|
| 0 | 4 | `magic` |
| 4 | 2 | `formatVersion` |
| 6 | 2 | `minorVersion` |
| 8 | 4 | `headerBytes` |
| 12 | 4 | `representationFlags` |
| 16 | 8 | `assetIdentity` |
| 24 | 8 | `sourceContentHash` |
| 32 | 8 | `buildRecipeHash` |
| 40 | 8 | `toolIdentity` |
| 48 | 8 | `generationIdentity` |
| 56 | 4 | `selectedCodecProfileId` |
| 60 | 4 | `selectedQuantizationProfileId` |
| 64 | 4 | `logicalPageSizeBytes` |
| 68 | 4 | `pageCount` |
| 72 | 4 | `blockCount` |
| 76 | 4 | `quantizationDomainCount` |
| 80 | 4 | `dependencyCount` |
| 84 | 4 | `reserved0` |
| 88 | 8 | `pageTableOffset` |
| 96 | 8 | `blockTableOffset` |
| 104 | 8 | `quantizationTableOffset` |
| 112 | 8 | `dependencyTableOffset` |
| 120 | 8 | `payloadOffset` |
| 128 | 8 | `payloadBytes` |
| 136 | 8 | `directoryHash` |
| 144 | 8 | `payloadHash` |
| 152 | 8 | `requiredCapabilityMask` |
| 160 | 8 | `attributeCoverageMask` |
| 168 | 8 | `formatContractId` |
| 176 | 8 | `directoryBytes` |
| 184 | 8 | `reserved1` |

Validate magic (`TEMF`), major/minor, header size and contract before following
any directory. Use checked multiplication and subtraction-based range checks
before allocation/decompression. Validate directory/payload/block integrity,
capability and attribute masks, page dependencies and codec/quantization domains
before publishing immutable runtime state. A rejected reload retains last-good
state. The packed record definitions and static assertions are the ABI authority.

## Decoded meshlet and virtual-geometry payloads

The records below describe reconstructed meshlet/virtual-geometry payload data.
They are **not** an alternative on-disk root. Historical meshlet header fields
such as `meshletFlags`, `virtualGeometryHeaderOffset`, `layout`, and `quantization`
now belong to `MeshletRuntimeLayout` after validated production block decoding.
The current compiler emits the virtual header and each nonempty table as separate
production streams. `ResourceManagerMesh.cpp` selects matching blocks by the exact
pair `streamKind` and `attributeMask`, decodes them, and concatenates their bytes in
directory order. It reconstructs typed vectors from those streams, rather than
following table offsets into a single decoded virtual-geometry payload.

For virtual-geometry records, `streamKind` is `TemfStreamKind::Metadata` (`9`)
and `attributeMask` is one exact `kTemfMetadata*` subkind below. These masks are
selectors for this lookup, not offsets or a substitute for the production root's
file-relative block-directory ranges. Empty source tables produce no stream.

| Metadata subkind | `attributeMask` | Decoded destination |
|---|---:|---|
| `kTemfMetadataVirtualHeader` | `1 << 4` | One `VirtualGeometryHeader` |
| `kTemfMetadataVirtualDetailLevels` | `1 << 5` | Detail-level records |
| `kTemfMetadataVirtualMaterialSections` | `1 << 6` | Material-section records |
| `kTemfMetadataVirtualSectionRanges` | `1 << 7` | Section-range records |
| `kTemfMetadataVirtualCullableGroups` | `1 << 8` | Cullable-group records |
| `kTemfMetadataVirtualPages` | `1 << 9` | Page records |
| `kTemfMetadataVirtualRequestedTargets` | `1 << 10` | Requested-target records |
| `kTemfMetadataVirtualSkippedDetails` | `1 << 11` | Skipped-detail records |
| `kTemfMetadataVirtualClusters` | `1 << 12` | Cluster records |
| `kTemfMetadataVirtualClusterPageLinks` | `1 << 13` | Cluster-page-link records |
| `kTemfMetadataVirtualConeSafety` | `1 << 14` | Cone-safety records |
| `kTemfMetadataVirtualValidationFlags` | `1 << 15` | Validation-flag records |
| `kTemfMetadataVirtualDiagnostics` | `1 << 16` | Diagnostic records |
| `kTemfMetadataVirtualDiagnosticStrings` | `1 << 17` | Diagnostic string bytes |

`kTemfMetadataRuntimeLayout` (`1 << 0`), meshlet records (`1 << 1`), sections
(`1 << 2`) and bounds (`1 << 3`) are separate metadata streams too. Geometry and
identity data use their own stream kinds. Attribute strides and codecs come from
the validated block descriptors and quantization domains.

### Historical meshlet packing notes

The older meshlet layout used the packing descriptions below. They are reference
notes for retained runtime fields, not the encoded production-block layout or a
universal decoded vertex stride. Read current stream descriptors as described above.

- **Vertex Attribute (12 bytes)**:
  - `Position`: `R10G10B10A2Snorm` (10-bit per axis).
  - `Normal`: `Oct32` (High-precision octahedral 2D mapping).
  - `TexCoord`: `R16G16Unorm` (16-bit precision).
- **Skinning Stream (8 bytes)**: 4x `uint8` Joint Indices + 4x `unorm8` Weights.
- **Meshlet Record (16 bytes)**: Stores `vertexOffset`, `vertexCount`, `primitiveIndexOffset`, `primitiveIndexCount`.

### Implementation Notes
- **Alignment**: Follow the production directory's recorded offsets and encoded sizes, then validate the decoded record layout. There is no universal 4096-byte alignment requirement for on-disk directories or encoded blocks; any aligned IO staging is a backend concern, not a file-format invariant.
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

### Virtual Geometry Header (124 bytes)

For a production virtual-geometry representation, the loader reads this record
from `Metadata` / `kTemfMetadataVirtualHeader`; the production representation flag
and required metadata streams determine availability. The compiler retains the
old packed-layout offset fields in this record and can emit nonzero values for
them. Current stream reconstruction does not use them to locate tables: they are
neither authoritative file offsets for TEMFPRD1 tables nor offsets into a single
decoded arena. Do not seek to them or assume they are zero.

The table below records the current C++ record shape, including its retained
legacy fields. It does not define a second production root or an old-file loader.

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | `payloadGeneration` | Current `kVirtualGeometryPayloadGeneration` |
| 4 | 4 | `headerSize` | `sizeof(VirtualGeometryHeader)` (124) |
| 8 | 4 | `detailLevelCount` | Record count for `detailLevel` data |
| 12 | 4 | `materialSectionCount` | Record count for `materialSection` data |
| 16 | 4 | `sectionRangeCount` | Record count for `sectionRange` data |
| 20 | 4 | `cullableGroupCount` | Record count for `cullableGroup` data |
| 24 | 4 | `pageCount` | Record count for `page` data |
| 28 | 4 | `diagnosticRecordCount` | Record count for `diagnosticRecord` data |
| 32 | 4 | `flags` | Cooked virtual-geometry validation flags |
| 36 | 4 | `sourceTriangleCount` | Record count for `sourceTriangle` data |
| 40 | 4 | `sourceVertexCount` | Record count for `sourceVertex` data |
| 44 | 4 | `requestedTargetCount` | Record count for `requestedTarget` data |
| 48 | 4 | `skippedDetailCount` | Record count for `skippedDetail` data |
| 52 | 4 | `clusterCount` | Record count for `cluster` data |
| 56 | 4 | `clusterPageLinkCount` | Record count for `clusterPageLink` data |
| 60 | 4 | `validationFlagCount` | Record count for `validationFlag` data |
| 64 | 4 | `coneSafetyCount` | Record count for `coneSafety` data |
| 68 | 4 | `detailLevelTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualDetailLevels` stream |
| 72 | 4 | `materialSectionTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualMaterialSections` stream |
| 76 | 4 | `sectionRangeTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualSectionRanges` stream |
| 80 | 4 | `cullableGroupTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualCullableGroups` stream |
| 84 | 4 | `pageTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualPages` stream |
| 88 | 4 | `diagnosticTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualDiagnostics` stream |
| 92 | 4 | `requestedTargetTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualRequestedTargets` stream |
| 96 | 4 | `skippedDetailTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualSkippedDetails` stream |
| 100 | 4 | `clusterTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualClusters` stream |
| 104 | 4 | `clusterPageLinkTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualClusterPageLinks` stream |
| 108 | 4 | `validationFlagTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualValidationFlags` stream |
| 112 | 4 | `coneSafetyTableOffset` | Retained legacy offset; use `kTemfMetadataVirtualConeSafety` stream |
| 116 | 4 | `diagnosticStringOffset` | Retained legacy offset; use `kTemfMetadataVirtualDiagnosticStrings` stream |
| 120 | 4 | `diagnosticStringSize` | Size of the separate diagnostic-string stream |

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

- Production virtual-geometry availability requires the representation flag, a
  correctly sized header metadata stream, and the required reconstructed tables.
  Legacy `MeshletRuntimeLayout` offset fields do not locate those streams.
- Validate encoded file ranges through the production directory, decode each
  selected stream with its declared codec/domain, and validate typed record
  sizes and index ranges before cache or GPU publication.
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
