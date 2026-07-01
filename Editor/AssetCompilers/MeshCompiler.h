// MeshCompiler.h — Mesh compilation pipeline interface
//
// Defines the interface and reporting structures for the offline mesh compilation pipeline. It provides functions for identifying supported mesh source formats and converting them into the engine's optimized meshlet-based TEMF format.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <array>
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
	std::uint32_t minimumSourceTriangles = 512u;
	std::uint32_t minimumGeneratedTriangles = 64u;
	std::uint32_t minimumMeshletsPerGeneratedLevel = 2u;
	float minimumUsefulTriangleReduction = 0.15f;
	float minimumUsefulMeshletReduction = 0.15f;
	float maximumGeometricError = 0.1f;
	std::string settingsSource = "ProjectDefaults";
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
	float maxGeometricError = 0.0f;
	VirtualGeometryAdaptiveCompileSettings adaptiveSettings;
	std::vector<VirtualGeometryRequestedTargetCompileReport> requestedTargets;
	std::vector<VirtualGeometryGeneratedLevelCompileReport> generatedLevels;
	std::vector<VirtualGeometrySkippedLevelCompileReport> skippedLevels;
	std::vector<VirtualGeometryClusterCompileReport> clusters;
	std::vector<VirtualGeometryPageCompileReport> pages;
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
	MeshCompileProgressCallback progressCallback = {});

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
	MeshCompileProgressCallback progressCallback = {});
