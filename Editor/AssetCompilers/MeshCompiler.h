// MeshCompiler.h — Mesh compilation pipeline interface
//
// Defines the interface and reporting structures for the offline mesh compilation pipeline. It provides functions for identifying supported mesh source formats and converting them into the engine's optimized meshlet-based TEMF format.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using MeshCompileProgressCallback = std::function<void(float progress, std::string_view stage, std::string_view message)>;

enum class VirtualGeometryCompileStatus : std::uint32_t
{
	Unknown = 0u,
	Cooked = 1u,
	Skipped = 2u,
	Failed = 3u,
	NeedsRebuild = 4u
};

struct VirtualGeometryAdaptiveCompileSettings
{
	std::vector<float> targetRatios;
	// Defaults keep every valid simplification. Artists can opt into stronger
	// percentage or count gates through an individual mesh override.
	std::uint32_t minimumSourceTriangles = 1u;
	std::uint32_t minimumGeneratedTriangles = 1u;
	std::uint32_t minimumMeshletsPerGeneratedLevel = 1u;
	float minimumUsefulTriangleReduction = 0.0f;
	float minimumUsefulMeshletReduction = 0.0f;
	float maximumGeometricError = 0.1f;
	std::string settingsSource = "ProjectDefaults";
};

struct VirtualGeometryConeSafetyCookSettings
{
	// Direct raw/static mesh cooks can opt into one-sided opaque proof when the
	// importer has no material ambiguity. Package importers set this false until
	// they can map material sidedness and alpha risk per emitted cluster.
	bool proveOneSidedOpaqueClusters = true;
	std::string disabledReason = "Imported package material safety metadata is not precise enough for cone culling yet.";
};

struct VirtualGeometryRequestedTargetCompileReport
{
	std::uint32_t targetIndex = 0u;
	float requestedRatio = 1.0f;
	std::uint32_t desiredTriangleCount = 0u;
	std::uint32_t desiredMeshletCount = 0u;
	std::string status = "Requested";
};

struct VirtualGeometryGeneratedLevelCompileReport
{
	std::uint32_t generatedLevelIndex = 0u;
	std::uint32_t requestedTargetIndex = 0u;
	std::uint32_t lodIndex = 0u;
	float actualRatio = 1.0f;
	std::uint32_t triangleCount = 0u;
	std::uint32_t meshletCount = 0u;
	float geometricError = 0.0f;
};

struct VirtualGeometrySkippedLevelCompileReport
{
	std::uint32_t targetIndex = 0u;
	float requestedRatio = 1.0f;
	std::string status = "Skipped";
	std::string reason = "None";
	std::uint32_t nearestGeneratedLevelIndex = 0u;
	// Candidate counts describe what meshoptimizer actually produced before
	// the adaptive gates merged, skipped, or rejected the requested target.
	std::uint32_t candidateTriangleCount = 0u;
	std::uint32_t candidateMeshletCount = 0u;
	float actualCandidateRatio = 0.0f;
	std::string message;
};

struct VirtualGeometryClusterCompileReport
{
	std::uint32_t clusterId = 0u;
	std::uint32_t parentClusterId = 0xffffffffu;
	std::uint32_t lodIndex = 0u;
	std::uint32_t groupCount = 0u;
	// Material section ownership is part of the cook report so package
	// diagnostics can prove mixed-detail cluster cuts did not lose section
	// identity while still keeping TEMF v1 free to evolve.
	std::uint32_t materialSectionId = 0u;
	std::uint32_t pageId = 0u;
	std::uint32_t fallbackClusterId = 0xffffffffu;
};

struct VirtualGeometryPageCompileReport
{
	std::uint32_t pageId = 0u;
	std::uint32_t firstLodIndex = 0u;
	std::uint32_t lodCount = 0u;
	std::uint32_t fallbackPageId = 0u;
	std::uint64_t byteSize = 0u;
	bool pinnedFallback = false;
};

struct VirtualGeometryShadowCodecCompileReport
{
	std::string codecProfile;
	std::uint32_t pageSizeKiB = 0u;
	std::uint64_t sourceBytes = 0u;
	std::uint64_t encodedBytes = 0u;
	std::uint64_t deterministicHash = 0u;
	bool complete = false;
};

