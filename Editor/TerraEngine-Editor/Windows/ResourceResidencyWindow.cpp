// ResourceResidencyWindow.cpp - ImGui surface for loaded resource residency.
//
// The panel reports RAM/VRAM residency decisions made by ResourceManager. It
// deliberately avoids persistent backend disk cache concepts because this spec
// only controls live loaded assets that can leave CPU/GPU memory.

#include "ResourceResidencyWindow.h"

#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Resource/ResourceResidency.h"

#include <imgui.h>
#include <string>
#include <string_view>

namespace
{
	const char* BoolText(bool value)
	{
		return value ? "yes" : "no";
	}

	void TextStringView(std::string_view value)
	{
		ImGui::TextUnformatted(value.data(), value.data() + value.size());
	}
}

ResourceResidencyWindow::ResourceResidencyWindow(MainEditorSubsystem* windowOwner)
	: EditorWindow("Resource Residency", false), owner(windowOwner)
{
}

void ResourceResidencyWindow::OnGui()
{
	if (!m_IsVisible)
	{
		ResourceManager::Instance().SetResidencyDiagnosticsEnabled(false);
		return;
	}

	ResourceManager::Instance().SetResidencyDiagnosticsEnabled(true);
	DrawSummary();
	ImGui::Spacing();
	DrawCandidates();
	ImGui::Spacing();
	DrawReleaseRecords();
}

bool ResourceResidencyWindow::OnCloseRequested()
{
	ResourceManager::Instance().SetResidencyDiagnosticsEnabled(false);
	return true;
}

void ResourceResidencyWindow::DrawSummary()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary summary = ResourceManager::Instance().GetResidencyDiagnosticsSummary();
	const TerraEngine::Resource::ResidencyPolicy policy = ResourceManager::Instance().GetResidencyPolicy();
	ImGui::SeparatorText("Summary");
	if (ImGui::BeginTable("ResourceResidencySummary", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Resident");
		ImGui::TableSetupColumn("Active");
		ImGui::TableSetupColumn("Pinned");
		ImGui::TableSetupColumn("Bytes");
		ImGui::TableSetupColumn("Policy");
		ImGui::TableSetupColumn("Reason");
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%zu", summary.residentAssetCount);
		ImGui::TableSetColumnIndex(1);
		ImGui::Text("%zu", summary.activeAssetCount);
		ImGui::TableSetColumnIndex(2);
		ImGui::Text("%zu", summary.pinnedAssetCount);
		ImGui::TableSetColumnIndex(3);
		ImGui::Text("%llu", static_cast<unsigned long long>(summary.estimatedResidentBytes));
		ImGui::TableSetColumnIndex(4);
		ImGui::Text("enabled=%s diagnostics=%s", BoolText(policy.enabled), BoolText(summary.diagnosticsEnabled));
		ImGui::TableSetColumnIndex(5);
		TextStringView(TerraEngine::Resource::ToString(summary.cleanupReason));
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawCandidates()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary summary = ResourceManager::Instance().GetResidencyDiagnosticsSummary();
	ImGui::SeparatorText("Release Candidates");
	if (summary.candidates.empty())
	{
		ImGui::TextDisabled("No candidates.");
		return;
	}

	if (ImGui::BeginTable("ResourceResidencyCandidates", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Asset");
		ImGui::TableSetupColumn("Type");
		ImGui::TableSetupColumn("Idle Seconds");
		ImGui::TableSetupColumn("Bytes");
		ImGui::TableSetupColumn("Reason");
		ImGui::TableSetupColumn("Source");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Resource::ReleaseCandidate& candidate : summary.candidates)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			const std::string id = TerraEngine::ToHexString(candidate.assetId);
			ImGui::TextUnformatted(id.c_str());
			ImGui::TableSetColumnIndex(1);
			TextStringView(TerraEngine::Resource::ToString(candidate.assetType));
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%.2f", candidate.idleSeconds);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%llu", static_cast<unsigned long long>(candidate.estimatedReclaimableBytes));
			ImGui::TableSetColumnIndex(4);
			TextStringView(TerraEngine::Resource::ToString(candidate.releaseReason));
			ImGui::TableSetColumnIndex(5);
			ImGui::TextWrapped("%s", candidate.sourceKey.c_str());
		}
		ImGui::EndTable();
	}
}

void ResourceResidencyWindow::DrawReleaseRecords()
{
	const TerraEngine::Resource::ResidencyDiagnosticsSummary summary = ResourceManager::Instance().GetResidencyDiagnosticsSummary();
	ImGui::SeparatorText("Releases");
	if (summary.records.empty())
	{
		ImGui::TextDisabled("No release records.");
		return;
	}

	if (ImGui::BeginTable("ResourceResidencyRecords", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
	{
		ImGui::TableSetupColumn("Frame");
		ImGui::TableSetupColumn("Asset");
		ImGui::TableSetupColumn("Type");
		ImGui::TableSetupColumn("Result");
		ImGui::TableSetupColumn("Bytes");
		ImGui::TableSetupColumn("Explanation");
		ImGui::TableHeadersRow();
		for (const TerraEngine::Resource::ReleaseRecord& record : summary.records)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.frameId));
			ImGui::TableSetColumnIndex(1);
			const std::string id = TerraEngine::ToHexString(record.assetId);
			ImGui::TextUnformatted(id.c_str());
			ImGui::TableSetColumnIndex(2);
			TextStringView(TerraEngine::Resource::ToString(record.assetType));
			ImGui::TableSetColumnIndex(3);
			TextStringView(TerraEngine::Resource::ToString(record.result));
			ImGui::TableSetColumnIndex(4);
			ImGui::Text("%llu", static_cast<unsigned long long>(record.bytesReclaimed));
			ImGui::TableSetColumnIndex(5);
			ImGui::TextWrapped("%s", record.explanation.c_str());
		}
		ImGui::EndTable();
	}
}
