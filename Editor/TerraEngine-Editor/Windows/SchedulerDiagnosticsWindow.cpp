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
#include "Engine/Core/Scheduling/WorkerSafety.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/UI/UISystem.h"

#include <cstddef>
#include <algorithm>
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

	const char* RenderViewText(RenderViewType viewType)
	{
		return ToString(viewType);
	}

	const char* UiPublishStatusText(UISystem::UIPublishStatus status)
	{
		switch (status)
		{
		case UISystem::UIPublishStatus::Published:
			return "published";
		case UISystem::UIPublishStatus::DiscardedStale:
			return "discarded stale";
		case UISystem::UIPublishStatus::FailedPreservedPrevious:
			return "failed, preserved previous";
		case UISystem::UIPublishStatus::Cancelled:
			return "cancelled";
		case UISystem::UIPublishStatus::ShutdownSkipped:
			return "shutdown skipped";
		case UISystem::UIPublishStatus::None:
		default:
			return "none";
		}
	}

	const char* WorkerSafetySeverityText(TerraEngine::Scheduling::WorkerSafetySeverity severity)
	{
		using TerraEngine::Scheduling::WorkerSafetySeverity;
		switch (severity)
		{
		case WorkerSafetySeverity::Error:
			return "error";
		case WorkerSafetySeverity::Warning:
			return "warning";
		case WorkerSafetySeverity::Info:
		default:
			return "info";
		}
	}
}

SchedulerDiagnosticsWindow::SchedulerDiagnosticsWindow(MainEditorSubsystem* windowOwner)
	: EditorWindow("Scheduler Diagnostics", false, "Diagnostics"), owner(windowOwner)
{
}

void SchedulerDiagnosticsWindow::OnGui()
{
	if (!m_IsVisible)
	{
		TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
		ScriptingSystem::SetScriptingDiagnosticsRequested(false);
		RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(false);
		UISystem::Instance().SetPreparationDiagnosticsRequested(false);
		return;
	}
	TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(true);
	ScriptingSystem::SetScriptingDiagnosticsRequested(true);
	RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(true);
	UISystem::Instance().SetPreparationDiagnosticsRequested(true);

	DrawSchedulerSummary();
	ImGui::Spacing();
	DrawWorkerCoverageSummary();
	ImGui::Spacing();
	DrawRuntimeLayoutSummary();
	ImGui::Spacing();
	DrawScriptingSummary();
	ImGui::Spacing();
	ImGui::SeparatorText("UI Preparation");
	const UISystem::UIPreparationDiagnostics uiDiagnostics = UISystem::Instance().GetLastPreparationDiagnostics();
	if (!uiDiagnostics.enabled)
	{
		ImGui::TextDisabled("Waiting for UI preparation diagnostics.");
	}
	else if (ImGui::BeginTable("SchedulerDiagnosticsUiPreparation", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Docs");
		ImGui::TableSetupColumn("Requests");
		ImGui::TableSetupColumn("Inline");
		ImGui::TableSetupColumn("Worker");
		ImGui::TableSetupColumn("Ready");
		ImGui::TableSetupColumn("Published");
		ImGui::TableSetupColumn("Discarded");
		ImGui::TableSetupColumn("Last");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%zu", uiDiagnostics.documentsVisited);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu", uiDiagnostics.requestsScheduled);
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", uiDiagnostics.inlinePreparations);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%zu", uiDiagnostics.workerPreparations);
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("%zu", uiDiagnostics.readyResultsIntegrated);
		ImGui::TableSetColumnIndex(5);
		ImGui::Text("%zu", uiDiagnostics.publishedResults);
		ImGui::TableSetColumnIndex(6);
		ImGui::Text("%zu stale / %zu failed", uiDiagnostics.staleResultsDiscarded, uiDiagnostics.failedResultsPreserved);
		ImGui::TableSetColumnIndex(7);
		ImGui::TextUnformatted(UiPublishStatusText(uiDiagnostics.lastPublishStatus));
		ImGui::EndTable();
	}
	ImGui::Spacing();
	DrawRenderGraphSummary();
	ImGui::Spacing();
	DrawEcsGraphSummaries();
}

