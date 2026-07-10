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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using TerraEngine::Profiling::Profiler;
using TerraEngine::Profiling::ProfilerFrameListItem;
using TerraEngine::Profiling::ProfilerFrameRecord;
using TerraEngine::Profiling::ProfilerModuleId;
using TerraEngine::Profiling::ProfilerSample;
using TerraEngine::Profiling::ProfilerSessionState;
using TerraEngine::Profiling::ProfilerTimingStreamAggregate;
using TerraEngine::Profiling::ProfilerTimingStreamStatus;

namespace
{
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
		isOpen = storage->GetBool(openStateId, true);
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

ProfilerWindow::ProfilerWindow(MainEditorSubsystem* windowOwner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Profiler", false, "Rendering"), owner(windowOwner)
{
}

// ProfilerWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::OnGui()
{
	DrawToolbar();
	ImGui::Separator();

	EditorProfilerService* service = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (owner && owner->GetContext())
	{
		service = owner->GetContext()->editorProfiler.get();
	}
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
	const ProfilerSessionState state = Profiler::Instance().GetState();
	const bool canUseProject = hasProject && !busy;
	const bool recording = state == ProfilerSessionState::Recording;
	const bool paused = state == ProfilerSessionState::Paused;
	const bool hasFrames = !Profiler::Instance().GetFrameListSnapshot().empty();
	const char* unavailableReason = !hasService
		? "Profiler service is not available."
		: (!hasProject ? "Open a project before recording profiler captures." : (busy ? "Profiler service is finishing another action." : ""));

	DrawToolbarGroupLabel("Live");
	if (ToolbarButton(recording ? "Recording" : "Record", canUseProject && !recording, unavailableReason))
	{
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
	const std::vector<ProfilerFrameListItem> frames = Profiler::Instance().GetFrameListSnapshot();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frames.empty())
	{
		ImGui::TextDisabled("No frames captured.");
	}
	const FrameDurationStats stats = BuildFrameDurationStats(frames);
	if (!frames.empty())
	{
		ImGui::TextDisabled("%zu frames  mean %.2f ms  p95 %.2f ms", stats.frameCount, stats.meanMs, stats.p95Ms);
	}
	std::vector<ProfilerFrameListItem> sortedFrames = frames;
	if (ImGui::BeginTable("ProfilerFrameRows", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, 82.0f);
		ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0)
		{
			const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
			const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
			std::sort(sortedFrames.begin(), sortedFrames.end(), [&](const ProfilerFrameListItem& left, const ProfilerFrameListItem& right) {
				int result = 0;
				switch (spec.ColumnIndex)
				{
				case 0:
					result = left.frameIndex < right.frameIndex ? -1 : (left.frameIndex > right.frameIndex ? 1 : 0);
					break;
				case 1:
				case 2:
					result = left.durationMs < right.durationMs ? -1 : (left.durationMs > right.durationMs ? 1 : 0);
					break;
				default:
					result = left.frameIndex < right.frameIndex ? -1 : (left.frameIndex > right.frameIndex ? 1 : 0);
					break;
				}
				if (result == 0)
				{
					// Stable tie-breaks keep rows from jumping when two frames
					// have nearly identical timings.
					result = left.frameIndex < right.frameIndex ? -1 : (left.frameIndex > right.frameIndex ? 1 : 0);
				}
				return ascending ? result < 0 : result > 0;
			});
			sortSpecs->SpecsDirty = false;
		}
		// The rolling capture can hold thousands of frames. Clip visible rows so
		// opening the profiler does not spend every editor frame formatting labels
		// that are outside the scroll viewport.
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(sortedFrames.size()));
		while (clipper.Step())
		{
			for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
			{
				const ProfilerFrameListItem& frame = sortedFrames[static_cast<std::size_t>(rowIndex)];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				char label[64];
				std::snprintf(label, sizeof(label), "#%llu%s",
					static_cast<unsigned long long>(frame.frameIndex),
					frame.complete ? "" : "*");
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns))
				{
					Profiler::Instance().SelectFrame(frame.frameIndex);
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::TextColored(FrameDurationColor(frame.durationMs), "%.2f", frame.durationMs);
				ImGui::TableSetColumnIndex(2);
				DrawTimeBar(frame.durationMs, std::max(stats.p99Ms, 1.0));
			}
		}
		ImGui::EndTable();
	}
	ImGui::EndChild();
}

