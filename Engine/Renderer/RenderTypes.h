// RenderTypes.h — Core Rendering Type Definitions
//
// Defines the foundational data structures and constants used throughout the rendering pipeline.
// This includes vertex layouts, material parameters, lighting structures, and frame-level configuration types.
// These types provide a common language between the scene representation and the low-level graphics API.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/common.hpp>
#include "Scene/SpatialDiagnostics.h"
#include "Utilities/UUID.h"

enum class RenderViewType : std::uint8_t
{
	Game = 0,
	Scene = 1,
	Shadow = 2,
	ExportRuntime = 3
};

inline const char* ToString(RenderViewType viewType)
{
	switch (viewType)
	{
	case RenderViewType::Game: return "Game";
	case RenderViewType::Scene: return "Scene";
	case RenderViewType::Shadow: return "Shadow";
	case RenderViewType::ExportRuntime: return "Export Runtime";
	default: return "Unknown";
	}
}

// RenderOutputKind names the supported states explicitly so serialization and switches stay readable.
enum class RenderOutputKind : std::uint8_t
{
	GameView = 0,
	SceneView = 1,
	MainRuntimeView = 2,
	OffscreenTexture = 3,
	ValidationOnly = 4
};

// RenderOutputPriority names the supported states explicitly so serialization and switches stay readable.
enum class RenderOutputPriority : std::uint8_t
{
	Required = 0,
	Optional = 1
};

// RenderSnapshotState names the supported states explicitly so serialization and switches stay readable.
enum class RenderSnapshotState : std::uint8_t
{
	Requested = 0,
	Acquired = 1,
	Prepared = 2,
	Recorded = 3,
	Submitted = 4,
	Completed = 5,
	Released = 6,
	Fallback = 7
};

// RenderSnapshotFallbackReason names the supported states explicitly so serialization and switches stay readable.
enum class RenderSnapshotFallbackReason : std::uint8_t
{
	None = 0,
	BudgetExhausted = 1,
	AllocationFailure = 2,
	UnsupportedTarget = 3,
	Recovery = 4
};

// RenderSnapshotFallbackResult names the supported states explicitly so serialization and switches stay readable.
enum class RenderSnapshotFallbackResult : std::uint8_t
{
	None = 0,
	SerializedSafely = 1,
	SkippedOptionalOutput = 2,
	FrameRecovery = 3
};

// QualityPreset names the supported states explicitly so serialization and switches stay readable.
enum class QualityPreset : std::uint32_t
{
	Low = 0,
	Medium = 1,
	High = 2,
	Ultra = 3,
	Custom = 4
};

// VirtualGeometryDebugVisualizationMode names the editor visualizer modes that
// can be selected without changing material authoring or scene state.
enum class VirtualGeometryDebugVisualizationMode : std::uint32_t
{
	None = 0u,
	LOD = 1u,
	MeshletId = 2u,
	Bounds = 3u,
	Culling = 4u,
	HiZ = 5u,
	Residency = 6u,
	Error = 7u,
	Cluster = 8u
};

inline const char* ToString(VirtualGeometryDebugVisualizationMode mode)
{
	switch (mode)
	{
	case VirtualGeometryDebugVisualizationMode::None: return "Off";
	case VirtualGeometryDebugVisualizationMode::LOD: return "LOD";
	case VirtualGeometryDebugVisualizationMode::MeshletId: return "Meshlet Id";
	case VirtualGeometryDebugVisualizationMode::Bounds: return "Bounds";
	case VirtualGeometryDebugVisualizationMode::Culling: return "Culling";
	case VirtualGeometryDebugVisualizationMode::HiZ: return "Hi-Z";
	case VirtualGeometryDebugVisualizationMode::Residency: return "Residency";
	case VirtualGeometryDebugVisualizationMode::Error: return "Error";
	case VirtualGeometryDebugVisualizationMode::Cluster: return "Cluster";
	default: return "Unknown";
	}
}

inline constexpr std::array<VirtualGeometryDebugVisualizationMode, 9u> kVirtualGeometryDebugVisualizationModes = {
	VirtualGeometryDebugVisualizationMode::None,
	VirtualGeometryDebugVisualizationMode::LOD,
	VirtualGeometryDebugVisualizationMode::Cluster,
	VirtualGeometryDebugVisualizationMode::MeshletId,
	VirtualGeometryDebugVisualizationMode::Bounds,
	VirtualGeometryDebugVisualizationMode::Culling,
	VirtualGeometryDebugVisualizationMode::Residency,
	VirtualGeometryDebugVisualizationMode::HiZ,
	VirtualGeometryDebugVisualizationMode::Error
};

inline constexpr const std::array<VirtualGeometryDebugVisualizationMode, 9u>& GetVirtualGeometryDebugVisualizationModes()
{
	// This is the complete editor-facing visualizer inventory. Keep it in sync
	// with the shader constants so the toolbar never exposes dead branches.
	return kVirtualGeometryDebugVisualizationModes;
}

inline constexpr bool IsVirtualGeometryDebugVisualizationModeExposed(VirtualGeometryDebugVisualizationMode mode)
{
	for (const VirtualGeometryDebugVisualizationMode exposedMode : kVirtualGeometryDebugVisualizationModes)
	{
		if (exposedMode == mode)
		{
			return true;
		}
	}
	return false;
}

inline constexpr VirtualGeometryDebugVisualizationMode NormalizeVirtualGeometryDebugVisualizationMode(VirtualGeometryDebugVisualizationMode mode)
{
	// Project files can still contain obsolete numeric debug-mode values. Those
	// are no longer real enum members, so fail closed to normal rendering.
	return IsVirtualGeometryDebugVisualizationModeExposed(mode) ? mode : VirtualGeometryDebugVisualizationMode::None;
}

struct VirtualGeometryDebugVisualizationLegendEntry
{
	VirtualGeometryDebugVisualizationMode mode = VirtualGeometryDebugVisualizationMode::None;
	const char* meaning = "";
};

inline constexpr std::array<VirtualGeometryDebugVisualizationLegendEntry, 9u> kVirtualGeometryDebugVisualizationLegend = {
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::None, "Normal material rendering." },
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::LOD, "Stable false color per selected detail level; nearby/high detail and far/coarse detail should hash differently." },
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::MeshletId, "Stable false color per rendered meshlet id so density and selected triangle groups are easy to read." },
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::Bounds, "Blue to yellow shows small to large meshlet bounds; magenta means bounds data is unavailable." },
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::Culling, "Green means drawn, red means frustum, cyan means drawn with trusted Hi-Z, magenta means missing culling data; cone culling is disabled until imported mesh sidedness is validated." },
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::HiZ, "Cyan means Hi-Z history is trusted for this view; gray means Hi-Z is inactive or invalid for culling." },
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::Residency, "Green means resident/active virtual geometry; orange means requested or missing pages; brown means inactive fallback." },
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::Error, "Cyan means no error pressure; red means fail-open, and orange means missing-page pressure." },
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::Cluster, "Stable false color per selected cluster packet so adaptive cluster identity changes are visible." }
};

inline const char* GetVirtualGeometryDebugVisualizationLegend(VirtualGeometryDebugVisualizationMode mode)
{
	for (const VirtualGeometryDebugVisualizationLegendEntry& entry : kVirtualGeometryDebugVisualizationLegend)
	{
		if (entry.mode == mode)
		{
			return entry.meaning;
		}
	}
	return "Unknown virtual geometry visualizer mode.";
}

