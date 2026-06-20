#include "RenderFrameBreakdown.h"

#include <algorithm>
#include <functional>
#include <thread>

namespace TerraEngine::Renderer {

	const char* ToString(BackendFrameCostCategory category)
	{
		switch (category)
		{
		case BackendFrameCostCategory::RhiBeginFrame: return "RHI Begin Frame";
		case BackendFrameCostCategory::ViewSubmission: return "View Submission";
		case BackendFrameCostCategory::RhiEndFrame: return "RHI End Frame";
		case BackendFrameCostCategory::StatsPublication: return "Stats Publication";
		case BackendFrameCostCategory::FenceWait: return "Fence Wait";
		case BackendFrameCostCategory::QueueIdleWait: return "Queue Idle Wait";
		case BackendFrameCostCategory::DeviceIdleWait: return "Device Idle Wait";
		case BackendFrameCostCategory::PresentWait: return "Present Wait";
		case BackendFrameCostCategory::ResizeRecreateWait: return "Resize/Recreate Wait";
		case BackendFrameCostCategory::TargetRecreation: return "Target Recreation";
		case BackendFrameCostCategory::ResourceCacheLookup: return "Resource Cache Lookup";
		case BackendFrameCostCategory::ResourceUpload: return "Resource Upload";
		case BackendFrameCostCategory::DescriptorUpdate: return "Descriptor Update";
		case BackendFrameCostCategory::MaterialBinding: return "Material Binding";
		case BackendFrameCostCategory::MeshBinding: return "Mesh Binding";
		case BackendFrameCostCategory::TextureBinding: return "Texture Binding";
		case BackendFrameCostCategory::PipelineLookup: return "Pipeline Lookup";
		case BackendFrameCostCategory::PipelineCreation: return "Pipeline Creation";
		case BackendFrameCostCategory::PrimaryCommandRecording: return "Primary Command Recording";
		case BackendFrameCostCategory::WorkerCommandRecording: return "Worker Command Recording";
		case BackendFrameCostCategory::QueueSubmission: return "Queue Submission";
		case BackendFrameCostCategory::Presentation: return "Presentation";
		case BackendFrameCostCategory::GpuReadback: return "GPU Readback";
		case BackendFrameCostCategory::DiagnosticsPublication: return "Diagnostics Publication";
		case BackendFrameCostCategory::VisibleFrameWork: return "Visible Frame Work";
		case BackendFrameCostCategory::QueuedBackgroundWork: return "Queued Background Work";
		default: return "Unknown";
		}
	}

	const char* ToString(BackendSubmitStage stage)
	{
		switch (stage)
		{
		case BackendSubmitStage::ImageAcquire: return "Image Acquire";
		case BackendSubmitStage::FenceWait: return "Fence Wait";
		case BackendSubmitStage::CommandBufferReset: return "Command Buffer Reset";
		case BackendSubmitStage::CommandBufferLifetime: return "Command Buffer Lifetime";
		case BackendSubmitStage::FrameAllocatorReset: return "Frame Allocator Reset";
		case BackendSubmitStage::CommandRecording: return "Command Recording";
		case BackendSubmitStage::DescriptorUpdate: return "Descriptor Update";
		case BackendSubmitStage::ResourceTransition: return "Resource Transition";
		case BackendSubmitStage::QueueSubmit: return "Queue Submit";
		case BackendSubmitStage::Present: return "Present";
		case BackendSubmitStage::SwapchainRecovery: return "Swapchain Recovery";
		case BackendSubmitStage::ForcedIdle: return "Forced Idle";
		case BackendSubmitStage::ViewSubmit: return "View Submit";
		default: return "Other";
		}
	}

	const char* ToString(BackendStallType type)
	{
		switch (type)
		{
		case BackendStallType::Acquire: return "acquire";
		case BackendStallType::Fence: return "fence";
		case BackendStallType::QueueSubmit: return "queue-submit";
		case BackendStallType::Present: return "present";
		case BackendStallType::CommandBufferLifetime: return "command-buffer-lifetime";
		case BackendStallType::Allocator: return "allocator";
		case BackendStallType::SwapchainRecovery: return "swapchain-recovery";
		case BackendStallType::DeviceIdle: return "device-idle";
		case BackendStallType::QueueIdle: return "queue-idle";
		case BackendStallType::PlatformThrottle: return "platform-throttle";
		default: return "unknown";
		}
	}

	const char* ToString(BackendStallSeverity severity)
	{
		switch (severity)
		{
		case BackendStallSeverity::Warning: return "warning";
		case BackendStallSeverity::Error: return "error";
		default: return "info";
		}
	}

