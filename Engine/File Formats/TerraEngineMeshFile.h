// TerraEngineMeshFile.h — Binary format definition for meshlet-based geometry (TEMF).
//
// This file defines the custom mesh format used by TerraEngine, optimized for
// modern meshlet-based rendering pipelines. It includes vertex attribute
// definitions, quantization parameters, and meshlet record structures used for
// efficient culling and GPU-driven rendering.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Versioned constants shared by the compressor and viewer when reading/writing the binary blob.
constexpr std::uint32_t kMeshMagicValue = 0x464d4554u; // 'TEMF' little endian
constexpr std::size_t kMaxVertexAttributes = 8;
constexpr std::uint16_t kMeshCurrentVersion = 1;
constexpr std::uint32_t kMeshletPackedVertexStrideBytes = 16u;
constexpr std::uint32_t kMeshletPrimitiveIndexSizeBytes = sizeof(std::uint8_t);
constexpr std::uint32_t kMeshletVertexRemapDecodedElementSizeBytes = sizeof(std::uint32_t);
constexpr std::uint32_t kMeshletVertexDataAlignmentBytes = 4096u;
constexpr std::uint32_t kMeshletIndexDataAlignmentBytes = 4096u;
constexpr std::uint32_t kMeshletMaxVertices = 64;
constexpr std::uint32_t kMeshletMaxTriangles = 124;
constexpr std::uint32_t kMeshletHeaderFlagHasBounds = 1u << 0u;
constexpr std::uint32_t kMeshletHeaderFlagHasVirtualGeometry = 1u << 1u;
constexpr std::uint32_t kVirtualGeometryPayloadGeneration = 2u;
constexpr std::uint32_t kVirtualGeometryInvalidIndex = 0xffffffffu;
constexpr std::uint32_t kVirtualGeometryValidationFlagAdaptiveTargetsPresent = 1u << 0u;
constexpr std::uint32_t kVirtualGeometryValidationFlagFallbackCoverageValidated = 1u << 1u;
constexpr std::uint32_t kVirtualGeometryValidationFlagClusterHierarchyValidated = 1u << 2u;
constexpr std::uint32_t kVirtualGeometryValidationFlagPageLinksValidated = 1u << 3u;
constexpr std::uint32_t kVirtualGeometryValidationFlagClusterCoverageValidated = 1u << 4u;

// Detail target status mirrors the renderer diagnostics. Records store integer
// values so the unstable TEMF v1 payload can be read by simple tools without
// including renderer headers.
enum class VirtualGeometryDetailTargetRecordStatus : std::uint32_t
{
	Requested = 0u,
	Generated = 1u,
	Merged = 2u,
	Skipped = 3u,
	Rejected = 4u
};

// Skipped reasons explain why a requested adaptive ratio did not become its
// own level. These names are intentionally specific enough for Inspector text
// and automation responses to avoid guessing from counts alone.
enum class VirtualGeometrySkippedDetailRecordReason : std::uint32_t
{
	None = 0u,
	MeshTooSmall = 1u,
	TooFewTriangles = 2u,
	TooFewMeshlets = 3u,
	InsufficientTriangleReduction = 4u,
	InsufficientMeshletReduction = 5u,
	InvalidSimplificationOutput = 6u,
	MaterialSafetyRejected = 7u,
	GeometricErrorRejected = 8u,
	DuplicateOfPreviousLevel = 9u
};

// Vertex attribute semantics mirror typical DCC exports and describe what the data represents.
// AttributeSemantic names the supported states explicitly so serialization and switches stay readable.
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
// AttributeFormat names the supported states explicitly so serialization and switches stay readable.
enum class AttributeFormat : std::uint32_t
{
	R10G10B10A2Snorm = 1,
	R10G10B10A2Unorm = 2,
	R16G16Unorm = 3,
	Oct32 = 4
};