inline constexpr std::uint32_t kVirtualGeometryFlagActive = 1u << 0u;
inline constexpr std::uint32_t kVirtualGeometryFlagFailOpen = 1u << 1u;
inline constexpr std::uint32_t kVirtualGeometryFlagHiZSelfOcclusionGuard = 1u << 2u;
inline constexpr std::uint32_t kVirtualGeometryFlagHiZNearPlaneGuard = 1u << 3u;
inline constexpr std::uint32_t kVirtualGeometryFlagHiZThinGeometryGuard = 1u << 4u;
inline constexpr std::uint32_t kVirtualGeometryDebugModeShift = 8u;
inline constexpr std::uint32_t kVirtualGeometryDebugModeMask = 0xFFu;

// VirtualGeometryInactiveReason keeps fail-open renderer states explainable in
// diagnostics instead of showing misleading zero counters.
enum class VirtualGeometryInactiveReason : std::uint32_t
{
	None = 0u,
	MissingMeshShaderRoute = 2u,
	InvalidAssetMetadata = 3u,
	MissingCullingMetadata = 4u,
	MissingResidencyData = 5u,
	UnsupportedView = 6u
};

inline const char* ToString(VirtualGeometryInactiveReason reason)
{
	switch (reason)
	{
	case VirtualGeometryInactiveReason::None: return "None";
	case VirtualGeometryInactiveReason::MissingMeshShaderRoute: return "Missing Mesh Shader Route";
	case VirtualGeometryInactiveReason::InvalidAssetMetadata: return "Invalid Asset Metadata";
	case VirtualGeometryInactiveReason::MissingCullingMetadata: return "Missing Culling Metadata";
	case VirtualGeometryInactiveReason::MissingResidencyData: return "Missing Residency Data";
	case VirtualGeometryInactiveReason::UnsupportedView: return "Unsupported View";
	default: return "Unknown";
	}
}

enum class VirtualGeometryCookStatus : std::uint32_t
{
	Unknown = 0u,
	Cooked = 1u,
	Skipped = 2u,
	Failed = 3u,
	NeedsRebuild = 4u
};

enum class VirtualGeometryCullOutcome : std::uint32_t
{
	Visible = 0u,
	FrustumCulled = 1u,
	ConeCulled = 2u,
	HiZCulled = 3u,
	FailOpen = 4u
};

enum class GeometryPageState : std::uint32_t
{
	NotResident = 0u,
	Requested = 1u,
	Loading = 2u,
	Resident = 3u,
	Failed = 4u,
	Evicting = 5u,
	Evicted = 6u,
	PendingIO = 7u,
	PendingUpload = 8u,
	Pinned = 9u
};

// Adaptive virtual geometry settings can come from project defaults, a sparse
// per-mesh Inspector override, or a temporary debug override. Keeping the source
// with the resolved profile lets diagnostics explain why a mesh cooked the way it did.
enum class VirtualGeometrySettingsSource : std::uint32_t
{
	ProjectDefaults = 0u,
	PerMeshOverride = 1u,
	DebugOverride = 2u
};

inline const char* ToString(VirtualGeometrySettingsSource source)
{
	switch (source)
	{
	case VirtualGeometrySettingsSource::ProjectDefaults: return "Project Defaults";
	case VirtualGeometrySettingsSource::PerMeshOverride: return "Per-Mesh Override";
	case VirtualGeometrySettingsSource::DebugOverride: return "Debug Override";
	default: return "Unknown";
	}
}

// Material safety tells the cooker how much simplification risk is acceptable
// before it rejects a candidate. Conservative is the default because material
// section mismatches create very visible holes and wrong-surface shading.
enum class VirtualGeometryMaterialSafetyPolicy : std::uint32_t
{
	Conservative = 0u,
	Balanced = 1u,
	Aggressive = 2u
};

inline const char* ToString(VirtualGeometryMaterialSafetyPolicy policy)
{
	switch (policy)
	{
	case VirtualGeometryMaterialSafetyPolicy::Conservative: return "Conservative";
	case VirtualGeometryMaterialSafetyPolicy::Balanced: return "Balanced";
	case VirtualGeometryMaterialSafetyPolicy::Aggressive: return "Aggressive";
	default: return "Unknown";
	}
}

// Fallback policy controls which always-renderable page/detail survives when a
// target is skipped or streaming cannot provide high detail in time.
enum class VirtualGeometryFallbackPolicy : std::uint32_t
{
	PreserveHighestSafeFallback = 0u,
	PreserveLowestSafeFallback = 1u,
	DebugForceRequested = 2u
};

inline const char* ToString(VirtualGeometryFallbackPolicy policy)
{
	switch (policy)
	{
	case VirtualGeometryFallbackPolicy::PreserveHighestSafeFallback: return "Preserve Highest Safe Fallback";
	case VirtualGeometryFallbackPolicy::PreserveLowestSafeFallback: return "Preserve Lowest Safe Fallback";
	case VirtualGeometryFallbackPolicy::DebugForceRequested: return "Debug Force Requested";
	default: return "Unknown";
	}
}

// Requested LOD targets are advisory. The cooker may generate, merge, skip, or
// reject a target when the resulting meshlet set would not be useful or safe.
enum class VirtualGeometryDetailTargetStatus : std::uint32_t
{
	Requested = 0u,
	Generated = 1u,
	Merged = 2u,
	Skipped = 3u,
	Rejected = 4u
};

inline const char* ToString(VirtualGeometryDetailTargetStatus status)
{
	switch (status)
	{
	case VirtualGeometryDetailTargetStatus::Requested: return "Requested";
	case VirtualGeometryDetailTargetStatus::Generated: return "Generated";
	case VirtualGeometryDetailTargetStatus::Merged: return "Merged";
	case VirtualGeometryDetailTargetStatus::Skipped: return "Skipped";
	case VirtualGeometryDetailTargetStatus::Rejected: return "Rejected";
	default: return "Unknown";
	}
}

enum class VirtualGeometrySkippedDetailReason : std::uint32_t
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

inline const char* ToString(VirtualGeometrySkippedDetailReason reason)
{
	switch (reason)
	{
	case VirtualGeometrySkippedDetailReason::None: return "None";
	case VirtualGeometrySkippedDetailReason::MeshTooSmall: return "Mesh Too Small";
	case VirtualGeometrySkippedDetailReason::TooFewTriangles: return "Too Few Triangles";
	case VirtualGeometrySkippedDetailReason::TooFewMeshlets: return "Too Few Meshlets";
	case VirtualGeometrySkippedDetailReason::InsufficientTriangleReduction: return "Insufficient Triangle Reduction";
	case VirtualGeometrySkippedDetailReason::InsufficientMeshletReduction: return "Insufficient Meshlet Reduction";
	case VirtualGeometrySkippedDetailReason::InvalidSimplificationOutput: return "Invalid Simplification Output";
	case VirtualGeometrySkippedDetailReason::MaterialSafetyRejected: return "Material Safety Rejected";
	case VirtualGeometrySkippedDetailReason::GeometricErrorRejected: return "Geometric Error Rejected";
	case VirtualGeometrySkippedDetailReason::DuplicateOfPreviousLevel: return "Duplicate Of Previous Level";
	default: return "Unknown";
	}
}

inline constexpr std::uint32_t kVirtualGeometryAdaptiveTargetCapacity = 8u;
inline constexpr std::array<float, kVirtualGeometryAdaptiveTargetCapacity> kVirtualGeometryDefaultHalfOctaveRatios = {
	1.0f,
	0.70710677f,
	0.5f,
	0.35355338f,
	0.25f,
	0.17677669f,
	0.125f,
	0.08838835f
};