	const char* ToString(ViewSubmitReuseDecision decision)
	{
		switch (decision)
		{
		case ViewSubmitReuseDecision::Reused: return "reused";
		case ViewSubmitReuseDecision::Separate: return "separate";
		case ViewSubmitReuseDecision::Unsafe: return "unsafe";
		case ViewSubmitReuseDecision::Unavailable: return "unavailable";
		default: return "unknown";
		}
	}

	bool IsBackendWaitCategory(BackendFrameCostCategory category)
	{
		return category == BackendFrameCostCategory::FenceWait
			|| category == BackendFrameCostCategory::QueueIdleWait
			|| category == BackendFrameCostCategory::DeviceIdleWait
			|| category == BackendFrameCostCategory::PresentWait
			|| category == BackendFrameCostCategory::ResizeRecreateWait;
	}

	bool IsBackendWaitStage(BackendSubmitStage stage)
	{
		return stage == BackendSubmitStage::ImageAcquire
			|| stage == BackendSubmitStage::FenceWait
			|| stage == BackendSubmitStage::CommandBufferLifetime
			|| stage == BackendSubmitStage::QueueSubmit
			|| stage == BackendSubmitStage::Present
			|| stage == BackendSubmitStage::SwapchainRecovery
			|| stage == BackendSubmitStage::ForcedIdle;
	}

	BackendStallType StallTypeForBackendStage(BackendSubmitStage stage)
	{
		switch (stage)
		{
		case BackendSubmitStage::ImageAcquire: return BackendStallType::Acquire;
		case BackendSubmitStage::FenceWait: return BackendStallType::Fence;
		case BackendSubmitStage::CommandBufferLifetime: return BackendStallType::CommandBufferLifetime;
		case BackendSubmitStage::FrameAllocatorReset: return BackendStallType::Allocator;
		case BackendSubmitStage::QueueSubmit: return BackendStallType::QueueSubmit;
		case BackendSubmitStage::Present: return BackendStallType::Present;
		case BackendSubmitStage::SwapchainRecovery: return BackendStallType::SwapchainRecovery;
		case BackendSubmitStage::ForcedIdle: return BackendStallType::DeviceIdle;
		default: return BackendStallType::Unknown;
		}
	}

	void AddBackendFrameCost(
		BackendFrameBreakdown& breakdown,
		BackendFrameCostCategory category,
		RenderViewType viewType,
		float milliseconds,
		std::string label,
		bool shared)
	{
		BackendFrameCostSample sample;
		sample.category = category;
		sample.viewType = viewType;
		sample.label = label.empty() ? ToString(category) : std::move(label);
		sample.milliseconds = std::max(0.0f, milliseconds);
		sample.wait = IsBackendWaitCategory(category);
		sample.shared = shared;
		breakdown.samples.push_back(std::move(sample));
	}

	void AddBackendStageTiming(
		BackendFrameBreakdown& breakdown,
		BackendSubmitStage stage,
		RenderViewType viewType,
		float milliseconds,
		std::uint32_t viewId,
		std::string label,
		bool viewScoped,
		std::string backendId)
	{
		BackendStageTiming timing;
		timing.frameId = breakdown.frameId;
		timing.viewId = viewId;
		timing.viewType = viewType;
		timing.stage = stage;
		timing.milliseconds = std::max(0.0f, milliseconds);
		timing.threadId = static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
		timing.backendId = std::move(backendId);
		timing.label = label.empty() ? ToString(stage) : std::move(label);
		timing.viewScoped = viewScoped;
		breakdown.stageTimings.push_back(std::move(timing));
	}

	BackendStallDiagnostic BuildBackendStallDiagnostic(
		const BackendStageTiming& timing,
		float thresholdMilliseconds,
		std::string recommendedAction)
	{
		BackendStallDiagnostic diagnostic;
		diagnostic.frameId = timing.frameId;
		diagnostic.viewId = timing.viewId;
		diagnostic.waitType = StallTypeForBackendStage(timing.stage);
		diagnostic.owningStage = timing.stage;
		diagnostic.milliseconds = timing.milliseconds;
		diagnostic.thresholdMilliseconds = std::max(0.0f, thresholdMilliseconds);
		diagnostic.viewScoped = timing.viewScoped;
		if (timing.milliseconds >= diagnostic.thresholdMilliseconds * 4.0f)
		{
			diagnostic.severity = BackendStallSeverity::Error;
		}
		else if (timing.milliseconds >= diagnostic.thresholdMilliseconds)
		{
			diagnostic.severity = BackendStallSeverity::Warning;
		}
		if (!recommendedAction.empty())
		{
			diagnostic.recommendedAction = std::move(recommendedAction);
		}
		else if (timing.stage == BackendSubmitStage::ForcedIdle)
		{
			diagnostic.recommendedAction = "Remove or defer the forced idle unless recovery correctness requires it.";
		}
		else if (timing.stage == BackendSubmitStage::FenceWait)
		{
			diagnostic.recommendedAction = "Check frame overlap and command-buffer lifetime before reusing this frame slot.";
		}
		else if (timing.stage == BackendSubmitStage::ImageAcquire || timing.stage == BackendSubmitStage::Present)
		{
			diagnostic.recommendedAction = "Check swapchain throttling, resize, or platform presentation behavior.";
		}
		else
		{
			diagnostic.recommendedAction = "Inspect backend synchronization for this stage.";
		}
		return diagnostic;
	}

