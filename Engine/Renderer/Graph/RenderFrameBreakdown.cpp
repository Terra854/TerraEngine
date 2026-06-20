#include "RenderFrameBreakdown.h"

#include <algorithm>

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
		default: return "Unknown";
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

		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			MarkBackendFrameCostUnavailable(breakdown, BackendFrameCostCategory::ViewSubmission, view.viewType, "View Submission " + std::to_string(view.outputId));
		}
		return breakdown;
	}

}
