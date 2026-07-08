// ProjectSettingsWindow.cpp - Unified Project Settings editor implementation.
//
// Hosts project and graphics settings in one category-driven window
// so old standalone settings windows no longer compete with the canonical
// Project Settings surface.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ProjectSettingsWindow.h"

#include "../Project/ProjectManager.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <imgui.h>
#include <string>

namespace
{
	constexpr float kCategoryListWidth = 220.0f;
	constexpr int kSearchBufferSize = 128;
	constexpr int kProjectNameBufferSize = 256;

	const char* QualityPresetLabel(QualityPreset preset)
	{
		switch (preset)
		{
		case QualityPreset::Low: return "Low";
		case QualityPreset::Medium: return "Medium";
		case QualityPreset::High: return "High";
		case QualityPreset::Ultra: return "Ultra";
		case QualityPreset::Custom: return "Custom";
		}
		return "Custom";
	}

	void CopyStringToBuffer(const std::string& source, char* buffer, std::size_t bufferSize)
	{
		std::memset(buffer, 0, bufferSize);
		std::memcpy(buffer, source.c_str(), std::min(source.size(), bufferSize - 1));
	}

	bool IsBlankText(const char* text)
	{
		if (text == nullptr)
		{
			return true;
		}

		while (*text != '\0')
		{
			if (!std::isspace(static_cast<unsigned char>(*text)))
			{
				return false;
			}
			++text;
		}
		return true;
	}

	void ShowOptionTooltip(const char* description)
	{
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", description);
		}
	}
}

ProjectSettingsWindow::ProjectSettingsWindow(MainEditorSubsystem* owner)
	: EditorWindow(std::string(TerraEngine::Editor::ProjectSettings::kProjectSettingsWindowTitle), false), m_Owner(owner)
{
}

void ProjectSettingsWindow::OpenToCategory(TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId categoryId)
{
	m_SelectedCategory = categoryId;
	SetVisible(true);
}

std::size_t ProjectSettingsWindow::GetFilteredCategoryCountForTests() const
{
	return TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories(m_SearchText).size();
}

bool ProjectSettingsWindow::HasCategoryForTests(TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId categoryId) const
{
	return TerraEngine::Editor::ProjectSettings::FindProjectSettingsCategory(categoryId) != nullptr;
}