struct VirtualGeometryCompileReport
{
	// Status names whether this compile produced virtual geometry metadata or
	// intentionally stayed on the existing flat meshlet path.
	VirtualGeometryCompileStatus status = VirtualGeometryCompileStatus::Unknown;
	std::uint32_t detailLevelCount = 0u;
	std::uint32_t cullableGroupCount = 0u;
	std::uint32_t pageCount = 0u;
	std::uint32_t sourceTriangleCount = 0u;
	std::uint32_t selectedFallbackAvailable = 0u;
	std::uint32_t coneSafetyRecordCount = 0u;
	std::uint32_t coneEligibleClusterCount = 0u;
	float maxGeometricError = 0.0f;
	VirtualGeometryAdaptiveCompileSettings adaptiveSettings;
	std::vector<VirtualGeometryRequestedTargetCompileReport> requestedTargets;
	std::vector<VirtualGeometryGeneratedLevelCompileReport> generatedLevels;
	std::vector<VirtualGeometrySkippedLevelCompileReport> skippedLevels;
	std::vector<VirtualGeometryClusterCompileReport> clusters;
	std::vector<VirtualGeometryPageCompileReport> pages;
	std::vector<VirtualGeometryShadowCodecCompileReport> shadowCodecProfiles;
	std::vector<std::string> warnings;
	std::vector<std::string> errors;
};

VirtualGeometryAdaptiveCompileSettings MakeDefaultVirtualGeometryAdaptiveCompileSettings();
VirtualGeometryAdaptiveCompileSettings SanitizeVirtualGeometryAdaptiveCompileSettings(const VirtualGeometryAdaptiveCompileSettings& settings);

// Normalized compile report consumed by CLI output and validation scripts.
struct MeshCompileReport
{
	std::filesystem::path assetPath;
	std::uint16_t formatVersion = 0;
	std::uint64_t sourceBytes = 0;
	std::uint64_t outputBytes = 0;
	std::uint64_t baselineBytes = 0;
	double compressionRatio = 0.0;
	bool baselinePass = true;
	bool deterministicReady = true;
	bool runtimeValidationPass = true;
	bool usedU16RemapEncoding = false;
	std::uint32_t meshletGlobalVertexCount = 0;
	std::string sourceHashHex;
	std::string status = "compile_fail";
	std::string failureReason;
	VirtualGeometryCompileReport virtualGeometry;
};

// RawMeshVertex is the package-import handoff format used when another parser,
// such as fastgltf, has already decoded source geometry. The mesh compiler still
// owns cleanup, tangent generation, meshlet building, and TEMF writing.
struct RawMeshVertex
{
	std::array<float, 3> position = { 0.0f, 0.0f, 0.0f };
	std::array<float, 3> normal = { 0.0f, 0.0f, 1.0f };
	std::array<float, 2> uv = { 0.0f, 0.0f };
};

// The package importers hand raw joint ids and floating-point weights to this
// bounded cooker record. It deliberately does not expose runtime palette state:
// the cooker derives that state deterministically before publishing TEMF.
struct SkinnedMeshInfluence
{
	std::uint32_t jointIndex = 0u;
	float weight = 0.0f;
};

struct SkinnedMeshCookVertex
{
	RawMeshVertex baseVertex;
	std::vector<SkinnedMeshInfluence> influences;
};

// Morph deltas retain a stable target id so a future runtime loader can reject
// mismatched mesh/morph bindings without relying on source-file array order.
struct SkinnedMeshMorphTarget
{
	std::uint32_t stableTargetId = 0u;
	std::vector<std::array<float, 3>> positionDeltas;
	// Optional source channels remain target-major in the sidecar writer. Empty
	// arrays mean the DCC source omitted that semantic and cook as zero float4s.
	std::vector<std::array<float, 3>> normalDeltas;
	std::vector<std::array<float, 3>> tangentDeltas;
};

struct SkinnedMeshCookInput
{
	std::vector<SkinnedMeshCookVertex> vertices;
	std::vector<std::uint32_t> indices;
	std::vector<SkinnedMeshMorphTarget> morphTargets;
};

struct SkinnedMeshCookSettings
{
	// A package importer can lower this operational budget for a target, but no
	// individual partition may exceed the immutable 256-joint format limit.
	std::uint32_t maximumPalettePartitions = std::numeric_limits<std::uint32_t>::max();
	// Source skeleton ids are checked before the worker allocates palette data.
	std::uint32_t sourceSkeletonJointCount = 4096u;
	// Skinned importers must copy the nonzero fingerprint emitted by the cooked
	// SkeletonAsset. Zero is valid only when every retained weight is zero and
	// the TEMF is therefore an unbound morph-only/identity payload.
	std::uint64_t skeletonCompatibilityFingerprint = 0u;
	// Deformable meshes must use the same LOD recipe as their inventory pass.
	// Otherwise the final skinning cook can silently replace it with LOD0 only.
	VirtualGeometryAdaptiveCompileSettings virtualGeometrySettings;
};

enum class SkinnedMeshCookFailure : std::uint8_t
{
	None,
	InvalidInput,
	InvalidInfluenceWeight,
	MissingJoint,
	InvalidMorphTarget,
	DuplicateMorphTarget,
	MissingSkeletonCompatibilityFingerprint,
	PalettePartitionLimitExceeded,
	StaticMeshCookFailed,
	StaticVertexLayoutIncompatible,
	PayloadRangeOverflow,
	FormatCompatibilityMismatch,
	StagedPayloadWriteFailed,
	PublicationFailed
};

