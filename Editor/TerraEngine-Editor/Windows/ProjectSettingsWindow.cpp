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
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Resource/ResourceResidency.h"
#include "Engine/Resource/ResourceStreaming.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <limits>
#include <nlohmann/json.hpp>
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

	nlohmann::json BuildResidencyPolicyJson(const TerraEngine::Resource::ResidencyPolicy& policy)
	{
		nlohmann::json object;
		object["enabled"] = policy.enabled;
		object["cleanupIntervalSeconds"] = policy.cleanupIntervalSeconds;
		object["cpu"] = {
			{ "idleSecondsThreshold", policy.cpu.idleSecondsThreshold },
			{ "memoryBudgetBytes", policy.cpu.memoryBudgetBytes },
			{ "reloadGraceSeconds", policy.cpu.reloadGraceSeconds }
		};
		object["gpu"] = {
			{ "idleSecondsThreshold", policy.gpu.idleSecondsThreshold },
			{ "memoryBudgetBytes", policy.gpu.memoryBudgetBytes },
			{ "reloadGraceSeconds", policy.gpu.reloadGraceSeconds },
			{ "strictBudgetMode", policy.gpu.strictBudgetMode }
		};
		return object;
	}

	nlohmann::json BuildStreamingPolicyJson(const TerraEngine::Resource::StreamingPolicy& policy)
	{
		nlohmann::json object;
		object["enabled"] = policy.enabled;
		object["deferScenePayloadsOnLoad"] = policy.deferScenePayloadsOnLoad;
		object["streamingDistance"] = policy.streamingDistance;
		object["visibleRetentionSeconds"] = policy.visibleRetentionSeconds;
		object["neededRetentionSeconds"] = policy.neededRetentionSeconds;
		object["maxLoadsPerUpdate"] = policy.maxLoadsPerUpdate;
		object["textureMipStreamingEnabled"] = policy.textureMipStreamingEnabled;
		object["textureMinStreamMip"] = policy.textureMinStreamMip;
		object["textureMaxStreamMip"] = policy.textureMaxStreamMip;
		object["virtualGeometryPageMemoryBudgetBytes"] = policy.virtualGeometryPageMemoryBudgetBytes;
		object["virtualGeometryPageUploadBudgetBytes"] = policy.virtualGeometryPageUploadBudgetBytes;
		object["virtualGeometryPageRetirementBudgetBytes"] = policy.virtualGeometryPageRetirementBudgetBytes;
		object["memoryBudgetInteraction"] = std::string(TerraEngine::Resource::ToString(policy.memoryBudgetInteraction));
		return object;
	}

	int MemoryBudgetInteractionIndex(TerraEngine::Resource::MemoryBudgetInteraction interaction)
	{
		switch (interaction)
		{
		case TerraEngine::Resource::MemoryBudgetInteraction::PreferStreamingRelease:
			return 1;
		case TerraEngine::Resource::MemoryBudgetInteraction::Conservative:
			return 2;
		case TerraEngine::Resource::MemoryBudgetInteraction::UseResidencyPolicy:
			return 0;
		}
		return 0;
	}

	TerraEngine::Resource::MemoryBudgetInteraction MemoryBudgetInteractionFromIndex(int index)
	{
		switch (index)
		{
		case 1:
			return TerraEngine::Resource::MemoryBudgetInteraction::PreferStreamingRelease;
		case 2:
			return TerraEngine::Resource::MemoryBudgetInteraction::Conservative;
		default:
			return TerraEngine::Resource::MemoryBudgetInteraction::UseResidencyPolicy;
		}
	}

	int BytesToMbInput(std::uint64_t bytes)
	{
		const std::uint64_t mb = bytes / (1024ull * 1024ull);
		return static_cast<int>(std::min<std::uint64_t>(mb, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
	}

	std::uint64_t MbInputToBytes(int value)
	{
		return static_cast<std::uint64_t>(std::max(0, value)) * 1024ull * 1024ull;
	}

	void PersistResourcePoliciesToProjectManifest(const TerraEngine::Resource::ResidencyPolicy& residencyPolicy,
		const TerraEngine::Resource::StreamingPolicy& streamingPolicy)
	{
		const std::filesystem::path projectRoot = ResourceManager::Instance().GetProjectRoot();
		if (projectRoot.empty())
		{
			return;
		}

		const std::filesystem::path manifestPath = projectRoot / "game.json";
		nlohmann::json manifest = nlohmann::json::object();
		if (std::filesystem::exists(manifestPath))
		{
			std::ifstream input(manifestPath);
			if (input)
			{
				try
				{
					input >> manifest;
				}
				catch (...)
				{
					// A malformed manifest should not crash Project Settings.
					// Replacing it with policy-only JSON would discard product
					// metadata, so leave the file untouched instead.
					return;
				}
			}
		}

		if (!manifest.is_object())
		{
			return;
		}

		// These policies are project/runtime settings. Persisting them beside
		// product metadata lets exports copy the same runtime contract without
		// hiding resource behavior inside a build profile.
		manifest["residencyPolicy"] = BuildResidencyPolicyJson(residencyPolicy);
		manifest["streamingPolicy"] = BuildStreamingPolicyJson(streamingPolicy);

		std::ofstream output(manifestPath);
		if (output)
		{
			output << manifest.dump(4);
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
	case TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Resources:
		DrawResourcesCategory();
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

void ProjectSettingsWindow::DrawResourcesCategory()
{
	ImGui::TextUnformatted("Resources");
	ImGui::Separator();

	if (m_Owner == nullptr || m_Owner->GetContext() == nullptr || m_Owner->GetContext()->projectManager == nullptr)
	{
		DrawUnavailableState("Resource settings are unavailable until the editor project manager is ready.");
		return;
	}

	ProjectManager* projectManager = m_Owner->GetContext()->projectManager.get();
	if (!projectManager->HasActiveProject())
	{
		DrawUnavailableState("Open a project to edit resource residency settings.");
		return;
	}

	ResourceManager& resourceManager = ResourceManager::Instance();
	TerraEngine::Resource::ResidencyPolicy residencyPolicy = resourceManager.GetResidencyPolicy();
	TerraEngine::Resource::StreamingPolicy streamingPolicy = resourceManager.GetStreamingPolicy();
	bool residencyChanged = false;
	bool streamingChanged = false;

	ImGui::SeparatorText("Loaded Resource Cleanup");
	if (ImGui::Checkbox("Enable Loaded Resource Cleanup", &residencyPolicy.enabled))
	{
		residencyChanged = true;
	}
	ShowOptionTooltip("Lets the resource manager unload idle loaded assets according to the RAM and VRAM residency policies below.");

	float cleanupInterval = static_cast<float>(residencyPolicy.cleanupIntervalSeconds);
	if (ImGui::InputFloat("Cleanup Interval Seconds", &cleanupInterval, 0.25f, 1.0f, "%.2f"))
	{
		residencyPolicy.cleanupIntervalSeconds = static_cast<double>(std::max(0.01f, cleanupInterval));
		residencyChanged = true;
	}

	ImGui::SeparatorText("RAM Residency");
	float cpuIdleSeconds = static_cast<float>(residencyPolicy.cpu.idleSecondsThreshold);
	if (ImGui::InputFloat("RAM Idle Seconds", &cpuIdleSeconds, 0.5f, 1.0f, "%.2f"))
	{
		residencyPolicy.cpu.idleSecondsThreshold = static_cast<double>(std::max(0.0f, cpuIdleSeconds));
		residencyChanged = true;
	}

	int cpuBudgetMb = BytesToMbInput(residencyPolicy.cpu.memoryBudgetBytes);
	if (ImGui::InputInt("RAM Budget MB (0=off)", &cpuBudgetMb))
	{
		residencyPolicy.cpu.memoryBudgetBytes = MbInputToBytes(cpuBudgetMb);
		residencyChanged = true;
	}

	float cpuReloadGrace = static_cast<float>(residencyPolicy.cpu.reloadGraceSeconds);
	if (ImGui::InputFloat("RAM Reload Grace Seconds", &cpuReloadGrace, 0.1f, 0.5f, "%.2f"))
	{
		residencyPolicy.cpu.reloadGraceSeconds = static_cast<double>(std::max(0.0f, cpuReloadGrace));
		residencyChanged = true;
	}

	ImGui::SeparatorText("VRAM Residency");
	float gpuIdleSeconds = static_cast<float>(residencyPolicy.gpu.idleSecondsThreshold);
	if (ImGui::InputFloat("VRAM Idle Seconds", &gpuIdleSeconds, 0.5f, 1.0f, "%.2f"))
	{
		residencyPolicy.gpu.idleSecondsThreshold = static_cast<double>(std::max(0.0f, gpuIdleSeconds));
		residencyChanged = true;
	}

	int gpuBudgetMb = BytesToMbInput(residencyPolicy.gpu.memoryBudgetBytes);
	if (ImGui::InputInt("VRAM Budget MB (0=off)", &gpuBudgetMb))
	{
		residencyPolicy.gpu.memoryBudgetBytes = MbInputToBytes(gpuBudgetMb);
		residencyChanged = true;
	}

	float gpuReloadGrace = static_cast<float>(residencyPolicy.gpu.reloadGraceSeconds);
	if (ImGui::InputFloat("VRAM Reload Grace Seconds", &gpuReloadGrace, 0.1f, 0.5f, "%.2f"))
	{
		residencyPolicy.gpu.reloadGraceSeconds = static_cast<double>(std::max(0.0f, gpuReloadGrace));
		residencyChanged = true;
	}

	if (ImGui::Checkbox("Strict VRAM Budget Mode", &residencyPolicy.gpu.strictBudgetMode))
	{
		residencyChanged = true;
	}
	ShowOptionTooltip("When enabled, VRAM budget pressure can release otherwise idle GPU payloads more aggressively.");

	ImGui::SeparatorText("Visibility Residency");
	if (ImGui::Checkbox("Track Visible Resource Needs##EnableVisibilityStreaming", &streamingPolicy.enabled))
	{
		streamingChanged = true;
	}
	ShowOptionTooltip("Tracks assets needed by visible editor/runtime content so streaming can prioritize them.");

	if (ImGui::Checkbox("Defer Scene Payloads Until Visible##DeferScenePayloadsOnLoad", &streamingPolicy.deferScenePayloadsOnLoad))
	{
		streamingChanged = true;
	}

	float visibleRetention = static_cast<float>(streamingPolicy.visibleRetentionSeconds);
	if (ImGui::InputFloat("Visible Resource Retention Seconds##VisibleRetentionSeconds", &visibleRetention, 0.25f, 1.0f, "%.2f"))
	{
		streamingPolicy.visibleRetentionSeconds = static_cast<double>(std::max(0.0f, visibleRetention));
		streamingChanged = true;
	}

	float neededRetention = static_cast<float>(streamingPolicy.neededRetentionSeconds);
	if (ImGui::InputFloat("Requested Resource Retention Seconds##NeededRetentionSeconds", &neededRetention, 0.25f, 1.0f, "%.2f"))
	{
		streamingPolicy.neededRetentionSeconds = static_cast<double>(std::max(0.0f, neededRetention));
		streamingChanged = true;
	}

	ImGui::SeparatorText("Texture Mip Streaming");
	if (ImGui::Checkbox("Enable Texture Mip Streaming", &streamingPolicy.textureMipStreamingEnabled))
	{
		streamingChanged = true;
	}

	float streamingDistance = static_cast<float>(streamingPolicy.streamingDistance);
	if (ImGui::InputFloat("Texture Mip Fade Distance##TextureMipFadeDistance", &streamingDistance, 1.0f, 10.0f, "%.2f"))
	{
		// Save/load still owns the underlying policy key name. The visible
		// editor label names the actual runtime effect: texture mip fading.
		streamingPolicy.streamingDistance = static_cast<double>(std::max(0.0f, streamingDistance));
		streamingChanged = true;
	}

	int minStreamMip = static_cast<int>(streamingPolicy.textureMinStreamMip);
	if (ImGui::InputInt("Near Texture Mip##TextureMinStreamMip", &minStreamMip))
	{
		streamingPolicy.textureMinStreamMip = static_cast<std::uint32_t>(std::max(0, minStreamMip));
		streamingChanged = true;
	}

	int maxStreamMip = static_cast<int>(streamingPolicy.textureMaxStreamMip);
	if (ImGui::InputInt("Far Texture Mip##TextureMaxStreamMip", &maxStreamMip))
	{
		streamingPolicy.textureMaxStreamMip = static_cast<std::uint32_t>(std::max(0, maxStreamMip));
		streamingChanged = true;
	}
	ImGui::TextDisabled("Texture mips only: mip 0 is full resolution; larger mips are lower detail.");

	ImGui::SeparatorText("Virtual Geometry Page Budgets");
	int vgPageBudgetMb = BytesToMbInput(streamingPolicy.virtualGeometryPageMemoryBudgetBytes);
	if (ImGui::InputInt("VG Page Memory Budget MB (0=off)", &vgPageBudgetMb))
	{
		streamingPolicy.virtualGeometryPageMemoryBudgetBytes = MbInputToBytes(vgPageBudgetMb);
		streamingChanged = true;
	}
	int vgUploadBudgetMb = BytesToMbInput(streamingPolicy.virtualGeometryPageUploadBudgetBytes);
	if (ImGui::InputInt("VG Page Upload Budget MB/frame (0=off)", &vgUploadBudgetMb))
	{
		streamingPolicy.virtualGeometryPageUploadBudgetBytes = MbInputToBytes(vgUploadBudgetMb);
		streamingChanged = true;
	}
	int vgRetirementBudgetMb = BytesToMbInput(streamingPolicy.virtualGeometryPageRetirementBudgetBytes);
	if (ImGui::InputInt("VG Page Retirement Budget MB/frame (0=off)", &vgRetirementBudgetMb))
	{
		streamingPolicy.virtualGeometryPageRetirementBudgetBytes = MbInputToBytes(vgRetirementBudgetMb);
		streamingChanged = true;
	}
	ImGui::TextDisabled("Fallback pages stay protected; page pressure targets high-detail pages first.");

	int maxLoads = static_cast<int>(streamingPolicy.maxLoadsPerUpdate);
	if (ImGui::InputInt("Max Streaming Loads (0=unlimited)", &maxLoads))
	{
		streamingPolicy.maxLoadsPerUpdate = static_cast<std::uint32_t>(std::max(0, maxLoads));
		streamingChanged = true;
	}

	const char* streamingBudgetItems[] = { "Use Residency Policy", "Prefer Streaming Release", "Conservative" };
	int streamingBudgetIndex = MemoryBudgetInteractionIndex(streamingPolicy.memoryBudgetInteraction);
	if (ImGui::Combo("Streaming Memory Interaction", &streamingBudgetIndex, streamingBudgetItems, IM_ARRAYSIZE(streamingBudgetItems)))
	{
		streamingPolicy.memoryBudgetInteraction = MemoryBudgetInteractionFromIndex(streamingBudgetIndex);
		streamingChanged = true;
	}

	if (residencyChanged)
	{
		resourceManager.SetResidencyPolicy(residencyPolicy);
	}

	if (streamingChanged)
	{
		resourceManager.SetStreamingPolicy(streamingPolicy);
	}

	if (residencyChanged || streamingChanged)
	{
		PersistResourcePoliciesToProjectManifest(resourceManager.GetResidencyPolicy(), resourceManager.GetStreamingPolicy());
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