// AdaptiveVirtualGeometryCookProfile is the resolved cook contract shared by
// project defaults, Inspector overrides, importers, and diagnostics. The target
// ratios are intentionally advisory so small or already-simple meshes can stop early.
struct AdaptiveVirtualGeometryCookProfile
{
	VirtualGeometrySettingsSource settingsSource = VirtualGeometrySettingsSource::ProjectDefaults;
	VirtualGeometryMaterialSafetyPolicy materialSafetyPolicy = VirtualGeometryMaterialSafetyPolicy::Conservative;
	VirtualGeometryFallbackPolicy fallbackPolicy = VirtualGeometryFallbackPolicy::PreserveHighestSafeFallback;
	std::array<float, kVirtualGeometryAdaptiveTargetCapacity> targetRatios = kVirtualGeometryDefaultHalfOctaveRatios;
	std::uint32_t targetRatioCount = kVirtualGeometryAdaptiveTargetCapacity;
	std::uint32_t minimumSourceTriangles = 512u;
	std::uint32_t minimumGeneratedTriangles = 64u;
	std::uint32_t minimumMeshletsPerGeneratedLevel = 2u;
	float minimumUsefulTriangleReduction = 0.15f;
	float minimumUsefulMeshletReduction = 0.15f;
	float maximumGeometricError = 0.1f;
	bool allowCustomTargetRatios = false;
};

inline AdaptiveVirtualGeometryCookProfile MakeDefaultAdaptiveVirtualGeometryCookProfile()
{
	AdaptiveVirtualGeometryCookProfile profile;
	return profile;
}

inline AdaptiveVirtualGeometryCookProfile SanitizeAdaptiveVirtualGeometryCookProfile(const AdaptiveVirtualGeometryCookProfile& input)
{
	AdaptiveVirtualGeometryCookProfile sanitized = input;
	if (sanitized.targetRatioCount == 0u || sanitized.targetRatioCount > kVirtualGeometryAdaptiveTargetCapacity)
	{
		sanitized.targetRatioCount = kVirtualGeometryAdaptiveTargetCapacity;
		sanitized.targetRatios = kVirtualGeometryDefaultHalfOctaveRatios;
	}

	float previousRatio = 2.0f;
	for (std::uint32_t index = 0u; index < sanitized.targetRatioCount; ++index)
	{
		float& ratio = sanitized.targetRatios[index];
		if (!std::isfinite(ratio) || ratio <= 0.0f)
		{
			ratio = index == 0u ? 1.0f : sanitized.targetRatios[index - 1u] * 0.5f;
		}
		ratio = glm::clamp(ratio, 0.01f, 1.0f);
		if (ratio > previousRatio)
		{
			ratio = previousRatio;
		}
		previousRatio = ratio;
	}

	sanitized.minimumSourceTriangles = std::max(sanitized.minimumSourceTriangles, 1u);
	sanitized.minimumGeneratedTriangles = std::max(sanitized.minimumGeneratedTriangles, 1u);
	sanitized.minimumMeshletsPerGeneratedLevel = std::max(sanitized.minimumMeshletsPerGeneratedLevel, 1u);
	sanitized.minimumUsefulTriangleReduction = std::isfinite(sanitized.minimumUsefulTriangleReduction)
		? glm::clamp(sanitized.minimumUsefulTriangleReduction, 0.0f, 0.95f)
		: 0.15f;
	sanitized.minimumUsefulMeshletReduction = std::isfinite(sanitized.minimumUsefulMeshletReduction)
		? glm::clamp(sanitized.minimumUsefulMeshletReduction, 0.0f, 0.95f)
		: 0.15f;
	sanitized.maximumGeometricError = std::isfinite(sanitized.maximumGeometricError)
		? glm::clamp(sanitized.maximumGeometricError, 0.0f, 1000.0f)
		: 0.1f;
	return sanitized;
}

// VirtualGeometryQualityProfile is copied into immutable render snapshots so
// worker preparation can select detail and request pages without reading live UI
// or project-settings objects.
struct VirtualGeometryQualityProfile
{
	float lodErrorScale = 1.0f;
	std::uint32_t maxLodDrop = 0u;
	// Hi-Z occlusion is always attempted once the pyramid resource exists. The
	// legacy setting fields remain serialized for compatibility, but shader-side
	// conservative sampling is the only thing that decides whether a meshlet can
	// be hidden behind previously rendered depth.
	bool hiZOcclusionEnabled = true;
	bool hiZSelfOcclusionGuardEnabled = true;
	bool hiZNearPlaneGuardEnabled = true;
	bool hiZThinGeometryGuardEnabled = true;
	VirtualGeometryDebugVisualizationMode debugVisualizationMode = VirtualGeometryDebugVisualizationMode::None;
	std::uint64_t geometryBudgetBytes = 0u;
	float preloadBias = 1.0f;
};

inline VirtualGeometryQualityProfile MakeDefaultVirtualGeometryQualityProfile()
{
	VirtualGeometryQualityProfile profile;
	profile.lodErrorScale = 1.0f;
	profile.maxLodDrop = 0u;
	profile.hiZOcclusionEnabled = true;
	profile.hiZSelfOcclusionGuardEnabled = true;
	profile.hiZNearPlaneGuardEnabled = true;
	profile.hiZThinGeometryGuardEnabled = true;
	profile.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::None;
	profile.geometryBudgetBytes = 0u;
	profile.preloadBias = 1.0f;
	return profile;
}

inline VirtualGeometryQualityProfile SanitizeVirtualGeometryQualityProfile(const VirtualGeometryQualityProfile& input)
{
	VirtualGeometryQualityProfile sanitized = input;
	sanitized.lodErrorScale = std::isfinite(sanitized.lodErrorScale)
		? glm::clamp(sanitized.lodErrorScale, 0.05f, 64.0f)
		: 1.0f;
	sanitized.preloadBias = std::isfinite(sanitized.preloadBias)
		? glm::clamp(sanitized.preloadBias, 0.0f, 16.0f)
		: 1.0f;
	sanitized.debugVisualizationMode = NormalizeVirtualGeometryDebugVisualizationMode(sanitized.debugVisualizationMode);
	return sanitized;
}

inline void HashVirtualGeometryQualityValue(std::uint64_t& seed, std::uint64_t value)
{
	// This helper is intentionally tiny and header-only because editor tests
	// use the same signature rule as the renderer: if any visible quality field
	// changes, cached visible-command packets must be invalidated.
	seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

inline std::uint64_t HashVirtualGeometryQualityFloat(float value)
{
	if (!std::isfinite(value))
	{
		return 0u;
	}
	return static_cast<std::uint64_t>(std::llround(static_cast<double>(value) * 100000.0));
}

inline std::uint64_t BuildVirtualGeometryQualityProfileSignature(const VirtualGeometryQualityProfile& input)
{
	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(input);
	std::uint64_t seed = 1469598103934665603ull;
	HashVirtualGeometryQualityValue(seed, HashVirtualGeometryQualityFloat(sanitized.lodErrorScale));
	HashVirtualGeometryQualityValue(seed, sanitized.maxLodDrop);
	HashVirtualGeometryQualityValue(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(sanitized.debugVisualizationMode)));
	HashVirtualGeometryQualityValue(seed, sanitized.geometryBudgetBytes);
	HashVirtualGeometryQualityValue(seed, HashVirtualGeometryQualityFloat(sanitized.preloadBias));
	return seed;
}

inline VirtualGeometryInactiveReason ResolveVirtualGeometryInactiveReason(
	const VirtualGeometryQualityProfile& quality,
	bool meshShaderRouteSupported)
{
	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(quality);
	if (!meshShaderRouteSupported)
	{
		return VirtualGeometryInactiveReason::MissingMeshShaderRoute;
	}
	return VirtualGeometryInactiveReason::None;
}