struct SkinnedMeshCookedVertex
{
	std::vector<SkinnedMeshInfluence> influences;
	std::vector<std::uint32_t> sourceJointIds;
	// These arrays expose the exact 8-byte GPU record produced by the cook. A
	// zeroed weight array is the explicit identity-deformation representation.
	std::array<std::uint8_t, 4> paletteJointIndices{};
	std::array<std::uint8_t, 4> quantizedWeights{};
	std::uint32_t palettePartitionIndex = std::numeric_limits<std::uint32_t>::max();
	float normalizedWeightSum = 0.0f;
	float discardedWeight = 0.0f;
	bool usesIdentityDeformation = false;
};

struct SkinnedMeshPalettePartition
{
	std::vector<std::uint32_t> sourceJointIds;
};

struct SkinnedMeshDeformableTemfSummary
{
	bool hasSkinning = false;
	std::uint32_t morphTargetCount = 0u;
	std::uint32_t palettePartitionCount = 0u;
	std::uint32_t virtualGeometryClusterCount = 0u;
	bool deformationEnvelopeValid = false;
	bool zeroWeightIdentityEncoded = false;
	bool virtualGeometryFailsOpen = false;
	bool skeletonCompatibilityValidated = false;
	std::uint64_t skeletonCompatibilityFingerprint = 0u;
};

struct SkinnedMeshCookReport
{
	SkinnedMeshCookFailure failure = SkinnedMeshCookFailure::None;
	bool published = false;
	bool preservedLastKnownGood = false;
	std::uint32_t zeroWeightVertexCount = 0u;
	std::uint32_t failingVertexIndex = std::numeric_limits<std::uint32_t>::max();
	std::uint32_t failingJointIndex = std::numeric_limits<std::uint32_t>::max();
	std::uint64_t skeletonCompatibilityFingerprint = 0u;
	float weightQuantizationUnit = 1.0f / 255.0f;
	std::string diagnosticCode;
	std::string diagnosticMessage;
	std::string recommendedCorrection;
	std::vector<SkinnedMeshCookedVertex> cookedVertices;
	std::vector<SkinnedMeshPalettePartition> palettePartitions;
	SkinnedMeshDeformableTemfSummary deformableTemf;
};

// Returns true when the file extension maps to a mesh source format the offline
// compiler can parse directly. Editor discovery and CLI validation both use this
// helper so supported formats stay in sync.
bool IsSupportedMeshSourceExtension(const std::filesystem::path& filePath);

// Builds a human-readable extension list for help text and diagnostics.
std::string GetSupportedMeshSourceExtensionsText();

// Compiles an input mesh into a meshlet-only TEMF artifact.
// Input: supported static mesh source path such as OBJ, FBX, glTF/GLB, DAE, PLY, or STL.
// Output: .temf file containing packed shared vertices, uint8 primitive indices, remap data, and meshlet tables.
// Returns false and emits diagnostics to stderr when parsing, meshlet generation, or file write fails.
bool CompileMesh(
	const std::filesystem::path& input,
	const std::filesystem::path& output,
	bool includeMeshletBounds = true,
	MeshCompileReport* outReport = nullptr,
	float smoothingAngle = 60.0f,
	float weldThreshold = 1e-6f,
	const VirtualGeometryAdaptiveCompileSettings* adaptiveSettings = nullptr,
	MeshCompileProgressCallback progressCallback = {},
	const VirtualGeometryConeSafetyCookSettings* coneSafetySettings = nullptr);

// Compiles geometry that has already been extracted from a package importer.
// Input: stable logical source name for reports plus decoded triangle vertices
// and indices. Output: same optimized TEMF artifact as CompileMesh().
bool CompileMeshFromRaw(
	const std::filesystem::path& logicalSource,
	const std::filesystem::path& output,
	const std::vector<RawMeshVertex>& vertices,
	const std::vector<std::uint32_t>& indices,
	bool includeMeshletBounds = true,
	MeshCompileReport* outReport = nullptr,
	float smoothingAngle = 60.0f,
	float weldThreshold = 1e-6f,
	const VirtualGeometryAdaptiveCompileSettings* adaptiveSettings = nullptr,
	MeshCompileProgressCallback progressCallback = {},
	const VirtualGeometryConeSafetyCookSettings* coneSafetySettings = nullptr);

// Cooks already-decoded package geometry into a staged deformable TEMF. Worker
// preparation normalizes/reduces weights and builds palettes; only the final
// replace operation touches the published path, preserving last-known-good data
// whenever validation or staging fails.
bool CompileSkinnedMeshFromRaw(
	const std::filesystem::path& logicalSource,
	const std::filesystem::path& output,
	const SkinnedMeshCookInput& input,
	SkinnedMeshCookReport* outReport = nullptr,
	const SkinnedMeshCookSettings& settings = {});
