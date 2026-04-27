// SkySettingsWindow.cpp — Sky Settings Window Implementation
//
// Implements the environmental configuration UI, exposing parameters for the engine's sky and lighting systems.
// It provides real-time adjustment of atmospheric scattering, sun direction, and ambient lighting intensity.

#include "SkySettingsWindow.h"

#include "../Assets/AssetDatabase.h"
#include "../Project/ProjectManager.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Components/LightComponent.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/SceneManager.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <string>
#include <vector>

namespace
{
	// struct DirectionalLightOption groups related state and operations so callers have one documented contract.
	struct DirectionalLightOption
	{
		TerraEngine::UUID id = TerraEngine::EMPTY_UUID;
		std::string name;
	};

	// CollectDirectionalLights is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::vector<DirectionalLightOption> CollectDirectionalLights()
	{
		std::vector<DirectionalLightOption> options;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (Object object : ObjectManager::instance().getObjectsWithComponents<LightComponent>())
		{
			LightComponent* light = object.getComponent<LightComponent>();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (light == nullptr || light->type != LightType::Directional)
			{
				continue;
			}

			DirectionalLightOption option;
			option.id = object.getUUID();
			option.name = object.getName();
			options.push_back(std::move(option));
		}

		return options;
	}
}

SkySettingsWindow::SkySettingsWindow(MainEditorSubsystem* owner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Sky Settings"), m_Owner(owner)
{
}

// SkySettingsWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void SkySettingsWindow::OnGui()
{
	if (m_Owner == nullptr || m_Owner->GetContext() == nullptr || !m_Owner->GetContext()->projectManager->HasActiveProject())
	{
		ImGui::TextDisabled("Open a project to edit scene sky settings.");
		return;
	}

	const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (activeScene == nullptr)
	{
		ImGui::TextDisabled("Open a scene to author the sky.");
		return;
	}

	SceneSkySettings settings = SceneManager::Instance().GetActiveSceneSkySettings();
	bool changed = false;

	ImGui::Text("Scene: %s", activeScene->name.c_str());
	ImGui::Separator();
	ImGui::TextWrapped("Sky direction always follows a directional light. If no directional light can be resolved, the background renders black.");
	ImGui::Spacing();

	changed |= ImGui::Checkbox("Enable Scene Sky", &settings.enabled);

	char sourceBuffer[512]{};
	const std::string sourceAssetString = settings.sourceAsset;
	std::memcpy(sourceBuffer, sourceAssetString.c_str(), std::min(sourceAssetString.size(), sizeof(sourceBuffer) - 1));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::InputText("Sky Source", sourceBuffer, sizeof(sourceBuffer)))
	{
		settings.sourceAsset = sourceBuffer;
		changed = true;
	}

	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Clear Source"))
	{
		settings.sourceAsset.clear();
		changed = true;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE"))
		{
			const TerraEngine::UUID assetGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (const AssetMetadata* metadata = AssetDatabase::Instance().FindAssetByGuid(assetGuid))
			{
				// Store the project-relative source path so ResourceManager can
				// resolve the same texture GUID in both editor and export runtime.
				settings.sourceAsset = metadata->sourcePath.generic_string();
				changed = true;
			}
		}
		ImGui::EndDragDropTarget();
	}

	ImGui::BeginDisabled(!settings.enabled);
	changed |= ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 8.0f);
	changed |= ImGui::SliderFloat("Exposure", &settings.exposure, 0.0f, 8.0f);
	changed |= ImGui::SliderFloat("Rotation Offset", &settings.rotationOffsetDegrees, -180.0f, 180.0f, "%.1f deg");
	changed |= ImGui::ColorEdit3("Horizon Tint", &settings.horizonTint.x);
	changed |= ImGui::ColorEdit3("Zenith Tint", &settings.zenithTint.x);
	changed |= ImGui::ColorEdit3("Ground Tint", &settings.groundTint.x);

	const std::vector<DirectionalLightOption> directionalLights = CollectDirectionalLights();
	std::string currentLightLabel = directionalLights.empty() ? "<No Directional Light>" : "<Auto First Directional Light>";
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const DirectionalLightOption& option : directionalLights)
	{
		if (option.id == settings.directionalLightObjectId)
		{
			currentLightLabel = option.name;
			break;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginCombo("Directional Light", currentLightLabel.c_str()))
	{
		if (ImGui::Selectable("<Auto First Directional Light>", settings.directionalLightObjectId == TerraEngine::EMPTY_UUID))
		{
			settings.directionalLightObjectId = TerraEngine::EMPTY_UUID;
			changed = true;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const DirectionalLightOption& option : directionalLights)
		{
			if (ImGui::Selectable(option.name.c_str(), option.id == settings.directionalLightObjectId))
			{
				settings.directionalLightObjectId = option.id;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Use Automatic Light"))
	{
		settings.directionalLightObjectId = TerraEngine::EMPTY_UUID;
		changed = true;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (const std::optional<entt::entity> selectedEntity = m_Owner->GetSelectedEntity(); selectedEntity.has_value())
	{
		if (auto selectedObject = ObjectManager::instance().getObject(*selectedEntity))
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (LightComponent* light = selectedObject->getComponent<LightComponent>(); light != nullptr && light->type == LightType::Directional)
			{
				ImGui::SameLine();
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::Button("Use Selected Light"))
				{
					settings.directionalLightObjectId = selectedObject->getUUID();
					changed = true;
				}
			}
		}
	}
	ImGui::EndDisabled();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (changed)
	{
		SceneManager::Instance().SetActiveSceneSkySettings(settings);
	}
}
