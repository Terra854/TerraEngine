// RenderTypes.h — Core Rendering Type Definitions
//
// Defines the foundational data structures and constants used throughout the rendering pipeline.
// This includes vertex layouts, material parameters, lighting structures, and frame-level configuration types.
// These types provide a common language between the scene representation and the low-level graphics API.

#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/common.hpp>
#include "Scene/SpatialDiagnostics.h"
#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"
#include "Renderer/VirtualGeometry/Geometry/VirtualGeometryRepresentations.h"
#include "Utilities/UUID.h"

namespace TerraEngine::Animation {
	struct PublishedAnimatorPose;
}

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

namespace TerraEngine::Renderer {
	// AnimationRenderFallbackPolicy is authored/runtime policy only. Render jobs
	// never repair animation state; they either consume one immutable compatible
	// pose generation, retain a compatible prior generation, or draw the bind
	// pose that is already present in the cooked mesh.
	enum class AnimationRenderFallbackPolicy : std::uint8_t
	{
		BindPose = 0U,
		HoldLastCompatiblePose = 1U
	};

	enum class AnimationRenderPoseSource : std::uint8_t
	{
		BindPose = 0U,
		CpuPose = 1U,
		HeldCpuPose = 2U,
		// GPU visual work has a complete immutable packet identity but deliberately
		// has no CPU pose. CPU deterministic and fallback modes never use this.
		GpuVisual = 3U
	};

	enum class AnimationRenderBindingStatus : std::uint8_t
	{
		Ready = 0U,
		SafePose = 1U,
		MissingPose = 2U,
		StaleGeneration = 3U,
		IncompatibleSkeleton = 4U,
		InvalidPose = 5U
	};

	// Prepared visibility and submission caches name every independent lifetime
	// that can change their answer. The fields are value generations rather than
	// resource pointers, so renderer workers can compare and hash them safely.
	struct RenderInvalidationGenerationRecord
	{
		std::uint64_t pose = 0U;
		std::uint64_t morph = 0U;
		std::uint64_t asset = 0U;
		std::uint64_t page = 0U;
		std::uint64_t view = 0U;
		std::uint64_t material = 0U;
		std::uint64_t renderer = 0U;
		std::uint64_t residency = 0U;

		auto operator<=>(const RenderInvalidationGenerationRecord&) const = default;
	};

