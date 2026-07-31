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
constexpr std::uint16_t kMeshCurrentMinorVersion = 0;
// TEMFPRD1 is a semantic contract identity, not a planning or feature number.
// Readers use it together with the 192-byte header size to reject every
// pre-production TEMF layout before allocating table or payload storage.
constexpr std::uint64_t kTemfProductionContractId = 0x31445250464d4554ull;
constexpr std::uint32_t kTemfProductionHeaderBytes = 192u;
constexpr std::uint32_t kTemfRepresentationDirectMeshlets = 1u << 0u;
constexpr std::uint32_t kTemfRepresentationVirtualGeometry = 1u << 1u;
constexpr std::uint32_t kTemfRepresentationDeformableStreams = 1u << 2u;
constexpr std::uint32_t kTemfRepresentationKnownMask =
	kTemfRepresentationDirectMeshlets
	| kTemfRepresentationVirtualGeometry
	| kTemfRepresentationDeformableStreams;
constexpr std::uint32_t kTemfDirectResidentPageId = 0xffffffffu;
constexpr std::uint32_t kTemfBlockFlagDirectResident = 1u << 0u;
constexpr std::uint32_t kTemfBlockKnownFlags = kTemfBlockFlagDirectResident;
constexpr std::uint32_t kTemfPageSize32KiB = 32u * 1024u;
constexpr std::uint32_t kTemfPageSize64KiB = 64u * 1024u;
constexpr std::uint32_t kTemfPageSize128KiB = 128u * 1024u;
constexpr std::uint32_t kTemfMaximumPageCount = (1u << 24u) - 1u;
constexpr std::uint32_t kTemfMaximumBlockCount = (1u << 26u) - 1u;
constexpr std::uint32_t kTemfMaximumQuantizationDomainCount = (1u << 20u) - 1u;
constexpr std::uint32_t kTemfMaximumDependencyCount = (1u << 28u) - 1u;
constexpr std::uint64_t kTemfMaximumDecodedBlockBytes = 256ull * 1024ull * 1024ull;
constexpr std::uint32_t kTemfDirectoryAlignment = 16u;
constexpr std::uint32_t kMeshletPackedVertexStrideBytes = 16u;
constexpr std::uint32_t kMeshletPrimitiveIndexSizeBytes = sizeof(std::uint8_t);
constexpr std::uint32_t kMeshletVertexRemapDecodedElementSizeBytes = sizeof(std::uint32_t);
constexpr std::uint32_t kMeshletVertexDataAlignmentBytes = 4096u;
constexpr std::uint32_t kMeshletIndexDataAlignmentBytes = 4096u;
constexpr std::uint32_t kMeshletMaxVertices = 64;
constexpr std::uint32_t kMeshletMaxTriangles = 124;
constexpr std::uint32_t kMeshletHeaderFlagHasBounds = 1u << 0u;
constexpr std::uint32_t kMeshletHeaderFlagHasVirtualGeometry = 1u << 1u;
constexpr std::uint32_t kTemfMaximumSkinInfluences = 4u;
constexpr std::uint32_t kTemfMaximumPaletteJoints = 256u;
constexpr std::uint32_t kTemfSkinningStrideBytes = 8u;
constexpr std::uint32_t kTemfDeformableFlagConservativeMorphEnvelope = 1u << 0u;
constexpr std::uint32_t kTemfDeformableFlagZeroWeightMeansIdentity = 1u << 1u;
constexpr std::uint32_t kTemfDeformableFlagVertexPaletteIndices = 1u << 2u;
constexpr std::uint32_t kTemfDeformableFlagDenseMorphDeltas = 1u << 3u;
constexpr std::uint32_t kVirtualGeometryPayloadGeneration = 2u;
constexpr std::uint32_t kVirtualGeometryInvalidIndex = 0xffffffffu;
constexpr std::uint32_t kVirtualGeometryValidationFlagAdaptiveTargetsPresent = 1u << 0u;
constexpr std::uint32_t kVirtualGeometryValidationFlagFallbackCoverageValidated = 1u << 1u;
constexpr std::uint32_t kVirtualGeometryValidationFlagClusterHierarchyValidated = 1u << 2u;
constexpr std::uint32_t kVirtualGeometryValidationFlagPageLinksValidated = 1u << 3u;
constexpr std::uint32_t kVirtualGeometryValidationFlagClusterCoverageValidated = 1u << 4u;
constexpr std::uint32_t kVirtualGeometryValidationFlagConeSafetyMetadataValidated = 1u << 5u;
constexpr std::uint32_t kVirtualGeometryConeSafetyFlagConeValid = 1u << 0u;
constexpr std::uint32_t kVirtualGeometryConeSafetyFlagEligible = 1u << 1u;
constexpr std::uint32_t kTemfDemandedPageMaximumSections = 16u;
constexpr std::uint64_t kVirtualGeometryCodecCapabilityExpanded = 1ull << 0u;
constexpr std::uint64_t kVirtualGeometryCodecCapabilityTerra = 1ull << 1u;
constexpr std::uint64_t kVirtualGeometryCodecCapabilityMeshoptimizer = 1ull << 2u;
constexpr std::uint64_t kVirtualGeometryCodecCapabilityAll =
	kVirtualGeometryCodecCapabilityExpanded
	| kVirtualGeometryCodecCapabilityTerra
	| kVirtualGeometryCodecCapabilityMeshoptimizer;

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
	Oct32 = 4,
	// The canonical production vertex is a fixed 128-bit container. These
	// formats describe byte-addressable fields within that container so tools
	// and diagnostics can validate the exact Vulkan shader ABI.
	R16G16B16Unorm = 5,
	Oct12x2 = 6,
	Oct10x2TangentSign = 7
};

