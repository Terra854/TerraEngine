// ProfilerWindow.cpp — Profiler Window Implementation
//
// Implements the Profiler Window for the TerraEngine editor, providing a visual interface for performance monitoring.
// It retrieves data from the EditorProfilerService and the engine's core Profiler to display CPU and GPU metrics via ImGui.
// This window allows developers to analyze frame times, task execution, and resource utilization in real-time.

#include "ProfilerWindow.h"

#include "../Core/EditorContext.h"
#include "../Profiling/EditorProfilerService.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using TerraEngine::Profiling::Profiler;
using TerraEngine::Profiling::ProfilerCaptureSettings;
using TerraEngine::Profiling::ProfilerFrameListItem;
using TerraEngine::Profiling::ProfilerFrameRecord;
using TerraEngine::Profiling::ProfilerModuleId;
using TerraEngine::Profiling::ProfilerSample;
using TerraEngine::Profiling::ProfilerSessionState;
using TerraEngine::Profiling::ProfilerTimingStreamAggregate;
using TerraEngine::Profiling::ProfilerTimingStreamStatus;

namespace
{
// Profiler presentation is intentionally refreshed below the render rate. A
// ten-Hz UI still feels live while keeping deep capture and renderer snapshots
// off the producer-critical path.
constexpr auto kProfilerWindowRefreshInterval = std::chrono::milliseconds(100);
// struct SampleTreeNode groups related state and operations so callers have one documented contract.
struct SampleTreeNode
{
	std::size_t sampleIndex = 0u;
	std::vector<std::size_t> childIndices;
	double selfDurationMs = 0.0;
};

// struct SampleTree groups related state and operations so callers have one documented contract.
struct SampleTree
{
	std::vector<ProfilerSample> samples;
	std::vector<SampleTreeNode> nodes;
	std::vector<std::size_t> rootIndices;
};

// struct HotSampleRow groups the columns needed for the sorted "what cost the frame" table.
struct HotSampleRow
{
	std::string name;
	std::string threadLabel;
	std::string taskLabel;
	double inclusiveMs = 0.0;
	double selfMs = 0.0;
	std::uint32_t depth = 0u;
};

// struct FrameDurationStats keeps capture-level timing answers close to the UI
// code that presents them, without changing the profiler capture file format.
struct FrameDurationStats
{
	std::size_t frameCount = 0u;
	std::size_t partialFrameCount = 0u;
	double latestMs = 0.0;
	double minMs = 0.0;
	double maxMs = 0.0;
	double meanMs = 0.0;
	double medianMs = 0.0;
	double p90Ms = 0.0;
	double p95Ms = 0.0;
	double p99Ms = 0.0;
	double stdDevMs = 0.0;
};

// FindModuleSummary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const TerraEngine::Profiling::ProfilerModuleSummary* FindModuleSummary(const ProfilerFrameRecord& frame, ProfilerModuleId moduleId)
{
	const auto found = std::find_if(frame.moduleSummaries.begin(), frame.moduleSummaries.end(), [moduleId](const auto& module) {
		return module.moduleId == moduleId;
	});
	return found == frame.moduleSummaries.end() ? nullptr : &(*found);
}

// Frames are easiest to compare when the color tells whether the frame is
// comfortably below an editor budget, getting expensive, or already over a
// common 60 FPS frame budget.
ImVec4 FrameDurationColor(double durationMs)
{
	if (durationMs >= 16.67)
	{
		return ImVec4(1.0f, 0.35f, 0.25f, 1.0f);
	}
	if (durationMs >= 8.33)
	{
		return ImVec4(1.0f, 0.80f, 0.25f, 1.0f);
	}
	return ImVec4(0.45f, 0.90f, 0.55f, 1.0f);
}

double SafePercent(double value, double total)
{
	if (total <= 0.0 || !std::isfinite(total) || !std::isfinite(value))
	{
		return 0.0;
	}
	return (value / total) * 100.0;
}

// DrawTimeBar renders a compact horizontal bar next to a numeric timing so
// spikes are visible without mentally comparing every row in the table.
void DrawTimeBar(double valueMs, double basisMs)
{
	float fraction = 0.0f;
	if (basisMs > 0.0 && std::isfinite(valueMs) && std::isfinite(basisMs))
	{
		fraction = static_cast<float>(std::clamp(valueMs / basisMs, 0.0, 1.0));
	}
	ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), "");
}

double PercentileFromSorted(const std::vector<double>& sortedValues, double percentile)
{
	if (sortedValues.empty())
	{
		return 0.0;
	}
	if (sortedValues.size() == 1u)
	{
		return sortedValues.front();
	}
	const double clampedPercentile = std::clamp(percentile, 0.0, 1.0);
	const double scaledIndex = clampedPercentile * static_cast<double>(sortedValues.size() - 1u);
	const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(scaledIndex));
	const std::size_t upperIndex = std::min(lowerIndex + 1u, sortedValues.size() - 1u);
	const double blend = scaledIndex - static_cast<double>(lowerIndex);
	return sortedValues[lowerIndex] + (sortedValues[upperIndex] - sortedValues[lowerIndex]) * blend;
}

FrameDurationStats BuildFrameDurationStats(const std::vector<ProfilerFrameListItem>& frames)
{
	FrameDurationStats stats;
	std::vector<double> values;
	values.reserve(frames.size());
	for (const ProfilerFrameListItem& frame : frames)
	{
		if (!frame.complete)
		{
			++stats.partialFrameCount;
			// A partial frame ends when capture lifecycle work interrupts the frame,
			// so its elapsed duration is not comparable to a naturally completed
			// frame. Keep it visible as recovery evidence without allowing it to
			// inflate the live mean or tail percentiles.
			continue;
		}
		if (std::isfinite(frame.durationMs) && frame.durationMs >= 0.0)
		{
			values.push_back(frame.durationMs);
			stats.latestMs = frame.durationMs;
		}
	}
	stats.frameCount = values.size();
	if (values.empty())
	{
		return stats;
	}
	const double sum = std::accumulate(values.begin(), values.end(), 0.0);
	stats.meanMs = sum / static_cast<double>(values.size());
	double varianceSum = 0.0;
	for (double value : values)
	{
		const double delta = value - stats.meanMs;
		varianceSum += delta * delta;
	}
	stats.stdDevMs = std::sqrt(varianceSum / static_cast<double>(values.size()));
	std::sort(values.begin(), values.end());
	stats.minMs = values.front();
	stats.maxMs = values.back();
	stats.medianMs = PercentileFromSorted(values, 0.50);
	stats.p90Ms = PercentileFromSorted(values, 0.90);
	stats.p95Ms = PercentileFromSorted(values, 0.95);
	stats.p99Ms = PercentileFromSorted(values, 0.99);
	return stats;
}

void DrawMetricCell(const char* label, double valueMs, const ImVec4& valueColor)
{
	ImGui::TableNextColumn();
	ImGui::TextDisabled("%s", label);
	ImGui::TextColored(valueColor, "%.3f ms", valueMs);
}