// ProfilerWindow::DrawFrameDetails is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawFrameDetails()
{
	DrawRendererRecoverySummary();

	const auto selectedFrame = Profiler::Instance().GetSelectedFrameSnapshot();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!selectedFrame.has_value())
	{
		ImGui::TextDisabled("Select a frame to inspect.");
		return;
	}

	DrawFrameOverview(*selectedFrame);

	const ScriptingSystem::ScriptingFrameResult& scriptingResult = ScriptingSystem::Instance().GetLatestScriptingFrameResult();
	if (scriptingResult.frameId != 0 && ImGui::TreeNode("Scripting Dispatch Counters"))
	{
		ImGui::Text("Updated: %zu  Worker: %zu  Protected: %zu", scriptingResult.updatedScripts, scriptingResult.workerScripts, scriptingResult.protectedScripts);
		ImGui::Text("Cache hits: %zu  Misses: %zu  Bridge avoided: %zu", scriptingResult.cacheHits, scriptingResult.cacheMisses, scriptingResult.bridgeCallsAvoided);
		ImGui::Text("Typed writes: %zu  JSON fallbacks: %zu  Coalesced: %zu", scriptingResult.typedMutations, scriptingResult.jsonMutationFallbacks, scriptingResult.coalescedMutations);
		ImGui::TreePop();
	}
	const TerraEngine::Renderer::RenderDiagnosticsSummary renderDiagnostics = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
	if (ImGui::TreeNode("Visible Command Reuse"))
	{
		const RenderStatsSnapshot gameStats = RenderSystem::Instance().GetLatestRenderStats(RenderViewType::Game);
		const RenderStatsSnapshot sceneStats = RenderSystem::Instance().GetLatestRenderStats(RenderViewType::Scene);
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
			drawStatsRow(gameStats);
			drawStatsRow(sceneStats);
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
	if (!renderDiagnostics.backendFrameBreakdown.stageTimings.empty() && ImGui::TreeNode("Backend Submit Path"))
	{
		if (ImGui::BeginTable("ProfilerBackendSubmitStages", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Stage");
			ImGui::TableSetupColumn("View");
			ImGui::TableSetupColumn("Time");
			ImGui::TableSetupColumn("Backend");
			ImGui::TableHeadersRow();
			for (const TerraEngine::Renderer::BackendStageTiming& timing : renderDiagnostics.backendFrameBreakdown.stageTimings)
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

void ProfilerWindow::DrawRendererRecoverySummary()
{
	const TerraEngine::Renderer::RenderDiagnosticsSummary renderDiagnostics = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
	const TerraEngine::Renderer::RendererRecoverySnapshot& recovery = renderDiagnostics.rendererRecovery;
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
	const std::vector<ProfilerFrameListItem> frames = Profiler::Instance().GetFrameListSnapshot();
	const FrameDurationStats stats = BuildFrameDurationStats(frames);

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

	const std::vector<ProfilerFrameRecord> fullFrames = Profiler::Instance().GetFramesSnapshot();
	const std::vector<ProfilerTimingStreamAggregate> timingStreams =
		TerraEngine::Profiling::BuildTimingStreamAggregates(fullFrames, std::optional<ProfilerFrameRecord>(frame));
	if (!timingStreams.empty() && ImGui::BeginTable("ProfilerTimingStreams", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
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
		for (const ProfilerTimingStreamAggregate& stream : timingStreams)
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
		ImGui::TextDisabled("Frames");
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
	ImGui::SeparatorText("Top Offenders");
	const SampleTree tree = BuildSampleTree(frame, selectedModule);
	std::vector<HotSampleRow> rows = BuildHotSampleRows(tree);
	if (rows.empty())
	{
		ImGui::TextDisabled("No hot samples for this module.");
		return;
	}

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
}

// ProfilerWindow::DrawSamples is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawSamples(const ProfilerFrameRecord& frame)
{
	ImGui::SeparatorText("Samples");
	const SampleTree tree = BuildSampleTree(frame, selectedModule);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (tree.samples.empty())
	{
		ImGui::TextDisabled("No samples for this module.");
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
}
