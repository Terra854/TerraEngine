// RHIDefinitions.h — Rendering Hardware Interface Definitions
//
// Defines the cross-backend constants and enumerations used to abstract graphics hardware functionality.
// It provides a unified set of types for formats, usage flags, and resource states that are mapped to specific APIs like Vulkan.
// This ensures that the high-level renderer remains decoupled from the underlying hardware implementation.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

// RenderViewType is defined by RenderTypes.h. The RHI identity-readback
// contract needs only its fixed underlying representation here, which avoids a
// circular RenderTypes -> animation -> RHIContext include during compilation.
enum class RenderViewType : std::uint8_t;

enum class RHIBackend
{
	None,
	Vulkan
};

// struct RHIConfig groups related state and operations so callers have one documented contract.
struct RHIConfig
{
	RHIBackend backend = RHIBackend::Vulkan;
	// Validation is a runtime policy, not a compile-mode capability. Keep the
	// portable RHI default off so standalone builds do not require SDK layers,
	// while individual launch profiles can request validation in any build type.
	bool enableValidationLayers = false;
};

namespace TerraEngine::Renderer {

	// ConventionalVisibilityIdentityReadbackRequest names one Scene/Game target
	// pixel without exposing a backend image handle to editor or scripting code.
	// Dedicated captures are opt-in so they never replace visible editor targets.
	struct ConventionalVisibilityIdentityReadbackRequest
	{
		RenderViewType viewType = static_cast<RenderViewType>(1u);
		glm::uvec2 pixel = glm::uvec2(0u);
		std::uint64_t frameId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::uint64_t targetGeneration = 0u;
		std::uint64_t originGeneration = 0u;
		std::uint64_t spatialGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		bool dedicatedCaptureTarget = false;
	};

	// This result is separate from color capture. The uint4 value is
	// authoritative only after the backend's fence-owned copy completes.
	struct ConventionalVisibilityIdentityReadbackResult
	{
		RenderViewType viewType = static_cast<RenderViewType>(1u);
		glm::uvec2 pixel = glm::uvec2(0u);
		std::uint64_t frameId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::uint64_t targetGeneration = 0u;
		std::uint64_t originGeneration = 0u;
		std::uint64_t spatialGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		bool dedicatedCaptureTarget = false;
		glm::uvec4 identity = glm::uvec4(0u);
		bool completed = false;
		bool authoritative = false;
		std::string rejectionReason;
	};