// Remap stream encoding tells loaders whether on-disk remap elements are 16-bit
// or 32-bit before they are expanded to a normalized uint32 runtime view.
enum class MeshletRemapEncoding : std::uint32_t
{
	Uint32 = 0,
	Uint16 = 1
};

// GetRemapElementSizeBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
constexpr std::uint32_t GetRemapElementSizeBytes(MeshletRemapEncoding encoding)
{
	return encoding == MeshletRemapEncoding::Uint16 ? static_cast<std::uint32_t>(sizeof(std::uint16_t)) : static_cast<std::uint32_t>(sizeof(std::uint32_t));
}

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
// struct VertexLayout groups related state and operations so callers have one documented contract.
struct VertexLayout
{
	std::uint32_t strideBytes;
	std::uint32_t attributeCount;
	VertexAttribute attributes[kMaxVertexAttributes];
};

// Parameters required to denormalize packed positions/UVs back into model space.
// struct QuantizationInfo groups related state and operations so callers have one documented contract.
struct QuantizationInfo
{
	float positionOffset[3];  // Center of the bounding box
	float positionScale[3];   // Half-extent for each axis (max deviation from center)
	float uvOffset[2];        // Minimum UV
	float uvScale[2];         // Range of UV coordinates
};

// Per-meshlet table entry. Offsets/counts point into packed meshlet streams.
// struct MeshletRecord groups related state and operations so callers have one documented contract.
struct MeshletRecord
{
	std::uint32_t vertexOffset;          // Offset into meshlet vertex remap stream.
	std::uint32_t vertexCount;           // Number of local vertices (max 64).
	std::uint32_t primitiveIndexOffset;  // Offset into uint8 meshlet-local primitive stream.
	std::uint32_t primitiveIndexCount;   // Primitive element count (triangle-aligned).
};

// Maps a render section to a contiguous range of meshlets.
// struct MeshletSection groups related state and operations so callers have one documented contract.
struct MeshletSection
{
	std::uint32_t meshletOffset;
	std::uint32_t meshletCount;
};

// Optional per-meshlet sphere + cone bounds for visibility/culling work.
// struct MeshletBounds groups related state and operations so callers have one documented contract.
struct MeshletBounds
{
	float center[3];
	float radius;
	float coneApex[3];
	float coneCutoff;
	float coneAxis[3];
	float coneApexOffset;
};

// VirtualGeometryHeader is the root table for the new in-place TEMF v1 payload.
// Offsets are absolute file offsets so the loader can validate each range before
// publishing any data to ResourceManager caches or GPU upload code.
struct VirtualGeometryHeader
{
	std::uint32_t payloadGeneration;
	std::uint32_t headerSize;
	std::uint32_t detailLevelCount;
	std::uint32_t materialSectionCount;
	std::uint32_t sectionRangeCount;
	std::uint32_t cullableGroupCount;
	std::uint32_t pageCount;
	std::uint32_t diagnosticRecordCount;
	std::uint32_t flags;
	std::uint32_t sourceTriangleCount;
	std::uint32_t sourceVertexCount;
	std::uint32_t requestedTargetCount;
	std::uint32_t skippedDetailCount;
	std::uint32_t clusterCount;
	std::uint32_t clusterPageLinkCount;
	std::uint32_t validationFlagCount;
	std::uint32_t detailLevelTableOffset;
	std::uint32_t materialSectionTableOffset;
	std::uint32_t sectionRangeTableOffset;
	std::uint32_t cullableGroupTableOffset;
	std::uint32_t pageTableOffset;
	std::uint32_t diagnosticTableOffset;
	std::uint32_t requestedTargetTableOffset;
	std::uint32_t skippedDetailTableOffset;
	std::uint32_t clusterTableOffset;
	std::uint32_t clusterPageLinkTableOffset;
	std::uint32_t validationFlagTableOffset;
	std::uint32_t diagnosticStringOffset;
	std::uint32_t diagnosticStringSize;
};