void ProjectSettingsWindow::OnGui()
{
	constexpr ImGuiTableFlags layoutFlags =
		ImGuiTableFlags_SizingFixedFit |
		ImGuiTableFlags_NoSavedSettings;

	if (ImGui::BeginTable("ProjectSettingsLayout", 2, layoutFlags, ImGui::GetContentRegionAvail()))
	{
		ImGui::TableSetupColumn("Categories", ImGuiTableColumnFlags_WidthFixed, kCategoryListWidth);
		ImGui::TableSetupColumn("Settings", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		DrawCategorySidebar();

		ImGui::TableSetColumnIndex(1);
		ImGui::BeginChild("ProjectSettingsDetails", ImVec2(0, 0), true);
		DrawSelectedCategory();
		ImGui::EndChild();

		ImGui::EndTable();
	}
}

void ProjectSettingsWindow::DrawCategorySidebar()
{
	ImGui::BeginChild("ProjectSettingsSidebar", ImVec2(0, 0), true);

	char searchBuffer[kSearchBufferSize]{};
	CopyStringToBuffer(m_SearchText, searchBuffer, sizeof(searchBuffer));
	if (ImGui::InputTextWithHint("##ProjectSettingsSearch", "Search", searchBuffer, sizeof(searchBuffer)))
	{
		m_SearchText = searchBuffer;
	}

	ImGui::Separator();

	const auto filteredCategories = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories(m_SearchText);
	if (filteredCategories.empty())
	{
		ImGui::TextDisabled("No matching settings.");
	}

	for (const auto& category : filteredCategories)
	{
		const bool selected = category.id == m_SelectedCategory;
		if (ImGui::Selectable(category.label.data(), selected))
		{
			m_SelectedCategory = category.id;
			m_ValidationMessage.clear();
		}
	}

	ImGui::EndChild();
}

void ProjectSettingsWindow::DrawSelectedCategory()
{
	switch (m_SelectedCategory)
	{
	case TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Project:
		DrawProjectCategory();
		break;
	case TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Graphics:
		DrawGraphicsCategory();
		break;
	default:
		DrawUnavailableState("This Project Settings category is no longer available.");
		break;
	}
}

void ProjectSettingsWindow::DrawProjectCategory()
{
	ImGui::TextUnformatted("Project");
	ImGui::Separator();

	if (m_Owner == nullptr || m_Owner->GetContext() == nullptr || m_Owner->GetContext()->projectManager == nullptr)
	{
		DrawUnavailableState("Project settings are unavailable until the editor project manager is ready.");
		return;
	}

	ProjectManager* projectManager = m_Owner->GetContext()->projectManager.get();
	if (!projectManager->HasActiveProject())
	{
		DrawUnavailableState("Open a project to edit project settings.");
		return;
	}

	ImGui::Text("Project Root: %s", projectManager->GetProjectRoot().string().c_str());

	char displayNameBuffer[kProjectNameBufferSize]{};
	CopyStringToBuffer(projectManager->GetSettings().projectDisplayName, displayNameBuffer, sizeof(displayNameBuffer));
	if (ImGui::InputText("Product Name", displayNameBuffer, sizeof(displayNameBuffer)))
	{
		if (IsBlankText(displayNameBuffer))
		{
			m_ValidationMessage = "Product Name cannot be empty. The previous saved value was kept.";
		}
		else if (projectManager->SetProjectDisplayName(displayNameBuffer))
		{
			m_ValidationMessage.clear();
		}
	}

	DrawValidationMessage();
}

void ProjectSettingsWindow::DrawGraphicsCategory()
{
	ImGui::TextUnformatted("Graphics");
	ImGui::Separator();

	RenderSystem& renderSystem = RenderSystem::Instance();
	QualityPreset preset = renderSystem.GetQualityPreset();
	if (ImGui::BeginCombo("Quality Preset", QualityPresetLabel(preset)))
	{
		const QualityPreset presets[] = {
			QualityPreset::Low,
			QualityPreset::Medium,
			QualityPreset::High,
			QualityPreset::Ultra,
			QualityPreset::Custom
		};

		for (QualityPreset candidate : presets)
		{
			const bool selected = candidate == preset;
			if (ImGui::Selectable(QualityPresetLabel(candidate), selected))
			{
				renderSystem.SetQualityPreset(candidate);
				preset = candidate;
			}
			if (selected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}
	ShowOptionTooltip("Applies a renderer quality preset. Custom is selected automatically when individual values are edited.");

	RenderSettings settings = renderSystem.GetRenderSettings();
	bool changed = false;

	int shadowResolution = static_cast<int>(settings.shadowResolution);
	if (ImGui::InputInt("Shadow Resolution", &shadowResolution))
	{
		settings.shadowResolution = static_cast<std::uint32_t>(std::max(128, shadowResolution));
		changed = true;
	}

	int shadowFilter = static_cast<int>(settings.shadowFilteringSize);
	if (ImGui::SliderInt("Shadow PCF Kernel", &shadowFilter, 1, 15))
	{
		if (shadowFilter % 2 == 0)
		{
			++shadowFilter;
		}
		settings.shadowFilteringSize = static_cast<std::uint32_t>(shadowFilter);
		changed = true;
	}

	int retainedLights = static_cast<int>(settings.maxDynamicLights);
	if (ImGui::SliderInt("Retained Light Budget", &retainedLights, 0, 1024))
	{
		settings.maxDynamicLights = static_cast<std::uint32_t>(std::max(0, retainedLights));
		settings.maxUploadedLights = std::max(settings.maxUploadedLights, settings.maxDynamicLights);
		settings.maxShadowLights = std::min(settings.maxShadowLights, settings.maxDynamicLights);
		settings.maxAreaLights = std::min(settings.maxAreaLights, settings.maxDynamicLights);
		changed = true;
	}

	int shadowLights = static_cast<int>(settings.maxShadowLights);
	if (ImGui::SliderInt("Shadow Light Budget", &shadowLights, 0, static_cast<int>(std::min(settings.maxDynamicLights, kMaxShadowResourceSlots))))
	{
		settings.maxShadowLights = static_cast<std::uint32_t>(std::clamp(shadowLights, 0, static_cast<int>(std::min(settings.maxDynamicLights, kMaxShadowResourceSlots))));
		changed = true;
	}

	if (ImGui::CollapsingHeader("Advanced Lighting Overrides"))
	{
		int uploadedLights = static_cast<int>(settings.maxUploadedLights);
		if (ImGui::SliderInt("Uploaded Light Budget", &uploadedLights, std::max(1, retainedLights), 1024))
		{
			settings.maxUploadedLights = static_cast<std::uint32_t>(std::max(uploadedLights, retainedLights));
			changed = true;
		}

		int areaLights = static_cast<int>(settings.maxAreaLights);
		if (ImGui::SliderInt("Area Light Budget", &areaLights, 0, std::max(0, retainedLights)))
		{
			settings.maxAreaLights = static_cast<std::uint32_t>(std::clamp(areaLights, 0, retainedLights));
			changed = true;
		}
	}

	bool enableLtc = settings.enableLTC > 0u;
	if (ImGui::Checkbox("Enable Area Lights (LTC)", &enableLtc))
	{
		settings.enableLTC = enableLtc ? 1u : 0u;
		changed = true;
	}

	bool enableIbl = settings.enableIBL > 0u;
	if (ImGui::Checkbox("Enable Indirect (IBL)", &enableIbl))
	{
		settings.enableIBL = enableIbl ? 1u : 0u;
		changed = true;
	}

	changed |= ImGui::SliderFloat("Indirect Strength", &settings.indirectStrength, 0.0f, 8.0f);
	changed |= ImGui::SliderFloat("AO Strength", &settings.ambientOcclusionStrength, 0.0f, 4.0f);

	int aoQuality = static_cast<int>(settings.ambientOcclusionQuality);
	if (ImGui::SliderInt("AO Quality", &aoQuality, 0, 3))
	{
		settings.ambientOcclusionQuality = static_cast<std::uint32_t>(std::clamp(aoQuality, 0, 3));
		changed = true;
	}

	int anisotropic = static_cast<int>(settings.anisotropicFiltering);
	if (ImGui::SliderInt("Anisotropic Filtering", &anisotropic, 1, 16))
	{
		settings.anisotropicFiltering = static_cast<std::uint32_t>(std::clamp(anisotropic, 1, 16));
		changed = true;
	}

	bool enableVrs = settings.variableRateShading > 0u;
	if (ImGui::Checkbox("Variable Rate Shading", &enableVrs))
	{
		settings.variableRateShading = enableVrs ? 1u : 0u;
		changed = true;
	}
	ShowOptionTooltip("Variable Rate Shading affects the next backend initialization or export package.");

	if (ImGui::CollapsingHeader("Renderer Diagnostics"))
	{
		bool enableValidation = settings.validationEnabled > 0u;
		if (ImGui::Checkbox("Vulkan Validation Layers", &enableValidation))
		{
			settings.validationEnabled = enableValidation ? 1u : 0u;
			changed = true;
		}
		ShowOptionTooltip("Requests Vulkan validation layers on the next editor/project-runtime launch. Export builds use the selected Build Profile toggle.");

		bool enableDebug = settings.debugEnabled > 0u;
		if (ImGui::Checkbox("Render Debug Diagnostics", &enableDebug))
		{
			settings.debugEnabled = enableDebug ? 1u : 0u;
			changed = true;
		}
		ShowOptionTooltip("Applies TerraEngine render debug diagnostics immediately and saves the same state for the next editor or export launch.");
	}

	if (ImGui::CollapsingHeader("Virtual Geometry", ImGuiTreeNodeFlags_DefaultOpen))
	{
		// This is a runtime selection scale, not an import/cook ratio. The
		// renderer multiplies cooked geometric error by this value before
		// accepting lower-detail clusters, so larger values preserve sharper
		// mesh detail at distance and smaller values allow earlier LOD drops.
		float vgLodScale = settings.virtualGeometry.lodErrorScale;
		if (ImGui::DragFloat("VG Mesh LOD Scale", &vgLodScale, 0.05f, 0.05f, 64.0f, "%.2f"))
		{
			settings.virtualGeometry.lodErrorScale = vgLodScale;
			changed = true;
		}
		ShowOptionTooltip("Runtime virtual geometry LOD scale. Higher values keep higher-detail mesh clusters longer; lower values switch to coarser clusters sooner.");
	}

	if (changed)
	{
		renderSystem.SetRenderSettings(settings);
	}
}

void ProjectSettingsWindow::DrawUnavailableState(const char* message)
{
	ImGui::TextDisabled("%s", message);
}

void ProjectSettingsWindow::DrawValidationMessage()
{
	if (!m_ValidationMessage.empty())
	{
		ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f), "%s", m_ValidationMessage.c_str());
	}
}
