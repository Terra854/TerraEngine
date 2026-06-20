// RenderFrameBreakdown.h - opt-in backend frame timing categories.
//
// RenderSystem can be slow because of command recording, but it can also be
// slow because the backend is waiting, rebuilding targets, uploading resources,
// publishing stats, or doing readback. These structures keep those costs named
// separately so diagnostics can point at the real bottleneck.

#pragma once

#include "RenderFrameSnapshot.h"
#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	enum class BackendFrameCostCategory
	{
		RhiBeginFrame,
		ViewSubmission,
		RhiEndFrame,
		StatsPublication,
		FenceWait,
		QueueIdleWait,
		DeviceIdleWait,
		PresentWait,
		ResizeRecreateWait,
		TargetRecreation,
		ResourceCacheLookup,
		ResourceUpload,
		DescriptorUpdate,
		MaterialBinding,
		MeshBinding,
		TextureBinding,
		PipelineLookup,
		PipelineCreation,
		PrimaryCommandRecording,
		WorkerCommandRecording,
		QueueSubmission,
		Presentation,
		GpuReadback,
		DiagnosticsPublication,
		VisibleFrameWork,
		QueuedBackgroundWork
	};

	enum class BackendSubmitStage
	{
		ImageAcquire,
		FenceWait,
		CommandBufferReset,
		CommandBufferLifetime,
		FrameAllocatorReset,
		CommandRecording,
		DescriptorUpdate,
		ResourceTransition,
		QueueSubmit,
		Present,
		SwapchainRecovery,
		ForcedIdle,
		ViewSubmit,
		Other
	};

	enum class BackendStallType
	{
		Acquire,
		Fence,
		QueueSubmit,
		Present,
		CommandBufferLifetime,
		Allocator,
		SwapchainRecovery,
		DeviceIdle,
		QueueIdle,
		PlatformThrottle,
		Unknown
	};

	enum class BackendStallSeverity
	{
		Info,
		Warning,
		Error
	};

	enum class ViewSubmitReuseDecision
	{
		Unknown,
		Reused,
		Separate,
		Unsafe,
		Unavailable
	};

	struct BackendFrameCostSample
	{
		BackendFrameCostCategory category = BackendFrameCostCategory::RhiBeginFrame;
		RenderViewType viewType = RenderViewType::Game;
		std::string label;
		float milliseconds = 0.0f;
		bool wait = false;
		bool shared = false;
		bool unavailable = false;
	};

	struct BackendStageTiming
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		RenderViewType viewType = RenderViewType::Game;
		BackendSubmitStage stage = BackendSubmitStage::Other;
		float milliseconds = 0.0f;
		std::uint32_t threadId = 0u;
		std::string backendId;
		std::string label;
		bool viewScoped = false;
	};

	struct BackendStallDiagnostic
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		BackendStallType waitType = BackendStallType::Unknown;
		BackendSubmitStage owningStage = BackendSubmitStage::Other;
		float milliseconds = 0.0f;
		float thresholdMilliseconds = 0.25f;
		BackendStallSeverity severity = BackendStallSeverity::Info;
		std::string recommendedAction;
		bool viewScoped = false;
	};

	struct ViewSubmitWorkItem
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		RenderViewType viewType = RenderViewType::Game;
		std::uint64_t cameraId = 0u;
		std::uint64_t targetId = 0u;
		std::uint64_t passSetHash = 0u;
		std::uint64_t overlayStateHash = 0u;
		float recordingMilliseconds = 0.0f;
		float submitMilliseconds = 0.0f;
		std::string sharedWorkKey;
		ViewSubmitReuseDecision reuseDecision = ViewSubmitReuseDecision::Unknown;
		std::string reuseReason;
	};

	struct BackendFrameBreakdown
	{
		std::uint64_t frameId = 0u;
		std::vector<BackendFrameCostSample> samples;
		std::vector<BackendStageTiming> stageTimings;
		std::vector<BackendStallDiagnostic> stallDiagnostics;
		std::vector<ViewSubmitWorkItem> viewSubmitWork;
		bool requested = false;
	};

	TERRAENGINE_API const char* ToString(BackendFrameCostCategory category);
	TERRAENGINE_API const char* ToString(BackendSubmitStage stage);
	TERRAENGINE_API const char* ToString(BackendStallType type);
	TERRAENGINE_API const char* ToString(BackendStallSeverity severity);
	TERRAENGINE_API const char* ToString(ViewSubmitReuseDecision decision);
	TERRAENGINE_API bool IsBackendWaitCategory(BackendFrameCostCategory category);
	TERRAENGINE_API bool IsBackendWaitStage(BackendSubmitStage stage);
	TERRAENGINE_API BackendStallType StallTypeForBackendStage(BackendSubmitStage stage);
	TERRAENGINE_API void AddBackendFrameCost(
		BackendFrameBreakdown& breakdown,
		BackendFrameCostCategory category,
		RenderViewType viewType,
		float milliseconds,
		std::string label = {},
		bool shared = false);
	TERRAENGINE_API void AddBackendStageTiming(
		BackendFrameBreakdown& breakdown,
		BackendSubmitStage stage,
		RenderViewType viewType,
		float milliseconds,
		std::uint32_t viewId = 0u,
		std::string label = {},
		bool viewScoped = false,
		std::string backendId = "vulkan");
	TERRAENGINE_API BackendStallDiagnostic BuildBackendStallDiagnostic(
		const BackendStageTiming& timing,
		float thresholdMilliseconds = 0.25f,
		std::string recommendedAction = {});
	TERRAENGINE_API void AddBackendStallDiagnostic(
		BackendFrameBreakdown& breakdown,
		const BackendStallDiagnostic& diagnostic);
	TERRAENGINE_API std::vector<BackendStallDiagnostic> CollectBackendStallDiagnostics(
		const BackendFrameBreakdown& breakdown,
		float thresholdMilliseconds = 0.25f);
	TERRAENGINE_API void AddViewSubmitWorkItem(
		BackendFrameBreakdown& breakdown,
		ViewSubmitWorkItem item);
	TERRAENGINE_API ViewSubmitWorkItem BuildViewSubmitWorkItem(
		std::uint64_t frameId,
		std::uint32_t viewId,
		RenderViewType viewType,
		std::uint64_t passSetHash,
		std::uint64_t overlayStateHash,
		float recordingMilliseconds,
		float submitMilliseconds,
		std::string sharedWorkKey = {});
	TERRAENGINE_API ViewSubmitReuseDecision CompareViewSubmitWork(
		const ViewSubmitWorkItem& source,
		const ViewSubmitWorkItem& candidate,
		std::string* reason = nullptr);
	TERRAENGINE_API void MarkBackendFrameCostUnavailable(
		BackendFrameBreakdown& breakdown,
		BackendFrameCostCategory category,
		RenderViewType viewType,
		std::string label = {},
		bool shared = false);
	TERRAENGINE_API float SumBackendFrameCost(const BackendFrameBreakdown& breakdown, BackendFrameCostCategory category);
	TERRAENGINE_API float SumBackendWaitCost(const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API float SumBackendRecordingCost(const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API float SumVisibleBackendFrameCost(const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API float SumQueuedBackendFrameCost(const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API float SumBackendStageDuration(const BackendFrameBreakdown& breakdown, BackendSubmitStage stage);
	TERRAENGINE_API float CalculateBackendStageAttributionPercent(const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API std::vector<BackendFrameCostSample> TopBackendFrameCosts(const BackendFrameBreakdown& breakdown, std::size_t maxCount);
	TERRAENGINE_API BackendFrameBreakdown BuildUnavailableBackendFrameBreakdown(const RenderFrameSnapshot& snapshot);

}