// Remap stream encoding tells loaders whether on-disk remap elements are 16-bit
// or 32-bit before they are expanded to a normalized uint32 runtime view.
enum class MeshletRemapEncoding : std::uint32_t
{
	Uint32 = 0,
	Uint16 = 1
};

// Production streams are separated so a request can decode only the data a
// page and render route need. Topology references page-local indices and must
// depend on the matching stable-identity/remap stream.
enum class TemfStreamKind : std::uint16_t
{
	Topology = 0u,
	VertexRemapStableIdentity = 1u,
	Position = 2u,
	NormalTangent = 3u,
	TextureColor = 4u,
	Material = 5u,
	Skinning = 6u,
	MorphDeformation = 7u,
	ProceduralUpdate = 8u,
	Metadata = 9u
};

// Metadata blocks are independently addressable records rather than a retired
// monolithic payload. The attribute mask names the exact decoded runtime data.
inline constexpr std::uint64_t kTemfMetadataRuntimeLayout = 1ull << 0u;
inline constexpr std::uint64_t kTemfMetadataMeshletRecords = 1ull << 1u;
inline constexpr std::uint64_t kTemfMetadataMeshletSections = 1ull << 2u;
inline constexpr std::uint64_t kTemfMetadataMeshletBounds = 1ull << 3u;
inline constexpr std::uint64_t kTemfMetadataVirtualHeader = 1ull << 4u;
inline constexpr std::uint64_t kTemfMetadataVirtualDetailLevels = 1ull << 5u;
inline constexpr std::uint64_t kTemfMetadataVirtualMaterialSections = 1ull << 6u;
inline constexpr std::uint64_t kTemfMetadataVirtualSectionRanges = 1ull << 7u;
inline constexpr std::uint64_t kTemfMetadataVirtualCullableGroups = 1ull << 8u;
inline constexpr std::uint64_t kTemfMetadataVirtualPages = 1ull << 9u;
inline constexpr std::uint64_t kTemfMetadataVirtualRequestedTargets = 1ull << 10u;
inline constexpr std::uint64_t kTemfMetadataVirtualSkippedDetails = 1ull << 11u;
inline constexpr std::uint64_t kTemfMetadataVirtualClusters = 1ull << 12u;
inline constexpr std::uint64_t kTemfMetadataVirtualClusterPageLinks = 1ull << 13u;
inline constexpr std::uint64_t kTemfMetadataVirtualConeSafety = 1ull << 14u;
inline constexpr std::uint64_t kTemfMetadataVirtualValidationFlags = 1ull << 15u;
inline constexpr std::uint64_t kTemfMetadataVirtualDiagnostics = 1ull << 16u;
inline constexpr std::uint64_t kTemfMetadataVirtualDiagnosticStrings = 1ull << 17u;
inline constexpr std::uint64_t kTemfMetadataDeformableRoot = 1ull << 18u;
inline constexpr std::uint64_t kTemfMetadataDeformablePalettes = 1ull << 19u;
inline constexpr std::uint64_t kTemfMetadataDeformablePaletteJoints = 1ull << 20u;
inline constexpr std::uint64_t kTemfMetadataDeformableVertexPalettes = 1ull << 21u;
inline constexpr std::uint64_t kTemfMetadataDeformableMorphs = 1ull << 22u;
inline constexpr std::uint64_t kTemfMetadataDeformableDemandedPages = 1ull << 23u;
inline constexpr std::uint64_t kTemfMetadataDeformableMorphPositions = 1ull << 24u;
inline constexpr std::uint64_t kTemfMetadataDeformableMorphNormals = 1ull << 25u;
inline constexpr std::uint64_t kTemfMetadataDeformableMorphTangents = 1ull << 26u;
// Demanded animated pages remain independently decodable in TEMFPRD1.  The
// directory names logical identities only; each blob uses its own descriptor
// owned range and integrity value instead of inheriting a monolithic offset.
inline constexpr std::uint64_t kTemfMetadataDeformableDemandedPageDirectory = 1ull << 27u;
inline constexpr std::uint64_t kTemfMetadataDeformableDemandedPageEntries = 1ull << 28u;
inline constexpr std::uint64_t kTemfMetadataDeformableDemandedPageRoots = 1ull << 29u;
inline constexpr std::uint64_t kTemfMetadataDeformableDemandedPageDependencies = 1ull << 30u;
inline constexpr std::uint64_t kTemfMetadataDeformableDemandedPageBlobs = 1ull << 31u;
// Canonical demanded-page capability bits are decoded from typed TEMFPRD1
// metadata, never from an appended virtual-page container.
inline constexpr std::uint32_t kTemfDeformablePageFlagSkinned = 1u << 0u;
inline constexpr std::uint32_t kTemfDeformablePageFlagMorphed = 1u << 1u;