// Runtime detail metadata is the CPU-side mirror of the cooked asset contract.
// It is intentionally compact so render preparation can copy it into snapshots.
struct VirtualGeometryDetailRuntime
{
	std::uint32_t lodIndex = 0u;
	std::uint32_t meshletOffset = 0u;
	std::uint32_t meshletCount = 0u;
	std::uint32_t triangleCount = 0u;
	float geometricError = 0.0f;
	float targetRatio = 1.0f;
	glm::vec3 boundsMin = glm::vec3(0.0f);
	glm::vec3 boundsMax = glm::vec3(0.0f);
	std::uint32_t requestedTargetIndex = 0u;
	VirtualGeometryDetailTargetStatus targetStatus = VirtualGeometryDetailTargetStatus::Generated;
};

struct VirtualGeometryRequestedTargetRuntime
{
	std::uint32_t targetIndex = 0u;
	float requestedRatio = 1.0f;
	std::uint32_t desiredTriangleCount = 0u;
	std::uint32_t desiredMeshletCount = 0u;
	VirtualGeometryDetailTargetStatus status = VirtualGeometryDetailTargetStatus::Requested;
};

struct VirtualGeometryGeneratedLevelRuntime
{
	std::uint32_t generatedLevelIndex = 0u;
	std::uint32_t requestedTargetIndex = 0u;
	std::uint32_t lodIndex = 0u;
	float actualRatio = 1.0f;
	std::uint32_t triangleCount = 0u;
	std::uint32_t meshletCount = 0u;
	float geometricError = 0.0f;
};

struct VirtualGeometrySkippedDetailRuntime
{
	std::uint32_t requestedTargetIndex = 0u;
	float requestedRatio = 1.0f;
	VirtualGeometryDetailTargetStatus status = VirtualGeometryDetailTargetStatus::Skipped;
	VirtualGeometrySkippedDetailReason reason = VirtualGeometrySkippedDetailReason::None;
	std::uint32_t nearestGeneratedLevelIndex = 0u;
	std::uint32_t candidateTriangleCount = 0u;
	std::uint32_t candidateMeshletCount = 0u;
	float actualCandidateRatio = 0.0f;
	std::string message;
};

struct VirtualGeometryCullableGroupRuntime
{
	std::uint32_t groupIndex = 0u;
	std::uint32_t lodIndex = 0u;
	std::uint32_t meshletIndex = 0u;
	std::uint32_t sectionId = 0u;
	std::uint32_t pageId = 0u;
	glm::vec3 boundsCenter = glm::vec3(0.0f);
	float boundsRadius = 0.0f;
	VirtualGeometryCullOutcome lastOutcome = VirtualGeometryCullOutcome::Visible;
};

struct VirtualGeometryClusterRuntime
{
	std::uint32_t clusterId = 0u;
	std::uint32_t parentClusterId = 0xffffffffu;
	std::uint32_t firstChildClusterId = 0xffffffffu;
	std::uint32_t childCount = 0u;
	std::uint32_t lodIndex = 0u;
	std::uint32_t firstGroupIndex = 0u;
	std::uint32_t groupCount = 0u;
	// Material ownership is kept with the cluster so validation and debug
	// tooling can prove a mixed-detail cut did not splice unrelated sections.
	std::uint32_t materialSectionId = 0u;
	std::uint32_t pageId = 0u;
	std::uint32_t fallbackClusterId = 0xffffffffu;
	float geometricError = 0.0f;
	glm::vec3 boundsCenter = glm::vec3(0.0f);
	float boundsRadius = 0.0f;
};

struct GeometryPageRuntime
{
	std::uint32_t pageId = 0u;
	std::uint32_t firstLodIndex = 0u;
	std::uint32_t lodCount = 0u;
	std::uint32_t groupOffset = 0u;
	std::uint32_t groupCount = 0u;
	GeometryPageState state = GeometryPageState::NotResident;
	std::uint64_t byteSize = 0u;
	float priority = 0.0f;
	std::uint32_t fallbackPageId = 0u;
	std::uint64_t lastUsedFrame = 0u;
	bool fallbackPage = false;
	bool pinnedFallback = false;
	std::uint32_t clusterOffset = 0u;
	std::uint32_t clusterCount = 0u;
	float screenImportance = 0.0f;
	std::string failureReason;
};

struct VirtualGeometryAssetRuntime
{
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	VirtualGeometryCookStatus cookStatus = VirtualGeometryCookStatus::Unknown;
	std::uint32_t sourceTriangleCount = 0u;
	std::uint32_t sourceVertexCount = 0u;
	// Cluster hierarchy validity only proves ids, ranges, and parent links are
	// well-formed. Mixed-detail rendering also needs a stronger coverage proof
	// that child clusters cover the same surface region as their parent.
	bool clusterCoverageValidated = false;
	AdaptiveVirtualGeometryCookProfile adaptiveCookProfile = MakeDefaultAdaptiveVirtualGeometryCookProfile();
	std::vector<VirtualGeometryRequestedTargetRuntime> requestedTargets;
	std::vector<VirtualGeometryGeneratedLevelRuntime> generatedLevels;
	std::vector<VirtualGeometrySkippedDetailRuntime> skippedDetails;
	std::vector<VirtualGeometryDetailRuntime> detailLevels;
	std::vector<VirtualGeometryCullableGroupRuntime> cullableGroups;
	std::vector<VirtualGeometryClusterRuntime> clusters;
	std::vector<GeometryPageRuntime> pages;
	std::vector<std::string> warnings;
};

// LightingReductionCategory names the supported states explicitly so serialization and switches stay readable.
enum class LightingReductionCategory : std::uint32_t
{
	None = 0u,
	Uploaded = 1u,
	Special = 2u,
	Retained = 3u,
	Shadow = 4u
};

// MaterialShadowBehavior names the supported states explicitly so serialization and switches stay readable.
enum class MaterialShadowBehavior : std::uint32_t
{
	Opaque = 0u,
	AlphaCutout = 1u,
	TransparentNoShadow = 2u,
	TranslucentApproximate = 3u,
	EmissiveUsesOpacity = 4u
};

// LightingDiagnosticCategory names the supported states explicitly so serialization and switches stay readable.
enum class LightingDiagnosticCategory : std::uint32_t
{
	Shadow = 0u,
	IndirectLighting = 1u,
	AmbientOcclusion = 2u,
	MaterialShadow = 3u,
	Budget = 4u
};

// LightingDiagnosticStatus names the supported states explicitly so serialization and switches stay readable.
enum class LightingDiagnosticStatus : std::uint32_t
{
	Active = 0u,
	Disabled = 1u,
	Degraded = 2u,
	Fallback = 3u,
	Unavailable = 4u
};

// ShadowResourceKind names the supported states explicitly so serialization and switches stay readable.
enum class ShadowResourceKind : std::uint32_t
{
	DirectionalCascade = 0u,
	PointCube = 1u,
	SpotAtlas = 2u,
	AreaAtlas = 3u,
	Unsupported = 4u
};

// ShadowQualityTier names the supported states explicitly so serialization and switches stay readable.
enum class ShadowQualityTier : std::uint32_t
{
	FullResolution = 0u,
	ReducedResolution = 1u,
	CachedOnly = 2u,
	Disabled = 3u
};

// ShadowDegradationReason names the supported states explicitly so serialization and switches stay readable.
enum class ShadowDegradationReason : std::uint32_t
{
	None = 0u,
	LightBudget = 1u,
	StorageBudget = 2u,
	AllocationFailure = 3u,
	UnsupportedLightType = 4u
};

// ShadowRefreshReason names the supported states explicitly so serialization and switches stay readable.
enum class ShadowRefreshReason : std::uint32_t
{
	RenderedThisFrame = 0u,
	CacheHit = 1u,
	LightMoved = 2u,
	ReceiverMoved = 3u,
	CacheInvalidated = 4u,
	Unavailable = 5u
};