	// This is the renderer-facing identity of one immutable GPU work packet. It
	// contains no Vulkan handles or live animator pointer; the backend resolves the
	// packet through the frame's separately retained presentation plan.
	struct AnimationGpuRenderBinding
	{
		// These copied packet values are the renderer's GPU-only identity. They
		// prevent a draw from borrowing a palette merely because another animator
		// happened to share a slot-like or mesh-like value in a later plan.
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t presentationGeneration = 0U;
		std::uint64_t sourceGeneration = 0U;
		std::uint64_t paletteGeneration = 0U;
		std::uint32_t planGeneration = 0U;
		std::uint32_t rendererGeneration = 0U;
		std::uint32_t workPacketIndex = std::numeric_limits<std::uint32_t>::max();
		std::uint32_t stableSubmissionOrdinal = 0U;
		std::uint32_t instanceSlot = TerraEngine::Animation::kInvalidAnimatorInstanceSlot;
		std::uint32_t instanceGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		std::uint32_t skeletonGeneration = 0U;
		std::uint32_t meshGeneration = 0U;
		std::uint32_t resourceGeneration = 0U;
		std::uint32_t sceneGeneration = 0U;
		std::uint32_t workGeneration = 0U;
		std::uint32_t programGeneration = 0U;
		std::uint32_t stageMask = 0U;
		// Exact mesh/render lifetimes stay CPU-only beside the frozen shader
		// generations. In particular, deformablePayloadGeneration must never be
		// narrowed into the 32-bit animation residency lane.
		TerraEngine::Animation::GpuAnimationRenderIdentity renderIdentity{};
		bool hybridCpuAuthority = false;
		bool valid = false;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return valid && stableInstanceId != 0U && presentationGeneration != 0U
				&& sourceGeneration != 0U && paletteGeneration != 0U
				&& planGeneration != 0U && rendererGeneration != 0U
				&& workPacketIndex != std::numeric_limits<std::uint32_t>::max()
				&& instanceSlot != TerraEngine::Animation::kInvalidAnimatorInstanceSlot
				&& instanceGeneration != 0U && componentGeneration != 0U
				&& skeletonGeneration != 0U && meshGeneration != 0U
				&& resourceGeneration != 0U && sceneGeneration != 0U
				&& workGeneration != 0U && programGeneration != 0U && stageMask != 0U
				&& renderIdentity.IsComplete();
		}
	};

	// CPU deterministic/fallback evaluation publishes the same graphics-facing
	// 3x4 palette format as GPU evaluation. The immutable vectors are built once
	// for a pose generation, then flattened into frame-owned Vulkan buffers at
	// the protected submission boundary.
	struct CpuAnimationDeformationData
	{
		std::vector<TerraEngine::Animation::GpuAnimationPaletteRecord> paletteMatrices;
		std::vector<float> morphWeights;
		std::uint32_t jointCount = 0U;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return jointCount != 0U
				&& jointCount <= TerraEngine::Animation::kGpuAnimationPaletteEntriesPerPartition
				&& !paletteMatrices.empty()
				&& paletteMatrices.size()
					<= TerraEngine::Animation::kGpuAnimationPaletteEntriesPerPartition
				&& morphWeights.size()
					<= TerraEngine::Animation::kGpuAnimationMaximumMorphTargetsV1;
		}
	};

	// One record ties immutable CPU output to the exact mesh and renderer
	// lifetimes that accepted it. Record indices are copied into DrawCommand, so
	// every Scene/Game/shadow view reuses one frame upload.
	struct CpuAnimationDeformationRecord
	{
		std::uint64_t stableInstanceId = 0U;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint64_t poseGeneration = 0U;
		std::uint64_t deformablePayloadGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::uint64_t skeletonCompatibilityFingerprint = 0U;
		std::shared_ptr<const CpuAnimationDeformationData> data;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return stableInstanceId != 0U && meshId != TerraEngine::EMPTY_UUID
				&& poseGeneration != 0U && deformablePayloadGeneration != 0U
				&& rendererGeneration != 0U
				&& skeletonCompatibilityFingerprint != 0U
				&& data != nullptr && data->IsComplete();
		}
	};

	struct CpuAnimationDeformationPlan
	{
		std::uint64_t frameId = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::vector<CpuAnimationDeformationRecord> records;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			if(frameId == 0U || rendererGeneration == 0U || records.empty()) {
				return false;
			}
			return std::all_of(records.begin(), records.end(),
				[this](const CpuAnimationDeformationRecord& record) {
					return record.IsComplete()
						&& record.rendererGeneration == rendererGeneration;
				});
		}
	};

	// A backend execution record is published only after the native command
	// recorder emitted at least one animation dispatch and the owning graphics
	// queue accepted that command buffer. It deliberately contains no timing or
	// pose data, so diagnostics can prove real GPU execution without introducing
	// a readback, fence wait, or performance-sensitive instrumentation path.
	struct AnimationGpuBackendExecutionRecord
	{
		std::uint64_t frameId = 0U;
		std::uint64_t submissionSerial = 0U;
		std::uint32_t planGeneration = 0U;
		std::uint32_t rendererGeneration = 0U;
		std::uint32_t recordedDispatchCount = 0U;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return frameId != 0U && submissionSerial != 0U
				&& planGeneration != 0U && rendererGeneration != 0U
				&& recordedDispatchCount != 0U;
		}

		auto operator<=>(const AnimationGpuBackendExecutionRecord&) const = default;
	};

	// Submission and completion are separate observations. The submitted record
	// proves the queue accepted real recorded commands; the completed record is
	// advanced only when the existing frame-slot fence is reused. Keeping both
	// prevents a newer in-flight submission from hiding the last proven
	// completion while still avoiding any animation-specific synchronization.
	struct AnimationGpuBackendExecutionSnapshot
	{
		AnimationGpuBackendExecutionRecord latestSubmitted{};
		AnimationGpuBackendExecutionRecord latestCompleted{};

		[[nodiscard]] bool HasSubmittedWork() const noexcept
		{
			return latestSubmitted.IsComplete();
		}

		[[nodiscard]] bool HasCompletedWork() const noexcept
		{
			return latestCompleted.IsComplete();
		}
	};

	// A DrawCommand copies this result. Keeping the publication bundle alive
	// guarantees that all pose spans remain valid until the protected render
	// submission has finished consuming the command.
	struct AnimationRenderBinding
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID animatorObjectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID skeletonAssetId = TerraEngine::EMPTY_UUID;
		std::uint64_t animatorObjectGeneration = 0U;
		std::uint64_t animationAuthorityGeneration = 0U;
		std::uint32_t animatorSlotIndex = 0xffffffffU;
		std::uint32_t animatorGeneration = 0U;
		std::uint64_t deformablePayloadGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		std::uint32_t skeletonGeneration = 0U;
		std::uint64_t skeletonCompatibilityFingerprint = 0U;
		AnimationRenderPoseSource source = AnimationRenderPoseSource::BindPose;
		AnimationRenderBindingStatus status = AnimationRenderBindingStatus::MissingPose;
		std::uint64_t posePublicationGeneration = 0U;
		bool safePose = true;
		RenderInvalidationGenerationRecord invalidation;
		AnimationGpuRenderBinding gpu;
		std::shared_ptr<const TerraEngine::Animation::PublishedAnimatorPose> pose;
		std::shared_ptr<const CpuAnimationDeformationData> cpuDeformation;
		std::uint32_t cpuDeformationRecordIndex =
			std::numeric_limits<std::uint32_t>::max();
		// This bit is published only when a physical page carries complete
		// skin/morph inputs and an exact deformed-page publication is available.
		bool pageLocalDeformationAvailable = false;

		[[nodiscard]] bool HasCpuPose() const noexcept
		{
			return pose != nullptr && status == AnimationRenderBindingStatus::Ready
				&& !safePose && source != AnimationRenderPoseSource::BindPose;
		}

		[[nodiscard]] bool HasCpuDeformation() const noexcept
		{
			return HasCpuPose() && cpuDeformation != nullptr
				&& cpuDeformation->IsComplete()
				&& cpuDeformationRecordIndex
					!= std::numeric_limits<std::uint32_t>::max();
		}

		[[nodiscard]] bool RequiresConservativeCoarseVisibility() const noexcept
		{
			// CPU broadphase bounds cannot follow GPU skinning or morph output.
			// Keep an actively deformed object in the candidate set and let the
			// current compute-published page bounds make the precise decision.
			return deformablePayloadGeneration != 0U
				&& posePublicationGeneration != 0U
				&& status == AnimationRenderBindingStatus::Ready
				&& !safePose
				&& source != AnimationRenderPoseSource::BindPose;
		}

		[[nodiscard]] bool RequiresOwnerMeshDeformationFallback() const noexcept
		{
			// A rejected or temporarily unavailable demanded page has no
			// independently verifiable skin/morph inputs. Only that fail-open
			// state collapses to the complete owner; healthy pages stay refinable.
			return !pageLocalDeformationAvailable
				&& RequiresConservativeCoarseVisibility();
		}
	};

	// The resolver receives only protected-boundary values. currentPose is one
	// immutable AnimationSystem publication; heldBinding includes the full mesh
	// identity that originally accepted its pose, so a mesh recook cannot reuse a
	// compatible-looking pointer from another deformable payload generation.
	struct AnimationRenderBindingRequest
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID animatorObjectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID skeletonAssetId = TerraEngine::EMPTY_UUID;
		std::uint64_t animatorObjectGeneration = 0U;
		std::uint64_t animationAuthorityGeneration = 0U;
		std::uint32_t animatorSlotIndex = 0xffffffffU;
		std::uint32_t animatorGeneration = 0U;
		std::uint64_t deformablePayloadGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		std::uint32_t skeletonGeneration = 0U;
		std::uint64_t skeletonCompatibilityFingerprint = 0U;
		// The renderer resolves this capability from the current mesh/page
		// publication, not from an animator. Carry it through binding resolution
		// so accepting a new CPU/GPU pose cannot erase demanded-page support.
		bool pageLocalDeformationAvailable = false;
		// This exact record names the render-side mesh and protected renderer/scene
		// publication that a GPU-only packet was lowered against. It complements
		// shader-visible generations without truncating a 64-bit payload lifetime.
		TerraEngine::Animation::GpuAnimationRenderIdentity
			expectedGpuRenderIdentity{};
		AnimationRenderFallbackPolicy fallbackPolicy = AnimationRenderFallbackPolicy::BindPose;
		RenderInvalidationGenerationRecord invalidation;
		std::shared_ptr<const TerraEngine::Animation::PublishedAnimatorPose> currentPose;
		// Render collection may resolve this copied identity from the immutable
		// presentation envelope before it asks the CPU pose cache. It is optional
		// because CPU and safe-fallback animation do not have GPU packet work.
		std::optional<AnimationGpuRenderBinding> gpuVisualIdentity;
		AnimationRenderBinding heldBinding{};
	};

	// RenderCaptureSourceKind identifies the camera source that an editor or
	// MCP request wants to sample. The renderer keeps this typed so diagnostics
	// can distinguish Game View, authored camera objects, and one-off cameras
	// without parsing tool-specific JSON.
	enum class RenderCaptureSourceKind : std::uint8_t
	{
		GameView = 0,
		CameraObject = 1,
		CustomCamera = 2
	};

	inline const char* ToString(RenderCaptureSourceKind value)
	{
		switch (value)
		{
		case RenderCaptureSourceKind::GameView: return "game_view";
		case RenderCaptureSourceKind::CameraObject: return "camera_object";
		case RenderCaptureSourceKind::CustomCamera: return "custom_camera";
		default: return "unknown";
		}
	}

	struct RenderCaptureRequest
	{
		std::string requestId;
		RenderCaptureSourceKind sourceKind = RenderCaptureSourceKind::GameView;
		RenderViewType viewType = RenderViewType::Game;
		std::string sourceName;
		TerraEngine::UUID sourceObjectId = TerraEngine::EMPTY_UUID;
		glm::mat4 viewMatrix = glm::mat4(1.0f);
		glm::mat4 projectionMatrix = glm::mat4(1.0f);
		glm::vec3 cameraWorldPosition = glm::vec3(0.0f);
		glm::uvec2 targetSize = glm::uvec2(1280u, 720u);
		std::string format = "image/png";
		std::string freshness = "latestCompleted";
		bool includeOverlays = false;
		bool transparentBackground = false;
		bool allowBackendFallback = true;
		std::uint32_t timeoutMs = 2000u;
		// A next-frame Game View request keeps its GameView source identity while
		// rendering through the dedicated offscreen target. This avoids returning
		// stale pixels when the docked Game View tab is not currently visible.
		bool dedicatedRenderTarget = false;
		// The render coordinator stamps the frame whose output the backend copies.
		// Callers leave this zero; it is read-only correlation metadata.
		std::uint64_t submittedFrameId = 0u;
	};

	struct RenderCaptureResult
	{
		std::string requestId;
		bool ok = false;
		std::string message;
		std::string errorCategory = "internal";
		std::string mimeType = "image/png";
		std::string base64Data;
		glm::uvec2 size = glm::uvec2(0u);
		RenderCaptureSourceKind sourceKind = RenderCaptureSourceKind::GameView;
		std::string sourceName;
		std::uint64_t frameId = 0u;
		std::string freshness = "latestCompleted";
		bool overlaysIncluded = false;
		bool backendReadback = false;
		bool deterministicFallback = false;
		bool pendingAsync = false;
		std::vector<std::string> warnings;
	};

	struct RenderCaptureLimits
	{
		std::uint32_t defaultWidth = 1280u;
		std::uint32_t defaultHeight = 720u;
		// A missing maximum means captures are not capped by an engine policy.
		// Callers still pass positive 32-bit dimensions, and backends may fail a
		// request locally if a specific allocation cannot be represented.
		std::optional<std::uint32_t> maxWidth;
		std::optional<std::uint32_t> maxHeight;
		std::uint32_t maxConcurrentCaptures = 1u;
		std::uint32_t maxQueuedCaptures = 4u;
		std::uint32_t defaultTimeoutMs = 2000u;
	};

	struct RenderCaptureQueueState
	{
		std::uint32_t pendingCaptureCount = 0u;
		std::uint32_t activeCaptureCount = 0u;
		std::uint32_t completedResultCount = 0u;
	};

	struct RenderCaptureAvailability
	{
		bool available = true;
		bool rendererInitialized = false;
		bool gameViewAvailable = false;
		bool backendReadbackAvailable = false;
		bool deterministicFallbackAvailable = true;
		bool queueFull = false;
		bool overBudget = false;
		bool retryable = true;
		std::string state = "ready";
		std::string message = "Capture is ready.";
		std::string backendId = "none";
		std::string protectedBoundary = "LiveGpuSubmission";
		RenderCaptureLimits limits;
		RenderCaptureQueueState queueState;
		std::vector<std::string> supportedSources = { "game_view", "camera_object", "custom_camera" };
		std::vector<std::string> notes;
	};

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

// VirtualGeometryDebugVisualizationMode names only the public editor
// visualizer modes that can be selected without changing material authoring or
// scene state. Keep internal experiments out of this enum; stale numeric values
// are normalized to Off before they reach UI or shader push constants.
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
	VirtualGeometryDebugVisualizationLegendEntry{ VirtualGeometryDebugVisualizationMode::HiZ, "Cyan means Hi-Z is active for this view; red means fail-open or uncertain projected bounds; gray means the pyramid resource is missing." },
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
inline constexpr std::uint32_t kVirtualGeometryFlagConeCullingSafe = 1u << 5u;
// The task stage publishes exact group-relative meshlet indices even when it is
// operating as a pass-through stage. Keeping that contract separate from the
// culling toggle prevents larger ranges from restarting every 32 meshlets.
inline constexpr std::uint32_t kVirtualGeometryFlagTaskPayloadValid = 1u << 6u;
inline constexpr std::uint32_t kVirtualGeometryDebugModeShift = 8u;
inline constexpr std::uint32_t kVirtualGeometryDebugModeMask = 0xFFu;
// Packet flags reserve two high bits for the route selected by the immutable
// material/capability classifier and eight more for the bounded shading bin.
// The low bits remain compatible with the existing fail-open shader contract.
inline constexpr std::uint32_t kVirtualGeometryRasterRouteShift = 16u;
inline constexpr std::uint32_t kVirtualGeometryRasterRouteMask = 0x3u;
inline constexpr std::uint32_t kVirtualGeometryMaterialBinShift = 20u;
inline constexpr std::uint32_t kVirtualGeometryMaterialBinMask = 0xFFu;

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

// VirtualGeometryCounterSource labels where per-view counters came from. This
// prevents editor overlays and tests from treating an unavailable GPU readback
// as a trustworthy zero.
enum class VirtualGeometryCounterSource : std::uint32_t
{
	Prepared = 0u,
	GPUReadback = 1u,
	DelayedGPUReadback = 2u,
	Unsupported = 3u,
	Unavailable = 4u
};

