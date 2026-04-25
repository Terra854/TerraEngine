#include "ProfilerWindow.h"

#include "../Core/EditorContext.h"
#include "../Profiling/EditorProfilerService.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Profiling/Profiler.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <unordered_map>

using TerraEngine::Profiling::Profiler;
using TerraEngine::Profiling::ProfilerFrameRecord;
using TerraEngine::Profiling::ProfilerModuleId;
using TerraEngine::Profiling::ProfilerSample;
using TerraEngine::Profiling::ProfilerSessionState;

namespace
{
struct SampleTreeNode
{
	std::size_t sampleIndex = 0u;
	std::vector<std::size_t> childIndices;
};

struct SampleTree
{
	std::vector<ProfilerSample> samples;
	std::vector<SampleTreeNode> nodes;
	std::vector<std::size_t> rootIndices;
};

SampleTree BuildSampleTree(const ProfilerFrameRecord& frame, ProfilerModuleId selectedModule)
{
	SampleTree tree;
	// Keep the table useful for timing inspection while rebuilding the parent
	// links that make nested profiler scopes readable at a glance.
	for (const ProfilerSample& sample : frame.samples)
	{
		if (sample.moduleId == selectedModule)
		{
			tree.samples.push_back(sample);
		}
	}

	std::sort(tree.samples.begin(), tree.samples.end(), [](const ProfilerSample& left, const ProfilerSample& right) {
		if (left.startMs == right.startMs)
		{
			return left.depth < right.depth;
		}
		return left.startMs < right.startMs;
	});

	tree.nodes.resize(tree.samples.size());
	std::unordered_map<std::uint64_t, std::size_t> sampleIndexById;
	sampleIndexById.reserve(tree.samples.size());
	for (std::size_t index = 0u; index < tree.samples.size(); ++index)
	{
		tree.nodes[index].sampleIndex = index;
		sampleIndexById.emplace(tree.samples[index].sampleId, index);
	}

	for (std::size_t index = 0u; index < tree.samples.size(); ++index)
	{
		const ProfilerSample& sample = tree.samples[index];
		const auto parentIt = sampleIndexById.find(sample.parentSampleId);
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

bool DrawDisclosureControl(bool hasChildren, bool isOpen)
{
	constexpr float kArrowSlotWidth = 7.0f;
	constexpr float kArrowSize = 4.0f;
	const float rowHeight = ImGui::GetTextLineHeight();
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
	if (hasChildren)
	{
		ImGuiStorage* storage = ImGui::GetStateStorage();
		const ImGuiID openStateId = ImGui::GetID("SampleOpen");
		isOpen = storage->GetBool(openStateId, true);
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
	: EditorWindow("Profiler", false), owner(windowOwner)
{
}

void ProfilerWindow::OnGui()
{
	DrawToolbar();
	ImGui::Separator();

	EditorProfilerService* service = nullptr;
	if (owner && owner->GetContext())
	{
		service = owner->GetContext()->editorProfiler.get();
	}
	if (service == nullptr || !service->HasActiveProject())
	{
		ImGui::TextDisabled("Open a project to use the profiler.");
		return;
	}

	const std::string status = service->GetStatusText();
	ImGui::Text("Status: %s", status.c_str());
	const std::string error = service->GetLastError();
	if (!error.empty())
	{
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", error.c_str());
	}

	constexpr ImGuiTableFlags layoutFlags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings;
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

void ProfilerWindow::DrawToolbar()
{
	EditorProfilerService* service = owner && owner->GetContext() ? owner->GetContext()->editorProfiler.get() : nullptr;
	const bool hasService = service != nullptr;
	const bool hasProject = hasService && service->HasActiveProject();
	const bool busy = hasService && service->IsBusy();
	const ProfilerSessionState state = Profiler::Instance().GetState();

	if (ImGui::Button("Record") && hasProject && !busy)
	{
		service->StartRecording();
	}
	ImGui::SameLine();
	if (ImGui::Button("Pause") && hasProject && !busy)
	{
		service->PauseRecording();
	}
	ImGui::SameLine();
	if (ImGui::Button("Resume") && hasProject && !busy)
	{
		service->ResumeRecording();
	}
	ImGui::SameLine();
	if (ImGui::Button("Finish Capture") && hasProject && !busy)
	{
		service->FinishCapture();
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear") && hasProject && !busy)
	{
		service->Clear();
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh List") && hasProject && !busy)
	{
		service->RefreshCaptureList();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%s", TerraEngine::Profiling::ToString(state));
}

void ProfilerWindow::DrawCaptureList()
{
	EditorProfilerService* service = owner && owner->GetContext() ? owner->GetContext()->editorProfiler.get() : nullptr;
	if (service == nullptr)
	{
		return;
	}

	ImGui::TextUnformatted("Project Captures");
	ImGui::BeginChild("ProfilerCaptures", ImVec2(0, 0), true);
	const auto captures = service->GetCaptureList();
	if (captures.empty())
	{
		ImGui::TextDisabled("No captures in %s", service->GetProfilerDirectory().string().c_str());
	}
	for (const auto& capture : captures)
	{
		if (ImGui::Selectable(capture.fileName.c_str()))
		{
			service->LoadCapture(capture.absolutePath);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%llu frames\n%s", static_cast<unsigned long long>(capture.frameCount), capture.metadata.savedAt.c_str());
		}
	}
	ImGui::EndChild();
}

void ProfilerWindow::DrawFrameList()
{
	ImGui::TextUnformatted("Frames");
	ImGui::BeginChild("ProfilerFrames", ImVec2(0, 0), true);
	const auto frames = Profiler::Instance().GetFramesSnapshot();
	if (frames.empty())
	{
		ImGui::TextDisabled("No frames captured.");
	}
	for (const ProfilerFrameRecord& frame : frames)
	{
		char label[128];
		std::snprintf(label, sizeof(label), "#%llu  %.2f ms%s",
			static_cast<unsigned long long>(frame.frameIndex),
			frame.durationMs,
			frame.complete ? "" : " (partial)");
		if (ImGui::Selectable(label))
		{
			Profiler::Instance().SelectFrame(frame.frameIndex);
		}
	}
	ImGui::EndChild();
}

void ProfilerWindow::DrawFrameDetails()
{
	const auto selectedFrame = Profiler::Instance().GetSelectedFrameSnapshot();
	if (!selectedFrame.has_value())
	{
		ImGui::TextDisabled("Select a frame to inspect.");
		return;
	}

	ImGui::Text("Frame %llu", static_cast<unsigned long long>(selectedFrame->frameIndex));
	ImGui::Text("Duration: %.3f ms", selectedFrame->durationMs);
	if (!selectedFrame->complete)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "Partial frame");
	}

	if (ImGui::BeginTabBar("ProfilerModules"))
	{
		for (ProfilerModuleId moduleId : {ProfilerModuleId::CpuEngine, ProfilerModuleId::TaskExecution})
		{
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

void ProfilerWindow::DrawModuleSummary(const ProfilerFrameRecord& frame)
{
	auto moduleIt = std::find_if(frame.moduleSummaries.begin(), frame.moduleSummaries.end(), [this](const auto& module) {
		return module.moduleId == selectedModule;
	});
	if (moduleIt == frame.moduleSummaries.end() || !moduleIt->available)
	{
		ImGui::TextDisabled("No data recorded for this module.");
		return;
	}

	ImGui::Text("Module total: %.3f ms", moduleIt->totalMs);
	if (ImGui::BeginTable("ProfilerModuleMetrics", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Metric");
		ImGui::TableSetupColumn("Value");
		ImGui::TableSetupColumn("Unit");
		ImGui::TableHeadersRow();
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

void ProfilerWindow::DrawSamples(const ProfilerFrameRecord& frame)
{
	ImGui::SeparatorText("Samples");
	const SampleTree tree = BuildSampleTree(frame, selectedModule);
	if (tree.samples.empty())
	{
		ImGui::TextDisabled("No samples for this module.");
		return;
	}

	const ImVec2 originalCellPadding = ImGui::GetStyle().CellPadding;
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1.0f, originalCellPadding.y));
	if (ImGui::BeginTable("ProfilerSamples", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Duration");
		ImGui::TableSetupColumn("Thread");
		ImGui::TableSetupColumn("Task");
		ImGui::TableHeadersRow();
		for (const std::size_t rootIndex : tree.rootIndices)
		{
			DrawSampleTreeRow(tree, rootIndex, frame.frameIndex);
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
}