	void AddBackendStallDiagnostic(
		BackendFrameBreakdown& breakdown,
		const BackendStallDiagnostic& diagnostic)
	{
		breakdown.stallDiagnostics.push_back(diagnostic);
	}

	std::vector<BackendStallDiagnostic> CollectBackendStallDiagnostics(
		const BackendFrameBreakdown& breakdown,
		float thresholdMilliseconds)
	{
		std::vector<BackendStallDiagnostic> diagnostics = breakdown.stallDiagnostics;
		for (const BackendStageTiming& timing : breakdown.stageTimings)
		{
			if (IsBackendWaitStage(timing.stage) && timing.milliseconds >= thresholdMilliseconds)
			{
				const bool alreadyRecorded = std::any_of(diagnostics.begin(), diagnostics.end(), [&timing](const BackendStallDiagnostic& diagnostic) {
					return diagnostic.frameId == timing.frameId
						&& diagnostic.viewId == timing.viewId
						&& diagnostic.owningStage == timing.stage
						&& diagnostic.waitType == StallTypeForBackendStage(timing.stage);
				});
				if (alreadyRecorded)
				{
					continue;
				}
				diagnostics.push_back(BuildBackendStallDiagnostic(timing, thresholdMilliseconds));
			}
		}
		return diagnostics;
	}

	void AddViewSubmitWorkItem(
		BackendFrameBreakdown& breakdown,
		ViewSubmitWorkItem item)
	{
		if (item.frameId == 0u)
		{
			item.frameId = breakdown.frameId;
		}
		breakdown.viewSubmitWork.push_back(std::move(item));
	}

	ViewSubmitWorkItem BuildViewSubmitWorkItem(
		std::uint64_t frameId,
		std::uint32_t viewId,
		RenderViewType viewType,
		std::uint64_t passSetHash,
		std::uint64_t overlayStateHash,
		float recordingMilliseconds,
		float submitMilliseconds,
		std::string sharedWorkKey)
	{
		ViewSubmitWorkItem item;
		item.frameId = frameId;
		item.viewId = viewId;
		item.viewType = viewType;
		item.passSetHash = passSetHash;
		item.overlayStateHash = overlayStateHash;
		item.recordingMilliseconds = std::max(0.0f, recordingMilliseconds);
		item.submitMilliseconds = std::max(0.0f, submitMilliseconds);
		item.sharedWorkKey = std::move(sharedWorkKey);
		item.reuseDecision = item.sharedWorkKey.empty() ? ViewSubmitReuseDecision::Unavailable : ViewSubmitReuseDecision::Unknown;
		item.reuseReason = item.sharedWorkKey.empty() ? "No shared backend work key was provided." : "Reuse has not been evaluated.";
		return item;
	}

	ViewSubmitReuseDecision CompareViewSubmitWork(
		const ViewSubmitWorkItem& source,
		const ViewSubmitWorkItem& candidate,
		std::string* reason)
	{
		if (source.sharedWorkKey.empty() || candidate.sharedWorkKey.empty())
		{
			if (reason)
			{
				*reason = "One view has no shared backend work key.";
			}
			return ViewSubmitReuseDecision::Unavailable;
		}
		if (source.sharedWorkKey != candidate.sharedWorkKey)
		{
			if (reason)
			{
				*reason = "Shared work keys differ.";
			}
			return ViewSubmitReuseDecision::Separate;
		}
		if (source.passSetHash != candidate.passSetHash)
		{
			if (reason)
			{
				*reason = "Pass sets differ.";
			}
			return ViewSubmitReuseDecision::Unsafe;
		}
		if (source.overlayStateHash != candidate.overlayStateHash)
		{
			if (reason)
			{
				*reason = "Editor overlay state differs.";
			}
			return ViewSubmitReuseDecision::Unsafe;
		}
		if (reason)
		{
			*reason = "Views use compatible backend-ready work.";
		}
		return ViewSubmitReuseDecision::Reused;
	}

	void MarkBackendFrameCostUnavailable(
		BackendFrameBreakdown& breakdown,
		BackendFrameCostCategory category,
		RenderViewType viewType,
		std::string label,
		bool shared)
	{
		BackendFrameCostSample sample;
		sample.category = category;
		sample.viewType = viewType;
		sample.label = label.empty() ? ToString(category) : std::move(label);
		sample.wait = IsBackendWaitCategory(category);
		sample.shared = shared;
		sample.unavailable = true;
		breakdown.samples.push_back(std::move(sample));
	}