inline constexpr std::uint32_t kFrameLightCandidateFlagShadowCapable = 1u << 0;
inline constexpr std::uint32_t kFrameLightCandidateFlagSpecialCost = 1u << 1;
inline constexpr std::uint32_t kFrameLightCandidateFlagDirectional = 1u << 2;
// Renderer-wide shadow slot capacity. The user-facing budget can be lower per
// quality preset, but it is no longer restricted to the old four-light limit.
inline constexpr std::uint32_t kMaxShadowResourceSlots = 32u;

inline constexpr std::uint32_t kGpuLightAnalysisFlagVisible = 1u << 0;
inline constexpr std::uint32_t kGpuLightAnalysisFlagDirectional = 1u << 1;

// struct RenderSettings groups related state and operations so callers have one documented contract.
struct RenderSettings
{
	std::uint32_t shadowResolution = 2048u;
	std::uint32_t shadowFilteringSize = 5u;
	// Scene-wide candidate budget uploaded into the lightweight GPU analysis
	// pass before the final retained-light list is chosen for shading.
	std::uint32_t maxUploadedLights = 256u;
	// Scene-wide per-frame cap for how many dynamic lights survive the final
	// screen-aware selection stage and reach clustered lighting.
	std::uint32_t maxDynamicLights = 128u;
	// Scene-wide per-frame cap for how many retained lights may request shadow
	// resources. The runtime grows shadow resources up to kMaxShadowResourceSlots.
	std::uint32_t maxShadowLights = 8u;
	// Optional cap for higher-cost area lights inside the retained-light set so
	// presets can bound expensive lighting classes independently.
	std::uint32_t maxAreaLights = 4u;
	std::uint32_t enableLTC = 1u;
	std::uint32_t enableIBL = 1u;
	// Controls the strength of environment and local indirect lighting without
	// changing direct light intensity, which lets authors tune shadow readability.
	float indirectStrength = 1.0f;
	float ambientOcclusionStrength = 1.0f;
	std::uint32_t ambientOcclusionQuality = 2u;
	std::uint32_t anisotropicFiltering = 4u;
	// Variable-rate shading changes the render-pass attachment layout, so it
	// is treated as a startup/render-profile option rather than a hot toggle.
	std::uint32_t variableRateShading = 0u;
	// Virtual geometry defaults disabled until the cooked TEMF payload, shader
	// path, and diagnostics have been implemented and validated story by story.
	VirtualGeometryQualityProfile virtualGeometry = MakeDefaultVirtualGeometryQualityProfile();
};

// Scene-level render settings can grow over time while keeping one explicit
// root block in scene JSON for non-object renderer-facing authoring data.
struct SceneRenderSettings
{
	float indirectStrength = 1.0f;
	float ambientOcclusionStrength = 1.0f;
	std::uint32_t ambientOcclusionQuality = 2u;
};

// Renderer-owned diagnostic state derived from the scene settings. This data
// is transient and should never be written back into scene files.
struct SkyRuntimeState
{
	bool isValid = true;
	bool usingFallback = true;
	bool hasProblem = false;
	bool dirty = true;
	bool forceBlackFallback = false;
	TerraEngine::UUID resolvedSkyHandle = TerraEngine::EMPTY_UUID;
	glm::vec3 resolvedSunDirection = glm::normalize(glm::vec3(0.0f, 0.35f, -1.0f));
};

// Compact per-frame sky payload submitted to the renderer background pass.
// struct FrameSkyData groups related state and operations so callers have one documented contract.
struct FrameSkyData
{
	bool renderBackground = false;
	bool hasTexture = false;
	bool usingFallback = true;
	bool forceBlack = false;
	float intensity = 1.0f;
	float exposure = 0.7f;
	float rotationOffsetDegrees = 0.0f;
	glm::vec3 horizonTint = glm::vec3(0.45f, 0.58f, 0.72f);
	glm::vec3 zenithTint = glm::vec3(0.08f, 0.16f, 0.36f);
	glm::vec3 groundTint = glm::vec3(0.06f, 0.07f, 0.09f);
	glm::vec3 sunDirection = glm::normalize(glm::vec3(0.0f, 0.35f, -1.0f));
	TerraEngine::UUID textureId = TerraEngine::EMPTY_UUID;
};

// ClampSkyTint is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline glm::vec3 ClampSkyTint(const glm::vec3& tint)
{
	return glm::clamp(tint, glm::vec3(0.0f), glm::vec3(4.0f));
}

// ClampSkyExposure constrains authored SkyComponent exposure before the value
// reaches FrameSkyData and shader constants.
inline float ClampSkyExposure(float exposure)
{
	return glm::clamp(exposure, 0.0f, 16.0f);
}

// ClampSkyRotationOffset keeps authored yaw offsets finite and bounded to a
// full turn so procedural sky math cannot receive invalid angles.
inline float ClampSkyRotationOffset(float rotationOffsetDegrees)
{
	return std::isfinite(rotationOffsetDegrees)
		? glm::clamp(rotationOffsetDegrees, -360.0f, 360.0f)
		: 0.0f;
}

// MakeDefaultSceneRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline SceneRenderSettings MakeDefaultSceneRenderSettings()
{
	SceneRenderSettings settings;
	settings.indirectStrength = 1.0f;
	settings.ambientOcclusionStrength = 1.0f;
	settings.ambientOcclusionQuality = 2u;
	return settings;
}

// SanitizeSceneRenderSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline SceneRenderSettings SanitizeSceneRenderSettings(const SceneRenderSettings& input)
{
	SceneRenderSettings sanitized = input;
	sanitized.indirectStrength = glm::clamp(sanitized.indirectStrength, 0.0f, 8.0f);
	sanitized.ambientOcclusionStrength = glm::clamp(sanitized.ambientOcclusionStrength, 0.0f, 4.0f);
	sanitized.ambientOcclusionQuality = glm::clamp(sanitized.ambientOcclusionQuality, 0u, 3u);
	return sanitized;
}

// struct DrawCommand groups related state and operations so callers have one documented contract.
struct DrawCommand
{
	TerraEngine::UUID meshId;
	TerraEngine::UUID materialId;
	glm::mat4 modelMatrix;
	std::uint32_t materialIndex = 0u;
	std::uint32_t meshDataIndex = 0u;
	std::uint32_t textureIndex = 0u;
	TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
	// Editor-only tint is submitted per draw so Scene View context modes can
	// de-emphasize surrounding objects without changing runtime materials.
	glm::vec4 editorTint = glm::vec4(0.0f);
	std::uint32_t virtualMeshletOffset = 0u;
	std::uint32_t virtualMeshletCount = 0u;
	std::uint32_t selectedVirtualLod = 0u;
	std::uint32_t virtualGeometryFlags = 0u;
	// These offsets/counts point at the immutable per-view selection packet.
	// Visible virtual geometry expands one draw per selected cluster, so each
	// command normally references a single sparse cluster record.
	std::uint32_t selectedVirtualClusterOffset = 0u;
	std::uint32_t selectedVirtualClusterCount = 0u;
	std::uint32_t selectedVirtualPageNeedOffset = 0u;
	std::uint32_t selectedVirtualPageNeedCount = 0u;
	bool taskShaderCullingEnabled = true;
};

// Each shadow-casting light receives its own filtered draw list so shadow
// passes only visit meshes that can actually intersect that light's coverage.
struct ShadowDrawList
{
	TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
	std::vector<DrawCommand> commands;
};

