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
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <unordered_map>

using TerraEngine::Profiling::Profiler;
using TerraEngine::Profiling::ProfilerFrameListItem;
using TerraEngine::Profiling::ProfilerFrameRecord;
using TerraEngine::Profiling::ProfilerModuleId;
using TerraEngine::Profiling::ProfilerSample;
using TerraEngine::Profiling::ProfilerSessionState;

namespace
{
// struct SampleTreeNode groups related state and operations so callers have one documented contract.
struct SampleTreeNode
{
	std::size_t sampleIndex = 0u;
	std::vector<std::size_t> childIndices;
};

// struct SampleTree groups related state and operations so callers have one documented contract.
struct SampleTree
{
	std::vector<ProfilerSample> samples;
	std::vector<SampleTreeNode> nodes;
	std::vector<std::size_t> rootIndices;
};

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
	return tree;
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
void DrawSampleTreeRow(const SampleTree& tree, std::size_t nodeIndex, std::uint64_t frameIndex)
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
	ImGui::TextUnformatted(sample.threadLabel.c_str());
	ImGui::TableSetColumnIndex(3);
	ImGui::TextUnformatted(sample.taskLabel.c_str());

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hasChildren && isOpen)
	{
		for (const std::size_t childIndex : node.childIndices)
		{
			DrawSampleTreeRow(tree, childIndex, frameIndex);
		}
	}
	ImGui::PopID();
}
}

ProfilerWindow::ProfilerWindow(MainEditorSubsystem* windowOwner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Profiler", false), owner(windowOwner)
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
	if (ImGui::BeginTable("ProfilerLayout", 3, layoutFlags, ImGui::GetContentRegionAvail()))
	{
		ImGui::TableSetupColumn("Captures", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		DrawCaptureList();
		ImGui::TableSetColumnIndex(1);
		DrawFrameList();
		ImGui::TableSetColumnIndex(2);
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Record") && hasProject && !busy)
	{
		service->StartRecording();
	}
	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Pause") && hasProject && !busy)
	{
		service->PauseRecording();
	}
	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Resume") && hasProject && !busy)
	{
		service->ResumeRecording();
	}
	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Finish Capture") && hasProject && !busy)
	{
		service->FinishCapture();
	}
	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Clear") && hasProject && !busy)
	{
		service->Clear();
	}
	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Refresh List") && hasProject && !busy)
	{
		service->RefreshCaptureList();
	}
	ImGui::SameLine();
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
	ImGui::BeginChild("ProfilerCaptures", ImVec2(0, 0), true);
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
	// The rolling capture can hold thousands of frames. Clip visible rows so
	// opening the profiler does not spend every editor frame formatting labels
	// that are outside the scroll viewport.
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(frames.size()));
	while (clipper.Step())
	{
		for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
		{
			const ProfilerFrameListItem& frame = frames[static_cast<std::size_t>(rowIndex)];
			char label[128];
			std::snprintf(label, sizeof(label), "#%llu  %.2f ms%s",
				static_cast<unsigned long long>(frame.frameIndex),
				frame.durationMs,
				frame.complete ? "" : " (partial)");
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Selectable(label))
			{
				Profiler::Instance().SelectFrame(frame.frameIndex);
			}
		}
	}
	ImGui::EndChild();
}

// ProfilerWindow::DrawFrameDetails is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawFrameDetails()
{
	const auto selectedFrame = Profiler::Instance().GetSelectedFrameSnapshot();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!selectedFrame.has_value())
	{
		ImGui::TextDisabled("Select a frame to inspect.");
		return;
	}

	ImGui::Text("Frame %llu", static_cast<unsigned long long>(selectedFrame->frameIndex));
	ImGui::Text("Duration: %.3f ms", selectedFrame->durationMs);
	const ScriptingSystem::ScriptingFrameResult& scriptingResult = ScriptingSystem::Instance().GetLatestScriptingFrameResult();
	if (scriptingResult.frameId != 0 && ImGui::TreeNode("Scripting Dispatch Counters"))
	{
		ImGui::Text("Updated: %zu  Worker: %zu  Protected: %zu", scriptingResult.updatedScripts, scriptingResult.workerScripts, scriptingResult.protectedScripts);
		ImGui::Text("Cache hits: %zu  Misses: %zu  Bridge avoided: %zu", scriptingResult.cacheHits, scriptingResult.cacheMisses, scriptingResult.bridgeCallsAvoided);
		ImGui::Text("Typed writes: %zu  JSON fallbacks: %zu  Coalesced: %zu", scriptingResult.typedMutations, scriptingResult.jsonMutationFallbacks, scriptingResult.coalescedMutations);
		ImGui::TreePop();
	}
	const TerraEngine::Renderer::RenderDiagnosticsSummary renderDiagnostics = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
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
	if (!selectedFrame->complete)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "Partial frame");
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
				DrawSamples(*selectedFrame);
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}
}

// ProfilerWindow::DrawModuleSummary is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProfilerWindow::DrawModuleSummary(const ProfilerFrameRecord& frame)
{
	auto moduleIt = std::find_if(frame.moduleSummaries.begin(), frame.moduleSummaries.end(), [this](const auto& module) {
		return module.moduleId == selectedModule;
	});
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (moduleIt == frame.moduleSummaries.end() || !moduleIt->available)
	{
		ImGui::TextDisabled("No data recorded for this module.");
		return;
	}

	ImGui::Text("Module total: %.3f ms", moduleIt->totalMs);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("ProfilerModuleMetrics", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Metric");
		ImGui::TableSetupColumn("Value");
		ImGui::TableSetupColumn("Unit");
		ImGui::TableHeadersRow();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& metric : moduleIt->metrics)
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
	if (ImGui::BeginTable("ProfilerSamples", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Duration");
		ImGui::TableSetupColumn("Thread");
		ImGui::TableSetupColumn("Task");
		ImGui::TableHeadersRow();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::size_t rootIndex : tree.rootIndices)
		{
			DrawSampleTreeRow(tree, rootIndex, frame.frameIndex);
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
}