	float SumBackendFrameCost(const BackendFrameBreakdown& breakdown, BackendFrameCostCategory category)
	{
		float total = 0.0f;
		for (const BackendFrameCostSample& sample : breakdown.samples)
		{
			if (sample.category == category && !sample.unavailable)
			{
				total += sample.milliseconds;
			}
		}
		return total;
	}

	float SumBackendWaitCost(const BackendFrameBreakdown& breakdown)
	{
		float total = 0.0f;
		for (const BackendFrameCostSample& sample : breakdown.samples)
		{
			if (sample.wait && !sample.unavailable)
			{
				total += sample.milliseconds;
			}
		}
		return total;
	}

	float SumBackendRecordingCost(const BackendFrameBreakdown& breakdown)
	{
		return SumBackendFrameCost(breakdown, BackendFrameCostCategory::PrimaryCommandRecording)
			+ SumBackendFrameCost(breakdown, BackendFrameCostCategory::WorkerCommandRecording);
	}

	float SumVisibleBackendFrameCost(const BackendFrameBreakdown& breakdown)
	{
		float total = 0.0f;
		for (const BackendFrameCostSample& sample : breakdown.samples)
		{
			if (!sample.unavailable && sample.category != BackendFrameCostCategory::QueuedBackgroundWork)
			{
				total += sample.milliseconds;
			}
		}
		return total;
	}

	float SumQueuedBackendFrameCost(const BackendFrameBreakdown& breakdown)
	{
		return SumBackendFrameCost(breakdown, BackendFrameCostCategory::QueuedBackgroundWork);
	}

	float SumBackendStageDuration(const BackendFrameBreakdown& breakdown, BackendSubmitStage stage)
	{
		float total = 0.0f;
		for (const BackendStageTiming& timing : breakdown.stageTimings)
		{
			if (timing.stage == stage)
			{
				total += timing.milliseconds;
			}
		}
		return total;
	}

	float CalculateBackendStageAttributionPercent(const BackendFrameBreakdown& breakdown)
	{
		float broadTotal = 0.0f;
		broadTotal += SumBackendFrameCost(breakdown, BackendFrameCostCategory::RhiBeginFrame);
		broadTotal += SumBackendFrameCost(breakdown, BackendFrameCostCategory::ViewSubmission);
		broadTotal += SumBackendFrameCost(breakdown, BackendFrameCostCategory::RhiEndFrame);
		float stageTotal = 0.0f;
		for (const BackendStageTiming& timing : breakdown.stageTimings)
		{
			stageTotal += timing.milliseconds;
		}
		if (broadTotal <= 0.0f)
		{
			return stageTotal > 0.0f ? 100.0f : 0.0f;
		}
		return std::clamp((stageTotal / broadTotal) * 100.0f, 0.0f, 100.0f);
	}

	std::vector<BackendFrameCostSample> TopBackendFrameCosts(const BackendFrameBreakdown& breakdown, std::size_t maxCount)
	{
		std::vector<BackendFrameCostSample> samples = breakdown.samples;
		samples.erase(std::remove_if(samples.begin(), samples.end(), [](const BackendFrameCostSample& sample) {
			return sample.unavailable;
		}), samples.end());
		std::stable_sort(samples.begin(), samples.end(), [](const BackendFrameCostSample& left, const BackendFrameCostSample& right) {
			return left.milliseconds > right.milliseconds;
		});
		if (samples.size() > maxCount)
		{
			samples.resize(maxCount);
		}
		return samples;
	}

	BackendFrameBreakdown BuildUnavailableBackendFrameBreakdown(const RenderFrameSnapshot& snapshot)
	{
		BackendFrameBreakdown breakdown;
		breakdown.frameId = snapshot.frameId;
		breakdown.requested = true;

		// The fallback breakdown gives the diagnostics window a complete shape
		// before backend probes are available. Each section is marked
		// unavailable rather than folded into a misleading "other" bucket.
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::RhiBeginFrame, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::RhiEndFrame, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::StatsPublication, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::FenceWait, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::TargetRecreation, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::ResourceUpload, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::DescriptorUpdate, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::PipelineCreation, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::PrimaryCommandRecording, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::WorkerCommandRecording, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::QueueSubmission, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::Presentation, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::GpuReadback, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::DiagnosticsPublication, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::VisibleFrameWork, RenderViewType::Game, {}, true);
		MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::QueuedBackgroundWork, RenderViewType::Game, {}, true);
		AddBackendStageTiming(breakdown, BackendSubmitStage::Other, RenderViewType::Game, 0.0f, 0u, "Backend stage timing unavailable", false);

		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::ViewSubmission, view.viewType, "View Submission " + std::to_string(view.outputId));
		}
		return breakdown;
	}

}
