// PreferencesWindow.cpp - Unified Preferences editor implementation.
//
// Hosts editor-wide settings that should follow the user instead of the active
// project. Project Settings remains responsible for project/runtime data.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "PreferencesWindow.h"

#include "../Automation/EditorMcpBridge.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>

using TerraEngine::Editor::Automation::EditorMcpBridge;
using TerraEngine::Editor::Automation::WriteMode;

namespace
{
	constexpr float kCategoryListWidth = 220.0f;
	constexpr int kSearchBufferSize = 128;

	void CopyStringToBuffer(const std::string& source, char* buffer, std::size_t bufferSize)
	{
		std::memset(buffer, 0, bufferSize);
		std::memcpy(buffer, source.c_str(), std::min(source.size(), bufferSize - 1));
	}

	void ShowOptionTooltip(const char* description)
	{
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", description);
		}
	}

	void ApplyRendererDiagnosticsToRenderSystem(bool vulkanValidationLayers, bool renderDebugDiagnostics)
	{
		// Preferences are per-user editor state. Apply them through the
		// transient renderer path so the active project RenderSettings.json is
		// not rewritten when a user changes local diagnostics.
		RenderSystem::Instance().ApplyTransientRenderDiagnostics(vulkanValidationLayers, renderDebugDiagnostics);
	}
}

PreferencesWindow::PreferencesWindow(MainEditorSubsystem* owner)
	: EditorWindow(std::string(TerraEngine::Editor::Preferences::kPreferencesWindowTitle), false), m_Owner(owner)
{
}

void PreferencesWindow::OpenToCategory(TerraEngine::Editor::Preferences::PreferencesCategoryId categoryId)
{
	m_SelectedCategory = categoryId;
	SetVisible(true);
}

void PreferencesWindow::ApplyStoredPreferencesToRenderSystem()
{
	if (m_Owner == nullptr || m_Owner->GetContext() == nullptr || m_Owner->GetContext()->projectManager == nullptr)
	{
		return;
	}

	ProjectManager* projectManager = m_Owner->GetContext()->projectManager.get();
	const EditorPreferences& preferences = projectManager->GetEditorPreferences();
	RenderSettings settings = RenderSystem::Instance().GetRenderSettings();

	if (!preferences.rendererDiagnosticsPreferencesInitialized)
	{
		// First run after upgrading from the old Project Settings location: keep
		// the loaded project render flags as the initial editor preference so an
		// existing local setup does not silently flip off.
		const bool vulkanValidationLayers = settings.validationEnabled > 0u;
		const bool renderDebugDiagnostics = settings.debugEnabled > 0u;
		projectManager->SetRendererDiagnosticPreferences(vulkanValidationLayers, renderDebugDiagnostics);
		ApplyRendererDiagnosticsToRenderSystem(vulkanValidationLayers, renderDebugDiagnostics);
		return;
	}

	ApplyRendererDiagnosticsToRenderSystem(preferences.vulkanValidationLayers, preferences.renderDebugDiagnostics);
}