// A requested target records the ratio the developer asked for before the
// cooker decides whether that level is useful enough to generate. The fields
// are counts, not offsets, because no geometry has been emitted at this point.
struct VirtualGeometryRequestedTargetRecord
{
	std::uint32_t targetIndex;        // Stable index in the requested target list.
	std::uint32_t status;             // VirtualGeometryDetailTargetRecordStatus.
	std::uint32_t desiredTriangleCount; // Approximate triangle goal after simplification.
	std::uint32_t desiredMeshletCount;  // Approximate meshlet goal for diagnostics.
	float requestedRatio;             // Advisory ratio relative to the source mesh.
};

// A detail level is a complete selectable representation for the MVP. Later
// page/group refinement can still point back to these stable LOD ranges.
struct VirtualGeometryDetailLevelRecord
{
	std::uint32_t lodIndex;
	std::uint32_t requestedTargetIndex;
	std::uint32_t targetStatus;
	std::uint32_t meshletOffset;
	std::uint32_t meshletCount;
	std::uint32_t vertexCount;
	std::uint32_t primitiveCount;
	std::uint32_t sectionRangeOffset;
	std::uint32_t sectionRangeCount;
	std::uint32_t pageRangeOffset;
	std::uint32_t pageRangeCount;
	float geometricError;
	float targetRatio;
	float boundsMin[3];
	float boundsMax[3];
};

using VirtualGeometryGeneratedLevelRecord = VirtualGeometryDetailLevelRecord;

// Skipped detail records keep every rejected or merged target visible to users.
// The message range points at the shared diagnostic string table to avoid fixed
// record bloat when most targets generate successfully.
struct VirtualGeometrySkippedDetailRecord
{
	std::uint32_t targetIndex;                 // Requested target that did not produce a unique level.
	std::uint32_t status;                      // VirtualGeometryDetailTargetRecordStatus, usually Skipped/Merged/Rejected.
	std::uint32_t reason;                      // VirtualGeometrySkippedDetailRecordReason.
	std::uint32_t nearestGeneratedLodIndex;    // Fallback/generated LOD used instead.
	std::uint32_t messageStringOffset;         // Offset into virtualGeometryDiagnosticStrings.
	std::uint32_t messageStringSize;           // Byte count for the explanatory message.
	std::uint32_t candidateTriangleCount;      // Actual attempted simplification triangles, or zero when no candidate existed.
	std::uint32_t candidateMeshletCount;       // Actual attempted simplification meshlets, or zero when no candidate existed.
	float requestedRatio;                      // Original advisory ratio.
	float actualCandidateRatio;                // Candidate triangle ratio relative to source geometry, or zero when unavailable.
	float actualNearestRatio;                  // Ratio of the selected/generated fallback.
};

// Section records preserve material identity independently from the physical
// meshlet order chosen by simplification and packing.
struct VirtualGeometryMaterialSectionRecord
{
	std::uint32_t sectionId;
	std::uint32_t materialSlot;
	std::uint32_t sourceTriangleCount;
	std::uint32_t perLodRangeOffset;
	std::uint32_t perLodRangeCount;
	std::uint32_t sourceNameStringOffset;
	std::uint32_t sourceNameStringSize;
};

// A section range names the meshlets and primitives that belong to one material
// section inside one detail level.
struct VirtualGeometrySectionRangeRecord
{
	std::uint32_t lodIndex;
	std::uint32_t sectionId;
	std::uint32_t meshletOffset;
	std::uint32_t meshletCount;
	std::uint32_t primitiveOffset;
	std::uint32_t primitiveCount;
};

// Cullable groups mirror meshlets for the first implementation but use a
// separate record so future region or page culling can evolve without changing
// editor-facing diagnostics.
struct VirtualGeometryCullableGroupRecord
{
	std::uint32_t groupIndex;
	std::uint32_t lodIndex;
	std::uint32_t vertexOffset;
	std::uint32_t vertexCount;
	std::uint32_t primitiveOffset;
	std::uint32_t primitiveCount;
	std::uint32_t sectionId;
	std::uint32_t pageId;
	float boundsCenter[3];
	float boundsRadius;
	float coneApex[3];
	float coneCutoff;
	float coneAxis[3];
	float coneApexOffset;
};