bool SchedulerDiagnosticsWindow::OnCloseRequested()
{
	TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
	ScriptingSystem::SetScriptingDiagnosticsRequested(false);
	RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(false);
	UISystem::Instance().SetPreparationDiagnosticsRequested(false);
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
	if (ImGui::BeginTable("SchedulerDiagnosticsHealth", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Scheduler");
		ImGui::TableSetupColumn("ECS Graphs");
		ImGui::TableSetupColumn("Queries");
		ImGui::TableSetupColumn("Transactions");
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
		ImGui::Text("%llu / %llu",
			static_cast<unsigned long long>(summary.transactionDiagnostics.committedCount),
			static_cast<unsigned long long>(summary.transactionDiagnostics.submittedCount));
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Committed / submitted scene transactions. Rejected: %llu, conflicts: %llu, largest batch: %zu",
				static_cast<unsigned long long>(summary.transactionDiagnostics.rejectedCount),
				static_cast<unsigned long long>(summary.transactionDiagnostics.conflictCount),
				summary.transactionDiagnostics.largestSubmittedBatchSize);
		}
		ImGui::TableSetColumnIndex(4);
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

void SchedulerDiagnosticsWindow::DrawWorkerCoverageSummary()
{
	if (owner == nullptr || owner->GetContext() == nullptr || owner->GetContext()->engine == nullptr)
	{
		ImGui::TextDisabled("Worker coverage is not available.");
		return;
	}

	DrawParallelFrameSummary();
	DrawHardBoundarySummary();
	DrawSafetyViolationSummary();
}

void SchedulerDiagnosticsWindow::DrawParallelFrameSummary()
{
	if (owner == nullptr || owner->GetContext() == nullptr || owner->GetContext()->engine == nullptr)
	{
		ImGui::TextDisabled("Parallel frame summary is not available.");
		return;
	}

	const TerraEngine::Scheduling::WorkerCoverageReport& report = owner->GetContext()->engine->GetWorkerCoverageReport();
	const TerraEngine::Scheduling::FrameExecutionPlan& framePlan = owner->GetContext()->engine->GetParallelFrameExecutionPlan();
	const TerraEngine::Scheduling::ArchitectureMigrationSummary& migration = framePlan.migrationSummary;
	ImGui::SeparatorText("Worker Coverage");
	ImGui::Text("Frame: %s, mode: %s, parallel: %s",
		framePlan.frameId.c_str(),
		TerraEngine::Scheduling::ToString(framePlan.executionMode).c_str(),
		framePlan.parallelExecutionUsed ? "true" : "false");
	if (ImGui::BeginTable("SchedulerDiagnosticsWorkerCoverage", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Worker");
		ImGui::TableSetupColumn("Protected");
		ImGui::TableSetupColumn("Inline");
		ImGui::TableSetupColumn("Rejected");
		ImGui::TableSetupColumn("Fallback");
		ImGui::TableSetupColumn("Barriers");
		ImGui::TableSetupColumn("Worker ms");
		ImGui::TableSetupColumn("Protected ms");
		ImGui::TableSetupColumn("Util");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%zu", migration.workerSafeCount);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu", migration.protectedCount);
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", migration.inlineCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%zu", migration.rejectedCount);
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("%zu", migration.fallbackCount);
		ImGui::TableSetColumnIndex(5);
		ImGui::Text("%zu", migration.barrierCount);
		ImGui::TableSetColumnIndex(6);
		ImGui::Text("%.3f", static_cast<double>(migration.workerPreparationTimeNs) / 1000000.0);
		ImGui::TableSetColumnIndex(7);
		ImGui::Text("%.3f", static_cast<double>(migration.protectedCommitTimeNs + migration.protectedBaselineTimeNs) / 1000000.0);
		ImGui::TableSetColumnIndex(8);
		ImGui::Text("%.2f", migration.workerUtilization);
		ImGui::EndTable();
	}
	DrawStringList("Worker:", report.workerCategories);
	DrawStringList("Protected:", report.protectedCategories);
	DrawStringList("Rejected:", report.rejectedCategories);
	DrawStringList("Fallback:", report.fallbackCategories);
}

