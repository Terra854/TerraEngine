// PreferencesWindow.cpp - Unified Preferences editor implementation.
//
// Hosts editor-wide settings that should follow the user instead of the active
// project. Project Settings remains responsible for project/runtime data.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "PreferencesWindow.h"

#include "../Project/ProjectManager.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>

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
	default:
		DrawUnavailableState("This Preferences category is no longer available.");
		break;
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