// Canonical deformable root has no file offsets or magic: all variable data is
// named by adjacent TEMFPRD1 stream descriptors.
struct TemfDeformableMetadata
{
	std::uint32_t skinningVertexCount;
	std::uint32_t palettePartitionCount;
	std::uint32_t morphTargetCount;
	std::uint32_t demandedPageCount;
	std::uint32_t sourceSkeletonJointCount;
	std::uint32_t flags;
	std::uint64_t skeletonCompatibilityFingerprint;
	float deformationBoundsMin[3];
	float deformationBoundsMax[3];
};

// The page directory is a compact identity index.  It deliberately contains
// no byte offsets: TEMFPRD1 block descriptors own every encoded blob range.
struct TemfDeformableDemandedPageDirectory
{
	std::uint32_t version;
	std::uint32_t entryCount;
	std::uint32_t rootCount;
	std::uint32_t dependencyCount;
	std::uint32_t blobCount;
	std::uint32_t flags;
	std::uint64_t meshPayloadGeneration;
	std::uint64_t skeletonCompatibilityFingerprint;
};

struct TemfDeformableDemandedPageEntry
{
	std::uint32_t pageId;
	std::uint32_t parentPageId;
	std::uint32_t fallbackPageId;
	std::uint32_t clusterId;
	std::uint32_t materialSectionId;
	std::uint32_t flags;
	std::uint32_t dependencyOffset;
	std::uint32_t dependencyCount;
	std::uint64_t blobId;
	std::uint64_t contentGeneration;
	float conservativeBoundsMin[3];
	float conservativeBoundsMax[3];
	float geometricError;
	float deformationError;
};

struct TemfDeformableDemandedPageRoot
{
	std::uint32_t pageId;
	std::uint32_t reserved;
};

struct TemfDeformableDemandedPageDependency
{
	std::uint32_t pageId;
	std::uint32_t requiredPageId;
	std::uint32_t flags;
	std::uint32_t reserved;
};