// Page records are optional in the MVP. A renderable asset must keep fallback
// detail visible even when no page records are present or higher-detail pages
// fail to stream.
struct VirtualGeometryPageRecord
{
	std::uint32_t pageId;
	std::uint32_t firstLodIndex;
	std::uint32_t lodCount;
	std::uint32_t groupOffset;
	std::uint32_t groupCount;
	std::uint32_t fallbackPageId;
	std::uint32_t clusterPageLinkOffset;
	std::uint32_t clusterPageLinkCount;
	std::uint32_t priority;
	std::uint64_t byteOffset;
	std::uint64_t byteSize;
};

// Cluster records describe a hierarchical coverage tree for mixed-detail cuts.
// Cookers split each detail into source-region-backed spatial clusters, then
// validate parent/child coverage before allowing sparse mixed-detail selection.
struct VirtualGeometryClusterRecord
{
	std::uint32_t clusterId;            // Stable cluster identifier inside the mesh.
	std::uint32_t parentClusterId;      // kVirtualGeometryInvalidIndex for the root.
	std::uint32_t firstChildClusterId;  // First child id when children are contiguous.
	std::uint32_t childCount;           // Number of child clusters.
	std::uint32_t lodIndex;             // Detail level that owns the cluster geometry.
	std::uint32_t groupOffset;          // First cullable group covered by this cluster.
	std::uint32_t groupCount;           // Number of cullable groups covered by this cluster.
	std::uint32_t materialSectionId;    // Material section ownership used by validation.
	std::uint32_t pageId;               // Primary page containing the cluster data.
	std::uint32_t fallbackClusterId;    // Coarser resident cluster used when the primary page is missing.
	float geometricError;               // Screen-error input for refinement decisions.
	float boundsCenter[3];              // Conservative local-space sphere center.
	float boundsRadius;                 // Conservative local-space sphere radius.
};

// Cluster-page links are a compact join table. They let one page provide data
// for multiple clusters and let one cluster reference fallback/streaming pages.
struct VirtualGeometryClusterPageLinkRecord
{
	std::uint32_t clusterId;   // Cluster whose geometry or fallback coverage is represented.
	std::uint32_t pageId;      // Page that must be resident for this link.
	std::uint32_t firstGroup;  // First cullable group supplied by the page.
	std::uint32_t groupCount;  // Number of groups supplied by the page.
	std::uint32_t flags;       // Reserved for fallback/primary/streaming role bits.
};

// Validation flags record which expensive cook/load checks were run. Tools can
// warn about stale data without revalidating the whole hierarchy every frame.
struct VirtualGeometryValidationFlagRecord
{
	std::uint32_t flags;                  // kVirtualGeometryValidationFlag* bit mask.
	std::uint32_t messageStringOffset;    // Optional text in the diagnostic string table.
	std::uint32_t messageStringSize;      // Byte count for that text.
};

// Diagnostic records point into the shared diagnostic string table. Keeping the
// text out of fixed records lets the cooker emit rich warnings without bloating
// every asset when there are no warnings.
struct VirtualGeometryDiagnosticRecord
{
	std::uint32_t severity;
	std::uint32_t codeStringOffset;
	std::uint32_t codeStringSize;
	std::uint32_t messageStringOffset;
	std::uint32_t messageStringSize;
};