inline const char* ToString(VirtualGeometryCounterSource source)
{
	switch (source)
	{
	case VirtualGeometryCounterSource::Prepared: return "Prepared";
	case VirtualGeometryCounterSource::GPUReadback: return "GPU Readback";
	case VirtualGeometryCounterSource::DelayedGPUReadback: return "Delayed GPU Readback";
	case VirtualGeometryCounterSource::Unsupported: return "Unsupported";
	case VirtualGeometryCounterSource::Unavailable: return "Unavailable";
	default: return "Unknown";
	}
}

// VirtualGeometryHiZState is the view-local Hi-Z resource contract. The state
// is diagnostic and conservative: anything outside Active must fail open rather
// than hide visible geometry.
enum class VirtualGeometryHiZState : std::uint32_t
{
	MissingPyramid = 0u,
	Active = 1u,
	FailOpen = 2u,
	Unsupported = 3u
};

inline const char* ToString(VirtualGeometryHiZState state)
{
	switch (state)
	{
	case VirtualGeometryHiZState::MissingPyramid: return "Missing Pyramid";
	case VirtualGeometryHiZState::Active: return "Active";
	case VirtualGeometryHiZState::FailOpen: return "Fail Open";
	case VirtualGeometryHiZState::Unsupported: return "Unsupported";
	default: return "Unknown";
	}
}

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
	Pinned = 9u,
	PendingRetirement = 10u,
	Retired = 11u
};

// VirtualGeometryPagePayloadSource names where a page payload physically lives
// before the shader consumes it. Keeping this separate from residency state
// lets diagnostics prove that normal rendering used streamed page payloads
// instead of silently reading the fully resident owning mesh buffer.
enum class VirtualGeometryPagePayloadSource : std::uint32_t
{
	Unknown = 0u,
	ResidentPagePayload = 1u,
	ResidentFallbackPayload = 2u,
	EmbeddedMeshPayload = 3u,
	ConservativeEmbeddedFallback = 4u,
	Missing = 5u,
	Invalid = 6u
};

inline const char* ToString(VirtualGeometryPagePayloadSource source)
{
	switch (source)
	{
	case VirtualGeometryPagePayloadSource::ResidentPagePayload: return "Resident Page Payload";
	case VirtualGeometryPagePayloadSource::ResidentFallbackPayload: return "Resident Fallback Payload";
	case VirtualGeometryPagePayloadSource::EmbeddedMeshPayload: return "Embedded Mesh Payload";
	case VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback: return "Conservative Embedded Fallback";
	case VirtualGeometryPagePayloadSource::Missing: return "Missing";
	case VirtualGeometryPagePayloadSource::Invalid: return "Invalid";
	case VirtualGeometryPagePayloadSource::Unknown:
	default: return "Unknown";
	}
}

// Page-local virtual-geometry descriptors are stored as 32-bit words because
// the shaders read them from the existing bindless uint buffer table. Offsets
// are word offsets into the same physical page payload buffer unless a future
// backend publishes a different payload buffer index.
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorStrideWords = 32u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorSourceWord = 2u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorBufferIndexWord = 8u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorIndexWord = 9u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorVertexWordOffset = 16u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorVertexWordSize = 17u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorPrimitiveWordOffset = 18u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorPrimitiveByteSize = 19u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorRemapWordOffset = 20u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorRemapCount = 21u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorMeshletWordOffset = 22u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorMeshletCount = 23u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorBoundsWordOffset = 24u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorBoundsCount = 25u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorDecodeWordOffset = 26u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorMaterialWordOffset = 27u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorMaterialCount = 28u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorLocalVertexCount = 29u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorLocalPrimitiveByteCount = 30u;
static constexpr std::uint32_t kVirtualGeometryPagePayloadDescriptorLocalRemapEncoding = 31u;

// VirtualGeometryEmittedPageSource is the renderer-visible answer for a
// selected cluster. It describes what actually reached the draw path after
// fallback substitution and validation, which can differ from the desired page.
enum class VirtualGeometryEmittedPageSource : std::uint32_t
{
	None = 0u,
	HighDetailPage = 1u,
	FallbackPage = 2u,
	ConservativeEmbeddedFallback = 3u,
	NoSubmittedWork = 4u,
	Invalid = 5u
};

inline const char* ToString(VirtualGeometryEmittedPageSource source)
{
	switch (source)
	{
	case VirtualGeometryEmittedPageSource::HighDetailPage: return "High Detail Page";
	case VirtualGeometryEmittedPageSource::FallbackPage: return "Fallback Page";
	case VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback: return "Conservative Embedded Fallback";
	case VirtualGeometryEmittedPageSource::NoSubmittedWork: return "No Submitted Work";
	case VirtualGeometryEmittedPageSource::Invalid: return "Invalid";
	case VirtualGeometryEmittedPageSource::None:
	default: return "None";
	}
}

// VirtualGeometryCounterSnapshot is copied into immutable frame snapshots and
// published diagnostics. It deliberately keeps source labels beside counts so
// tools can explain prepared CPU estimates versus delayed GPU readback values.
struct VirtualGeometryCounterSnapshot
{
	VirtualGeometryCounterSource source = VirtualGeometryCounterSource::Prepared;
	std::uint64_t preparedFrameId = 0u;
	std::uint64_t readbackFrameId = 0u;
	std::uint32_t readbackLatencyFrames = 0u;
	std::string unavailableReason;
	std::uint32_t toleranceMeshlets = 1u;
	float tolerancePercent = 1.0f;
	std::uint32_t sourceMeshlets = 0u;
	std::uint32_t selectedMeshlets = 0u;
	std::uint32_t dispatchedMeshlets = 0u;
	std::uint32_t culledMeshlets = 0u;
	std::uint32_t culledByFrustum = 0u;
	std::uint32_t culledByCone = 0u;
	std::uint32_t culledByHiZ = 0u;
	std::uint32_t fallbackMeshlets = 0u;
	std::uint32_t residentPages = 0u;
	std::uint32_t requestedPages = 0u;
	std::uint32_t missingPages = 0u;
	std::uint32_t fallbackSubstitutions = 0u;
	std::uint32_t residentPagePayloads = 0u;
	std::uint32_t fallbackPagePayloads = 0u;
	std::uint32_t embeddedFallbackPayloads = 0u;
	std::uint32_t emittedPageSources = 0u;
	bool authoritative = false;
};

inline bool VirtualGeometryCounterWithinTolerance(
	std::uint32_t expected,
	std::uint32_t actual,
	const VirtualGeometryCounterSnapshot& counters)
{
	const std::uint32_t difference = expected > actual ? expected - actual : actual - expected;
	const float percentAllowance = static_cast<float>(expected) * std::max(counters.tolerancePercent, 0.0f) * 0.01f;
	const std::uint32_t allowedDifference = std::max(
		counters.toleranceMeshlets,
		static_cast<std::uint32_t>(std::ceil(percentAllowance)));
	return difference <= allowedDifference;
}

inline std::string FormatVirtualGeometryCounterOverlayValue(
	std::string_view label,
	std::uint32_t value,
	VirtualGeometryCounterSource source,
	const std::string& reason)
{
	std::string text(label);
	if ((source == VirtualGeometryCounterSource::Unsupported
		|| source == VirtualGeometryCounterSource::Unavailable)
		&& value == 0u)
	{
		text += ": unavailable";
		if (!reason.empty())
		{
			text += " (";
			text += reason;
			text += ")";
		}
		return text;
	}

	text += " [";
	text += ToString(source);
	text += "]: ";
	text += std::to_string(value);
	if (source == VirtualGeometryCounterSource::DelayedGPUReadback && !reason.empty())
	{
		text += " (";
		text += reason;
		text += ")";
	}
	return text;
}

// VirtualGeometryHiZValidationState travels with one view snapshot. Keeping it
// as plain data lets Scene View, Game View, runtime, and export diagnostics own
// separate Hi-Z explanations without sharing backend objects.
struct VirtualGeometryHiZValidationState
{
	VirtualGeometryHiZState state = VirtualGeometryHiZState::MissingPyramid;
	std::string reason;
	glm::uvec2 pyramidSize = glm::uvec2(0u);
	glm::uvec2 renderSize = glm::uvec2(0u);
	std::uint64_t cameraSignature = 0u;
	std::uint64_t depthSceneSignature = 0u;
	std::uint32_t warmupFrames = 0u;
	bool pyramidReady = false;
	bool failOpen = true;
};

enum class VirtualGeometryBatchFallbackMode : std::uint32_t
{
	None = 0u,
	SinglePacketDraw = 1u,
	UnsupportedBackend = 2u,
	MissingResource = 3u,
	ValidationFallback = 4u
};

enum class VirtualGeometryFallbackCoverageStatus : std::uint32_t
{
	Complete = 0u,
	Partial = 1u,
	Missing = 2u,
	Invalid = 3u
};

enum class VirtualGeometryConeMaterialSidedness : std::uint32_t
{
	OneSided = 0u,
	DoubleSided = 1u,
	Unknown = 2u,
	Mixed = 3u
};

enum class VirtualGeometryConeWindingState : std::uint32_t
{
	Consistent = 0u,
	Mixed = 1u,
	Unknown = 2u,
	Invalid = 3u
};

enum class VirtualGeometryConeAlphaRisk : std::uint32_t
{
	Opaque = 0u,
	AlphaCutoutRisk = 1u,
	AlphaBlendRisk = 2u,
	Unknown = 3u
};

enum class VirtualGeometryConeThinShellRisk : std::uint32_t
{
	Low = 0u,
	High = 1u,
	Unknown = 2u
};

enum class VirtualGeometryPerformanceGateStatus : std::uint32_t
{
	Pass = 0u,
	Fail = 1u,
	Warning = 2u,
	Unsupported = 3u
};

enum class VirtualGeometryPerformanceGateSource : std::uint32_t
{
	Prepared = 0u,
	GPUReadback = 1u,
	DelayedGPUReadback = 2u,
	Unsupported = 3u,
	Manual = 4u
};