void DrawDisabledReasonTooltip(const char* reason)
{
	if (reason != nullptr && reason[0] != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
	{
		ImGui::SetTooltip("%s", reason);
	}
}

bool ToolbarButton(const char* label, bool enabled, const char* disabledReason)
{
	if (!enabled)
	{
		ImGui::BeginDisabled();
	}
	const bool pressed = ImGui::Button(label);
	if (!enabled)
	{
		ImGui::EndDisabled();
		DrawDisabledReasonTooltip(disabledReason);
	}
	return enabled && pressed;
}

void DrawToolbarGroupLabel(const char* label)
{
	ImGui::TextDisabled("%s", label);
	ImGui::SameLine();
}

const char* TimingDisplayValue(const ProfilerTimingStreamAggregate& aggregate, char* buffer, std::size_t bufferSize)
{
	if (aggregate.selectedStatus == ProfilerTimingStreamStatus::Valid)
	{
		std::snprintf(buffer, bufferSize, "%.3f ms", aggregate.selectedMilliseconds);
		return buffer;
	}
	return TerraEngine::Profiling::ToString(aggregate.selectedStatus);
}

const char* TimingAggregateValue(const ProfilerTimingStreamAggregate& aggregate, double value, char* buffer, std::size_t bufferSize)
{
	// Aggregate columns are only meaningful when at least one valid sample
	// contributed to the stream. Unavailable GPU timing must not look like a
	// measured zero-millisecond result.
	if (aggregate.sampleCount == 0u)
	{
		return "n/a";
	}
	std::snprintf(buffer, bufferSize, "%.3f", value);
	return buffer;
}

// DrawEvidenceMetrics renders unit-tagged capture summaries.  Each row carries
// its availability so absent optional streams stay distinguishable from a
// valid zero-duration stage.
void DrawEvidenceMetrics(const char* tableId, const std::vector<EditorProfilerMetricSummary>& metrics)
{
	if (metrics.empty())
	{
		ImGui::TextDisabled("No stream was recorded at this capture level.");
		return;
	}
	if (!ImGui::BeginTable(tableId, 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		return;
	}
	ImGui::TableSetupColumn("Stream", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("Median");
	ImGui::TableSetupColumn("P95");
	ImGui::TableSetupColumn("P99");
	ImGui::TableSetupColumn("Maximum");
	ImGui::TableSetupColumn("Samples");
	ImGui::TableSetupColumn("Availability", ImGuiTableColumnFlags_WidthStretch, 1.5f);
	ImGui::TableHeadersRow();
	for (const EditorProfilerMetricSummary& metric : metrics)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(metric.label.c_str());
		ImGui::TableSetColumnIndex(1);
		ImGui::Text(metric.available ? "%.3f %s" : "n/a", metric.median, metric.unit.c_str());
		ImGui::TableSetColumnIndex(2);
		ImGui::Text(metric.available ? "%.3f %s" : "n/a", metric.p95, metric.unit.c_str());
		ImGui::TableSetColumnIndex(3);
		ImGui::Text(metric.available ? "%.3f %s" : "n/a", metric.p99, metric.unit.c_str());
		ImGui::TableSetColumnIndex(4);
		ImGui::Text(metric.available ? "%.3f %s" : "n/a", metric.maximum, metric.unit.c_str());
		ImGui::TableSetColumnIndex(5);
		ImGui::Text("%llu valid / %llu unavailable",
			static_cast<unsigned long long>(metric.sampleCount),
			static_cast<unsigned long long>(metric.unavailableCount));
		ImGui::TableSetColumnIndex(6);
		if (metric.available)
		{
			ImGui::TextUnformatted("available");
		}
		else
		{
			ImGui::TextUnformatted(metric.unavailableReason.empty() ? "unavailable" : metric.unavailableReason.c_str());
		}
	}
	ImGui::EndTable();
}

// BuildSampleTree is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
SampleTree BuildSampleTree(const ProfilerFrameRecord& frame, ProfilerModuleId selectedModule)
{
	SampleTree tree;
	// Keep the table useful for timing inspection while rebuilding the parent
	// links that make nested profiler scopes readable at a glance.
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ProfilerSample& sample : frame.samples)
	{
		if (sample.moduleId == selectedModule)
		{
			tree.samples.push_back(sample);
		}
	}

	std::sort(tree.samples.begin(), tree.samples.end(), [](const ProfilerSample& left, const ProfilerSample& right) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (left.startMs == right.startMs)
		{
			return left.depth < right.depth;
		}
		return left.startMs < right.startMs;
	});

	tree.nodes.resize(tree.samples.size());
	std::unordered_map<std::uint64_t, std::size_t> sampleIndexById;
	sampleIndexById.reserve(tree.samples.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0u; index < tree.samples.size(); ++index)
	{
		tree.nodes[index].sampleIndex = index;
		sampleIndexById.emplace(tree.samples[index].sampleId, index);
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0u; index < tree.samples.size(); ++index)
	{
		const ProfilerSample& sample = tree.samples[index];
		const auto parentIt = sampleIndexById.find(sample.parentSampleId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (sample.parentSampleId != 0u && parentIt != sampleIndexById.end())
		{
			tree.nodes[parentIt->second].childIndices.push_back(index);
		}
		else
		{
			tree.rootIndices.push_back(index);
		}
	}

	for (SampleTreeNode& node : tree.nodes)
	{
		double childDurationMs = 0.0;
		for (const std::size_t childIndex : node.childIndices)
		{
			childDurationMs += tree.samples[tree.nodes[childIndex].sampleIndex].durationMs;
		}
		const double inclusiveMs = tree.samples[node.sampleIndex].durationMs;
		node.selfDurationMs = std::max(0.0, inclusiveMs - childDurationMs);
	}
	return tree;
}

std::vector<HotSampleRow> BuildHotSampleRows(const SampleTree& tree)
{
	std::vector<HotSampleRow> rows;
	rows.reserve(tree.samples.size());
	for (const SampleTreeNode& node : tree.nodes)
	{
		const ProfilerSample& sample = tree.samples[node.sampleIndex];
		if (sample.durationMs <= 0.0)
		{
			continue;
		}
		rows.push_back({
			sample.name,
			sample.threadLabel,
			sample.taskLabel,
			sample.durationMs,
			node.selfDurationMs,
			sample.depth
		});
	}
	std::sort(rows.begin(), rows.end(), [](const HotSampleRow& left, const HotSampleRow& right) {
		if (left.selfMs == right.selfMs)
		{
			return left.inclusiveMs > right.inclusiveMs;
		}
		return left.selfMs > right.selfMs;
	});
	return rows;
}

// DrawDisclosureControl is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool DrawDisclosureControl(bool hasChildren, bool isOpen)
{
	constexpr float kArrowSlotWidth = 7.0f;
	constexpr float kArrowSize = 4.0f;
	const float rowHeight = ImGui::GetTextLineHeight();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!hasChildren)
	{
		ImGui::Dummy(ImVec2(kArrowSlotWidth, rowHeight));
		ImGui::SameLine(0.0f, 1.0f);
		return false;
	}

	const ImVec2 buttonPosition = ImGui::GetCursorScreenPos();
	const bool clicked = ImGui::InvisibleButton("SampleDisclosure", ImVec2(kArrowSlotWidth, rowHeight));
	const ImU32 arrowColor = ImGui::GetColorU32(ImGuiCol_Text);
	const ImVec2 center(
		buttonPosition.x + kArrowSlotWidth * 0.5f,
		buttonPosition.y + rowHeight * 0.5f
	);
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (isOpen)
	{
		drawList->AddTriangleFilled(
			ImVec2(center.x - kArrowSize, center.y - kArrowSize * 0.45f),
			ImVec2(center.x + kArrowSize, center.y - kArrowSize * 0.45f),
			ImVec2(center.x, center.y + kArrowSize * 0.65f),
			arrowColor
		);
	}
	else
	{
		drawList->AddTriangleFilled(
			ImVec2(center.x - kArrowSize * 0.4f, center.y - kArrowSize),
			ImVec2(center.x - kArrowSize * 0.4f, center.y + kArrowSize),
			ImVec2(center.x + kArrowSize * 0.65f, center.y),
			arrowColor
		);
	}
	ImGui::SameLine(0.0f, 1.0f);
	return clicked;
}