void SchedulerDiagnosticsWindow::DrawHardBoundarySummary()
{
	if (owner == nullptr || owner->GetContext() == nullptr || owner->GetContext()->engine == nullptr)
	{
		ImGui::TextDisabled("Hard-boundary summary is not available.");
		return;
	}

	const TerraEngine::Scheduling::WorkerCoverageReport& report = owner->GetContext()->engine->GetWorkerCoverageReport();
	ImGui::SeparatorText("Hard Boundaries");
	if (report.hardBoundaryFindings.empty())
	{
		ImGui::TextDisabled("No hard boundaries reported this frame.");
		return;
	}

	if (ImGui::BeginTable("SchedulerDiagnosticsHardBoundaries", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Boundary");
		ImGui::TableHeadersRow();
		for (const std::string& finding : report.hardBoundaryFindings)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextWrapped("%s", finding.c_str());
		}
		ImGui::EndTable();
	}
}

void SchedulerDiagnosticsWindow::DrawSafetyViolationSummary()
{
	ImGui::SeparatorText("Worker Safety Violations");
	const std::vector<TerraEngine::Scheduling::WorkerSafetyViolation> violations = TerraEngine::Scheduling::SnapshotWorkerSafetyViolations();
	if (violations.empty())
	{
		ImGui::TextDisabled("No direct worker mutation violations captured.");
		return;
	}

	if (ImGui::BeginTable("SchedulerDiagnosticsWorkerSafetyViolations", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Severity");
		ImGui::TableSetupColumn("Workload");
		ImGui::TableSetupColumn("Operation");
		ImGui::TableSetupColumn("Message");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Scheduling::WorkerSafetyViolation& violation : violations)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(WorkerSafetySeverityText(violation.severity));
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(violation.workloadName.c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(violation.operationName.c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::TextWrapped("%s Action: %s", violation.message.c_str(), violation.recommendedAction.c_str());
		}
		ImGui::EndTable();
	}
}

void SchedulerDiagnosticsWindow::DrawRuntimeLayoutSummary()
{
	if (owner == nullptr || owner->GetContext() == nullptr || owner->GetContext()->engine == nullptr)
	{
		ImGui::TextDisabled("Runtime layout diagnostics are not available.");
		return;
	}

	const TerraEngine::Scene::LayoutDiagnosticsSummary& summary = owner->GetContext()->engine->GetRuntimeLayoutDiagnostics();
	const TerraEngine::Renderer::RenderDiagnosticsSummary renderSummary = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
	ImGui::SeparatorText("Runtime Layout");
	if (ImGui::BeginTable("SchedulerDiagnosticsRuntimeLayout", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Authored");
		ImGui::TableSetupColumn("Active");
		ImGui::TableSetupColumn("Transforms");
		ImGui::TableSetupColumn("Render Records");
		ImGui::TableSetupColumn("Rejected");
		ImGui::TableSetupColumn("Fallbacks");
		ImGui::TableSetupColumn("Stale Refs");
		ImGui::TableSetupColumn("Small Guard");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%zu", summary.totalObjectCount);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu", summary.activeObjectCount);
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", summary.transformRecordCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%zu", renderSummary.runtimeLayoutRenderRecordCount);
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("%zu", renderSummary.runtimeLayoutRejectedRecordCount);
		ImGui::TableSetColumnIndex(5);
		ImGui::Text("%zu", summary.fallbackCount);
		ImGui::TableSetColumnIndex(6);
		ImGui::Text("%zu", summary.staleObjectHandleCount);
		ImGui::TableSetColumnIndex(7);
		ImGui::TextUnformatted(summary.smallSceneGuardTriggered ? "yes" : "no");
		ImGui::EndTable();
	}
	ImGui::Text("Fallback details: metadata scans=%zu, object facades=%zu, compact refs=%zu",
		summary.metadataScanFallbackCount,
		summary.fullObjectFacadeFallbackCount,
		summary.objectHandleFallbackCount);
	DrawStringList("Rebuild:", summary.rebuildReasons);
	DrawStringList("Render rejects:", renderSummary.runtimeLayoutRejectionReasons);
}

