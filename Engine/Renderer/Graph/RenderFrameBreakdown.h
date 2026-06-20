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
		DiagnosticsPublication
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

	struct BackendFrameBreakdown
	{
		std::uint64_t frameId = 0u;
		std::vector<BackendFrameCostSample> samples;
		bool requested = false;
	};

	TERRAENGINE_API const char* ToString(BackendFrameCostCategory category);
	TERRAENGINE_API bool IsBackendWaitCategory(BackendFrameCostCategory category);
	TERRAENGINE_API void AddBackendFrameCost(
		BackendFrameBreakdown& breakdown,
		BackendFrameCostCategory category,
		RenderViewType viewType,
		float milliseconds,
		std::string label = {},
		bool shared = false);
	TERRAENGINE_API void MarkBackendFrameCostUnavailable(
		BackendFrameBreakdown& breakdown,
		BackendFrameCostCategory category,
		RenderViewType viewType,
		std::string label = {},
		bool shared = false);
	TERRAENGINE_API float SumBackendFrameCost(const BackendFrameBreakdown& breakdown, BackendFrameCostCategory category);
	TERRAENGINE_API float SumBackendWaitCost(const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API float SumBackendRecordingCost(const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API std::vector<BackendFrameCostSample> TopBackendFrameCosts(const BackendFrameBreakdown& breakdown, std::size_t maxCount);
	TERRAENGINE_API BackendFrameBreakdown BuildUnavailableBackendFrameBreakdown(const RenderFrameSnapshot& snapshot);

}