// DrawSampleTreeRow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void DrawSampleTreeRow(const SampleTree& tree, std::size_t nodeIndex, std::uint64_t frameIndex, double frameDurationMs)
{
	const SampleTreeNode& node = tree.nodes[nodeIndex];
	const ProfilerSample& sample = tree.samples[node.sampleIndex];
	const bool hasChildren = !node.childIndices.empty();
	constexpr float kTreeIndentPixels = 12.0f;

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(sample.depth) * kTreeIndentPixels);
	bool isOpen = true;
	const std::string rowId = std::to_string(frameIndex) + ":" + std::to_string(sample.sampleId);
	ImGui::PushID(rowId.c_str());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasChildren)
	{
		ImGuiStorage* storage = ImGui::GetStateStorage();
		const ImGuiID openStateId = ImGui::GetID("SampleOpen");
		isOpen = storage->GetBool(openStateId, false);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (DrawDisclosureControl(true, isOpen))
		{
			isOpen = !isOpen;
			storage->SetBool(openStateId, isOpen);
		}
	}
	else
	{
		// Leaf rows reserve the same tiny arrow slot so names align within
		// each depth level without using ImGui tree nodes inside a table row.
		DrawDisclosureControl(false, true);
	}
	ImGui::TextUnformatted(sample.name.c_str());

	ImGui::TableSetColumnIndex(1);
	ImGui::Text("%.3f", sample.durationMs);
	ImGui::TableSetColumnIndex(2);
	ImGui::Text("%.3f", node.selfDurationMs);
	ImGui::TableSetColumnIndex(3);
	ImGui::Text("%.1f%%", SafePercent(sample.durationMs, frameDurationMs));
	ImGui::TableSetColumnIndex(4);
	DrawTimeBar(sample.durationMs, frameDurationMs);
	ImGui::TableSetColumnIndex(5);
	ImGui::TextUnformatted(sample.threadLabel.c_str());
	ImGui::TableSetColumnIndex(6);
	ImGui::TextUnformatted(sample.taskLabel.c_str());

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasChildren && isOpen)
	{
		for (const std::size_t childIndex : node.childIndices)
		{
			DrawSampleTreeRow(tree, childIndex, frameIndex, frameDurationMs);
		}
	}
	ImGui::PopID();
}
}

// ProfilerWindowCache owns immutable presentation data. It is defined in this
// translation unit so the public window header does not expose renderer or ImGui
// analysis implementation types.
struct ProfilerWindowCache
{
	std::uint64_t profilerRevision = 0u;
	ProfilerSessionState state = ProfilerSessionState::Idle;
	ProfilerCaptureSettings settings;
	std::vector<ProfilerFrameListItem> frames;
	std::vector<ProfilerFrameListItem> sortedFrames;
	FrameDurationStats frameDurationStats;
	// Detailed frame payloads remain outside the live refresh path until the user
	// explicitly pins a row. Completed frames are immutable, so one copy can be
	// reused while newer compact frame-list entries continue arriving.
	std::optional<std::uint64_t> pinnedFrameIndex;
	std::optional<ProfilerFrameRecord> selectedFrame;
	bool frameOrderDirty = true;
	bool sampleAnalysisValid = false;
	std::uint64_t sampleAnalysisFrameIndex = 0u;
	ProfilerModuleId sampleAnalysisModule = ProfilerModuleId::CpuEngine;
	SampleTree sampleTree;
	std::vector<HotSampleRow> hotSampleRows;
	bool captureEvidenceOpenLastFrame = false;
	bool captureEvidenceValid = false;
	EditorProfilerCaptureSummary captureEvidenceSummary;
	TerraEngine::Profiling::ProfilerCaptureDetailLevel captureEvidenceDetailLevel =
		TerraEngine::Profiling::ProfilerCaptureDetailLevel::Aggregate;
	bool timingDistributionOpenLastFrame = false;
	bool timingDistributionValid = false;
	std::vector<ProfilerTimingStreamAggregate> timingStreams;
	std::chrono::steady_clock::time_point nextProfilerRefresh{};
	std::chrono::steady_clock::time_point nextRendererRefresh{};
	TerraEngine::Renderer::RendererRecoverySnapshot rendererRecovery;
	std::vector<TerraEngine::Renderer::BackendStageTiming> backendStageTimings;
	RenderStatsSnapshot gameStats;
	RenderStatsSnapshot sceneStats;
};

