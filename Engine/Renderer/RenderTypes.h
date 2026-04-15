#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "Utilities/UUID.h"

enum class RenderViewType : std::uint8_t
{
	Game = 0,
	Scene = 1
};

enum class QualityPreset : std::uint32_t
{
	Low = 0,
	Medium = 1,
	High = 2,
	Ultra = 3,
	Custom = 4
};

enum class LightingReductionCategory : std::uint32_t
{
	None = 0u,
	Uploaded = 1u,
	Special = 2u,
	Retained = 3u,
	Shadow = 4u
};

inline constexpr std::uint32_t kFrameLightCandidateFlagShadowCapable = 1u << 0;
inline constexpr std::uint32_t kFrameLightCandidateFlagSpecialCost = 1u << 1;
inline constexpr std::uint32_t kFrameLightCandidateFlagDirectional = 1u << 2;

inline constexpr std::uint32_t kGpuLightAnalysisFlagVisible = 1u << 0;
inline constexpr std::uint32_t kGpuLightAnalysisFlagDirectional = 1u << 1;

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
	// resources. The runtime also clamps this against the available shadow slots.
	std::uint32_t maxShadowLights = 4u;
	// Optional cap for higher-cost area lights inside the retained-light set so
	// presets can bound expensive lighting classes independently.
	std::uint32_t maxAreaLights = 4u;
	std::uint32_t enableLTC = 1u;
	std::uint32_t enableIBL = 1u;
	std::uint32_t anisotropicFiltering = 4u;
};

struct DrawCommand
{
	TerraEngine::UUID meshId;
	TerraEngine::UUID materialId;
	glm::mat4 modelMatrix;
	std::uint32_t materialIndex = 0u;
	std::uint32_t meshDataIndex = 0u;
	std::uint32_t textureIndex = 0u;
	bool taskShaderCullingEnabled = true;
};

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
	float padding = 0.0f;
};

inline constexpr std::uint32_t kLightingClusterDimX = 16u;
inline constexpr std::uint32_t kLightingClusterDimY = 9u;
inline constexpr std::uint32_t kLightingClusterDimZ = 24u;

// Compact light payload uploaded once per frame and consumed by forward shaders.
struct alignas(16) GPULight
{
	glm::vec4 positionAndType = glm::vec4(0.0f);       // xyz = position, w = type enum value
	glm::vec4 directionAndRange = glm::vec4(0.0f);     // xyz = direction, w = attenuation range
	glm::vec4 colorAndIntensity = glm::vec4(0.0f);     // rgb = color, w = scalar intensity
	glm::vec4 coneAnglesAndArea = glm::vec4(0.0f);     // x = innerCos, y = outerCos, z = width, w = height
	glm::vec4 shadowAndPadding = glm::vec4(0.0f);      // x = castsShadows flag, y = shadow resource index
	glm::vec4 extraParams = glm::vec4(0.0f);           // x = shadowStrength, y = shadowBias, z = indirectMultiplier, w = authored priority
};
static_assert(sizeof(GPULight) == 96, "GPULight layout changed; update shader mirror types.");

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
	std::uint32_t gpuVisibleCandidateCount = 0u;
	std::uint32_t retainedDirectionalLightCount = 0u;
	std::uint32_t retainedAuthoredPriorityLightCount = 0u;
	std::uint32_t retainedLightChurnCount = 0u;
	std::uint32_t retainedLightReplacementCount = 0u;
	std::uint32_t stabilityBiasAppliedCount = 0u;
	std::uint32_t shadowReuseCount = 0u;
	std::uint32_t shadowReplacementCount = 0u;
	bool gpuAssistActive = false;
	bool customOverrideActive = false;
	bool budgetReductionOccurred = false;
	bool uploadedReductionOccurred = false;
	bool specialReductionOccurred = false;
	bool shadowReductionOccurred = false;
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

struct UIRenderBatch
{
	glm::vec2 viewportSize = glm::vec2(0.0f, 0.0f);
	std::vector<UIStaticQuad> staticQuads;
	std::vector<UIImmediateQuad> immediateQuads;
	std::vector<UITextRun> textRuns;
	std::vector<UIImageRun> imageRuns;

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
	std::vector<FrameLightCandidate> lightCandidates;
	std::vector<DrawCommand> commands;
	UIRenderBatch uiBatch;
};

// GPU-facing frustum/camera payload consumed by the task shader path.
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
	std::uint32_t globalTaskCullingEnabled = 1u;
	std::uint32_t taskShaderSupported = 0u;
	std::uint32_t clusteredLightingValid = 0u;
	std::uint32_t clusteredLightingOverflowed = 0u;
};
static_assert(sizeof(FrameCullingDataGPU) == 320, "Frame culling payload must stay tightly packed for GPU uploads.");

// GPU counters written by task shader atomic operations per view/frame slot.
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
	bool taskCullingActive = false;
	bool hiZActive = false;
	bool vrsActive = false;
	bool valid = false;
};