inline const char* ToString(VirtualGeometryPerformanceGateSource source)
{
	switch (source)
	{
	case VirtualGeometryPerformanceGateSource::Prepared: return "Prepared";
	case VirtualGeometryPerformanceGateSource::GPUReadback: return "GPU Readback";
	case VirtualGeometryPerformanceGateSource::DelayedGPUReadback: return "Delayed GPU Readback";
	case VirtualGeometryPerformanceGateSource::Unsupported: return "Unsupported";
	case VirtualGeometryPerformanceGateSource::Manual: return "Manual";
	default: return "Unknown";
	}
}

enum class VisibleStreamingResourceKind : std::uint32_t
{
	Mesh = 0u,
	Material = 1u,
	Texture = 2u
};

namespace TerraEngine::Renderer {

	// VisibleResourceTouch is the compact resource identity published from a
	// cached visible-command result. It lets the renderer say "this unique mesh
	// or material is still visible" once per view instead of repeating the same
	// ResourceManager calls for every draw command that references it.
	struct VisibleResourceTouch
	{
		TerraEngine::UUID resourceId = TerraEngine::EMPTY_UUID;
		::VisibleStreamingResourceKind kind = ::VisibleStreamingResourceKind::Mesh;
		std::int32_t priority = 100;
	};

	// Texture touches also carry the nearest visible distance observed while the
	// visible-command record was rebuilt. Texture streaming uses the nearest
	// distance so cache-hit frames keep the highest needed mip quality.
	struct VisibleTextureTouch
	{
		TerraEngine::UUID textureId = TerraEngine::EMPTY_UUID;
		double nearestViewDistance = 0.0;
		std::int32_t priority = 90;
	};

	struct VisibleStreamingTouchSet
	{
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t outputId = 0u;
		std::uint32_t sourceCommandCount = 0u;
		std::uint32_t duplicateEliminationCount = 0u;
		std::vector<VisibleResourceTouch> meshes;
		std::vector<VisibleResourceTouch> materials;
		std::vector<VisibleTextureTouch> textures;
		bool valid = false;
	};

	struct DiagnosticDemandState
	{
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t outputId = 0u;
		bool compactRequired = true;
		bool detailedRequired = false;
		std::vector<std::string> reasons;
	};

	struct VisibleCommandReuseTelemetry
	{
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t outputId = 0u;
		bool cacheHit = false;
		bool conservativeRefresh = false;
		bool detailedDiagnosticsRequested = false;
		std::uint32_t commandCount = 0u;
		std::uint32_t copiedCommandCount = 0u;
		std::uint32_t uniqueMeshTouches = 0u;
		std::uint32_t uniqueMaterialTouches = 0u;
		std::uint32_t uniqueTextureTouches = 0u;
		std::uint32_t duplicateResourceTouchesEliminated = 0u;
		std::string fallbackReason;
	};

}

// VirtualGeometryClusterPacket is the renderer-neutral work atom produced from
// a selected cluster cut. Later GPU-driven stages can upload the same table to
// an SSBO, while current CPU tests can inspect the copied values directly.
struct VirtualGeometryClusterPacket
{
	std::uint32_t packetId = 0u;
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	std::uint32_t selectedLod = 0u;
	std::uint32_t clusterId = 0u;
	std::uint32_t meshletOffset = 0u;
	std::uint32_t meshletCount = 0u;
	glm::vec3 boundsMin = glm::vec3(0.0f);
	glm::vec3 boundsMax = glm::vec3(0.0f);
	glm::vec4 projectedBounds = glm::vec4(0.0f);
	std::uint32_t pageId = 0u;
	std::uint32_t fallbackPageId = 0u;
	VirtualGeometryPagePayloadSource payloadSource = VirtualGeometryPagePayloadSource::Unknown;
	VirtualGeometryEmittedPageSource emittedSource = VirtualGeometryEmittedPageSource::None;
	std::uint64_t pagePayloadByteOffset = 0u;
	std::uint64_t pagePayloadByteSize = 0u;
	std::uint32_t payloadBufferIndex = 0xffffffffu;
	std::uint32_t payloadDescriptorIndex = 0xffffffffu;
	std::string fallbackReason;
	std::uint32_t debugIdentity = 0u;
	// These are preparation policy values. The GPU mirror packs them into its
	// existing flags word without widening the frozen packet ABI.
	std::uint32_t rasterRoute = 0u;
	std::uint32_t materialBin = 0u;
	bool fallbackPacket = false;
};

// ClusterWorkBatch groups compatible packet ranges into fewer submitted work
// items. It carries fallback details so tests can distinguish intentional
// single-packet correctness draws from accidentally exploding every cluster
// into separate work.
struct ClusterWorkBatch
{
	std::uint32_t batchId = 0u;
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	std::uint32_t packetOffset = 0u;
	std::uint32_t packetCount = 0u;
	std::uint32_t selectedMeshlets = 0u;
	std::uint32_t submittedMeshlets = 0u;
	std::string batchReason;
	VirtualGeometryBatchFallbackMode fallbackMode = VirtualGeometryBatchFallbackMode::None;
	std::string fallbackReason;
};

// CompactedVisibilitySet summarizes the packet table after rejection and
// fail-open decisions. The counters are deliberately packet-oriented; exact
// meshlet counts still come from task-shader/GPU readback snapshots.
struct CompactedVisibilitySet
{
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint32_t candidatePackets = 0u;
	std::uint32_t visiblePackets = 0u;
	std::uint32_t rejectedByFrustum = 0u;
	std::uint32_t rejectedByHiZ = 0u;
	std::uint32_t rejectedByResidency = 0u;
	std::uint32_t rejectedByCone = 0u;
	std::uint32_t failOpenPackets = 0u;
	std::uint32_t submittedBatches = 0u;
	VirtualGeometryCounterSource source = VirtualGeometryCounterSource::Prepared;
};

enum class VirtualGeometryGpuFrameSetValidity : std::uint32_t
{
	Valid = 0u,
	Delayed = 1u,
	Invalid = 2u,
	Unsupported = 3u,
	FailOpen = 4u
};

enum class VirtualGeometryGeneratedSubmissionValidationState : std::uint32_t
{
	Valid = 0u,
	Delayed = 1u,
	Mismatch = 2u,
	Overflow = 3u,
	Unsupported = 4u,
	FailOpen = 5u
};

// GpuVirtualGeometryPacketRecord is the stable CPU mirror of the packet layout
// that the Vulkan path may upload for task/mesh shader consumption. Keeping a
// mirror in RenderTypes lets tests validate ownership and fallback decisions
// without reading live GPU buffers.
struct GpuVirtualGeometryPacketRecord
{
	std::uint32_t packetId = 0u;
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameId = 0u;
	TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	std::uint32_t selectedLod = 0u;
	std::uint32_t clusterId = 0u;
	std::uint32_t meshletOffset = 0u;
	std::uint32_t meshletCount = 0u;
	glm::vec3 boundsMin = glm::vec3(0.0f);
	glm::vec3 boundsMax = glm::vec3(0.0f);
	glm::vec4 projectedBounds = glm::vec4(0.0f);
	std::uint32_t pageId = 0u;
	std::uint32_t fallbackPageId = 0u;
	VirtualGeometryPagePayloadSource payloadSource = VirtualGeometryPagePayloadSource::Unknown;
	VirtualGeometryEmittedPageSource emittedSource = VirtualGeometryEmittedPageSource::None;
	std::uint64_t pagePayloadByteOffset = 0u;
	std::uint64_t pagePayloadByteSize = 0u;
	std::uint32_t payloadBufferIndex = 0xffffffffu;
	std::uint32_t payloadDescriptorIndex = 0xffffffffu;
	std::string fallbackReason;
	std::uint32_t debugIdentity = 0u;
	std::uint32_t flags = 0u;
};

// VirtualGeometryVisibilityPick is the route-neutral identity returned to editor
// tools after they query the latest compacted visibility records. It deliberately
// contains object/cluster/meshlet identity rather than a backend buffer address,
// so Scene View never retains a Vulkan resource past its owning frame.
struct VirtualGeometryVisibilityPick
{
	TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	std::uint64_t frameId = 0u;
	std::uint32_t packetId = 0u;
	std::uint32_t clusterId = 0u;
	std::uint32_t meshletId = 0u;
	std::uint32_t selectedLod = 0u;
	std::uint32_t debugIdentity = 0u;
	bool fallback = false;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return objectId != TerraEngine::EMPTY_UUID
			&& meshId != TerraEngine::EMPTY_UUID
			&& frameId != 0u;
	}
};

namespace TerraEngine::Renderer {

	// AuthoritativeVisibilityPickStatus keeps automation from presenting a queued
	// GPU copy or an unavailable renderer as if it were a completed object pick.
	enum class AuthoritativeVisibilityPickStatus : std::uint8_t
	{
		Unavailable,
		Pending,
		Ready
	};

	inline const char* ToString(AuthoritativeVisibilityPickStatus status)
	{
		switch (status)
		{
		case AuthoritativeVisibilityPickStatus::Unavailable: return "Unavailable";
		case AuthoritativeVisibilityPickStatus::Pending: return "Pending";
		case AuthoritativeVisibilityPickStatus::Ready: return "Ready";
		default: return "Unavailable";
		}
	}

	// AuthoritativeVisibilityPickState is a copied automation-facing status. It
	// owns no Vulkan handles and exposes a pick only after the owning frame fence
	// has completed and RenderSystem has mapped its typed identity to a packet.
	struct AuthoritativeVisibilityPickState
	{
		AuthoritativeVisibilityPickStatus status = AuthoritativeVisibilityPickStatus::Unavailable;
		glm::uvec2 pixel = glm::uvec2(0u);
		std::optional<::VirtualGeometryVisibilityPick> pick;
		std::string reason;
	};

}