ProfilerWindow::ProfilerWindow(MainEditorSubsystem* windowOwner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Profiler", false, "Rendering"), owner(windowOwner), cache(std::make_unique<ProfilerWindowCache>())
{
}

ProfilerWindow::~ProfilerWindow() = default;

void ProfilerWindow::RefreshLiveSnapshot()
{
	Profiler& profiler = Profiler::Instance();
	const auto now = std::chrono::steady_clock::now();
	const std::uint64_t requestedRevision = profiler.GetSnapshotRevision();
	if (cache->profilerRevision != requestedRevision && now >= cache->nextProfilerRefresh)
	{
		// Recording publishes a profiler frame on every engine frame. Cap the
		// presentation refresh so opening this window cannot turn those immutable
		// capture buffers into a second frame-rate consumer.
		cache->nextProfilerRefresh = now + kProfilerWindowRefreshInterval;
		const ProfilerSessionState previousState = cache->state;
		cache->state = profiler.GetState();
		cache->settings = profiler.GetSettings();
		const std::optional<std::uint64_t> previousSelectedFrameIndex = cache->selectedFrame.has_value()
			? std::optional<std::uint64_t>{cache->selectedFrame->frameIndex}
			: std::nullopt;
		const bool liveCapture = cache->state == ProfilerSessionState::Recording
			|| cache->state == ProfilerSessionState::Paused;
		if (!liveCapture)
		{
			// Frame presentation is a post-capture operation. Copy and analyze the
			// complete history only after Finish (or after loading another capture),
			// never while Recording or Paused can still append more frames.
			cache->frames = profiler.GetFrameListSnapshot();
			cache->frameDurationStats = BuildFrameDurationStats(cache->frames);
			cache->frameOrderDirty = true;
		}
		else if (previousState != cache->state && cache->state == ProfilerSessionState::Recording)
		{
			// Starting a new capture clears stale rows once. The subwindow then stays
			// empty and stable until the recording is finalized.
			cache->frames.clear();
			cache->sortedFrames.clear();
			cache->frameDurationStats = {};
			cache->pinnedFrameIndex.reset();
			cache->selectedFrame.reset();
			cache->sampleAnalysisValid = false;
			cache->frameOrderDirty = true;
		}
		bool shouldCopySelectedFrame = false;
		if (cache->pinnedFrameIndex.has_value())
		{
			shouldCopySelectedFrame = !cache->selectedFrame.has_value()
				|| cache->selectedFrame->frameIndex != *cache->pinnedFrameIndex;
		}
		else if (!liveCapture)
		{
			// Loaded and completed captures retain the established default frame
			// selection. Active recording has no selectable rows because the Frames
			// subwindow remains deferred until the capture is finalized.
			shouldCopySelectedFrame = true;
		}
		else
		{
			cache->selectedFrame.reset();
		}
		if (shouldCopySelectedFrame)
		{
			cache->selectedFrame = profiler.GetSelectedFrameSnapshot();
		}
		const std::optional<std::uint64_t> refreshedSelectedFrameIndex = cache->selectedFrame.has_value()
			? std::optional<std::uint64_t>{cache->selectedFrame->frameIndex}
			: std::nullopt;
		if (previousSelectedFrameIndex != refreshedSelectedFrameIndex)
		{
			// Completed frames are immutable. Preserve sample analysis when a
			// user is inspecting an older frame while recording continues.
			cache->sampleAnalysisValid = false;
		}
		if (cache->state != ProfilerSessionState::Recording || previousState != cache->state)
		{
			// Loading, clearing, pausing, or finishing changes the document as a
			// whole. Refresh an expanded evidence panel once for those discrete
			// events, while ordinary recorded frames remain explicitly cached.
			cache->captureEvidenceValid = false;
			cache->timingDistributionValid = false;
			cache->sampleAnalysisValid = false;
		}

		// If a frame completed between the individual immutable reads, leave the
		// cache revision stale so the next editor frame immediately converges.
		const std::uint64_t completedRevision = profiler.GetSnapshotRevision();
		cache->profilerRevision = completedRevision == requestedRevision ? completedRevision : 0u;
	}

	if (now < cache->nextRendererRefresh)
	{
		return;
	}

	// Compact renderer reads avoid copying virtual-geometry visibility records,
	// residency diagnostics, and string-heavy render graph evidence that this
	// window never displays.
	RenderSystem& renderer = RenderSystem::Instance();
	cache->rendererRecovery = renderer.GetLatestRendererRecoverySnapshot();
	cache->backendStageTimings = renderer.GetLatestBackendStageTimings();
	cache->gameStats = renderer.GetLatestRenderStats(RenderViewType::Game);
	cache->sceneStats = renderer.GetLatestRenderStats(RenderViewType::Scene);
	cache->nextRendererRefresh = now + kProfilerWindowRefreshInterval;
}

void ProfilerWindow::RefreshSampleAnalysis(const ProfilerFrameRecord& frame)
{
	if (cache->sampleAnalysisValid
		&& cache->sampleAnalysisFrameIndex == frame.frameIndex
		&& cache->sampleAnalysisModule == selectedModule)
	{
		return;
	}

	// Build parent links and hot rows once for the cached selected frame. Both UI
	// sections share this analysis instead of copying and sorting samples twice.
	cache->sampleTree = BuildSampleTree(frame, selectedModule);
	cache->hotSampleRows = BuildHotSampleRows(cache->sampleTree);
	cache->sampleAnalysisFrameIndex = frame.frameIndex;
	cache->sampleAnalysisModule = selectedModule;
	cache->sampleAnalysisValid = true;
}

// ProfilerWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::OnGui()
{
	EditorProfilerService* service = nullptr;
	if (owner && owner->GetContext())
	{
		service = owner->GetContext()->editorProfiler.get();
	}
	if (service != nullptr && service->HasActiveProject())
	{
		RefreshLiveSnapshot();
	}

	DrawToolbar();
	ImGui::Separator();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (service == nullptr || !service->HasActiveProject())
	{
		ImGui::TextDisabled("Open a project to use the profiler.");
		return;
	}

	const std::string status = service->GetStatusText();
	ImGui::Text("Status: %s", status.c_str());
	const std::string error = service->GetLastError();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!error.empty())
	{
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", error.c_str());
	}

	constexpr ImGuiTableFlags layoutFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("ProfilerLayout", 2, layoutFlags, ImGui::GetContentRegionAvail()))
	{
		ImGui::TableSetupColumn("Capture Navigation", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("Frame Analysis", ImGuiTableColumnFlags_WidthStretch, 2.7f);
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		DrawCaptureList();
		ImGui::Separator();
		DrawFrameList();
		ImGui::TableSetColumnIndex(1);
		DrawFrameDetails();

		ImGui::EndTable();
	}
}

