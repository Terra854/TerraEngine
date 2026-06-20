// SchedulerDiagnosticsWindow.cpp - ImGui diagnostics for frame scheduler metadata.
//
// This panel is the user-facing control surface for the expensive ECS graph
// diagnostics path. The runtime keeps diagnostics off until this window is
// visible, then publishes compact summaries that are cheap for the editor to
// render as tables.

#include "SchedulerDiagnosticsWindow.h"

#include "../Core/EditorContext.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Scheduling/EcsQueryTaskGraph.h"
#include "Engine/Core/Scheduling/SystemScheduler.h"

#include <cstddef>
#include <imgui.h>
#include <string>
#include <vector>

namespace
{
	struct EcsDiagnosticsTotals
	{
		std::size_t graphCount = 0;
		std::size_t queryCount = 0;
		std::size_t diagnosticCount = 0;
		bool allGraphsValid = true;
		bool allGraphsCompleted = true;
	};

	EcsDiagnosticsTotals CountEcsDiagnostics(const std::vector<TerraEngine::Scheduling::EcsSystemGraphFrameSummary>& summaries)
	{
		EcsDiagnosticsTotals totals;
		totals.graphCount = summaries.size();
		for (const TerraEngine::Scheduling::EcsSystemGraphFrameSummary& summary : summaries)
		{
			totals.queryCount += summary.queryPlans.size();
			totals.diagnosticCount += summary.diagnostics.size();
			totals.allGraphsValid = totals.allGraphsValid && summary.valid;
			totals.allGraphsCompleted = totals.allGraphsCompleted && summary.completed;
		}
		return totals;
	}

	ImVec4 DiagnosticColor(TerraEngine::Scheduling::ComponentAccessDiagnosticSeverity severity)
	{
		using TerraEngine::Scheduling::ComponentAccessDiagnosticSeverity;
		switch (severity)
		{
		case ComponentAccessDiagnosticSeverity::Error:
			return ImVec4(1.0f, 0.35f, 0.25f, 1.0f);
		case ComponentAccessDiagnosticSeverity::Warning:
			return ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
		case ComponentAccessDiagnosticSeverity::Info:
		default:
			return ImVec4(0.55f, 0.72f, 1.0f, 1.0f);
		}
	}

	void DrawStringList(const char* label, const std::vector<std::string>& values)
	{
		ImGui::TextUnformatted(label);
		ImGui::SameLine();
		if (values.empty())
		{
			ImGui::TextDisabled("none");
			return;
		}

		std::string joined;
		for (const std::string& value : values)
		{
			if (!joined.empty())
			{
				joined += ", ";
			}
			joined += value;
		}
		ImGui::TextWrapped("%s", joined.c_str());
	}

	const char* QueryWorkText(const TerraEngine::Scheduling::EcsComponentQueryPlan& plan)
	{
		if (plan.declaration.runOnceWithoutObjectSnapshot)
		{
			return "direct scan";
		}
		if (plan.executionPolicy == TerraEngine::Scheduling::EcsQueryExecutionPolicy::WorkerTaskGroup)
		{
			return "chunked";
		}
		return "inline";
	}
}

SchedulerDiagnosticsWindow::SchedulerDiagnosticsWindow(MainEditorSubsystem* windowOwner)
	: EditorWindow("Scheduler Diagnostics", false), owner(windowOwner)
{
}

void SchedulerDiagnosticsWindow::OnGui()
{
	if (!m_IsVisible)
	{
		TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
		return;
	}
	TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(true);

	DrawSchedulerSummary();
	ImGui::Spacing();
	DrawEcsGraphSummaries();
}

bool SchedulerDiagnosticsWindow::OnCloseRequested()
{
	TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
	return true;
}