// struct PushConstants groups related state and operations so callers have one documented contract.
struct PushConstants
{
	glm::mat4 viewProj;
	glm::mat4 model;
	std::uint32_t materialIndex = 0u;
	std::uint32_t meshDataIndex = 0u;
	std::uint32_t textureIndex = 0u;
	std::uint32_t vrsActive = 0u;
	std::uint32_t taskCullingEnabled = 0u;
	std::uint32_t statsSlotIndex = 0u;
	float cullingEpsilon = 0.001f;
	std::uint32_t lightDataOffset = 0u;
	std::uint32_t clusterDataOffset = 0u;
	std::uint32_t lightIndexOffset = 0u;
	std::uint32_t indirectDrawOffset = 0u;
	std::uint32_t virtualMeshletOffset = 0u;
	std::uint32_t virtualMeshletCount = 0u;
	std::uint32_t selectedVirtualLod = 0u;
	std::uint32_t virtualGeometryFlags = 0u;
	// Cluster/page packet metadata follows the selected cluster range so shader
	// layouts can visualize sparse cluster dispatch and fallback page choices.
	std::uint32_t selectedVirtualClusterOffset = 0u;
	std::uint32_t selectedVirtualClusterCount = 0u;
	std::uint32_t selectedVirtualPageNeedOffset = 0u;
	std::uint32_t selectedVirtualPageNeedCount = 0u;
	// rgb is the tint target color and a is blend strength. Runtime draws leave
	// this at zero so authoring cues never leak into exported/game rendering.
	alignas(16) glm::vec4 editorTint = glm::vec4(0.0f);
};

inline constexpr std::uint32_t kLightingClusterDimX = 16u;
inline constexpr std::uint32_t kLightingClusterDimY = 9u;
inline constexpr std::uint32_t kLightingClusterDimZ = 24u;

// Compact light payload uploaded once per frame and consumed by forward shaders.
// struct alignas groups related state and operations so callers have one documented contract.
struct alignas(16) GPULight
{
	glm::vec4 positionAndType = glm::vec4(0.0f);       // xyz = position, w = type enum value
	glm::vec4 directionAndRange = glm::vec4(0.0f);     // xyz = direction, w = attenuation range
	glm::vec4 colorAndIntensity = glm::vec4(0.0f);     // rgb = color, w = scalar intensity
	glm::vec4 coneAnglesAndArea = glm::vec4(0.0f);     // x = innerCos, y = outerCos, z = width, w = height
	glm::vec4 shadowAndPadding = glm::vec4(0.0f);      // x = castsShadows flag, y = shadow resource index
	glm::vec4 shadowRegion = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f); // xy = atlas UV offset, zw = atlas UV scale
	glm::vec4 extraParams = glm::vec4(0.0f);           // x = shadowStrength, y = shadowBias, z = indirectMultiplier, w = authored priority
	glm::vec4 shadowMatrixC0 = glm::vec4(0.0f);        // Column 0 of the exact light view-projection used for 2D shadow maps
	glm::vec4 shadowMatrixC1 = glm::vec4(0.0f);        // Column 1; storing columns mirrors GLM and avoids shader-side matrix transposition
	glm::vec4 shadowMatrixC2 = glm::vec4(0.0f);        // Column 2; point lights keep using cubemap face projections instead
	glm::vec4 shadowMatrixC3 = glm::vec4(0.0f);        // Column 3; receivers sample the same projection that rendered the depth map
};
static_assert(sizeof(GPULight) == 176, "GPULight layout changed; update shader mirror types.");

// CPU-side light candidate metadata keeps stable identifiers and author hints
// beside the GPU payload so temporal retention and diagnostics remain
// deterministic without changing the shader-facing light structure.
struct FrameLightCandidate
{
	TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
	GPULight gpuLight{};
	float stabilityPreference = 0.0f;
	std::uint32_t candidateFlags = 0u;
};

// Small GPU-written analysis record used by the hybrid CPU/GPU selection path.
// Keeping this buffer compact makes ordinary runtime diagnostics inexpensive.
struct alignas(16) GPULightAnalysisRecord
{
	float gpuVisibilityScore = 0.0f;
	float gpuRangeCoverage = 0.0f;
	std::uint32_t flags = 0u;
	std::uint32_t reserved = 0u;
};
static_assert(sizeof(GPULightAnalysisRecord) == 16, "GPU light analysis layout changed; update shader mirrors.");

// Cluster table entry mapping one cluster to a segment of the flat light-index list.
// struct alignas groups related state and operations so callers have one documented contract.
struct alignas(16) ClusterData
{
	std::uint32_t offset = 0u;
	std::uint32_t count = 0u;
	float padding[2];
	glm::vec3 minBounds = glm::vec3(0.0f);
	float padding1 = 0.0f;
	glm::vec3 maxBounds = glm::vec3(0.0f);
	float padding2 = 0.0f;
};
static_assert(sizeof(ClusterData) == 48, "ClusterData layout changed; update shader mirror types.");

// Per-frame metadata summarizing how the variable-length shared light list was
// populated. Validation tools consume this summary so they can reason about
// dense coverage, sparse-scene waste, and deterministic range stability
// without reverse-engineering renderer internals.
struct ClusterLightBuildMetadata
{
	std::uint32_t clusterCount = 0u;
	std::uint32_t activeLightCount = 0u;
	std::uint32_t lightIndexCapacity = 0u;
	std::uint32_t clusterDataOffset = 0u;
	std::uint32_t lightIndexOffset = 0u;
	std::uint32_t usedIndexCount = 0u;
	std::uint32_t actualAssignedLightCount = 0u;
	std::uint32_t emptyClusterCount = 0u;
	std::uint32_t maxClusterLightCount = 0u;
	std::uint32_t outOfBoundsRangeCount = 0u;
	std::uint32_t invalidLightIndexCount = 0u;
	std::uint32_t overflowClusterCount = 0u;
	std::uint32_t determinismMismatchCount = 0u;
	std::uint64_t rangeHash = 0u;
	bool clusteredLightingAvailable = false;
	bool overflowDetected = false;
};

// Flat range snapshot for one cluster. Keeping this structure small makes it
// cheap to export and easy to compare across repeated regression runs.
struct ClusterLightRangeSnapshot
{
	std::uint32_t clusterIndex = 0u;
	std::uint32_t offset = 0u;
	std::uint32_t count = 0u;
	bool outOfBounds = false;
};

// Immutable validation payload returned by the renderer after a frame
// completes. Callers receive copies so tooling can inspect the data without
// mutating renderer-owned state.
struct ClusterValidationSnapshot
{
	RenderViewType viewType = RenderViewType::Game;
	ClusterLightBuildMetadata buildMetadata{};
	std::vector<ClusterLightRangeSnapshot> ranges;
	bool valid = false;
};

// Lightweight per-frame budgeting summary returned without GPU readback so the
// editor, QA tools, and validation scripts can verify preset behavior during
// ordinary runtime sessions.
struct ShadowResourceAssignmentDiagnostic
{
	TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
	ShadowResourceKind resourceKind = ShadowResourceKind::Unsupported;
	std::uint32_t resourceSlot = 0u;
	std::uint32_t storageRegionId = 0u;
	std::uint32_t atlasId = 0u;
	std::uint32_t regionX = 0u;
	std::uint32_t regionY = 0u;
	std::uint32_t regionWidth = 0u;
	std::uint32_t regionHeight = 0u;
	std::uint64_t estimatedBytes = 0u;
	std::uint64_t frameSerial = 0u;
	std::uint64_t framesSinceLastSeen = 0u;
	float retentionScore = 0.0f;
	ShadowQualityTier qualityTier = ShadowQualityTier::FullResolution;
	ShadowDegradationReason degradationReason = ShadowDegradationReason::None;
	ShadowRefreshReason refreshReason = ShadowRefreshReason::RenderedThisFrame;
	bool assigned = false;
	bool reusedPreviousOwner = false;
	bool replacedPreviousOwner = false;
	bool shadowedPreviousFrame = false;
	bool reusedCachedResult = false;
	bool refreshedThisFrame = true;
	bool invalidatedThisFrame = false;
};