	// RenderSystem snapshots this value before queueing an asynchronous identity
	// copy. The generation tuple is backend-neutral even though the current owner
	// is Vulkan, so completion can reject a resized/recreated target explicitly.
	struct ConventionalVisibilityIdentityTargetMetadata
	{
		RenderViewType viewType = static_cast<RenderViewType>(1u);
		std::uint64_t frameId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::uint64_t targetGeneration = 0u;
		std::uint64_t originGeneration = 0u;
		std::uint64_t spatialGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		bool dedicatedCaptureTarget = false;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return frameId != 0u && rendererGeneration != 0u
				&& targetGeneration != 0u && originGeneration != 0U
				&& spatialGeneration != 0U && residencyGeneration != 0U;
		}
	};

	enum class RenderBackendQueueMode
	{
		Parallel,
		Serial,
		Unavailable,
		Fallback
	};

	struct BackendQueueCapability
	{
		std::string category;
		RenderBackendQueueMode mode = RenderBackendQueueMode::Serial;
		bool handoffRequired = false;
		std::string fallbackReason;
	};

	enum class VirtualGeometryBackendStage : std::uint8_t
	{
		Traversal,
		Compaction,
		IndirectGeneration,
		PageDecode,
		TriangleSetup,
		HardwareRaster,
		ComputeRaster,
		VisibilityResolve,
		Shading,
		StreamingUpdate,
		Deformation,
		Shadow,
		Capture,
		RayTracing
	};

	enum class VirtualGeometryBackendRoute : std::uint8_t
	{
		None,
		ProcessorOracle,
		MeshShader,
		Compute,
		Indexed
	};

	enum class BackendMeasurementAvailability : std::uint8_t
	{
		Pending,
		Unavailable,
		Invalid,
		Measured
	};

	// Generated-work capabilities describe launch mechanics only. Execution
	// evidence is published separately after the backend has submitted and
	// observed the matching generation.
	struct VirtualGeometryGeneratedWorkCapability
	{
		bool supportsDeviceAuthoredWork = false;
		bool supportsIndirectCount = false;
		bool supportsBoundedNonCount = false;
		bool supportsZeroCommandSubmission = false;
		std::uint32_t maximumCommandCount = 0u;
		std::uint32_t commandAlignment = 0u;
		std::uint32_t commandStride = 0u;
	};

	struct VirtualGeometryBackendExecutionReceipt
	{
		VirtualGeometryBackendStage stage =
			VirtualGeometryBackendStage::Traversal;
		VirtualGeometryBackendRoute route =
			VirtualGeometryBackendRoute::None;
		std::uint64_t frameNumber = 0u;
		std::uint32_t viewId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::uint64_t geometryGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		std::uint64_t commandGeneration = 0u;
		std::uint64_t completionFence = 0u;
		bool commandRecorded = false;
		bool commandSubmitted = false;
		bool completionObserved = false;
		bool generationMatched = false;
		std::string evidenceId;
		std::string failureReason;

		[[nodiscard]] bool IsExecuted() const noexcept
		{
			return frameNumber != 0u
				&& rendererGeneration != 0u
				&& geometryGeneration != 0u
				&& commandGeneration != 0u
				&& completionFence != 0u
				&& commandRecorded
				&& commandSubmitted
				&& completionObserved
				&& generationMatched
				&& !evidenceId.empty();
		}
	};

	struct VirtualGeometryBackendMeasurement
	{
		VirtualGeometryBackendStage stage =
			VirtualGeometryBackendStage::Traversal;
		std::uint64_t frameNumber = 0u;
		std::uint32_t viewId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::uint64_t queryPoolGeneration = 0u;
		std::uint32_t firstQuery = 0u;
		std::uint32_t queryCount = 0u;
		std::uint64_t rawTicks = 0u;
		double durationMilliseconds = 0.0;
		BackendMeasurementAvailability availability =
			BackendMeasurementAvailability::Unavailable;
		std::string reason;

		[[nodiscard]] bool HasAuthoritativeValue() const noexcept
		{
			return availability == BackendMeasurementAvailability::Measured
				&& frameNumber != 0u
				&& rendererGeneration != 0u
				&& queryPoolGeneration != 0u
				&& queryCount != 0u;
		}
	};

	// BackendCapabilityProfile is the renderer-facing promise a graphics API
	// makes before it receives frame work. RenderSystem uses these values to
	// decide what can run on workers and which operations must stay serialized.
	struct BackendCapabilityProfile
	{
		std::string backendId = "none";
		bool supportsParallelRecording = false;
		bool supportsAsyncUploadPreparation = false;
		bool supportsTransientResources = false;
		bool supportsSecondaryRecording = false;
		bool supportsParallelShadowRecording = false;
		bool supportsParallelPerViewRecording = false;
		bool supportsParallelPipelineStatePreparation = false;
		bool supportsParallelDescriptorPreparation = false;
		bool supportsParallelUploadPlanning = false;
		bool supportsParallelTransitionPlanning = false;
		bool supportsParallelCulling = false;
		bool supportsParallelLightPreparation = false;
		bool supportsParallelUiPreparation = false;
		bool supportsParallelPacketValidation = false;
		bool supportsPacketLocalDiagnostics = false;
		bool supportsRenderTaskGroups = false;
		bool supportsFrameOverlap = false;
		bool supportsRenderWorkBudgets = false;
		bool supportsMultiQueueReadiness = false;
		bool supportsWorkerOwnedPreparation = false;
		bool supportsImageReadback = false;
		// Virtual geometry GPU packet support is split into explicit promises so
		// RenderSystem can fail open one stage without disabling the whole mesh
		// shader route. These are capability flags, not user-facing toggles.
		bool supportsVirtualGeometryGpuPacketBuffers = false;
		bool supportsVirtualGeometryGpuCompaction = false;
		bool supportsVirtualGeometryGeneratedSubmissions = false;
		bool supportsVirtualGeometryCounterReadback = false;
		bool supportsVirtualGeometryComputeRaster = false;
		bool supportsVirtualGeometryVisibilityResolve = false;
		bool supportsVirtualGeometryShadingBins = false;
		VirtualGeometryGeneratedWorkCapability virtualGeometryGeneratedWork{};
		std::uint32_t maxRecordingWorkers = 1u;
		std::uint32_t workerSchedulingThreshold = 128u;
		std::uint32_t maxPreparedFrames = 1u;
		std::vector<std::string> requiredSerialBoundaries;
		std::vector<std::string> diagnosticCapabilities;
		std::vector<BackendQueueCapability> queueCapabilities;
	};

}