// ProfilerWindow::DrawToolbar is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawToolbar()
{
	EditorProfilerService* service = owner && owner->GetContext() ? owner->GetContext()->editorProfiler.get() : nullptr;
	const bool hasService = service != nullptr;
	const bool hasProject = hasService && service->HasActiveProject();
	const bool busy = hasService && service->IsBusy();
	const ProfilerSessionState state = cache->state;
	const bool canUseProject = hasProject && !busy;
	const bool recording = state == ProfilerSessionState::Recording;
	const bool paused = state == ProfilerSessionState::Paused;
	const bool hasFrames = !cache->frames.empty();
	const char* unavailableReason = !hasService
		? "Profiler service is not available."
		: (!hasProject ? "Open a project before recording profiler captures." : (busy ? "Profiler service is finishing another action." : ""));

	DrawToolbarGroupLabel("Live");
	if (ToolbarButton(recording ? "Recording Every Frame" : "Record Every Frame", canUseProject && !recording, unavailableReason))
	{
		cache->pinnedFrameIndex.reset();
		cache->selectedFrame.reset();
		cache->sampleAnalysisValid = false;
		service->StartRecording();
	}
	ImGui::SameLine();
	if (ToolbarButton("Pause", canUseProject && recording, recording ? "" : "Pause is available only while recording."))
	{
		service->PauseRecording();
	}
	ImGui::SameLine();
	if (ToolbarButton("Resume", canUseProject && paused, paused ? "" : "Resume is available only while paused."))
	{
		service->ResumeRecording();
	}
	ImGui::SameLine();
	if (ToolbarButton("Finish", canUseProject && (recording || paused), (recording || paused) ? "" : "Start or resume a capture before finishing it."))
	{
		service->FinishCapture();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	DrawToolbarGroupLabel("Captures");
	if (ToolbarButton("Refresh", canUseProject, unavailableReason))
	{
		service->RefreshCaptureList();
	}
	ImGui::SameLine();
	if (ToolbarButton("Clear", canUseProject && hasFrames, hasFrames ? "" : "No captured frames are available to clear."))
	{
		cache->pinnedFrameIndex.reset();
		cache->selectedFrame.reset();
		cache->sampleAnalysisValid = false;
		service->Clear();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	DrawToolbarGroupLabel("Status");
	ImGui::TextDisabled("%s", TerraEngine::Profiling::ToString(state));
}

// ProfilerWindow::DrawCaptureList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawCaptureList()
{
	EditorProfilerService* service = owner && owner->GetContext() ? owner->GetContext()->editorProfiler.get() : nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (service == nullptr)
	{
		return;
	}

	ImGui::TextUnformatted("Project Captures");
	const float capturesHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.30f);
	ImGui::BeginChild("ProfilerCaptures", ImVec2(0, capturesHeight), true);
	const auto captures = service->GetCaptureList();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (captures.empty())
	{
		ImGui::TextDisabled("No captures in %s", service->GetProfilerDirectory().string().c_str());
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& capture : captures)
	{
		if (ImGui::Selectable(capture.fileName.c_str()))
		{
			cache->pinnedFrameIndex.reset();
			cache->selectedFrame.reset();
			cache->sampleAnalysisValid = false;
			service->LoadCapture(capture.absolutePath);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%llu frames\n%s", static_cast<unsigned long long>(capture.frameCount), capture.metadata.savedAt.c_str());
		}
	}
	ImGui::EndChild();
}

// ProfilerWindow::DrawFrameList is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawFrameList()
{
	ImGui::TextUnformatted("Frames");
	ImGui::BeginChild("ProfilerFrames", ImVec2(0, 0), true);
	const bool captureActive = cache->state == ProfilerSessionState::Recording
		|| cache->state == ProfilerSessionState::Paused;
	if (captureActive)
	{
		ImGui::TextDisabled("Frame history will appear after the recording is finished.");
		ImGui::EndChild();
		return;
	}
	const std::vector<ProfilerFrameListItem>& frames = cache->frames;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frames.empty())
	{
		ImGui::TextDisabled("No frames captured.");
	}
	const FrameDurationStats& stats = cache->frameDurationStats;
	if (!frames.empty())
	{
		ImGui::TextDisabled("%zu complete  %zu partial  mean %.2f ms  p95 %.2f ms",
			stats.frameCount,
			stats.partialFrameCount,
			stats.meanMs,
			stats.p95Ms);
	}
	std::vector<ProfilerFrameListItem>& sortedFrames = cache->sortedFrames;
	std::optional<std::uint64_t> requestedFrameSelection;
	if (ImGui::BeginTable("ProfilerFrameRows", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 82.0f);
		ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		bool frameIndexOrder = true;
		bool frameIndexAscending = false;
		if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0)
		{
			const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
			frameIndexOrder = spec.ColumnIndex == 0;
			frameIndexAscending = spec.SortDirection == ImGuiSortDirection_Ascending;
			if (!frameIndexOrder && (cache->frameOrderDirty || sortSpecs->SpecsDirty))
			{
				const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
				sortedFrames = frames;
				std::sort(sortedFrames.begin(), sortedFrames.end(), [&](const ProfilerFrameListItem& left, const ProfilerFrameListItem& right) {
					int result = left.durationMs < right.durationMs ? -1 : (left.durationMs > right.durationMs ? 1 : 0);
					if (result == 0)
					{
						// Stable tie-breaks keep rows from jumping when two frames
						// have nearly identical timings.
						result = left.frameIndex < right.frameIndex ? -1 : (left.frameIndex > right.frameIndex ? 1 : 0);
					}
					return ascending ? result < 0 : result > 0;
				});
				cache->frameOrderDirty = false;
			}
			sortSpecs->SpecsDirty = false;
		}
		// The compact snapshot is already chronological. Frame-index sorting is
		// therefore a direct or reverse view and never needs an O(H log H) copy.
		const std::vector<ProfilerFrameListItem>& displayedFrames = frameIndexOrder ? frames : sortedFrames;
		// A completed capture can hold thousands of frames. Clip visible rows so
		// opening the profiler does not spend every editor frame formatting labels
		// that are outside the scroll viewport.
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(displayedFrames.size()));
		while (clipper.Step())
		{
			for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
			{
				const std::size_t displayIndex = frameIndexOrder && !frameIndexAscending
					? displayedFrames.size() - 1u - static_cast<std::size_t>(rowIndex)
					: static_cast<std::size_t>(rowIndex);
				const ProfilerFrameListItem& frame = displayedFrames[displayIndex];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				char label[64];
				std::snprintf(label, sizeof(label), "#%llu%s",
					static_cast<unsigned long long>(frame.frameIndex),
					frame.complete ? "" : "*");
				const bool selected = cache->pinnedFrameIndex == frame.frameIndex
					|| (cache->selectedFrame.has_value()
						&& cache->selectedFrame->frameIndex == frame.frameIndex);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					requestedFrameSelection = frame.frameIndex;
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::TextColored(FrameDurationColor(frame.durationMs), "%.2f", frame.durationMs);
				ImGui::TableSetColumnIndex(2);
				DrawTimeBar(frame.durationMs, std::max(stats.p99Ms, 1.0));
			}
		}
		ImGui::EndTable();
	}
	if (requestedFrameSelection.has_value())
	{
		// Selection publishes a new profiler revision. The next OnGui call takes
		// one coherent detailed copy, then pins that immutable frame while the
		// compact live list continues to grow without repeated payload copies.
		cache->pinnedFrameIndex = *requestedFrameSelection;
		cache->selectedFrame.reset();
		cache->sampleAnalysisValid = false;
		Profiler::Instance().SelectFrame(*requestedFrameSelection);
	}
	ImGui::EndChild();
}