// GpuVirtualGeometryBatchDescriptor mirrors a compatible packet run that can be
// consumed by GPU-driven submission. It deliberately repeats the compatibility
// keys so stale generated submissions can be rejected without touching live
// renderer state.
struct GpuVirtualGeometryBatchDescriptor
{
	std::uint32_t batchId = 0u;
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameId = 0u;
	std::uint32_t packetOffset = 0u;
	std::uint32_t packetCount = 0u;
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	std::uint32_t selectedMeshlets = 0u;
	std::uint32_t submittedMeshlets = 0u;
	VirtualGeometryDebugVisualizationMode debugMode = VirtualGeometryDebugVisualizationMode::None;
	VirtualGeometryBatchFallbackMode fallbackMode = VirtualGeometryBatchFallbackMode::None;
	std::uint32_t flags = 0u;
};

struct GpuVirtualGeometryPacketFrameSet
{
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameId = 0u;
	glm::uvec2 targetSize = glm::uvec2(0u);
	VirtualGeometryDebugVisualizationMode debugMode = VirtualGeometryDebugVisualizationMode::None;
	std::uint32_t packetOffset = 0u;
	std::uint32_t packetCount = 0u;
	std::uint32_t batchOffset = 0u;
	std::uint32_t batchCount = 0u;
	std::uint32_t compactedPacketOffset = 0u;
	std::uint32_t compactedPacketCount = 0u;
	std::uint32_t generatedSubmissionOffset = 0u;
	std::uint32_t generatedSubmissionCount = 0u;
	VirtualGeometryGpuFrameSetValidity validity = VirtualGeometryGpuFrameSetValidity::Valid;
	std::string fallbackReason;
};

struct GpuPacketCompactionDispatch
{
	std::uint32_t dispatchId = 0u;
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameId = 0u;
	std::uint32_t candidatePacketCount = 0u;
	std::uint32_t candidateBatchCount = 0u;
	std::uint32_t outputCapacity = 0u;
	VirtualGeometryHiZState hiZBaselineState = VirtualGeometryHiZState::MissingPyramid;
	VirtualGeometryDebugVisualizationMode debugMode = VirtualGeometryDebugVisualizationMode::None;
	bool failOpenOnInvalid = true;
	std::string fallbackReason;
};

struct GeneratedVirtualGeometrySubmissionRecord
{
	std::uint32_t recordId = 0u;
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameId = 0u;
	std::uint32_t batchId = 0u;
	std::uint32_t packetOffset = 0u;
	std::uint32_t packetCount = 0u;
	std::uint32_t indirectTaskCount = 0u;
	TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	std::uint32_t counterOffset = 0u;
	VirtualGeometryCounterSource source = VirtualGeometryCounterSource::Prepared;
	VirtualGeometryGeneratedSubmissionValidationState validationState = VirtualGeometryGeneratedSubmissionValidationState::Valid;
	std::string fallbackReason;
};

struct VirtualGeometryCounterReadbackRecord
{
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameId = 0u;
	VirtualGeometryCounterSource source = VirtualGeometryCounterSource::Prepared;
	std::uint32_t latencyFrames = 0u;
	std::uint32_t candidatePackets = 0u;
	std::uint32_t compactedPackets = 0u;
	std::uint32_t submittedBatches = 0u;
	std::uint32_t rejectedPackets = 0u;
	std::uint32_t failOpenPackets = 0u;
	std::uint32_t selectedMeshlets = 0u;
	std::uint32_t dispatchedMeshlets = 0u;
	std::uint32_t culledMeshlets = 0u;
	std::uint32_t culledByHiZ = 0u;
	std::string unavailableReason;
};

struct VirtualGeometryGpuFallbackRecord
{
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameId = 0u;
	std::string reason;
	std::uint32_t affectedPackets = 0u;
	std::uint32_t affectedBatches = 0u;
	std::string coveragePolicy = "FailOpenCoverage";
	std::string diagnostic;
};

// PageResidencyRecord mirrors the page state needed by virtual geometry
// selection without exposing live ResourceManager containers to worker jobs.
struct PageResidencyRecord
{
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	std::uint32_t pageId = 0u;
	std::uint64_t meshPayloadGeneration = 0u;
	std::uint64_t pageContentGeneration = 0u;
	std::uint64_t residencyGeneration = 0u;
	std::uint32_t coveredClusterOffset = 0u;
	std::uint32_t coveredClusterCount = 0u;
	std::vector<std::uint32_t> coveredClusters;
	std::uint64_t byteCost = 0u;
	std::uint64_t payloadByteOffset = 0u;
	std::uint64_t payloadByteSize = 0u;
	GeometryPageState state = GeometryPageState::NotResident;
	VirtualGeometryPagePayloadSource payloadSource = VirtualGeometryPagePayloadSource::Unknown;
	std::uint32_t fallbackPageId = 0u;
	bool isFallback = false;
	bool protectedFallback = false;
	std::uint64_t lastRequestedFrame = 0u;
	std::uint64_t lastUsedFrame = 0u;
	std::uint64_t retirementFrame = 0u;
	std::int32_t priority = 0;
	std::string failureReason;
};

// VirtualGeometryPagePayloadRecord is the immutable payload address table that
// render preparation, upload planning, diagnostics, and tests can share. It
// intentionally stores page-local offsets and counts, not pointers to live
// mesh or backend allocations.
struct VirtualGeometryPagePayloadRecord
{
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	std::uint32_t pageId = 0u;
	std::uint32_t fallbackPageId = 0xffffffffu;
	VirtualGeometryPagePayloadSource source = VirtualGeometryPagePayloadSource::Unknown;
	std::uint32_t selectedLod = 0u;
	std::uint32_t clusterOffset = 0u;
	std::uint32_t clusterCount = 0u;
	std::uint32_t meshletOffset = 0u;
	std::uint32_t meshletCount = 0u;
	std::uint32_t vertexOffset = 0u;
	std::uint32_t vertexCount = 0u;
	std::uint32_t primitiveOffset = 0u;
	std::uint32_t primitiveCount = 0u;
	std::uint32_t materialSectionId = 0u;
	std::uint64_t payloadByteOffset = 0u;
	std::uint64_t payloadByteSize = 0u;
	std::uint32_t payloadBufferIndex = 0xffffffffu;
	std::uint32_t payloadDescriptorIndex = 0xffffffffu;
	std::uint64_t allocationId = 0u;
	std::uint64_t meshPayloadGeneration = 0u;
	std::uint64_t pageContentGeneration = 0u;
	std::uint64_t residencyGeneration = 0u;
	std::uint64_t publicationGeneration = 0u;
	std::uint64_t safeRetirementFrame = 0u;
	std::uint32_t animatedGeometryAbiVersion = 0u;
	bool pageLocalDeformationReady = false;
	bool currentGeometryValid = false;
	bool historyValid = false;
	bool dynamicBoundsValid = false;
	bool resident = false;
	bool pinnedFallback = false;
	bool valid = false;
	std::string owningMeshFallbackReason;
	std::string diagnostic;
};

// VirtualGeometryPageRetirementRecord is a value-only handoff from residency
// planning to backend lifetime management. It carries page ids and byte costs;
// actual Vulkan allocations still retire through protected frame-slot cleanup.
struct VirtualGeometryPageRetirementRecord
{
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	std::uint32_t pageId = 0u;
	std::uint64_t frameId = 0u;
	std::uint64_t byteSize = 0u;
	bool protectedFallback = false;
	bool retired = false;
	std::string reason;
};

// VirtualGeometryEmittedPageSourceRecord answers "what did this selected
// cluster draw from?" for one view. Tests use it to reject accidental normal
// use of the owning mesh buffer, while diagnostics use it to explain fallback.
struct VirtualGeometryEmittedPageSourceRecord
{
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameId = 0u;
	TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
	std::uint32_t clusterId = 0u;
	std::uint32_t selectedLod = 0u;
	std::uint32_t pageId = 0u;
	std::uint32_t fallbackPageId = 0xffffffffu;
	VirtualGeometryPagePayloadSource payloadSource = VirtualGeometryPagePayloadSource::Unknown;
	VirtualGeometryEmittedPageSource emittedSource = VirtualGeometryEmittedPageSource::None;
	std::uint32_t meshletOffset = 0u;
	std::uint32_t meshletCount = 0u;
	std::uint64_t payloadByteOffset = 0u;
	std::uint64_t payloadByteSize = 0u;
	std::uint32_t payloadBufferIndex = 0xffffffffu;
	std::uint32_t payloadDescriptorIndex = 0xffffffffu;
	bool fallbackSubstitution = false;
	bool conservativeEmbeddedFallback = false;
	std::string reason;
};

// FallbackCoverageChain records how a missing desired cluster remains covered
// by resident fallback geometry, which is the rule that prevents holes while
// real page streaming catches up.
struct FallbackCoverageChain
{
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	std::uint32_t desiredClusterId = 0u;
	std::uint32_t fallbackClusterId = 0xffffffffu;
	std::uint32_t fallbackPageId = 0u;
	VirtualGeometryFallbackCoverageStatus coverageStatus = VirtualGeometryFallbackCoverageStatus::Complete;
	std::uint32_t substitutedMeshlets = 0u;
	std::string reason;
};

// HiZRegressionSnapshot preserves the currently accepted Hi-Z behavior. Feature
// 030 intentionally does not add quality tiers; new batching work must match
// this baseline or fail open.
struct HiZRegressionSnapshot
{
	std::uint32_t viewId = 0u;
	RenderViewType viewType = RenderViewType::Game;
	std::uint64_t frameIndex = 0u;
	VirtualGeometryHiZState state = VirtualGeometryHiZState::MissingPyramid;
	std::string reason;
	std::uint32_t testedMeshlets = 0u;
	std::uint32_t occludedMeshlets = 0u;
	std::uint32_t visibleMeshlets = 0u;
	std::uint32_t failOpenMeshlets = 0u;
	std::uint32_t invalidDepthSamples = 0u;
	std::uint32_t recoveryFrames = 0u;
	bool baselineMatched = true;
};

