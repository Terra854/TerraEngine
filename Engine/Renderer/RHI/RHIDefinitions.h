// RHIDefinitions.h — Rendering Hardware Interface Definitions
//
// Defines the cross-backend constants and enumerations used to abstract graphics hardware functionality.
// It provides a unified set of types for formats, usage flags, and resource states that are mapped to specific APIs like Vulkan.
// This ensures that the high-level renderer remains decoupled from the underlying hardware implementation.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
		std::uint32_t maxRecordingWorkers = 1u;
		std::uint32_t workerSchedulingThreshold = 128u;
		std::uint32_t maxPreparedFrames = 1u;
		std::vector<std::string> requiredSerialBoundaries;
		std::vector<std::string> diagnosticCapabilities;
		std::vector<BackendQueueCapability> queueCapabilities;
	};

}