// Blob records pair a logical page with one independently decoded production
// block.  The payload bytes, stored size, and encoded integrity are supplied
// by that block descriptor, so this record remains relocation-free.
struct TemfDeformableDemandedPageBlob
{
	std::uint64_t blobId;
	std::uint64_t contentGeneration;
	std::uint64_t decodedIntegrity;
	std::uint32_t pageId;
	std::uint32_t decodedBytes;
	std::uint32_t flags;
	std::uint32_t reserved;
};

// A demanded-page payload is a self-contained, independently decoded runtime
// packet.  TEMFPRD1 owns its outer byte range and integrity; inner section
// offsets are relative to this decoded packet and never address the file.
enum class TemfDeformablePageSectionKind : std::uint32_t
{
	PackedVertices = 0u,
	Topology = 1u,
	VertexRemap = 2u,
	Meshlets = 3u,
	MeshletBounds = 4u,
	DecodeConstants = 5u,
	SkinRecords = 6u,
	JointPalette = 7u,
	MorphTargets = 8u,
	MorphDeltas = 9u,
	SourceVertexIdentities = 10u,
	Clusters = 11u,
	ErrorSamples = 12u
};

struct TemfDeformablePageSection
{
	TemfDeformablePageSectionKind kind;
	std::uint32_t flags;
	std::uint32_t relativeByteOffset;
	std::uint32_t byteSize;
	std::uint32_t elementCount;
	std::uint32_t elementStride;
};

struct TemfDeformableDemandedPagePayload
{
	std::uint32_t pageId;
	std::uint32_t clusterId;
	std::uint32_t materialSectionId;
	std::uint32_t flags;
	std::uint32_t sectionCount;
	std::uint32_t sectionTableOffset;
	std::uint32_t decodedByteSize;
	std::uint32_t reserved;
	std::uint64_t meshPayloadGeneration;
	std::uint64_t contentGeneration;
	std::uint64_t skeletonCompatibilityFingerprint;
};

struct TemfDeformablePageSkinRecord
{
	std::uint8_t localJointIndices[4];
	std::uint8_t normalizedWeights[4];
};

struct TemfDeformablePageMorphRecord
{
	std::uint32_t stableTargetId;
	std::uint32_t firstDeltaIndex;
	std::uint32_t deltaCount;
	std::uint32_t flags;
};

struct TemfDeformablePageMorphVertex
{
	float positionDelta[4];
	float normalDelta[4];
	float tangentDelta[4];
};

struct TemfDeformablePageSourceIdentity
{
	std::uint64_t stableSourceVertexId;
};

enum class TemfQuantizationDomainScope : std::uint16_t
{
	SharedBoundary = 0u,
	PageInterior = 1u,
	Asset = 2u,
	DeformableEnvelope = 3u
};

// Profile IDs are persistent ABI values. ExpandedProduction is the bounded
// upload-ready oracle and the only valid codec for direct-resident blocks.
enum class TemfCodecProfile : std::uint32_t
{
	ExpandedProduction = 1u,
	MeshoptimizerProductionStatic = 2u,
	MeshoptimizerProductionUpdateable = 3u
};