// ProfilerWindow::DrawFrameDetails is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawFrameDetails()
{
	DrawRendererRecoverySummary();
	DrawCaptureEvidenceSummary();

	const std::optional<ProfilerFrameRecord>& selectedFrame = cache->selectedFrame;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!selectedFrame.has_value())
	{
		const bool captureActive = cache->state == ProfilerSessionState::Recording
			|| cache->state == ProfilerSessionState::Paused;
		ImGui::TextDisabled(captureActive
			? "Frame analysis will become available after the recording is finished."
			: "Select a captured frame to inspect detailed scopes.");
		return;
	}
	if (cache->pinnedFrameIndex.has_value())
	{
		ImGui::TextDisabled("Pinned frame #%llu; live frame-list updates remain compact.",
			static_cast<unsigned long long>(*cache->pinnedFrameIndex));
		ImGui::SameLine();
		if (ImGui::SmallButton("Return to Live List"))
		{
			cache->pinnedFrameIndex.reset();
			cache->selectedFrame.reset();
			cache->sampleAnalysisValid = false;
			return;
		}
	}

	DrawFrameOverview(*selectedFrame);
	DrawPhysicsSummary(*selectedFrame);

	const ScriptingSystem::ScriptingFrameResult& scriptingResult = ScriptingSystem::Instance().GetLatestScriptingFrameResult();
	if (scriptingResult.frameId != 0 && ImGui::TreeNode("Scripting Dispatch Counters"))
	{
		ImGui::Text("Updated: %zu  Worker: %zu  Protected: %zu", scriptingResult.updatedScripts, scriptingResult.workerScripts, scriptingResult.protectedScripts);
		ImGui::Text("Cache hits: %zu  Misses: %zu  Bridge avoided: %zu", scriptingResult.cacheHits, scriptingResult.cacheMisses, scriptingResult.bridgeCallsAvoided);
		ImGui::Text("Typed writes: %zu  JSON fallbacks: %zu  Coalesced: %zu", scriptingResult.typedMutations, scriptingResult.jsonMutationFallbacks, scriptingResult.coalescedMutations);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Visible Command Reuse"))
	{
		if (ImGui::BeginTable("ProfilerVisibleCommandReuseRows", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("View");
			ImGui::TableSetupColumn("Cache");
			ImGui::TableSetupColumn("Commands");
			ImGui::TableSetupColumn("Copied");
			ImGui::TableSetupColumn("Unique Touches");
			ImGui::TableSetupColumn("Deduped");
			ImGui::TableSetupColumn("Diagnostics");
			ImGui::TableHeadersRow();
			const auto drawStatsRow = [](const RenderStatsSnapshot& stats) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(ToString(stats.viewType));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(stats.visibleCommandCacheHit ? "hit" : "miss");
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%u", stats.visibleCommandCount);
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%u", stats.visibleCommandCopiedCount);
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%u", stats.visibleStreamingUniqueMeshes + stats.visibleStreamingUniqueMaterials + stats.visibleStreamingUniqueTextures);
				ImGui::TableSetColumnIndex(5);
				ImGui::Text("%u", stats.visibleStreamingDuplicateTouchesEliminated);
				ImGui::TableSetColumnIndex(6);
				ImGui::TextUnformatted(stats.detailedRenderDiagnosticsRequested ? "detailed" : "compact");
			};
			drawStatsRow(cache->gameStats);
			drawStatsRow(cache->sceneStats);
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
	if (!cache->backendStageTimings.empty() && ImGui::TreeNode("Backend Submit Path"))
	{
		if (ImGui::BeginTable("ProfilerBackendSubmitStages", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Stage");
			ImGui::TableSetupColumn("View");
			ImGui::TableSetupColumn("Time");
			ImGui::TableSetupColumn("Backend");
			ImGui::TableHeadersRow();
			for (const TerraEngine::Renderer::BackendStageTiming& timing : cache->backendStageTimings)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(timing.label.empty() ? TerraEngine::Renderer::ToString(timing.stage) : timing.label.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(timing.viewType == RenderViewType::Scene ? "Scene" : "Game");
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%.3f ms", timing.milliseconds);
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(timing.backendId.c_str());
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTabBar("ProfilerModules"))
	{
		for (ProfilerModuleId moduleId : {ProfilerModuleId::CpuEngine, ProfilerModuleId::TaskExecution})
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::BeginTabItem(TerraEngine::Profiling::ToString(moduleId)))
			{
				selectedModule = moduleId;
				DrawModuleSummary(*selectedFrame);
				DrawHotSamples(*selectedFrame);
				DrawSamples(*selectedFrame);
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}
}

void ProfilerWindow::DrawCaptureEvidenceSummary()
{
	ImGui::SeparatorText("Capture Evidence");
	// Whole-capture synthesis deep-copies and scans every retained typed stream.
	// Keep it out of the normal redraw path until the user explicitly asks for
	// the analysis; selected-frame details above remain cheap and live.
	const bool captureEvidenceOpen = ImGui::TreeNode("Whole Capture Analysis");
	if (!captureEvidenceOpen)
	{
		cache->captureEvidenceOpenLastFrame = false;
		ImGui::TextDisabled("%s capture; expand to aggregate retained evidence.",
			TerraEngine::Profiling::ToString(cache->settings.detailLevel));
		return;
	}
	const bool captureEvidenceOpenedThisFrame = !cache->captureEvidenceOpenLastFrame;
	cache->captureEvidenceOpenLastFrame = true;
	const bool refreshCaptureEvidence = ImGui::Button("Refresh Whole Capture Analysis");
	if (captureEvidenceOpenedThisFrame || refreshCaptureEvidence)
	{
		cache->captureEvidenceValid = false;
	}
	if (!cache->captureEvidenceValid)
	{
		// The full document is copied only when this disclosure opens or the user
		// explicitly refreshes it. An expanded saved layout therefore cannot scan
		// a growing recording on every ImGui redraw.
		const TerraEngine::Profiling::ProfilerCaptureDocument document = Profiler::Instance().GetDocumentSnapshot();
		cache->captureEvidenceSummary = EditorProfilerService::SummarizeDocument("live", document);
		cache->captureEvidenceDetailLevel = document.metadata.settings.detailLevel;
		cache->captureEvidenceValid = true;
	}
	else
	{
		ImGui::SameLine();
		ImGui::TextDisabled("cached; refresh to include newer frames");
	}
	const EditorProfilerCaptureSummary& summary = cache->captureEvidenceSummary;
	ImGui::Text("v%u  %s capture  %llu committed frames  %llu B",
		summary.payloadVersion,
		TerraEngine::Profiling::ToString(cache->captureEvidenceDetailLevel),
		static_cast<unsigned long long>(summary.committedFrameCount),
		static_cast<unsigned long long>(summary.artifactSizeBytes));
	if (summary.recoveredIncompleteTail)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.25f, 1.0f), "Recovery warning: incomplete final chunk was discarded; committed frames remain readable.");
	}
	else if (!summary.captureComplete)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f), "Capture is incomplete and was not recoverable.");
	}
	else
	{
		ImGui::TextDisabled("Capture is complete.");
	}

	if (ImGui::TreeNode("Physics and Scene View Stages"))
	{
		DrawEvidenceMetrics("ProfilerStageEvidence", summary.stageTimings);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Protected Boundary Breakdown"))
	{
		DrawEvidenceMetrics("ProfilerBoundaryEvidence", summary.protectedBoundaryTimings);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Presentation and Responsiveness"))
	{
		DrawEvidenceMetrics("ProfilerPresentationWaitEvidence", summary.presentationWaitTimings);
		DrawEvidenceMetrics("ProfilerResponsivenessEvidence", summary.responsivenessTimings);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Worker Timeline Summary"))
	{
		if (summary.workerHistories.empty())
		{
			ImGui::TextDisabled("No worker telemetry was recorded at this capture level.");
		}
		else if (ImGui::BeginTable("ProfilerWorkerTimeline", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Worker");
			ImGui::TableSetupColumn("Run P95");
			ImGui::TableSetupColumn("Wait P95");
			ImGui::TableSetupColumn("Idle P95");
			ImGui::TableSetupColumn("Max Queue");
			ImGui::TableSetupColumn("Useful Items");
			ImGui::TableHeadersRow();
			for (const EditorProfilerWorkerSummary& worker : summary.workerHistories)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0); ImGui::Text("%u", worker.workerId);
				ImGui::TableSetColumnIndex(1); ImGui::Text(worker.runDuration.available ? "%.3f ms" : "n/a", worker.runDuration.p95);
				ImGui::TableSetColumnIndex(2); ImGui::Text(worker.waitDuration.available ? "%.3f ms" : "n/a", worker.waitDuration.p95);
				ImGui::TableSetColumnIndex(3); ImGui::Text(worker.idleDuration.available ? "%.3f ms" : "n/a", worker.idleDuration.p95);
				ImGui::TableSetColumnIndex(4); ImGui::Text("%llu", static_cast<unsigned long long>(worker.maximumQueueDepth));
				ImGui::TableSetColumnIndex(5); ImGui::Text("%llu", static_cast<unsigned long long>(worker.totalUsefulWorkItems));
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Physics Work Volumes"))
	{
		const EditorProfilerWorkVolumeSummary& volume = summary.workVolumes;
		ImGui::Text("Ticks %llu  Total %llu  Active %llu  Examined %llu",
			static_cast<unsigned long long>(volume.measuredTickCount), static_cast<unsigned long long>(volume.totalBodies),
			static_cast<unsigned long long>(volume.activeBodies), static_cast<unsigned long long>(volume.examinedBodies));
		ImGui::Text("Extracted %llu  Converted %llu  Changed %llu  Published %llu",
			static_cast<unsigned long long>(volume.extractedBodies), static_cast<unsigned long long>(volume.convertedBodies),
			static_cast<unsigned long long>(volume.changedBodies), static_cast<unsigned long long>(volume.publishedBodies));
		ImGui::TreePop();
	}
	ImGui::TreePop();
}

void ProfilerWindow::DrawPhysicsSummary(const TerraEngine::Profiling::ProfilerFrameRecord& frame)
{
	// Read captured fields instead of a live snapshot so replay works after the
	// world stops and never depends on the dedicated diagnostics window.
	if (!frame.physics.has_value())
	{
		ImGui::TextDisabled("No physics counters were recorded for this frame.");
		return;
	}
	const TerraEngine::Profiling::ProfilerPhysicsFrameCounters& physics = *frame.physics;
	if (!ImGui::TreeNode("Physics Counters"))
	{
		return;
	}
	ImGui::Text("Tick %llu  Bodies %llu  Contacts %llu  Queries %llu  Events %llu",
		static_cast<unsigned long long>(physics.tick), static_cast<unsigned long long>(physics.bodyCount),
		static_cast<unsigned long long>(physics.contactCount), static_cast<unsigned long long>(physics.queryCount),
		static_cast<unsigned long long>(physics.eventCount));
	ImGui::Text("Rejected %llu  Stale %llu  Command queue %llu",
		static_cast<unsigned long long>(physics.rejectedCount), static_cast<unsigned long long>(physics.staleCount),
		static_cast<unsigned long long>(physics.commandQueueDepth));
	ImGui::Text("Shape cache %llu B (hits %llu / misses %llu)",
		static_cast<unsigned long long>(physics.shapeCacheBytes),
		static_cast<unsigned long long>(physics.shapeCacheHits),
		static_cast<unsigned long long>(physics.shapeCacheMisses));
	ImGui::Text("Snapshots %llu B / %llu B high-water; provider allocations %llu / %llu high-water",
		static_cast<unsigned long long>(physics.snapshotBytes),
		static_cast<unsigned long long>(physics.snapshotHighWaterBytes),
		static_cast<unsigned long long>(physics.providerAllocations),
		static_cast<unsigned long long>(physics.providerAllocationHighWater));
	if (ImGui::BeginTable("ProfilerPhysicsTiming", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Prepare"); ImGui::TableSetupColumn("Commit"); ImGui::TableSetupColumn("Simulate");
		ImGui::TableSetupColumn("Queries"); ImGui::TableSetupColumn("Publish"); ImGui::TableHeadersRow(); ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0); ImGui::Text("%.3f ms", physics.prepareMs);
		ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f ms", physics.commitMs);
		ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f ms", physics.simulateMs);
		ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f ms", physics.queryMs);
		ImGui::TableSetColumnIndex(4); ImGui::Text("%.3f ms", physics.publishMs);
		ImGui::EndTable();
	}
	ImGui::TreePop();
}

void ProfilerWindow::DrawRendererRecoverySummary()
{
	const TerraEngine::Renderer::RendererRecoverySnapshot& recovery = cache->rendererRecovery;
	if (!TerraEngine::Renderer::IsRendererRecoveryActive(recovery.state)
		&& recovery.finalOutcome == TerraEngine::Renderer::RendererRecoveryOutcome::None)
	{
		return;
	}

	ImGui::SeparatorText("Renderer Recovery");
	ImGui::Text("%s: %s", TerraEngine::Renderer::ToString(recovery.state), recovery.message.c_str());
	if (ImGui::BeginTable("ProfilerRendererRecovery", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Incident");
		ImGui::TableSetupColumn("Stage");
		ImGui::TableSetupColumn("Duplicates");
		ImGui::TableSetupColumn("Outcome");
		ImGui::TableSetupColumn("Dump");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(recovery.incidentId.empty() ? "-" : recovery.incidentId.c_str());
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%s  %s", TerraEngine::Renderer::ToString(recovery.firstFailureStage), recovery.firstFailureResult.c_str());
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%u", recovery.duplicateFailureCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::TextUnformatted(TerraEngine::Renderer::ToString(recovery.finalOutcome));
		ImGui::TableSetColumnIndex(4);
		ImGui::TextUnformatted(recovery.crashDumpStatus.empty() ? "n/a" : recovery.crashDumpStatus.c_str());
		ImGui::EndTable();
	}
}

// ProfilerWindow::DrawFrameOverview is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawFrameOverview(const ProfilerFrameRecord& frame)
{
	const FrameDurationStats& stats = cache->frameDurationStats;

	ImGui::SeparatorText("Frame Overview");
	ImGui::Text("Frame %llu", static_cast<unsigned long long>(frame.frameIndex));
	ImGui::SameLine();
	ImGui::TextColored(FrameDurationColor(frame.durationMs), "%.3f ms", frame.durationMs);
	if (!frame.complete)
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "partial");
	}

	if (stats.frameCount == 0u)
	{
		ImGui::TextDisabled("No complete timing window is available yet.");
		return;
	}

	// Distribution analysis copies and sorts the rolling frame capture. Keep that
	// work behind an explicit disclosure so the normal profiler redraw stays
	// proportional to the lightweight frame list instead of the capture length.
	const bool timingDistributionOpen = ImGui::TreeNode("Timing Distribution");
	if (timingDistributionOpen)
	{
		const bool timingDistributionOpenedThisFrame = !cache->timingDistributionOpenLastFrame;
		cache->timingDistributionOpenLastFrame = true;
		const bool refreshTimingDistribution = ImGui::Button("Refresh Timing Distribution");
		if (timingDistributionOpenedThisFrame
			|| refreshTimingDistribution)
		{
			cache->timingDistributionValid = false;
		}
		if (!cache->timingDistributionValid)
		{
			// Aggregate the complete frame set only on explicit presentation
			// changes. The cached table remains stable while recording grows.
			const std::vector<ProfilerFrameRecord> fullFrames = Profiler::Instance().GetFramesSnapshot();
			cache->timingStreams =
				TerraEngine::Profiling::BuildTimingStreamAggregates(fullFrames, std::optional<ProfilerFrameRecord>(frame));
			cache->timingDistributionValid = true;
		}
		else
		{
			ImGui::SameLine();
			ImGui::TextDisabled("cached; refresh after selection or to include newer frames");
		}
		if (!cache->timingStreams.empty() && ImGui::BeginTable("ProfilerTimingStreams", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Timing");
			ImGui::TableSetupColumn("Selected");
			ImGui::TableSetupColumn("Mean");
			ImGui::TableSetupColumn("Median");
			ImGui::TableSetupColumn("P95");
			ImGui::TableSetupColumn("P99");
			ImGui::TableSetupColumn("Min");
			ImGui::TableSetupColumn("Max");
			ImGui::TableSetupColumn("Samples");
			ImGui::TableSetupColumn("Source");
			ImGui::TableHeadersRow();
			for (const ProfilerTimingStreamAggregate& stream : cache->timingStreams)
			{
				char selectedValue[64];
				char aggregateValue[64];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(stream.label.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(TimingDisplayValue(stream, selectedValue, sizeof(selectedValue)));
				if (!stream.selectedStatusReason.empty() && ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", stream.selectedStatusReason.c_str());
				}
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.meanMs, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.medianMs, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.p95Ms, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(5);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.p99Ms, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(6);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.minMs, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(7);
				ImGui::TextUnformatted(TimingAggregateValue(stream, stream.maxMs, aggregateValue, sizeof(aggregateValue)));
				ImGui::TableSetColumnIndex(8);
				ImGui::Text("%zu / %zu missing", stream.sampleCount, stream.missingCount);
				ImGui::TableSetColumnIndex(9);
				ImGui::TextUnformatted(TerraEngine::Profiling::ToString(stream.source));
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
	else
	{
		cache->timingDistributionOpenLastFrame = false;
	}
	// Explain the accounting boundary beside the values it governs. The frame
	// duration excludes editor work; viewport CPU is nested and GPU can overlap.
	ImGui::TextDisabled("Game profiling: only Play Mode frames are captured. Frame = Engine CPU; Viewport CPU is included and GPU is independent.");

	if (ImGui::BeginTable("ProfilerFrameStats", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
	{
		DrawMetricCell("Mean", stats.meanMs, FrameDurationColor(stats.meanMs));
		DrawMetricCell("Median", stats.medianMs, FrameDurationColor(stats.medianMs));
		DrawMetricCell("P90", stats.p90Ms, FrameDurationColor(stats.p90Ms));
		DrawMetricCell("P95", stats.p95Ms, FrameDurationColor(stats.p95Ms));
		DrawMetricCell("P99", stats.p99Ms, FrameDurationColor(stats.p99Ms));
		ImGui::EndTable();
	}
	if (ImGui::BeginTable("ProfilerFrameStatsSecondary", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
	{
		DrawMetricCell("Min", stats.minMs, FrameDurationColor(stats.minMs));
		DrawMetricCell("Max", stats.maxMs, FrameDurationColor(stats.maxMs));
		DrawMetricCell("Std Dev", stats.stdDevMs, FrameDurationColor(stats.stdDevMs));
		ImGui::TableNextColumn();
		ImGui::TextDisabled("Complete");
		ImGui::Text("%zu", stats.frameCount);
		ImGui::TableNextColumn();
		ImGui::TextDisabled("Partial");
		ImGui::Text("%zu", stats.partialFrameCount);
		ImGui::EndTable();
	}

	ImGui::TextDisabled("Selected frame vs p95");
	DrawTimeBar(frame.durationMs, std::max(stats.p95Ms, 1.0));
}

// ProfilerWindow::DrawModuleSummary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawModuleSummary(const ProfilerFrameRecord& frame)
{
	const TerraEngine::Profiling::ProfilerModuleSummary* moduleSummary = FindModuleSummary(frame, selectedModule);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (moduleSummary == nullptr || !moduleSummary->available)
	{
		ImGui::TextDisabled("No data recorded for this module.");
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("ProfilerModuleMetrics", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Metric");
		ImGui::TableSetupColumn("Value");
		ImGui::TableSetupColumn("Unit");
		ImGui::TableHeadersRow();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& metric : moduleSummary->metrics)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(metric.name.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", metric.value);
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(metric.unit.c_str());
		}
		ImGui::EndTable();
	}
}

// ProfilerWindow::DrawHotSamples is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawHotSamples(const ProfilerFrameRecord& frame)
{
	// Sorting every captured scope is analysis work, not ordinary live display.
	// Keep it behind an explicit disclosure and reuse the result for the full
	// hierarchy if the user opens both sections for the same pinned frame.
	if (!ImGui::TreeNode("Top Offenders"))
	{
		ImGui::TextDisabled("Expand to sort the cached frame's hottest scopes.");
		return;
	}
	RefreshSampleAnalysis(frame);
	const std::vector<HotSampleRow>& rows = cache->hotSampleRows;
	if (rows.empty())
	{
		ImGui::TextDisabled("No hot samples for this module.");
		ImGui::TreePop();
		return;
	}
	// Inclusive rows contain their child scopes, so adding rows would count the
	// same execution interval more than once. Self time is the non-overlapping cost.
	ImGui::TextDisabled("Inclusive timings are nested and must not be added together; Self excludes child scopes.");

	const std::size_t rowLimit = std::min<std::size_t>(rows.size(), 12u);
	if (ImGui::BeginTable("ProfilerHotSamples", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableSetupColumn("Self");
		ImGui::TableSetupColumn("Incl.");
		ImGui::TableSetupColumn("% Frame");
		ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Thread");
		ImGui::TableHeadersRow();
		for (std::size_t rowIndex = 0u; rowIndex < rowLimit; ++rowIndex)
		{
			const HotSampleRow& row = rows[rowIndex];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(row.name.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%.3f", row.selfMs);
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%.3f", row.inclusiveMs);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%.1f%%", SafePercent(row.inclusiveMs, frame.durationMs));
			ImGui::TableSetColumnIndex(4);
			DrawTimeBar(row.selfMs, std::max(frame.durationMs, 1.0));
			ImGui::TableSetColumnIndex(5);
			ImGui::TextUnformatted(row.threadLabel.c_str());
		}
		ImGui::EndTable();
	}
	ImGui::TreePop();
}

// ProfilerWindow::DrawSamples is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawSamples(const ProfilerFrameRecord& frame)
{
	ImGui::SeparatorText("Samples");
	// Large captures can contain thousands of nested markers. Keep the complete
	// hierarchy opt-in, and start each parent collapsed, so merely opening the
	// profiler never emits every sample row into ImGui.
	if (!ImGui::TreeNode("Sample Hierarchy"))
	{
		ImGui::TextDisabled("Expand to inspect the cached scope tree.");
		return;
	}
	RefreshSampleAnalysis(frame);
	const SampleTree& tree = cache->sampleTree;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (tree.samples.empty())
	{
		ImGui::TextDisabled("No samples for this module.");
		ImGui::TreePop();
		return;
	}

	const ImVec2 originalCellPadding = ImGui::GetStyle().CellPadding;
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1.0f, originalCellPadding.y));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("ProfilerSamples", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableSetupColumn("Incl.");
		ImGui::TableSetupColumn("Self");
		ImGui::TableSetupColumn("% Frame");
		ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Thread");
		ImGui::TableSetupColumn("Task");
		ImGui::TableHeadersRow();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::size_t rootIndex : tree.rootIndices)
		{
			DrawSampleTreeRow(tree, rootIndex, frame.frameIndex, frame.durationMs);
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
	ImGui::TreePop();
}