// ConeSafetyMetadata is conservative by default. Unknown imported sidedness,
// winding, alpha, or thin-shell risk disables cone culling for that cluster.
struct ConeSafetyMetadata
{
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	std::uint32_t clusterId = 0u;
	VirtualGeometryConeMaterialSidedness materialSidedness = VirtualGeometryConeMaterialSidedness::Unknown;
	VirtualGeometryConeWindingState windingState = VirtualGeometryConeWindingState::Unknown;
	VirtualGeometryConeAlphaRisk alphaRisk = VirtualGeometryConeAlphaRisk::Unknown;
	VirtualGeometryConeThinShellRisk thinShellRisk = VirtualGeometryConeThinShellRisk::Unknown;
	bool coneValid = false;
	bool eligible = false;
	std::string disabledReason = "Cone safety metadata is unavailable.";
};

inline std::string BuildVirtualGeometryConeDisabledReason(const ConeSafetyMetadata& metadata)
{
	if (!metadata.coneValid)
	{
		return "Cone bounds are missing or invalid.";
	}
	if (metadata.materialSidedness != VirtualGeometryConeMaterialSidedness::OneSided)
	{
		return "Material sidedness is not proven one-sided.";
	}
	if (metadata.windingState != VirtualGeometryConeWindingState::Consistent)
	{
		return "Triangle winding is not proven consistent.";
	}
	if (metadata.alphaRisk != VirtualGeometryConeAlphaRisk::Opaque)
	{
		return "Material alpha risk is not proven opaque.";
	}
	if (metadata.thinShellRisk != VirtualGeometryConeThinShellRisk::Low)
	{
		return "Thin-shell risk is not proven low.";
	}
	return {};
}

inline bool IsVirtualGeometryConeCullingEligible(const ConeSafetyMetadata& metadata)
{
	return BuildVirtualGeometryConeDisabledReason(metadata).empty();
}

inline ConeSafetyMetadata NormalizeVirtualGeometryConeSafetyMetadata(ConeSafetyMetadata metadata)
{
	// Every caller uses this one rule so imported unknowns, double-sided
	// materials, mixed winding, alpha cutouts, and thin shells all fail open in
	// the same visible way instead of quietly arming shader cone culling.
	metadata.disabledReason = BuildVirtualGeometryConeDisabledReason(metadata);
	metadata.eligible = metadata.disabledReason.empty();
	if (metadata.eligible)
	{
		metadata.disabledReason.clear();
	}
	return metadata;
}

// ShadowVisibilitySelection belongs to a light or cascade view. It never
// reuses camera Hi-Z because shadow visibility answers a different question.
struct ShadowVisibilitySelection
{
	std::uint32_t shadowViewId = 0u;
	TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
	std::uint64_t frameIndex = 0u;
	std::uint32_t candidatePackets = 0u;
	std::uint32_t selectedPackets = 0u;
	std::uint32_t rejectedByLightFrustum = 0u;
	std::uint32_t selectedMeshlets = 0u;
	std::uint32_t dispatchedMeshlets = 0u;
	std::vector<std::string> fallbackReasons;
	bool reusedCameraHiZ = false;
};

// PerformanceGateResult is the virtual-geometry-specific gate record used by
// scalability tests and editor diagnostics. It complements the generic render
// performance gate by naming the fixture, metric, and source explicitly.
struct PerformanceGateResult
{
	std::string gateId;
	std::string fixtureId;
	RenderViewType viewType = RenderViewType::Game;
	std::uint32_t viewId = 0u;
	// Asset/page scope is optional because some gates cover an aggregate view
	// or an old CPU-prepared estimate. When present, automation can point a
	// failing page-streaming gate at the asset/page family that caused it.
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	std::uint32_t pageId = std::numeric_limits<std::uint32_t>::max();
	std::string metricName;
	float threshold = 0.0f;
	float actual = 0.0f;
	std::string unit;
	VirtualGeometryPerformanceGateStatus status = VirtualGeometryPerformanceGateStatus::Pass;
	VirtualGeometryPerformanceGateSource source = VirtualGeometryPerformanceGateSource::Prepared;
	std::string diagnostic;
};

namespace TerraEngine::Renderer {

	inline constexpr std::uint32_t kVirtualGeometryOcclusionDebugSampleCapacity = 64u;

	inline bool VirtualGeometryCounterWithinTolerance(
		std::uint32_t expected,
		std::uint32_t actual,
		const ::VirtualGeometryCounterSnapshot& counters)
	{
		return ::VirtualGeometryCounterWithinTolerance(expected, actual, counters);
	}

	inline std::string FormatVirtualGeometryCounterOverlayValue(
		std::string_view label,
		std::uint32_t value,
		::VirtualGeometryCounterSource source,
		const std::string& reason)
	{
		return ::FormatVirtualGeometryCounterOverlayValue(label, value, source, reason);
	}

	// OcclusionDebugSample is a compact, user-facing explanation record. It is
	// intentionally bounded per view so diagnostics can stay enabled in the
	// editor without trying to mirror every meshlet decision from the GPU.
	struct OcclusionDebugSample
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint32_t clusterId = 0xffffffffu;
		std::uint32_t meshletId = 0xffffffffu;
		glm::vec4 projectedBounds = glm::vec4(0.0f);
		std::uint32_t sampledMip = 0u;
		VirtualGeometryCullOutcome outcome = VirtualGeometryCullOutcome::Visible;
		std::string reason;
	};

	inline std::string FormatVirtualGeometryHiZOverlayStatus(
		VirtualGeometryHiZState state,
		bool hiZActive,
		const std::string& reason)
	{
		if (hiZActive && state == VirtualGeometryHiZState::Active)
		{
			return "Active";
		}

		std::string label = ToString(state);
		if (label == "Unknown")
		{
			label = hiZActive ? "Active" : "Off";
		}
		if (!reason.empty())
		{
			label += ": ";
			label += reason;
		}
		return label;
	}

}

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
	// Import defaults do not discard valid LODs. These gates remain available
	// as per-mesh overrides when an asset benefits from stronger merging.
	std::uint32_t minimumSourceTriangles = 1u;
	std::uint32_t minimumGeneratedTriangles = 1u;
	std::uint32_t minimumMeshletsPerGeneratedLevel = 1u;
	float minimumUsefulTriangleReduction = 0.0f;
	float minimumUsefulMeshletReduction = 0.0f;
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
		: 0.0f;
	sanitized.minimumUsefulMeshletReduction = std::isfinite(sanitized.minimumUsefulMeshletReduction)
		? glm::clamp(sanitized.minimumUsefulMeshletReduction, 0.0f, 0.95f)
		: 0.0f;
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
	// Separate limits prevent animated outputs from silently consuming the
	// static page pool. Zero retains the existing developer "unlimited" mode.
	std::uint64_t deformationCurrentBudgetBytes = 0u;
	std::uint64_t deformationHistoryBudgetBytes = 0u;
	std::uint64_t deformationBoundsBudgetBytes = 0u;
	float preloadBias = 1.0f;
	// Transition stability is a runtime selection aid, not a cook setting. The
	// pixel band keeps tiny camera jitter from swapping cluster cuts every
	// frame, while stableFrameCount bounds how long a near-threshold candidate
	// must persist before it replaces the current cut.
	float lodTransitionHysteresisPixels = 0.35f;
	std::uint32_t lodTransitionStableFrames = 3u;
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
	profile.deformationCurrentBudgetBytes = 0u;
	profile.deformationHistoryBudgetBytes = 0u;
	profile.deformationBoundsBudgetBytes = 0u;
	profile.preloadBias = 1.0f;
	profile.lodTransitionHysteresisPixels = 0.35f;
	profile.lodTransitionStableFrames = 3u;
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
	sanitized.lodTransitionHysteresisPixels = std::isfinite(sanitized.lodTransitionHysteresisPixels)
		? glm::clamp(sanitized.lodTransitionHysteresisPixels, 0.0f, 8.0f)
		: 0.35f;
	sanitized.lodTransitionStableFrames = glm::clamp(sanitized.lodTransitionStableFrames, 1u, 16u);
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
	HashVirtualGeometryQualityValue(
		seed, sanitized.deformationCurrentBudgetBytes);
	HashVirtualGeometryQualityValue(
		seed, sanitized.deformationHistoryBudgetBytes);
	HashVirtualGeometryQualityValue(
		seed, sanitized.deformationBoundsBudgetBytes);
	HashVirtualGeometryQualityValue(seed, HashVirtualGeometryQualityFloat(sanitized.preloadBias));
	HashVirtualGeometryQualityValue(seed, HashVirtualGeometryQualityFloat(sanitized.lodTransitionHysteresisPixels));
	HashVirtualGeometryQualityValue(seed, sanitized.lodTransitionStableFrames);
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
	float deformationError = 0.0f;
	glm::vec3 boundsCenter = glm::vec3(0.0f);
	float boundsRadius = 0.0f;
};