void SchedulerDiagnosticsWindow::DrawScriptingSummary()
{
	ImGui::SeparatorText("Scripting");
	const ScriptingSystem::ScriptingFrameResult& result = ScriptingSystem::Instance().GetLatestScriptingFrameResult();
	if (result.frameId == 0)
	{
		ImGui::TextDisabled("Waiting for scripting frame data.");
		return;
	}

	if (ImGui::BeginTable("SchedulerDiagnosticsScripting", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Frame");
		ImGui::TableSetupColumn("Updated");
		ImGui::TableSetupColumn("Worker");
		ImGui::TableSetupColumn("Protected");
		ImGui::TableSetupColumn("Mutations");
		ImGui::TableSetupColumn("Failures");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%llu", static_cast<unsigned long long>(result.frameId));
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu", result.updatedScripts);
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", result.workerScripts);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%zu", result.protectedScripts);
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("%zu accepted / %zu rejected", result.acceptedMutations, result.rejectedMutations);
		ImGui::TableSetColumnIndex(5);
		ImGui::Text("%zu", result.failedScripts);
		ImGui::EndTable();
	}

	if (ImGui::BeginTable("SchedulerDiagnosticsScriptingCaches", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Cache Hits");
		ImGui::TableSetupColumn("Cache Misses");
		ImGui::TableSetupColumn("Bridge Avoided");
		ImGui::TableSetupColumn("Typed Writes");
		ImGui::TableSetupColumn("JSON Fallbacks");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%zu", result.cacheHits);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu", result.cacheMisses);
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", result.bridgeCallsAvoided);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%zu typed / %zu coalesced", result.typedMutations, result.coalescedMutations);
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("%zu", result.jsonMutationFallbacks);
		ImGui::EndTable();
	}

	if (!result.accessReport.messages.empty() && ImGui::TreeNode("Scripting Messages"))
	{
		for (const std::string& message : result.accessReport.messages)
		{
			ImGui::TextWrapped("%s", message.c_str());
		}
		ImGui::TreePop();
	}

	const ScriptingSystem::ScriptFrameReadView& readView = ScriptingSystem::Instance().GetLatestScriptFrameReadView();
	if (!readView.scriptRecords.empty() && ImGui::TreeNode("Script Instances"))
	{
		if (ImGui::BeginTable("SchedulerDiagnosticsScriptInstances", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
		{
			ImGui::TableSetupColumn("Object");
			ImGui::TableSetupColumn("Script Type");
			ImGui::TableSetupColumn("Binding ID");
			ImGui::TableSetupColumn("Lifecycle");
			ImGui::TableSetupColumn("Version");
			ImGui::TableSetupColumn("Status");
			ImGui::TableHeadersRow();

			for (const ScriptingSystem::ScriptInstanceRecord& record : readView.scriptRecords)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				std::string objectName = "Object " + std::to_string(record.objectId);
				const entt::entity objectEntity = static_cast<entt::entity>(record.objectId);
				if (std::optional<Object> object = ObjectManager::instance().getObject(objectEntity))
				{
					objectName = ObjectManager::instance().getObjectName(*object);
				}
				ImGui::TextUnformatted(objectName.c_str());

				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(record.scriptType.c_str());

				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(record.bindingId.c_str());

				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(ScriptingSystem::FormatScriptLifecycleState(record.lifecycleState).c_str());

				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%llu / %llu", static_cast<unsigned long long>(record.authoredStateVersion), static_cast<unsigned long long>(record.runtimeObservedVersion));

				ImGui::TableSetColumnIndex(5);
				std::string status = ScriptingSystem::ClassifyScriptInstanceStatus(record);
				if (status == "Stale (Update Pending)")
				{
					ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "%s", status.c_str());
				}
				else
				{
					ImGui::TextUnformatted(status.c_str());
				}
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}
}

void SchedulerDiagnosticsWindow::DrawRenderGraphSummary()
{
	ImGui::SeparatorText("Render Graph");
	const TerraEngine::Renderer::RenderDiagnosticsSummary summary = RenderSystem::Instance().GetLatestRenderGraphDiagnostics();
	if (summary.frameId == 0u)
	{
		ImGui::TextDisabled("Waiting for render graph frame data.");
		return;
	}

	if (ImGui::BeginTable("SchedulerDiagnosticsRenderGraph", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Frame");
		ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Worker");
		ImGui::TableSetupColumn("Serial");
		ImGui::TableSetupColumn("Skipped");
		ImGui::TableSetupColumn("Failed");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%llu", static_cast<unsigned long long>(summary.frameId));
		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted(summary.valid ? "valid" : "invalid");
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", summary.workerPacketCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%zu", summary.serialPacketCount);
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("%zu", summary.skippedPacketCount);
		ImGui::TableSetColumnIndex(5);
		ImGui::Text("%zu", summary.failedPacketCount);
		ImGui::EndTable();
	}

	if (!summary.topPassCosts.empty() && ImGui::TreeNode("Top Pass Costs"))
	{
		const std::size_t visibleCount = std::min<std::size_t>(3u, summary.topPassCosts.size());
		for (std::size_t index = 0u; index < visibleCount; ++index)
		{
			const TerraEngine::Renderer::RenderPassTiming& timing = summary.topPassCosts[index];
			ImGui::Text("%s: %.3f ms", timing.passId.c_str(), timing.milliseconds);
		}
		ImGui::TreePop();
	}

	if ((!summary.backendFrameBreakdown.samples.empty()
		|| !summary.backendFrameBreakdown.stageTimings.empty()
		|| !summary.backendStalls.empty()
		|| !summary.viewSubmitWork.empty())
		&& ImGui::TreeNode("Backend Frame Breakdown"))
	{
		if (ImGui::BeginTable("SchedulerDiagnosticsRenderBackendBreakdown", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Category");
			ImGui::TableSetupColumn("View");
			ImGui::TableSetupColumn("Time");
			ImGui::TableSetupColumn("Wait");
			ImGui::TableSetupColumn("Scope");
			ImGui::TableHeadersRow();
			for (const TerraEngine::Renderer::BackendFrameCostSample& sample : summary.backendFrameBreakdown.samples)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(sample.label.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(RenderViewText(sample.viewType));
				ImGui::TableSetColumnIndex(2);
				if (sample.unavailable)
				{
					ImGui::TextDisabled("unavailable");
				}
				else
				{
					ImGui::Text("%.3f ms", sample.milliseconds);
				}
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(sample.wait ? "yes" : "no");
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(sample.shared ? "shared" : "view");
			}
			ImGui::EndTable();
		}

		if (!summary.backendFrameBreakdown.stageTimings.empty() && ImGui::TreeNode("Backend Submit Stages"))
		{
			if (ImGui::BeginTable("SchedulerDiagnosticsBackendSubmitStages", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Stage");
				ImGui::TableSetupColumn("View");
				ImGui::TableSetupColumn("View Id");
				ImGui::TableSetupColumn("Time");
				ImGui::TableSetupColumn("Thread");
				ImGui::TableSetupColumn("Backend");
				ImGui::TableHeadersRow();
				for (const TerraEngine::Renderer::BackendStageTiming& timing : summary.backendFrameBreakdown.stageTimings)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(timing.label.empty() ? TerraEngine::Renderer::ToString(timing.stage) : timing.label.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(RenderViewText(timing.viewType));
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%u", timing.viewId);
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%.3f ms", timing.milliseconds);
					ImGui::TableSetColumnIndex(4);
					ImGui::Text("%u", timing.threadId);
					ImGui::TableSetColumnIndex(5);
					ImGui::TextUnformatted(timing.backendId.c_str());
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}

		if (!summary.backendStalls.empty() && ImGui::TreeNode("Backend Stalls"))
		{
			if (ImGui::BeginTable("SchedulerDiagnosticsBackendStalls", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Severity");
				ImGui::TableSetupColumn("Type");
				ImGui::TableSetupColumn("Stage");
				ImGui::TableSetupColumn("View Id");
				ImGui::TableSetupColumn("Time");
				ImGui::TableSetupColumn("Action");
				ImGui::TableHeadersRow();
				for (const TerraEngine::Renderer::BackendStallDiagnostic& stall : summary.backendStalls)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(TerraEngine::Renderer::ToString(stall.severity));
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(TerraEngine::Renderer::ToString(stall.waitType));
					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(TerraEngine::Renderer::ToString(stall.owningStage));
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%u", stall.viewId);
					ImGui::TableSetColumnIndex(4);
					ImGui::Text("%.3f / %.3f ms", stall.milliseconds, stall.thresholdMilliseconds);
					ImGui::TableSetColumnIndex(5);
					ImGui::TextWrapped("%s", stall.recommendedAction.c_str());
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}

		if (!summary.viewSubmitWork.empty() && ImGui::TreeNode("Scene/Game Submit Reuse"))
		{
			if (ImGui::BeginTable("SchedulerDiagnosticsViewSubmitReuse", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("View");
				ImGui::TableSetupColumn("View Id");
				ImGui::TableSetupColumn("Recording");
				ImGui::TableSetupColumn("Submit");
				ImGui::TableSetupColumn("Decision");
				ImGui::TableSetupColumn("Key");
				ImGui::TableSetupColumn("Reason");
				ImGui::TableHeadersRow();
				for (const TerraEngine::Renderer::ViewSubmitWorkItem& item : summary.viewSubmitWork)
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(RenderViewText(item.viewType));
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%u", item.viewId);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%.3f ms", item.recordingMilliseconds);
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%.3f ms", item.submitMilliseconds);
					ImGui::TableSetColumnIndex(4);
					ImGui::TextUnformatted(TerraEngine::Renderer::ToString(item.reuseDecision));
					ImGui::TableSetColumnIndex(5);
					ImGui::TextUnformatted(item.sharedWorkKey.c_str());
					ImGui::TableSetColumnIndex(6);
					ImGui::TextWrapped("%s", item.reuseReason.c_str());
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}

	if (!summary.renderWorkerGroups.empty() && ImGui::TreeNode("Render Worker Groups"))
	{
		if (ImGui::BeginTable("SchedulerDiagnosticsRenderWorkerGroups", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Group");
			ImGui::TableSetupColumn("Work");
			ImGui::TableSetupColumn("Workers");
			ImGui::TableSetupColumn("Serial");
			ImGui::TableSetupColumn("State");
			ImGui::TableHeadersRow();
			for (const TerraEngine::Renderer::RenderWorkerTaskGroup& group : summary.renderWorkerGroups)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(group.label.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%zu", group.requestedWorkItems);
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%zu", group.workerCount);
				ImGui::TableSetColumnIndex(3);
				ImGui::Text("%zu", group.serialCount);
				ImGui::TableSetColumnIndex(4);
				ImGui::TextWrapped("%s", group.cancelled ? group.cancellationReason.c_str() : "active");
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}

	if (!summary.budgetDiagnostics.empty() && ImGui::TreeNode("Render Budgets"))
	{
		if (ImGui::BeginTable("SchedulerDiagnosticsRenderBudgets", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Category");
			ImGui::TableSetupColumn("Usage");
			ImGui::TableSetupColumn("Limit");
			ImGui::TableSetupColumn("Policy");
			ImGui::TableSetupColumn("State");
			ImGui::TableHeadersRow();
			for (const TerraEngine::Renderer::RenderBudgetDiagnostic& budget : summary.budgetDiagnostics)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(budget.label.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%zu", budget.currentUsage);
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%zu", budget.limit);
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(TerraEngine::Renderer::ToString(budget.overflowPolicy).c_str());
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(budget.overBudget ? "over" : "ok");
			}
			ImGui::EndTable();
		}
		ImGui::TreePop();
	}

	if (!summary.queueReadiness.empty() && ImGui::TreeNode("Render Queues"))
	{
		for (const TerraEngine::Renderer::RenderQueueReadiness& readiness : summary.queueReadiness)
		{
			ImGui::TextWrapped("%s: %s%s %s",
				TerraEngine::Renderer::ToString(readiness.category).c_str(),
				TerraEngine::Renderer::ToString(readiness.mode).c_str(),
				readiness.handoffRequired ? " handoff" : "",
				readiness.fallbackReason.c_str());
		}
		ImGui::TreePop();
	}

	if (!summary.performanceGates.empty() && ImGui::TreeNode("Performance Gates"))
	{
		for (const TerraEngine::Renderer::RenderPerformanceGateResult& gate : summary.performanceGates)
		{
			ImGui::TextWrapped("%s: %s (baseline %.3f, latest %.3f, threshold %.1f%%)",
				gate.workloadName.c_str(),
				gate.passed ? "pass" : "fail",
				gate.baselineValue,
				gate.latestValue,
				gate.thresholdPercent);
		}
		ImGui::TreePop();
	}

	if (!summary.serializedReasons.empty() && ImGui::TreeNode("Serialized Work"))
	{
		for (const std::string& reason : summary.serializedReasons)
		{
			ImGui::TextWrapped("%s", reason.c_str());
		}
		ImGui::TreePop();
	}

	if (!summary.resourceWarnings.empty() && ImGui::TreeNode("Resource Warnings"))
	{
		for (const std::string& warning : summary.resourceWarnings)
		{
			ImGui::TextWrapped("%s", warning.c_str());
		}
		ImGui::TreePop();
	}

	if (!summary.backendFallbacks.empty() && ImGui::TreeNode("Backend Details"))
	{
		for (const std::string& fallback : summary.backendFallbacks)
		{
			ImGui::TextWrapped("%s", fallback.c_str());
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