void SchedulerDiagnosticsWindow::DrawSchedulerSummary()
{
	if (owner == nullptr || owner->GetContext() == nullptr || owner->GetContext()->engine == nullptr)
	{
		ImGui::TextDisabled("Engine is not available.");
		return;
	}

	const TerraEngine::Scheduling::ScheduleSummary& summary = owner->GetContext()->engine->GetSchedulerSummary();
	const std::vector<TerraEngine::Scheduling::EcsSystemGraphFrameSummary> ecsSummaries = TerraEngine::Scheduling::PublishedEcsGraphFrameSummaries();
	const EcsDiagnosticsTotals totals = CountEcsDiagnostics(ecsSummaries);

	ImGui::SeparatorText("Health");
	if (ImGui::BeginTable("SchedulerDiagnosticsHealth", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Scheduler");
		ImGui::TableSetupColumn("ECS Graphs");
		ImGui::TableSetupColumn("Queries");
		ImGui::TableSetupColumn("Diagnostics");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(summary.valid ? "Valid" : "Invalid");
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu %s", totals.graphCount, totals.allGraphsValid && totals.allGraphsCompleted ? "valid" : "needs attention");
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", totals.queryCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%zu", summary.diagnostics.size() + totals.diagnosticCount);
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Diagnostics");
	if (summary.diagnostics.empty() && totals.diagnosticCount == 0)
	{
		ImGui::TextDisabled("No issues.");
	}

	if (!summary.diagnostics.empty() && ImGui::BeginTable("SchedulerDiagnosticsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Severity");
		ImGui::TableSetupColumn("Category");
		ImGui::TableSetupColumn("System");
		ImGui::TableSetupColumn("Resource");
		ImGui::TableSetupColumn("Message");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Scheduling::ScheduleDiagnostic& diagnostic : summary.diagnostics)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(TerraEngine::Scheduling::ToString(diagnostic.severity).c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(TerraEngine::Scheduling::ToString(diagnostic.category).c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(diagnostic.systemName.c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(diagnostic.resourceName.c_str());
			ImGui::TableSetColumnIndex(4);
			ImGui::TextWrapped("%s Action: %s", diagnostic.message.c_str(), diagnostic.recommendedAction.c_str());
		}
		ImGui::EndTable();
	}

	for (const TerraEngine::Scheduling::EcsSystemGraphFrameSummary& ecsSummary : ecsSummaries)
	{
		for (const TerraEngine::Scheduling::EcsRaceDiagnostic& diagnostic : ecsSummary.diagnostics)
		{
			ImGui::TextColored(DiagnosticColor(diagnostic.severity), "%s/%s", TerraEngine::Scheduling::ToString(diagnostic.severity).c_str(), diagnostic.code.c_str());
			ImGui::SameLine();
			ImGui::TextWrapped("%s %s", diagnostic.queryName.c_str(), diagnostic.details.c_str());
		}
	}

	if (ImGui::TreeNode("Scheduler Details"))
	{
		DrawStringList("Phases:", summary.phases);
		DrawStringList("Systems:", summary.systems);
		DrawStringList("Main thread:", summary.mainThreadSystems);
		DrawStringList("Query groups:", summary.componentQueryGroups);
		if (!summary.ecsGraphSummary.empty())
		{
			ImGui::TextWrapped("ECS graph: %s", summary.ecsGraphSummary.c_str());
		}
		ImGui::TreePop();
	}
}

void SchedulerDiagnosticsWindow::DrawEcsGraphSummaries()
{
	ImGui::SeparatorText("ECS Query Graphs");
	const std::vector<TerraEngine::Scheduling::EcsSystemGraphFrameSummary> summaries = TerraEngine::Scheduling::PublishedEcsGraphFrameSummaries();
	if (summaries.empty())
	{
		ImGui::TextDisabled("Waiting for frame data.");
		return;
	}

	for (const TerraEngine::Scheduling::EcsSystemGraphFrameSummary& summary : summaries)
	{
		const std::string graphId = summary.jobGraphSummary.graphId.empty() ? "ecs-query-task-graph" : summary.jobGraphSummary.graphId;
		const std::string label = graphId + "  (" + std::to_string(summary.queryPlans.size()) + " queries, " + std::to_string(summary.diagnostics.size()) + " diagnostics)";
		if (!ImGui::TreeNode(label.c_str()))
		{
			continue;
		}

		ImGui::Text("State: %s, %s", summary.valid ? "valid" : "invalid", summary.completed ? "completed" : "incomplete");
		if (ImGui::BeginTable((graphId + "Queries").c_str(), 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
		{
			ImGui::TableSetupColumn("Query");
			ImGui::TableSetupColumn("Policy");
			ImGui::TableSetupColumn("Work");
			ImGui::TableSetupColumn("Invalidated");
			ImGui::TableSetupColumn("Access");
			ImGui::TableHeadersRow();
			for (const TerraEngine::Scheduling::EcsComponentQueryPlan& plan : summary.queryPlans)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(plan.declaration.queryName.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(TerraEngine::Scheduling::ToString(plan.executionPolicy).c_str());
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(QueryWorkText(plan));
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(plan.invalidatedByMutation ? "yes" : "no");
				ImGui::TableSetColumnIndex(4);
				if (plan.declaration.access.workerEligible)
				{
					ImGui::TextUnformatted("worker");
				}
				else
				{
					ImGui::TextWrapped("main thread: %s", plan.declaration.access.mainThreadReason.c_str());
				}
			}
			ImGui::EndTable();
		}

		if (summary.diagnostics.empty())
		{
			ImGui::TextDisabled("No ECS graph diagnostics.");
		}
		else
		{
			for (const TerraEngine::Scheduling::EcsRaceDiagnostic& diagnostic : summary.diagnostics)
			{
				ImGui::TextColored(DiagnosticColor(diagnostic.severity), "%s/%s", TerraEngine::Scheduling::ToString(diagnostic.severity).c_str(), diagnostic.code.c_str());
				ImGui::SameLine();
				ImGui::TextWrapped("%s %s", diagnostic.queryName.c_str(), diagnostic.details.c_str());
			}
		}
		ImGui::TreePop();
	}
}