// VirtualGeometryLODTransitionState is the small public record that snapshots,
// diagnostics, and tests can inspect. RenderSystem keeps the actual previous
// cluster cut privately; this record only describes why the cut was accepted,
// held, or reset for one view/object pair.
struct VirtualGeometryLODTransitionState
{
	RenderViewType viewType = RenderViewType::Game;
	std::uint32_t outputId = 0u;
	TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
	std::uint64_t viewStateKey = 0u;
	std::uint64_t lastStableSelectionSignature = 0u;
	std::uint64_t candidateSelectionSignature = 0u;
	std::uint32_t stableFrameCount = 0u;
	std::uint32_t heldFrameCount = 0u;
	std::uint32_t acceptedFrameCount = 0u;
	float hysteresisBandPixels = 0.35f;
	float transitionMarginPixels = 0.0f;
	float candidateMaxErrorPixels = 0.0f;
	VirtualGeometryDebugVisualizationMode debugVisualizationMode = VirtualGeometryDebugVisualizationMode::None;
	bool heldPreviousSelection = false;
	bool resetThisFrame = false;
	bool valid = false;
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
		bool protectedFallback = false;
		std::uint32_t clusterOffset = 0u;
	std::uint32_t clusterCount = 0u;
	std::uint64_t payloadByteOffset = 0u;
	std::uint64_t payloadByteSize = 0u;
	std::uint64_t meshPayloadGeneration = 0u;
	std::uint64_t pageContentGeneration = 0u;
	std::uint64_t residencyGeneration = 0u;
	std::uint64_t publicationGeneration = 0u;
	std::uint64_t allocationId = 0u;
	std::uint64_t safeRetirementFrame = 0u;
	std::uint32_t animatedGeometryAbiVersion = 0u;
	bool eligibleForNewWork = false;
	bool pageLocalDeformationReady = false;
	// These indices are assigned by the protected renderer/resource boundary
	// once a page payload has a shader-visible storage-buffer binding. Keeping
	// them with the page snapshot lets worker-safe cluster selection propagate
	// physical payload identity without reading live Vulkan descriptor state.
	std::uint32_t payloadBufferIndex = 0xffffffffu;
	std::uint32_t payloadDescriptorIndex = 0xffffffffu;
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
	// Aggregate profiles are immutable cooker facts. The fallback is still a
	// normal cluster id, so visibility and residency remain typed cluster/page
	// operations instead of treating an aggregate profile as draw geometry.
	struct AggregateProfileRuntime
	{
		::TerraEngine::Renderer::VirtualGeometryAggregateRepresentation representation{};
		std::uint32_t fallbackClusterId = 0xffffffffu;
		bool legacyDerived = false;

		[[nodiscard]] bool Valid() const noexcept
		{
			return representation.Valid() && fallbackClusterId != 0xffffffffu;
		}
	};
	std::vector<AggregateProfileRuntime> aggregateProfiles;
	std::vector<ConeSafetyMetadata> coneSafety;
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
	// Vulkan validation is startup-only because layer selection happens during
	// instance creation. Render debug diagnostics share the same saved profile
	// but can be mirrored into the running renderer when the editor toggles it.
	std::uint32_t validationEnabled = 0u;
	std::uint32_t debugEnabled = 0u;
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
	// Route preparation copies the selected representation onto every cached
	// draw command. View selection must consult this value so a page-less,
	// direct-resident TEMF never enters hierarchy, page, or VG raster work.
	TerraEngine::Renderer::GeometryRoute geometryRoute =
		TerraEngine::Renderer::GeometryRoute::DirectMeshlet;
	// Animation binding is a copied, immutable generation bundle. Static meshes
	// leave it at the bind-pose default and pay no animation-system lookup cost.
	TerraEngine::Renderer::AnimationRenderBinding animationBinding{};
	// Editor-only tint is submitted per draw so Scene View context modes can
	// de-emphasize surrounding objects without changing runtime materials.
	glm::vec4 editorTint = glm::vec4(0.0f);
	std::uint32_t virtualMeshletOffset = 0u;
	std::uint32_t virtualMeshletCount = 0u;
	std::uint32_t selectedVirtualLod = 0u;
	std::uint32_t virtualGeometryFlags = 0u;
	// These offsets/counts point at the immutable per-view selection packet.
	// Visible virtual geometry can now batch compatible contiguous packet
	// ranges, so a command may reference one cluster or a small packet run.
	std::uint32_t selectedVirtualClusterOffset = 0u;
	std::uint32_t selectedVirtualClusterCount = 0u;
	std::uint32_t selectedVirtualPageNeedOffset = 0u;
	std::uint32_t selectedVirtualPageNeedCount = 0u;
	std::uint32_t virtualGeometryPageId = 0u;
	std::uint32_t virtualGeometryFallbackPageId = 0xffffffffu;
	VirtualGeometryPagePayloadSource virtualGeometryPayloadSource = VirtualGeometryPagePayloadSource::Unknown;
	VirtualGeometryEmittedPageSource virtualGeometryEmittedSource = VirtualGeometryEmittedPageSource::None;
	std::uint64_t virtualGeometryPagePayloadByteOffset = 0u;
	std::uint64_t virtualGeometryPagePayloadByteSize = 0u;
	// Draw commands carry the resolved page-payload binding from RenderSystem
	// into the backend. Invalid values deliberately mean "no physical page
	// descriptor was proven", which keeps diagnostics from confusing metadata
	// with renderable GPU storage.
	std::uint32_t virtualGeometryPayloadBufferIndex = 0xffffffffu;
	std::uint32_t virtualGeometryPayloadDescriptorIndex = 0xffffffffu;
	// Page-local payload descriptors are generated per submitted packet, not per
	// selected-cluster row. Keeping the packet descriptor id separate prevents
	// imported package meshes from decoding the wrong page-local stream when
	// cluster selection and packet compaction no longer have identical offsets.
	std::uint32_t virtualGeometryPacketDescriptorIndex = 0xffffffffu;
	bool taskShaderCullingEnabled = true;