void PreferencesWindow::OnBeforeBegin()
{
	// Preferences is a transient editor-owned surface rather than a docked
	// workspace panel. Give every new appearance a usable centered rectangle so
	// a first open cannot collapse to the table's zero-content startup size.
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 preferredSize(
		std::min(900.0f, viewport->WorkSize.x * 0.75f),
		std::min(650.0f, viewport->WorkSize.y * 0.75f));
	const ImVec2 center(
		viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
		viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
	ImGui::SetNextWindowSize(preferredSize, ImGuiCond_Appearing);
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

void PreferencesWindow::OnGui()
{
	constexpr ImGuiTableFlags layoutFlags =
		ImGuiTableFlags_SizingFixedFit |
		ImGuiTableFlags_NoSavedSettings;

	if (ImGui::BeginTable("PreferencesLayout", 2, layoutFlags, ImGui::GetContentRegionAvail()))
	{
		ImGui::TableSetupColumn("Categories", ImGuiTableColumnFlags_WidthFixed, kCategoryListWidth);
		ImGui::TableSetupColumn("Settings", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		DrawCategorySidebar();

		ImGui::TableSetColumnIndex(1);
		ImGui::BeginChild("PreferencesDetails", ImVec2(0, 0), true);
		DrawSelectedCategory();
		ImGui::EndChild();

		ImGui::EndTable();
	}
}

void PreferencesWindow::DrawCategorySidebar()
{
	ImGui::BeginChild("PreferencesSidebar", ImVec2(0, 0), true);

	char searchBuffer[kSearchBufferSize]{};
	CopyStringToBuffer(m_SearchText, searchBuffer, sizeof(searchBuffer));
	if (ImGui::InputTextWithHint("##PreferencesSearch", "Search", searchBuffer, sizeof(searchBuffer)))
	{
		m_SearchText = searchBuffer;
	}

	ImGui::Separator();

	const auto filteredCategories = TerraEngine::Editor::Preferences::FilterPreferencesCategories(m_SearchText);
	if (filteredCategories.empty())
	{
		ImGui::TextDisabled("No matching preferences.");
	}

	for (const auto& category : filteredCategories)
	{
		const bool selected = category.id == m_SelectedCategory;
		if (ImGui::Selectable(category.label.data(), selected))
		{
			m_SelectedCategory = category.id;
		}
	}

	ImGui::EndChild();
}

void PreferencesWindow::DrawSelectedCategory()
{
	switch (m_SelectedCategory)
	{
	case TerraEngine::Editor::Preferences::PreferencesCategoryId::Diagnostics:
		DrawRendererDiagnosticsCategory();
		break;
	case TerraEngine::Editor::Preferences::PreferencesCategoryId::McpBridge:
		DrawMcpBridgeCategory();
		break;
	default:
		DrawUnavailableState("This Preferences category is no longer available.");
		break;
	}
}

void PreferencesWindow::DrawMcpBridgeCategory()
{
	ImGui::TextUnformatted("MCP Bridge");
	ImGui::Separator();

	EditorMcpBridge* bridge = nullptr;
	if (m_Owner != nullptr && m_Owner->GetContext() != nullptr)
	{
		bridge = m_Owner->GetContext()->editorMcpBridge.get();
	}
	if (bridge == nullptr)
	{
		DrawUnavailableState("MCP bridge is not initialized.");
		return;
	}

	// Enablement and write access remain editor-owned preferences. MCP clients
	// cannot use their own connection to grant or expand these permissions.
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

	const std::uint16_t configuredPort = bridge->GetConfiguredListenPort();
	if (!m_McpListenPortDraftInitialized)
	{
		// Keep a draft so clearing or replacing the number can pass through an
		// invalid partial value without overwriting the last valid saved port.
		m_McpListenPortDraft = static_cast<int>(configuredPort);
		m_McpListenPortDraftInitialized = true;
	}

	ImGui::SeparatorText("Connection");
	ImGui::BeginDisabled(enabled);
	const bool portEdited = ImGui::InputInt("Listen port", &m_McpListenPortDraft, 1, 100);
	ImGui::EndDisabled();
	ShowOptionTooltip("Uses a fixed loopback port from 1024 through 65535. Port zero and random fallback are not allowed.");

	const bool validPort = TerraEngine::Editor::Preferences::IsValidEditorMcpListenPort(m_McpListenPortDraft);
	const bool portChanged = m_McpListenPortDraft != static_cast<int>(configuredPort);
	if (!enabled && portEdited && validPort && portChanged)
	{
		// Valid edits save immediately; no separate apply action is needed. The
		// bridge is disabled here, so changing the preference cannot disconnect a
		// live MCP client.
		bridge->SetListenPort(static_cast<std::uint16_t>(m_McpListenPortDraft));
	}
	if (!validPort)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Choose a port from 1024 through 65535.");
	}
	if (enabled)
	{
		ImGui::TextDisabled("Disable the bridge before changing its listen port.");
	}

	// The management surface remains in Preferences. Tabs keep typed runtime
	// state readable without reintroducing a dedicated MCP editor window.
	const nlohmann::json status = bridge->BuildStatusJson();
	if (ImGui::BeginTabBar("McpBridgeManagementTabs"))
	{
		if (ImGui::BeginTabItem("Status"))
		{
			ImGui::Text("State: %s", status.value("state", "unknown").c_str());
			ImGui::Text("Permission: %s", status.value("writeMode", "read_only").c_str());
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
			ImGui::Text("Recent activity: %llu", static_cast<unsigned long long>(status.value("recentActivityCount", 0ull)));
			ImGui::TextWrapped("Last diagnostics: %s", status.value("lastDiagnosticRequest", "never").c_str());
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Jobs"))
		{
			// Jobs are presented from the typed session resource, so imports,
			// compilation, exports, and profiler captures share one lifecycle UI.
			const nlohmann::json jobResource = bridge->ReadResource("terraengine://jobs/recent");
			const bool typedJobsAvailable = jobResource.value("ok", true);
			nlohmann::json jobs = jobResource.value("jobs", nlohmann::json::array());
			if (!typedJobsAvailable)
			{
				// Older version-one bridges exposed export work through diagnostics.
				// Retain that presentation while the typed job resource is absent.
				const nlohmann::json diagnostics = bridge->ReadResource("terraengine://diagnostics/summary");
				for (const auto& surface : diagnostics.value("surfaces", nlohmann::json::array()))
				{
					if (surface.value("surface", "") != "export")
					{
						continue;
					}
					jobs = surface.value("details", nlohmann::json::object()).value("jobs", nlohmann::json::array());
					for (auto& job : jobs)
					{
						job["kind"] = "export";
						job["cancellable"] = job.value("state", "") != "canceled";
					}
				}
			}
			if (jobs.empty())
			{
				ImGui::TextDisabled("No retained automation jobs.");
			}
			for (const auto& job : jobs)
			{
				const std::string jobId = job.value("jobId", "unknown-job");
				const std::string state = job.value("state", "unknown");
				const std::string jobLabel = job.value("profile", job.value("kind", "automation"));
				ImGui::PushID(jobId.c_str());
				ImGui::TextWrapped("%s - %s [%s]",
					jobId.c_str(),
					jobLabel.c_str(),
					state.c_str());
				ImGui::ProgressBar(static_cast<float>(job.value("progress", 0.0)), ImVec2(-1.0f, 0.0f), job.value("stage", "").c_str());
				const std::string correlationId = job.value("correlationId", "");
				if (!correlationId.empty())
				{
					ImGui::TextDisabled("Correlation: %s", correlationId.c_str());
				}
				const bool terminal = state == "succeeded" || state == "failed" || state == "canceled";
				if (!terminal && job.value("cancellable", false) && ImGui::Button("Request cancellation"))
				{
					bridge->CallTool(typedJobsAvailable ? "jobs.cancel" : "export.cancel", {{"jobId", jobId}});
				}
				ImGui::Separator();
				ImGui::PopID();
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Approvals"))
		{
			const auto approvals = bridge->GetPendingApprovals();
			if (approvals.empty())
			{
				ImGui::TextDisabled("No pending approvals.");
			}
			for (const auto& approval : approvals)
			{
				ImGui::PushID(approval.approvalId.c_str());
				ImGui::Text("%s", approval.capability.c_str());
				ImGui::TextDisabled("Approval %s | Request %s", approval.approvalId.c_str(), approval.requestId.c_str());
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
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Resources"))
		{
			for (const auto& resource : bridge->ListResources())
			{
				ImGui::TextUnformatted(resource.value("name", "Unnamed resource").c_str());
				ImGui::TextDisabled("%s", resource.value("uri", "").c_str());
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Activity"))
		{
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
				if (!record.requestId.empty())
				{
					ImGui::TextDisabled("Request: %s", record.requestId.c_str());
				}
			}
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void PreferencesWindow::DrawRendererDiagnosticsCategory()
{
	ImGui::TextUnformatted("Diagnostics");
	ImGui::Separator();

	if (m_Owner == nullptr || m_Owner->GetContext() == nullptr || m_Owner->GetContext()->projectManager == nullptr)
	{
		DrawUnavailableState("Preferences are unavailable until the editor project manager is ready.");
		return;
	}

	ProjectManager* projectManager = m_Owner->GetContext()->projectManager.get();
	EditorPreferences preferences = projectManager->GetEditorPreferences();
	bool vulkanValidationLayers = preferences.vulkanValidationLayers;
	bool renderDebugDiagnostics = preferences.renderDebugDiagnostics;
	bool changed = false;

	ImGui::SeparatorText("Renderer Diagnostics");
	if (ImGui::Checkbox("Vulkan Validation Layers", &vulkanValidationLayers))
	{
		changed = true;
	}
	ShowOptionTooltip("Requests Vulkan validation layers for this editor session and the next editor launch. Export builds use the selected Build Profile toggle.");

	if (ImGui::Checkbox("Render Debug Diagnostics", &renderDebugDiagnostics))
	{
		changed = true;
	}
	ShowOptionTooltip("Enables TerraEngine render debug diagnostics for the local editor session.");

	if (changed)
	{
		projectManager->SetRendererDiagnosticPreferences(vulkanValidationLayers, renderDebugDiagnostics);
		ApplyRendererDiagnosticsToRenderSystem(vulkanValidationLayers, renderDebugDiagnostics);
	}
}

void PreferencesWindow::DrawUnavailableState(const char* message)
{
	ImGui::TextDisabled("%s", message);
}
