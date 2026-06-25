// EditorMcpBridgeWindow.cpp - ImGui controls for the editor MCP bridge.
//
// This window keeps local assistant access visible. It does not execute MCP
// requests itself; it only toggles bridge policy and displays bridge state.

#include "EditorMcpBridgeWindow.h"

#include "../Automation/EditorMcpBridge.h"
#include "../Core/EditorContext.h"
#include "../Subsystems/MainEditorSubsystem.h"

#include <imgui.h>

using TerraEngine::Editor::Automation::BridgeState;
using TerraEngine::Editor::Automation::EditorMcpBridge;
using TerraEngine::Editor::Automation::ToString;
using TerraEngine::Editor::Automation::WriteMode;

EditorMcpBridgeWindow::EditorMcpBridgeWindow(MainEditorSubsystem* windowOwner)
	: EditorWindow("MCP Bridge", false), owner(windowOwner)
{
}

void EditorMcpBridgeWindow::OnGui()
{
	EditorMcpBridge* bridge = nullptr;
	if (owner != nullptr && owner->GetContext() != nullptr)
	{
		bridge = owner->GetContext()->editorMcpBridge.get();
	}
	if (bridge == nullptr)
	{
		ImGui::TextDisabled("MCP bridge is not initialized.");
		return;
	}

	bool enabled = bridge->IsEnabled();
	if (ImGui::Checkbox("Enabled", &enabled))
	{
		bridge->SetEnabled(enabled);
	}

	int modeIndex = bridge->GetWriteMode() == WriteMode::WritesEnabled ? 1 : 0;
	const char* modes[] = {"Read only", "Writes enabled"};
	if (ImGui::Combo("Write mode", &modeIndex, modes, 2))
	{
		bridge->SetWriteMode(modeIndex == 1 ? WriteMode::WritesEnabled : WriteMode::ReadOnly);
	}

	const nlohmann::json status = bridge->BuildStatusJson();
	ImGui::SeparatorText("Status");
	ImGui::Text("State: %s", status.value("state", "unknown").c_str());
	const std::string endpoint = status.value("endpoint", "");
	if (!endpoint.empty())
	{
		ImGui::Text("Endpoint: %s", endpoint.c_str());
	}
	else if (!bridge->GetTransportLastError().empty())
	{
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", bridge->GetTransportLastError().c_str());
	}
	ImGui::Text("Clients: %llu", static_cast<unsigned long long>(status.value("connectedClientCount", 0ull)));
	ImGui::Text("Pending requests: %llu", static_cast<unsigned long long>(status.value("pendingRequestCount", 0ull)));
	ImGui::Text("Pending approvals: %llu", static_cast<unsigned long long>(status.value("pendingApprovalCount", 0ull)));
	ImGui::TextWrapped("Last diagnostics: %s", status.value("lastDiagnosticRequest", "never").c_str());

	ImGui::SeparatorText("Resources");
	for (const auto& resource : bridge->ListResources())
	{
		ImGui::BulletText("%s", resource.value("name", "").c_str());
	}

	ImGui::SeparatorText("Approvals");
	const auto approvals = bridge->GetPendingApprovals();
	if (approvals.empty())
	{
		ImGui::TextDisabled("No pending approvals.");
	}
	for (const auto& approval : approvals)
	{
		ImGui::PushID(approval.approvalId.c_str());
		ImGui::TextWrapped("%s", approval.summary.c_str());
		if (ImGui::Button("Approve"))
		{
			bridge->ResolveApproval(approval.approvalId, true);
		}
		ImGui::SameLine();
		if (ImGui::Button("Deny"))
		{
			bridge->ResolveApproval(approval.approvalId, false);
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			bridge->CancelApproval(approval.approvalId);
		}
		ImGui::Separator();
		ImGui::PopID();
	}

	ImGui::SeparatorText("Export");
	const nlohmann::json diagnostics = bridge->ReadResource("terraengine://diagnostics/summary");
	for (const auto& surface : diagnostics.value("surfaces", nlohmann::json::array()))
	{
		if (surface.value("surface", "") != "export")
		{
			continue;
		}
		const auto jobs = surface.value("details", nlohmann::json::object()).value("jobs", nlohmann::json::array());
		if (jobs.empty())
		{
			ImGui::TextDisabled("No export jobs.");
		}
		for (const auto& job : jobs)
		{
			ImGui::PushID(job.value("jobId", "").c_str());
			ImGui::TextWrapped("%s [%s]", job.value("profile", "").c_str(), job.value("state", "").c_str());
			float progress = job.value("progress", 0.0f);
			ImGui::ProgressBar(progress);
			if (job.value("state", "") != "canceled" && ImGui::Button("Cancel export"))
			{
				bridge->CallTool("export.cancel", {{"jobId", job.value("jobId", "")}});
			}
			ImGui::PopID();
		}
	}

	ImGui::SeparatorText("Activity");
	const auto activity = bridge->GetRecentActivity(32);
	if (activity.empty())
	{
		ImGui::TextDisabled("No activity yet.");
	}
	for (const auto& record : activity)
	{
		ImGui::TextWrapped("#%llu %s [%s] %s",
			static_cast<unsigned long long>(record.sequence),
			record.capability.c_str(),
			record.outcome.c_str(),
			record.message.c_str());
	}
}