// struct FrameBudgetDiagnosticsSnapshot groups related state and operations so callers have one documented contract.
struct FrameBudgetDiagnosticsSnapshot
{
	RenderViewType viewType = RenderViewType::Game;
	QualityPreset activePreset = QualityPreset::High;
	LightingReductionCategory reductionCategory = LightingReductionCategory::None;
	std::uint32_t uploadedLightBudget = 0u;
	std::uint32_t retainedLightBudget = 0u;
	std::uint32_t shadowLightBudget = 0u;
	std::uint32_t specialLightBudget = 0u;
	std::uint32_t candidateLightCount = 0u;
	std::uint32_t uploadedLightCount = 0u;
	std::uint32_t retainedLightCount = 0u;
	std::uint32_t specialLightCount = 0u;
	std::uint32_t shadowEligibleLightCount = 0u;
	std::uint32_t shadowedLightCount = 0u;
	std::uint32_t configuredShadowCapacity = 0u;
	std::uint32_t activeShadowCount = 0u;
	std::uint32_t allocatedRegionCount = 0u;
	std::uint32_t allocated2DRegionCount = 0u;
	std::uint32_t allocatedPointRegionCount = 0u;
	std::uint32_t allocatedDirectionalRegionCount = 0u;
	std::uint32_t allocatedSpotRegionCount = 0u;
	std::uint32_t allocatedAreaRegionCount = 0u;
	std::uint64_t estimatedShadowMemoryBytes = 0u;
	std::uint32_t gpuVisibleCandidateCount = 0u;
	std::uint32_t retainedDirectionalLightCount = 0u;
	std::uint32_t retainedAuthoredPriorityLightCount = 0u;
	std::uint32_t retainedLightChurnCount = 0u;
	std::uint32_t retainedLightReplacementCount = 0u;
	std::uint32_t stabilityBiasAppliedCount = 0u;
	std::uint32_t shadowReuseCount = 0u;
	std::uint32_t shadowReplacementCount = 0u;
	std::uint32_t cacheReuseCount = 0u;
	std::uint32_t cacheRefreshCount = 0u;
	std::uint32_t cacheInvalidationCount = 0u;
	std::uint32_t degradedShadowCount = 0u;
	std::uint32_t allocationFailureCount = 0u;
	// These counters explain how much the shadow-caster list shrank once lights
	// stopped reusing the camera-visible draw list as a proxy.
	std::uint32_t cameraVisibleShadowCasterCount = 0u;
	std::uint32_t lightVisibleShadowCasterCount = 0u;
	std::uint32_t shadowDrawSubmissionCount = 0u;
	// Per-light assignment data explains which shadow-capable retained lights
	// received a resource this frame and which ones were degraded by the budget.
	std::vector<ShadowResourceAssignmentDiagnostic> shadowAssignments;
	bool gpuAssistActive = false;
	bool customOverrideActive = false;
	bool budgetReductionOccurred = false;
	bool uploadedReductionOccurred = false;
	bool specialReductionOccurred = false;
	bool shadowReductionOccurred = false;
	bool shadowPassRecorded = false;
	bool valid = false;
};

// struct LightingDiagnosticEntry groups related state and operations so callers have one documented contract.
struct LightingDiagnosticEntry
{
	LightingDiagnosticCategory category = LightingDiagnosticCategory::Budget;
	LightingDiagnosticStatus status = LightingDiagnosticStatus::Active;
	TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
	std::string message;
	std::string recommendedAction;
};

// struct LightingDiagnosticsSnapshot groups related state and operations so callers have one documented contract.
struct LightingDiagnosticsSnapshot
{
	RenderViewType viewType = RenderViewType::Game;
	std::vector<LightingDiagnosticEntry> entries;
	bool indirectLightingActive = false;
	bool ambientOcclusionActive = false;
	bool valid = false;
};

// Static quads are the CPU-side output used for menu backgrounds, panels, and
// other layout-driven visuals before a GPU backend consumes them.
struct UIStaticQuad
{
	glm::vec2 position = glm::vec2(0.0f, 0.0f);
	glm::vec2 size = glm::vec2(0.0f, 0.0f);
	glm::vec4 color = glm::vec4(1.0f);
	float cornerRadius = 0.0f;
};

// Immediate quads support fast-changing HUD elements that bypass layout and
// are rewritten every frame.
struct UIImmediateQuad
{
	glm::vec2 position = glm::vec2(0.0f, 0.0f);
	glm::vec2 size = glm::vec2(0.0f, 0.0f);
	glm::vec4 color = glm::vec4(1.0f);
	glm::vec4 uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
};

// Text runs are kept at a higher level than individual glyph vertices so the
// editor can preview them with ImGui while the runtime SDF atlas work evolves.
struct UITextRun
{
	std::string text;
	glm::vec2 position = glm::vec2(0.0f, 0.0f);
	glm::vec2 bounds = glm::vec2(0.0f, 0.0f);
	glm::vec4 color = glm::vec4(1.0f);
	float fontSize = 16.0f;
};

// Image runs allow the UI to display texture assets with layout-driven sizing,
// offloading the resource resolution and drawing to the GPU backend.
struct UIImageRun
{
	std::string texturePath;
	glm::vec2 position = glm::vec2(0.0f, 0.0f);
	glm::vec2 size = glm::vec2(0.0f, 0.0f);
	glm::vec4 color = glm::vec4(1.0f);
	float cornerRadius = 0.0f;
};

// struct UIRenderBatch groups related state and operations so callers have one documented contract.
struct UIRenderBatch
{
	glm::vec2 viewportSize = glm::vec2(0.0f, 0.0f);
	std::vector<UIStaticQuad> staticQuads;
	std::vector<UIImmediateQuad> immediateQuads;
	std::vector<UITextRun> textRuns;
	std::vector<UIImageRun> imageRuns;

	// Append is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Append(const UIRenderBatch& other)
	{
		if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
		{
			viewportSize = other.viewportSize;
		}

		staticQuads.insert(staticQuads.end(), other.staticQuads.begin(), other.staticQuads.end());
		immediateQuads.insert(immediateQuads.end(), other.immediateQuads.begin(), other.immediateQuads.end());
		textRuns.insert(textRuns.end(), other.textRuns.begin(), other.textRuns.end());
		imageRuns.insert(imageRuns.end(), other.imageRuns.begin(), other.imageRuns.end());
	}
};

// struct FrameRenderData groups related state and operations so callers have one documented contract.
struct FrameRenderData
{
	glm::mat4 viewMatrix;
	glm::mat4 projectionMatrix;
	glm::vec3 cameraWorldPosition = glm::vec3(0.0f);
	float deltaTimeSeconds = 0.0f;
	bool globalTaskShaderCullingEnabled = true;
	float cullingEpsilon = 0.001f;
	QualityPreset activePreset = QualityPreset::High;
	bool customOverrideActive = false;
	RenderSettings settings;
	FrameSkyData sky;
	std::vector<FrameLightCandidate> lightCandidates;
	std::vector<DrawCommand> commands;
	// Scene View can request a whole-view authoring tint for non-mesh passes
	// such as sky and grid while individual DrawCommand tint preserves the
	// focused prefab branch at normal color.
	glm::vec4 editorContextTint = glm::vec4(0.0f);
	// Per-light shadow lists are separate from the main camera-visible list so
	// shadow passes can cull against each light instead of the active camera.
	std::vector<ShadowDrawList> shadowDrawLists;
	UIRenderBatch uiBatch;
};

// struct RenderOutputRequest groups related state and operations so callers have one documented contract.
struct RenderOutputRequest
{
	// Describes one renderer output before command recording starts. Today the
	// main producers are the editor Scene/Game views; future RenderTexture
	// cameras can use the same fields to request their own isolated payloads.
	RenderOutputKind kind = RenderOutputKind::GameView;
	RenderOutputPriority priority = RenderOutputPriority::Required;
	RenderViewType diagnosticsView = RenderViewType::Game;
	glm::uvec2 targetSize = glm::uvec2(0u);
	std::uint32_t outputId = 0u;
};