// Quantization profile IDs are persistent cook-recipe values. The profiles
// deliberately start at one so zero remains an invalid or uninitialized file.
enum class TemfQuantizationProfile : std::uint32_t
{
	Cinematic = 1u,
	Balanced = 2u,
	Memory = 3u
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
	std::uint32_t coneSafetyCount;
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
	std::uint32_t coneSafetyTableOffset;
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

// TEMF v1 keeps page records in the virtual-geometry payload while the engine
// is still allowed to break asset compatibility. Fallback coverage must remain
// visible even when higher-detail pages fail to stream.
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

// Cone safety records are separate from meshoptimizer cone bounds. A cone bound
// says what might be culled; this record says whether imported material,
// winding, alpha, and thin-shell checks proved that culling is safe.
struct VirtualGeometryConeSafetyRecord
{
	std::uint32_t clusterId;
	std::uint32_t groupOffset;
	std::uint32_t groupCount;
	std::uint32_t materialSidedness;      // Mirrors VirtualGeometryConeMaterialSidedness values.
	std::uint32_t windingState;           // Mirrors VirtualGeometryConeWindingState values.
	std::uint32_t alphaRisk;              // Mirrors VirtualGeometryConeAlphaRisk values.
	std::uint32_t thinShellRisk;          // Mirrors VirtualGeometryConeThinShellRisk values.
	std::uint32_t flags;                  // kVirtualGeometryConeSafetyFlag* bit mask.
	std::uint32_t disabledReasonStringOffset;
	std::uint32_t disabledReasonStringSize;
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

// Canonical TEMF production records use byte packing and 64-bit file-relative
// offsets. These declarations are independent from the temporary payload
// structures below so the cooker and runtime validator share one exact ABI.
struct TemfProductionHeader
{
	std::uint32_t magic = kMeshMagicValue;
	std::uint16_t formatVersion = kMeshCurrentVersion;
	std::uint16_t minorVersion = kMeshCurrentMinorVersion;
	std::uint32_t headerBytes = kTemfProductionHeaderBytes;
	std::uint32_t representationFlags;
	std::uint64_t assetIdentity;
	std::uint64_t sourceContentHash;
	std::uint64_t buildRecipeHash;
	std::uint64_t toolIdentity;
	std::uint64_t generationIdentity;
	std::uint32_t selectedCodecProfileId;
	std::uint32_t selectedQuantizationProfileId;
	std::uint32_t logicalPageSizeBytes;
	std::uint32_t pageCount;
	std::uint32_t blockCount;
	std::uint32_t quantizationDomainCount;
	std::uint32_t dependencyCount;
	std::uint32_t reserved0;
	std::uint64_t pageTableOffset;
	std::uint64_t blockTableOffset;
	std::uint64_t quantizationTableOffset;
	std::uint64_t dependencyTableOffset;
	std::uint64_t payloadOffset;
	std::uint64_t payloadBytes;
	std::uint64_t directoryHash;
	std::uint64_t payloadHash;
	std::uint64_t requiredCapabilityMask;
	std::uint64_t attributeCoverageMask;
	std::uint64_t formatContractId = kTemfProductionContractId;
	std::uint64_t directoryBytes;
	std::uint64_t reserved1;
};

struct TemfPageDescriptor
{
	std::uint32_t logicalPageId;
	std::uint32_t flags;
	std::uint32_t firstBlockIndex;
	std::uint32_t blockCount;
	std::uint32_t firstDependencyIndex;
	std::uint32_t dependencyCount;
	std::uint64_t attributedStoredBytes;
	std::uint64_t decodedBytes;
	std::uint64_t pageIntegrity;
	std::uint64_t reserved[2];
};

struct TemfBlockDescriptor
{
	std::uint64_t blockId;
	std::uint32_t logicalPageId;
	std::uint16_t blockOrdinal;
	std::uint16_t streamKind;
	std::uint64_t attributeMask;
	std::uint32_t codecProfileId;
	std::uint32_t codecVersion;
	std::uint32_t quantizationDomainId;
	std::uint32_t flags;
	std::uint64_t encodedPayloadOffset;
	std::uint64_t encodedBytes;
	std::uint64_t decodedBytes;
	std::uint32_t decodedElementCount;
	std::uint32_t decodedStride;
	std::uint32_t requiredAlignment;
	std::uint32_t firstDependencyIndex;
	std::uint32_t dependencyCount;
	std::uint32_t reserved;
	std::uint64_t requiredCapabilityMask;
	std::uint64_t encodedIntegrity;
	std::uint64_t decodedIntegrity;
};

struct TemfQuantizationDomain
{
	std::uint32_t domainId;
	std::uint16_t domainVersion;
	std::uint16_t domainScope;
	std::uint16_t streamKind;
	std::uint8_t componentCount;
	std::uint8_t signedness;
	std::uint32_t flags;
	std::uint8_t componentBitWidths[4];
	std::uint32_t reserved;
	double origin[4];
	double extent[4];
	double quantizationStep[4];
	std::uint64_t domainHash;
};

struct TemfBlockDependency
{
	std::uint64_t targetBlockId;
	std::uint32_t flags;
	std::uint32_t reserved;
};

// This block-local record contains only the constants needed to rebuild the
// runtime view. It deliberately omits legacy file offsets: TEMFPRD1 owns data
// through typed descriptors, not an implicit monolithic byte layout.
struct TemfRuntimeDecodeMetadata
{
	std::uint16_t formatVersion;
	std::uint16_t reserved;
	std::uint32_t meshletVertexCount;
	std::uint32_t meshletVertexRemapCount;
	std::uint32_t meshletPrimitiveIndexCount;
	std::uint32_t meshletCount;
	std::uint32_t meshletSectionCount;
	std::uint32_t meshletBoundsCount;
	std::uint32_t meshletVertexRemapEncoding;
	std::uint32_t meshletFlags;
	VertexLayout layout;
	QuantizationInfo quantization;
};

// This is decoded runtime-only meshlet state retained while renderer consumers
// migrate to production block packets. It is never serialized: TEMFPRD1 files
// always begin with the 192-byte TemfProductionHeader declared above.
struct MeshletRuntimeLayout
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
	// Zero keeps historical/static TEMF unchanged. A nonzero value is the
	// absolute offset of an appended DeformableMeshPayloadHeader, which older
	// runtime readers safely ignore while newer readers can validate separately.
	std::uint32_t virtualGeometryReserved;
	VertexLayout layout;
	QuantizationInfo quantization;
};

// Canonical deformable records are descriptor-owned TEMFPRD1 metadata. Their
// indices are relative to decoded companion streams and never file offsets.
struct TemfDeformablePaletteRecord
{
	std::uint32_t firstJointIndex;
	std::uint32_t jointCount;
};

struct TemfDeformableMorphRecord
{
	std::uint32_t stableTargetId;
	std::uint32_t firstDeltaIndex;
	std::uint32_t deltaCount;
	std::uint32_t flags;
};

// Page sizes are explicit experiment profiles rather than an implicit cooker
// constant. Their ids are serialized, allowing a later profile decision to
// remain reproducible when all three candidates are still supported.
enum class VirtualGeometryPageSizeProfile : std::uint32_t
{
	KiB32 = 0u,
	KiB64 = 1u,
	KiB128 = 2u
};

constexpr std::uint32_t GetVirtualGeometryPageProfileBytes(
	VirtualGeometryPageSizeProfile profile)
{
	switch(profile)
	{
	case VirtualGeometryPageSizeProfile::KiB32: return 32u * 1024u;
	case VirtualGeometryPageSizeProfile::KiB64: return 64u * 1024u;
	case VirtualGeometryPageSizeProfile::KiB128: return 128u * 1024u;
	}
	return 0u;
}

constexpr bool IsVirtualGeometryPagePayloadWithinProfile(
	VirtualGeometryPageSizeProfile profile,
	std::uint32_t decodedByteSize)
{
	const std::uint32_t profileBytes = GetVirtualGeometryPageProfileBytes(profile);
	return profileBytes != 0u && decodedByteSize <= profileBytes;
}

// This stable byte hash is shared by offline prototype encoders and the
// reference decoder. It is an integrity identifier, not a security primitive.
inline std::uint64_t CalculateVirtualGeometryCompactHash(std::span<const std::uint8_t> bytes)
{
	std::uint64_t hash = 1469598103934665603ull;
	for(const std::uint8_t byte : bytes)
	{
		hash = (hash ^ byte) * 1099511628211ull;
	}
	return hash;
}

enum class VirtualGeometryCompactCodec : std::uint32_t
{
	Expanded = 0u,
	Terra = 1u,
	Meshoptimizer = 2u
};

enum class VirtualGeometryExtensionType : std::uint32_t
{
	CompactPage = 0u,
	ExpandedBlock = 1u,
	TerraBlock = 2u,
	MeshoptimizerBlock = 3u
};

#pragma pack(pop)

static_assert(sizeof(TemfProductionHeader) == 192u, "Canonical TEMF v1 production headers must stay byte exact");
static_assert(offsetof(TemfProductionHeader, representationFlags) == 12u, "TEMF representation flags have a persistent byte offset");
static_assert(offsetof(TemfProductionHeader, formatContractId) == 168u, "TEMFPRD1 has a persistent byte offset");
static_assert(sizeof(TemfPageDescriptor) == 64u, "TEMF production page descriptors must stay byte exact");
static_assert(sizeof(TemfBlockDescriptor) == 112u, "TEMF production block descriptors must stay byte exact");
static_assert(sizeof(TemfQuantizationDomain) == 128u, "TEMF production quantization domains must stay byte exact");
static_assert(sizeof(TemfBlockDependency) == 16u, "TEMF production dependency records must stay byte exact");
static_assert(sizeof(TemfDeformableDemandedPageDirectory) == 40u,
	"Demanded-page directory metadata must stay byte exact");
static_assert(sizeof(TemfDeformableDemandedPageEntry) == 80u,
	"Demanded-page entry metadata must stay byte exact");
static_assert(sizeof(TemfDeformableDemandedPageRoot) == 8u,
	"Demanded-page root metadata must stay byte exact");
static_assert(sizeof(TemfDeformableDemandedPageDependency) == 16u,
	"Demanded-page dependency metadata must stay byte exact");
static_assert(sizeof(TemfDeformableDemandedPageBlob) == 40u,
	"Demanded-page blob metadata must stay byte exact");
static_assert(sizeof(TemfDeformablePageSection) == 24u,
	"Demanded-page section records must stay byte exact");
static_assert(sizeof(TemfDeformableDemandedPagePayload) == 56u,
	"Demanded-page payload identity must stay byte exact");
static_assert(sizeof(TemfDeformablePageSkinRecord) == 8u,
	"Demanded-page skin records use fixed compact influences");
static_assert(sizeof(TemfDeformablePageMorphRecord) == 16u,
	"Demanded-page morph records use relative delta indices");
static_assert(sizeof(TemfDeformablePageMorphVertex) == 48u,
	"Demanded-page morph vertices use fixed float4 channels");
static_assert(sizeof(TemfDeformablePageSourceIdentity) == 8u,
	"Demanded-page source identities are full width");
static_assert(sizeof(MeshletRuntimeLayout) <= 640, "Runtime meshlet layout should remain compact");
static_assert(sizeof(TemfDeformablePaletteRecord) == 8u,
	"Canonical deformable palette records must stay byte exact");
static_assert(sizeof(TemfDeformableMorphRecord) == 16u,
	"Canonical deformable morph records must stay byte exact");
// MeshHeader is the sole on-disk root ABI. Historical meshlet fields live only
// in MeshletRuntimeLayout after validated production block reconstruction.
using MeshHeader = TemfProductionHeader;

// In-memory representation of an entire .temf payload.
// Raw byte storage keeps payload ownership in one place while spans/vectors expose typed views.
struct Mesh
{
	// This decoded layout is populated only after production-directory
	// validation; it is intentionally distinct from the serialized root header.
	MeshletRuntimeLayout runtimeLayout{};
	// The loader records the immutable cooked-file location so streaming can
	// issue exact compact-page reads without reopening ResourceManager state.
	std::string sourcePath;
	std::vector<std::uint8_t> storage;
	// Decoded canonical blocks own the byte spans consumed by the renderer.
	std::vector<std::uint8_t> decodedProductionStorage;
	// Attribute production blocks are independently decoded and then
	// interleaved once into this upload-ready fixed-stride runtime buffer.
	std::vector<std::uint8_t> reconstructedVertexData;
	std::span<const std::uint8_t> meshletVertexData;
	// Runtime always works with uint32 remap entries even if on-disk storage used uint16.
	std::vector<std::uint32_t> meshletVertexRemapDecoded;
	std::span<const std::uint8_t> meshletVertexRemapData;
	std::span<const std::uint8_t> meshletSkinningData;
	std::span<const std::uint8_t> meshletPrimitiveIndexData;
	// Deformable metadata remains available after file validation. Render and
	// animation systems use these copied records to prove that a pose, skeleton,
	// and palette belong to the same cooked generation without reopening the
	// source file or retaining pointers into temporary loader storage.
	bool hasDeformablePayload = false;
	// The loader derives this 64-bit token from the checked canonical streams.
	// A recook therefore invalidates prior render bindings without process-local
	// load order becoming part of the identity.
	std::uint64_t deformablePayloadGeneration = 0U;
	TemfDeformableMetadata deformableMetadata{};
	std::vector<TemfDeformablePaletteRecord> deformablePalettes;
	std::vector<TemfDeformableMorphRecord> deformableMorphs;
	std::vector<std::uint32_t> deformablePaletteJoints;
	std::vector<std::uint32_t> deformableVertexPaletteIndices;
	std::vector<std::array<float, 3>> deformableMorphPositions;
	std::vector<std::array<float, 3>> deformableMorphNormals;
	std::vector<std::array<float, 3>> deformableMorphTangents;
	// The decoder copies canonical demanded-page records only after every
	// descriptor-owned blob and dependency has been validated.
	bool hasDeformableDemandedPages = false;
	TemfDeformableDemandedPageDirectory deformableDemandedPageDirectory{};
	std::vector<TemfDeformableDemandedPageEntry> deformableDemandedPageEntries;
	std::vector<TemfDeformableDemandedPageRoot> deformableDemandedPageRoots;
	std::vector<TemfDeformableDemandedPageDependency> deformableDemandedPageDependencies;
	std::vector<TemfDeformableDemandedPageBlob> deformableDemandedPageBlobs;
	std::vector<std::vector<TemfDeformablePageSection>> deformableDemandedPageSections;
	std::vector<std::vector<std::uint8_t>> deformableDemandedPagePayloads;
	std::vector<MeshletRecord> meshletRecords;
	std::vector<MeshletSection> meshletSections;
	std::vector<MeshletBounds> meshletBounds;
	bool hasVirtualGeometry = false;
	std::uint32_t virtualGeometryPayloadGeneration = 0u;
	VirtualGeometryHeader virtualGeometryHeader{};
	// Direct-resident and paged meshes share the same compact detail record.
	// Direct meshes consume only meshlet ranges and error values; they never
	// interpret the page, cluster, or hierarchy fields below.
	std::vector<VirtualGeometryDetailLevelRecord> virtualGeometryDetailLevels;
	std::vector<VirtualGeometryMaterialSectionRecord> virtualGeometryMaterialSections;
	std::vector<VirtualGeometrySectionRangeRecord> virtualGeometrySectionRanges;
	std::vector<VirtualGeometryCullableGroupRecord> virtualGeometryCullableGroups;
	std::vector<VirtualGeometryPageRecord> virtualGeometryPages;
	std::vector<VirtualGeometryRequestedTargetRecord> virtualGeometryRequestedTargets;
	std::vector<VirtualGeometrySkippedDetailRecord> virtualGeometrySkippedDetails;
	std::vector<VirtualGeometryClusterRecord> virtualGeometryClusters;
	std::vector<VirtualGeometryClusterPageLinkRecord> virtualGeometryClusterPageLinks;
	std::vector<VirtualGeometryConeSafetyRecord> virtualGeometryConeSafety;
	std::vector<VirtualGeometryValidationFlagRecord> virtualGeometryValidationFlags;
	std::vector<VirtualGeometryDiagnosticRecord> virtualGeometryDiagnostics;
	std::string virtualGeometryDiagnosticStrings;
	// Runtime policy metadata derived from validated page tables. These values
	// are exact byte facts for tooling; they do not imply the current mesh
	// shader can decode page-local buffers until that shader path exists.
	bool virtualGeometryPageStreamingReady = false;
	std::uint64_t virtualGeometryFullOwnerPayloadBytes = 0u;
	std::uint64_t virtualGeometryResidentHighDetailPageBytes = 0u;
	std::uint64_t virtualGeometryResidentFallbackPageBytes = 0u;
	std::uint64_t virtualGeometryProtectedFallbackBytes = 0u;
	std::string virtualGeometryOwnerFallbackReason;
	// Runtime-only physical residency publication. Values use GeometryPageState's
	// stable numeric layout, but live here as plain data so this file format
	// header does not depend on renderer implementation headers. The loader
	// publishes only bytes physically available to the renderer; page-table
	// membership alone never implies that a high-detail page is resident.
	std::vector<std::uint32_t> virtualGeometryPhysicalPageStates;
	std::vector<std::uint64_t> virtualGeometryPhysicalPagePublicationGenerations;
	std::uint64_t virtualGeometryPhysicalPagePublicationGeneration = 0u;
	// Object-level culling needs one conservative mesh bound before the GPU
	// sees individual meshlets. It is derived during load so render systems can
	// cheaply transform it into world space without walking every meshlet.
	bool hasLocalBounds = false;
	float localBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
	float localBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
};