// Final on-disk header that prefixes every meshlet-only TEMF file.
// struct MeshHeader groups related state and operations so callers have one documented contract.
struct MeshHeader
{
	char magic[4];
	std::uint16_t version;
	std::uint16_t reserved;
	std::uint32_t headerSize;
	std::uint32_t meshletVertexCount;         // Global packed-vertex stream count.
	std::uint32_t meshletVertexRemapCount;    // Number of remap entries (decoded count).
	std::uint32_t meshletPrimitiveIndexCount; // Number of uint8 entries in primitive stream.
	std::uint32_t meshletCount;
	std::uint32_t meshletSectionCount;
	std::uint32_t meshletBoundsCount;
	std::uint32_t meshletVertexDataOffset;
	std::uint32_t meshletVertexRemapDataOffset;
	std::uint32_t meshletPrimitiveIndexDataOffset;
	std::uint32_t meshletTableOffset;
	std::uint32_t meshletSectionOffset;
	std::uint32_t meshletVertexRemapEncoding; // MeshletRemapEncoding
	std::uint32_t meshletFlags;               // kMeshletHeaderFlag*
	std::uint32_t meshletBoundsOffset;
	std::uint32_t meshletSkinningDataOffset; // Byte offset to 8-byte skinning stream (Joints/Weights)
	std::uint32_t meshletSkinningDataCount;  // Number of vertices with skinning data
	std::uint32_t virtualGeometryHeaderOffset; // Absolute offset to VirtualGeometryHeader when kMeshletHeaderFlagHasVirtualGeometry is set.
	std::uint32_t virtualGeometryHeaderSize; // Size of the root virtual geometry header for atomic loader validation.
	std::uint32_t virtualGeometryPayloadGeneration; // kVirtualGeometryPayloadGeneration for this unstable TEMF v1 development payload.
	std::uint32_t virtualGeometryReserved;
	VertexLayout layout;
	QuantizationInfo quantization;
};
#pragma pack(pop)

static_assert(sizeof(MeshHeader) <= 640, "TEMF header should remain compact");

// In-memory representation of an entire .temf payload.
// Raw byte storage keeps payload ownership in one place while spans/vectors expose typed views.
struct Mesh
{
	MeshHeader header{};
	std::vector<std::uint8_t> storage;
	std::span<const std::uint8_t> meshletVertexData;
	// Runtime always works with uint32 remap entries even if on-disk storage used uint16.
	std::vector<std::uint32_t> meshletVertexRemapDecoded;
	std::span<const std::uint8_t> meshletVertexRemapData;
	std::span<const std::uint8_t> meshletSkinningData;
	std::span<const std::uint8_t> meshletPrimitiveIndexData;
	std::vector<MeshletRecord> meshletRecords;
	std::vector<MeshletSection> meshletSections;
	std::vector<MeshletBounds> meshletBounds;
	bool hasVirtualGeometry = false;
	std::uint32_t virtualGeometryPayloadGeneration = 0u;
	VirtualGeometryHeader virtualGeometryHeader{};
	std::vector<VirtualGeometryDetailLevelRecord> virtualGeometryDetailLevels;
	std::vector<VirtualGeometryMaterialSectionRecord> virtualGeometryMaterialSections;
	std::vector<VirtualGeometrySectionRangeRecord> virtualGeometrySectionRanges;
	std::vector<VirtualGeometryCullableGroupRecord> virtualGeometryCullableGroups;
	std::vector<VirtualGeometryPageRecord> virtualGeometryPages;
	std::vector<VirtualGeometryRequestedTargetRecord> virtualGeometryRequestedTargets;
	std::vector<VirtualGeometrySkippedDetailRecord> virtualGeometrySkippedDetails;
	std::vector<VirtualGeometryClusterRecord> virtualGeometryClusters;
	std::vector<VirtualGeometryClusterPageLinkRecord> virtualGeometryClusterPageLinks;
	std::vector<VirtualGeometryValidationFlagRecord> virtualGeometryValidationFlags;
	std::vector<VirtualGeometryDiagnosticRecord> virtualGeometryDiagnostics;
	std::string virtualGeometryDiagnosticStrings;
	// Object-level culling needs one conservative mesh bound before the GPU
	// sees individual meshlets. It is derived during load so render systems can
	// cheaply transform it into world space without walking every meshlet.
	bool hasLocalBounds = false;
	float localBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
	float localBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
};