// struct RenderSnapshotHandle groups related state and operations so callers have one documented contract.
struct RenderSnapshotHandle
{
	// A handle points at one fixed slice inside every per-frame GPU buffer that
	// can change while views are queued. Offsets are element offsets, not byte
	// offsets, so shaders can add them directly to structured-buffer indices.
	std::uint32_t slotIndex = 0u;
	std::uint32_t statsSlotIndex = 0u;
	std::uint32_t lightDataOffset = 0u;
	std::uint32_t lightAnalysisInputOffset = 0u;
	std::uint32_t clusterDataOffset = 0u;
	std::uint32_t lightIndexOffset = 0u;
	std::uint32_t clusterDepthOffset = 0u;
	std::uint32_t indirectDrawOffset = 0u;
	RenderSnapshotState state = RenderSnapshotState::Requested;
	bool valid = false;
};

// struct RenderSnapshotOwnerSummary groups related state and operations so callers have one documented contract.
struct RenderSnapshotOwnerSummary
{
	// Diagnostics copy the ownership result by value after a frame completes.
	// This lets editor tools inspect which output owned which snapshot without
	// touching Vulkan objects or renderer-owned lifetime state.
	RenderOutputKind kind = RenderOutputKind::GameView;
	RenderOutputPriority priority = RenderOutputPriority::Required;
	RenderSnapshotState state = RenderSnapshotState::Requested;
	glm::uvec2 targetSize = glm::uvec2(0u);
	std::uint32_t outputId = 0u;
	std::uint32_t slotIndex = 0u;
	bool lightingIsolated = false;
	bool cullingIsolated = false;
	bool clusteringIsolated = false;
	bool shadowAssignmentsIsolated = false;
	bool indirectDrawsIsolated = false;
};

// struct RenderSnapshotFallbackEvent groups related state and operations so callers have one documented contract.
struct RenderSnapshotFallbackEvent
{
	// Fallback events are exceptional paths. Normal warmed-up rendering should
	// not use them; when the finite snapshot budget is exhausted, these fields
	// explain whether a required output serialized safely or an optional output
	// was skipped instead of reusing another view's mutable buffers.
	RenderOutputKind kind = RenderOutputKind::GameView;
	RenderSnapshotFallbackReason reason = RenderSnapshotFallbackReason::None;
	RenderSnapshotFallbackResult result = RenderSnapshotFallbackResult::None;
	std::uint32_t outputId = 0u;
	bool required = true;
};

// struct RenderSnapshotDiagnosticsSnapshot groups related state and operations so callers have one documented contract.
struct RenderSnapshotDiagnosticsSnapshot
{
	// Snapshot diagnostics are cheap CPU-side counters and owner records. They
	// are intentionally separate from frame captures so tests and editor panels
	// can detect unsafe sharing before a visual corruption report is needed.
	std::uint32_t activeSnapshotCount = 0u;
	std::uint32_t completedSnapshotCount = 0u;
	std::uint32_t peakSnapshotCount = 0u;
	std::uint32_t maxSnapshotsPerFrame = 0u;
	std::vector<RenderSnapshotOwnerSummary> ownerSummaries;
	std::vector<RenderSnapshotFallbackEvent> fallbackEvents;
	bool lightingIsolationValid = true;
	bool clusterIsolationValid = true;
	bool shadowIsolationValid = true;
	bool indirectDrawIsolationValid = true;
	bool valid = false;
};

// GPU-facing frustum/camera payload consumed by the task shader path.
// struct alignas groups related state and operations so callers have one documented contract.
struct alignas(16) FrameCullingDataGPU
{
	std::array<glm::vec4, 6> frustumPlanes{};
	glm::vec4 cameraPositionAndEpsilon = glm::vec4(0.0f, 0.0f, 0.0f, 0.001f);
	glm::uvec4 clusterGridAndLightCount = glm::uvec4(kLightingClusterDimX, kLightingClusterDimY, kLightingClusterDimZ, 0u);
	glm::vec4 clusterDepthAndViewport = glm::vec4(0.1f, 100.0f, 1920.0f, 1080.0f);
	glm::mat4 viewMatrix = glm::mat4(1.0f);
	glm::mat4 invProjection = glm::mat4(1.0f);
	glm::uvec4 renderSettings0 = glm::uvec4(2048u, 5u, 128u, 1u); // x: shadowRes, y: shadowFilter, z: retainedLightBudget, w: enableLTC
	glm::uvec4 renderSettings1 = glm::uvec4(1u, 4u, 4u, 256u);    // x: enableIBL, y: aniso, z: shadowLightBudget, w: uploadedLightBudget
	// x: indirectStrength, y: ambientOcclusionStrength, z: ambientOcclusionQuality,
	// w: 1 only after an initialized same-size Hi-Z pyramid, quality toggle,
	// and per-draw Hi-Z guard bits pass. View/target/projection/depth-scene
	// ownership is diagnostic-only, so a zero value means a hard
	// resource/guard gate failed.
	glm::vec4 renderSettings2 = glm::vec4(1.0f, 1.0f, 2.0f, 0.0f);
	std::uint32_t globalTaskCullingEnabled = 1u;
	std::uint32_t taskShaderSupported = 0u;
	std::uint32_t clusteredLightingValid = 0u;
	std::uint32_t clusteredLightingOverflowed = 0u;
};
static_assert(sizeof(FrameCullingDataGPU) == 336, "Frame culling payload must stay tightly packed for GPU uploads.");

// GPU counters written by task shader atomic operations per view/frame slot.
// struct alignas groups related state and operations so callers have one documented contract.
struct alignas(16) MeshletCullingCountersGPU
{
	std::uint32_t meshletsTotal = 0u;
	std::uint32_t meshletsDispatched = 0u;
	std::uint32_t meshletsCulled = 0u;
	std::uint32_t meshletsCulledHiZ = 0u;
};
static_assert(sizeof(MeshletCullingCountersGPU) == 16, "Meshlet counter payload must keep a stable 16-byte layout.");

// UI-facing immutable snapshot returned by the renderer. The renderer owns
// writes while editor windows read copies, so no caller mutates shared state.
struct RenderStatsSnapshot
{
	RenderViewType viewType = RenderViewType::Game;
	float frameTimeMs = 0.0f;
	float fps = 0.0f;
	std::uint32_t drawCalls = 0u;
	std::uint32_t triangleCount = 0u;
	std::uint32_t meshletsTotal = 0u;
	std::uint32_t meshletsDispatched = 0u;
	std::uint32_t meshletsCulled = 0u;
	std::uint32_t meshletsCulledHiZ = 0u;
	std::uint32_t spatialIndexedObjectCount = 0u;
	std::uint32_t spatialVisibleCandidateCount = 0u;
	std::uint32_t spatialRejectedObjectCount = 0u;
	std::uint32_t spatialFallbackObjectCount = 0u;
	TerraEngine::Scene::SpatialRefreshMode spatialRefreshMode = TerraEngine::Scene::SpatialRefreshMode::Uninitialized;
	std::uint32_t spatialAddedObjectCount = 0u;
	std::uint32_t spatialUpdatedObjectCount = 0u;
	std::uint32_t spatialRemovedObjectCount = 0u;
	std::uint32_t spatialUnchangedObjectCount = 0u;
	std::uint32_t spatialFullRefreshObjectCount = 0u;
	std::uint32_t transformDirtyRootCount = 0u;
	std::uint32_t transformProcessedObjectCount = 0u;
	std::uint32_t boundsRecomputedObjectCount = 0u;
	bool taskCullingActive = false;
	bool hiZActive = false;
	bool vrsActive = false;
	bool valid = false;
};