	[[nodiscard]] bool UsesVirtualGeometryRoute() const noexcept
	{
		return geometryRoute == TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware
			|| geometryRoute == TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute;
	}
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
	// Bindless storage-buffer indices for the current view's GPU packet and
	// batch descriptor tables. 0xFFFFFFFF means the shader must keep using the
	// push-constant range as the conservative fail-open source of truth.
	std::uint32_t virtualGeometryPacketBufferIndex = 0xFFFFFFFFu;
	std::uint32_t virtualGeometryBatchBufferIndex = 0xFFFFFFFFu;
	std::uint32_t virtualGeometryPagePayloadBufferIndex = 0xFFFFFFFFu;
	// Page-source metadata labels the payload that this draw intends to use.
	// The byte range is deliberately truncated to low 32-bit words here because
	// shader-visible payload buffers are still bounded by per-view uploads.
	std::uint32_t virtualGeometryPayloadSource = 0u;
	std::uint32_t virtualGeometryEmittedSource = 0u;
	std::uint32_t virtualGeometryPagePayloadByteOffset = 0u;
	std::uint32_t virtualGeometryPagePayloadByteSize = 0u;
	std::uint32_t virtualGeometryPacketDescriptorIndex = 0xffffffffu;
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

// VirtualGeometryDecodedPageUploadRecord is the immutable value handoff from
// worker-safe exact-range decoding to the protected RHI upload boundary. The
// owned bytes and every identity required for publication travel together, so
// a backend never needs to read live ResourceManager or streaming containers.
struct VirtualGeometryDecodedPageUploadRecord
{
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	std::uint32_t pageId = 0xffffffffu;
	std::uint32_t fallbackPageId = 0xffffffffu;
	std::uint32_t clusterId = 0xffffffffu;
	std::vector<std::uint32_t> dependencyPageIds;
	std::uint64_t publicationGeneration = 0u;
	std::uint64_t storedByteCount = 0u;
	std::uint64_t decodedByteCount = 0u;
	std::uint64_t integrityValue = 0u;
	std::vector<std::uint8_t> decodedBytes;
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
	// These vectors are immutable per-view mirrors of selected virtual geometry
	// work. Vulkan may upload them into GPU packet buffers during the protected
	// render boundary; it must not reach back into RenderSystem caches.
	std::vector<GpuVirtualGeometryPacketRecord> virtualGeometryGpuPackets;
	std::vector<GpuVirtualGeometryBatchDescriptor> virtualGeometryGpuBatches;
	std::vector<GpuVirtualGeometryPacketFrameSet> virtualGeometryGpuFrameSets;
	std::vector<GpuPacketCompactionDispatch> virtualGeometryGpuCompactionDispatches;
		std::vector<GeneratedVirtualGeometrySubmissionRecord> virtualGeometryGeneratedSubmissions;
		std::vector<VirtualGeometryCounterReadbackRecord> virtualGeometryCounterReadbacks;
		std::vector<VirtualGeometryGpuFallbackRecord> virtualGeometryGpuFallbackRecords;
		std::vector<VirtualGeometryPageRetirementRecord> virtualGeometryPageRetirements;
		// A decoded page remains non-resident until the backend uploads this
		// record and the owning frame-slot fence completes.
		std::vector<VirtualGeometryDecodedPageUploadRecord> virtualGeometryDecodedPageUploads;
		// Scene View can request a whole-view authoring tint for non-mesh passes
	// such as sky and grid while individual DrawCommand tint preserves the
	// focused prefab branch at normal color.
	glm::vec4 editorContextTint = glm::vec4(0.0f);
	// Scene View overlays can hide the editor grid without changing authored
	// scene data or Game View output. The flag is carried per frame so Vulkan
	// records or skips the grid pass from immutable render data.
	bool sceneViewGridVisible = true;
	// Scene View orthographic mode uses a flat editor background so the
	// procedural sky horizon does not read as perspective depth in an ortho view.
	bool sceneViewOrthographic = false;
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
	std::uint32_t hiZTests = 0u;
	std::uint32_t hiZProjectionFailed = 0u;
	std::uint32_t hiZFullRectCulled = 0u;
	std::uint32_t hiZRectCoverageCulled = 0u;
	std::uint32_t hiZCenterCulled = 0u;
	std::uint32_t hiZVertexCoverageCulled = 0u;
	std::uint32_t hiZNoOcclusionProof = 0u;
	std::uint32_t hiZReserved = 0u;
};
static_assert(sizeof(MeshletCullingCountersGPU) == 48, "Meshlet counter payload must keep a stable 16-byte aligned layout.");

// UI-facing immutable snapshot returned by the renderer. The renderer owns
// writes while editor windows read copies, so no caller mutates shared state.
struct RenderStatsSnapshot
{
	RenderViewType viewType = RenderViewType::Game;
	// Completed view size lets lightweight UI stats validate backend-owned
	// resources, such as Hi-Z pyramids, without reaching back into live Vulkan
	// render targets.
	glm::uvec2 renderSize = glm::uvec2(0u);
	float frameTimeMs = 0.0f;
	float fps = 0.0f;
	std::uint32_t drawCalls = 0u;
	std::uint32_t triangleCount = 0u;
	std::uint32_t meshletsTotal = 0u;
	std::uint32_t meshletsDispatched = 0u;
	std::uint32_t meshletsCulled = 0u;
	std::uint32_t meshletsCulledHiZ = 0u;
	std::uint32_t virtualGeometryClusterPackets = 0u;
	std::uint32_t virtualGeometrySubmittedBatches = 0u;
	std::uint32_t virtualGeometrySinglePacketFallbackBatches = 0u;
	std::uint32_t virtualGeometryCandidatePackets = 0u;
	std::uint32_t virtualGeometryCompactedPackets = 0u;
	std::uint32_t virtualGeometryRejectedPackets = 0u;
	std::uint32_t virtualGeometryFailOpenPackets = 0u;
	std::uint32_t virtualGeometryResidentPages = 0u;
	std::uint32_t virtualGeometryRequestedPages = 0u;
	std::uint32_t virtualGeometryMissingPages = 0u;
	std::uint32_t virtualGeometryFailedPages = 0u;
	std::uint32_t virtualGeometryEvictedPages = 0u;
	std::uint32_t virtualGeometryPinnedFallbackPages = 0u;
	// Page-source counters summarize which independently resident payloads a
	// completed view actually used. They are copied from immutable diagnostics
	// so lightweight editor surfaces do not walk live residency tables.
	std::uint32_t virtualGeometryResidentPagePayloads = 0u;
	std::uint32_t virtualGeometryHighDetailPagePayloads = 0u;
	std::uint32_t virtualGeometryFallbackPagePayloads = 0u;
	std::uint32_t virtualGeometryEmbeddedFallbackPagePayloads = 0u;
	std::uint32_t virtualGeometryEmittedHighDetailPages = 0u;
	std::uint32_t virtualGeometryEmittedFallbackPages = 0u;
	std::uint32_t virtualGeometryEmittedEmbeddedFallbackPages = 0u;
	std::uint32_t virtualGeometryMissingPageSources = 0u;
	std::uint32_t virtualGeometryPageSourceRequests = 0u;
	std::uint32_t virtualGeometryPageSourceRetirements = 0u;
	std::uint64_t virtualGeometryResidentPagePayloadBytes = 0u;
	std::uint64_t virtualGeometryFallbackPagePayloadBytes = 0u;
	std::uint64_t virtualGeometryEmbeddedFallbackBytes = 0u;
	// GPU packet diagnostics are copied from the immutable render graph
	// snapshot. Editor windows and automation read these fields without
	// touching live RenderSystem or Vulkan state.
	std::uint32_t virtualGeometryGpuPacketRecords = 0u;
	std::uint32_t virtualGeometryGpuBatchDescriptors = 0u;
	std::uint32_t virtualGeometryGpuPacketFrameSets = 0u;
	std::uint32_t virtualGeometryGpuValidFrameSets = 0u;
	std::uint32_t virtualGeometryGpuInvalidFrameSets = 0u;
	std::uint32_t virtualGeometryGpuCompactionDispatches = 0u;
	std::uint32_t virtualGeometryGeneratedSubmissions = 0u;
	std::uint32_t virtualGeometryValidGeneratedSubmissions = 0u;
	std::uint32_t virtualGeometryInvalidGeneratedSubmissions = 0u;
	std::uint32_t virtualGeometryFallbackGeneratedSubmissions = 0u;
	std::uint32_t virtualGeometryGpuCounterReadbacks = 0u;
	std::uint32_t virtualGeometryGpuCounterLatencyFrames = 0u;
	std::uint32_t virtualGeometryGpuFallbackRecords = 0u;
	std::uint32_t virtualGeometryGpuFallbackAffectedPackets = 0u;
	std::uint32_t virtualGeometryGpuFallbackAffectedBatches = 0u;
	std::string virtualGeometryGpuFallbackReason;
	// Route diagnostics are copied from the immutable frame route envelope. They
	// let editor/automation distinguish direct suppression from an unavailable
	// virtual-geometry path without reading renderer-owned mutable state.
	std::string geometryRoute;
	std::string geometryRouteSource;
	std::uint64_t geometryRoutePolicyIdentity = 0u;
	std::uint64_t geometryRouteCalibrationIdentity = 0u;
	std::uint32_t geometryRouteHysteresisFrames = 0u;
	std::uint32_t geometryRouteDirectBatchCount = 0u;
	std::uint32_t geometryRouteSharedInstanceCount = 0u;
	std::uint32_t geometryRouteSuppressedPageRequests = 0u;
	std::uint32_t geometryRouteSuppressedDecodeBlocks = 0u;
	std::uint32_t geometryRouteSuppressedResidencyRecords = 0u;
	std::string geometryRouteFallbackReason;
	// Performance gate summaries stay compact in the stats path. Full gate
	// records remain in RenderDiagnosticsSummary for profiler and automation
	// details, while these counts let lightweight readers detect regressions.
	std::uint32_t virtualGeometryPerformanceGateCount = 0u;
	std::uint32_t virtualGeometryPerformanceGateFailures = 0u;
	std::uint32_t virtualGeometryPerformanceGateUnsupported = 0u;
	std::string virtualGeometryFirstFailedPerformanceGate;
	bool visibleCommandCacheHit = false;
	bool visibleCommandConservativeRefresh = false;
	bool detailedRenderDiagnosticsRequested = false;
	std::uint32_t visibleCommandCount = 0u;
	std::uint32_t visibleCommandCopiedCount = 0u;
	std::uint32_t visibleStreamingUniqueMeshes = 0u;
	std::uint32_t visibleStreamingUniqueMaterials = 0u;
	std::uint32_t visibleStreamingUniqueTextures = 0u;
	std::uint32_t visibleStreamingDuplicateTouchesEliminated = 0u;
	std::string visibleCommandReuseFallbackReason;
	std::uint32_t hiZTests = 0u;
	std::uint32_t hiZProjectionFailed = 0u;
	std::uint32_t hiZFullRectCulled = 0u;
	std::uint32_t hiZRectCoverageCulled = 0u;
	std::uint32_t hiZCenterCulled = 0u;
	std::uint32_t hiZVertexCoverageCulled = 0u;
	std::uint32_t hiZNoOcclusionProof = 0u;
	VirtualGeometryCounterSource meshletCounterSource = VirtualGeometryCounterSource::Prepared;
	std::uint64_t meshletCounterFrameId = 0u;
	std::uint32_t meshletCounterLatencyFrames = 0u;
	std::string meshletCounterReason;
	// GPU timestamps are resolved only after the owning Vulkan frame-slot fence
	// completes. The renderer publishes the measured duration here so editor
	// profiler UI can show real GPU execution time instead of CPU submit cost.
	bool gpuTimestampValid = false;
	float gpuTimestampMs = 0.0f;
	std::uint64_t gpuTimestampFrameId = 0u;
	std::uint32_t gpuTimestampLatencyFrames = 0u;
	std::string gpuTimestampReason;
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
	VirtualGeometryHiZState hiZState = VirtualGeometryHiZState::MissingPyramid;
	std::string hiZStatusReason;
	std::uint32_t hiZWarmupFrames = 0u;
	bool hiZBaselineMatched = true;
	std::uint32_t hiZFalseOcclusionRiskWarnings = 0u;
	bool vrsActive = false;
	bool valid = false;
};

namespace TerraEngine::Renderer
{
	// A timing snapshot is copied from a delayed backend query only after it is
	// checked against the exact publication frame, view, and renderer generation.
	// Its optional duration keeps pending and failed queries from looking like zero.
	struct VirtualGeometryGpuTimingSnapshot
	{
		VirtualGeometryCapabilityStage stage = VirtualGeometryCapabilityStage::Traversal;
		VirtualGeometryFrameViewIdentity identity{};
		VirtualGeometryMeasurementAvailability availability = VirtualGeometryMeasurementAvailability::Unavailable;
		std::optional<std::uint64_t> durationNanoseconds;
		std::string queryIdentity;
		std::string executionEvidenceId;
		std::string reason;
		std::uint32_t latencyFrames = 0u;

		[[nodiscard]] bool HasAuthoritativeDuration() const noexcept
		{
			return availability == VirtualGeometryMeasurementAvailability::Measured
				&& durationNanoseconds.has_value() && !queryIdentity.empty()
				&& !executionEvidenceId.empty();
		}

		[[nodiscard]] bool IsGenerationSafeFor(const VirtualGeometryFrameViewIdentity& publication) const noexcept
		{
			return identity == publication;
		}
	};

	// Automation reads this immutable copy from RenderDiagnostics instead of
	// polling live RHI query pools. Raw measurements preserve availability and
	// reasons, while timingSnapshots contain only generation-safe numeric values.
	struct VirtualGeometryCapabilityDiagnosticsSnapshot
	{
		std::string schemaVersion = "1";
		VirtualGeometryFrameViewIdentity publicationIdentity{};
		std::vector<VirtualGeometryCapabilityEvidence> capabilities;
		std::vector<VirtualGeometryMeasurementRecord> measurements;
		std::vector<VirtualGeometryGpuTimingSnapshot> timingSnapshots;
	};
}
