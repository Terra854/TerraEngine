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
	struct DirectionalLightOption
	{
		TerraEngine::UUID id = TerraEngine::EMPTY_UUID;
		std::string name;
	};

	std::vector<DirectionalLightOption> CollectDirectionalLights()
	{
		std::vector<DirectionalLightOption> options;
		for (Object object : ObjectManager::instance().getObjectsWithComponents<LightComponent>())
		{
			LightComponent* light = object.getComponent<LightComponent>();
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
	: EditorWindow("Sky Settings"), m_Owner(owner)
{
}

void SkySettingsWindow::OnGui()
{
	if (m_Owner == nullptr || m_Owner->GetContext() == nullptr || !m_Owner->GetContext()->projectManager->HasActiveProject())
	{
		ImGui::TextDisabled("Open a project to edit scene sky settings.");
		return;
	}

	const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
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
	if (ImGui::InputText("Sky Source", sourceBuffer, sizeof(sourceBuffer)))
	{
		settings.sourceAsset = sourceBuffer;
		changed = true;
	}

	ImGui::SameLine();
	if (ImGui::Button("Clear Source"))
	{
		settings.sourceAsset.clear();
		changed = true;
	}

	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE"))
		{
			const TerraEngine::UUID assetGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);
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
	for (const DirectionalLightOption& option : directionalLights)
	{
		if (option.id == settings.directionalLightObjectId)
		{
			currentLightLabel = option.name;
			break;
		}
	}

	if (ImGui::BeginCombo("Directional Light", currentLightLabel.c_str()))
	{
		if (ImGui::Selectable("<Auto First Directional Light>", settings.directionalLightObjectId == TerraEngine::EMPTY_UUID))
		{
			settings.directionalLightObjectId = TerraEngine::EMPTY_UUID;
			changed = true;
		}

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

	if (ImGui::Button("Use Automatic Light"))
	{
		settings.directionalLightObjectId = TerraEngine::EMPTY_UUID;
		changed = true;
	}

	if (const std::optional<entt::entity> selectedEntity = m_Owner->GetSelectedEntity(); selectedEntity.has_value())
	{
		if (auto selectedObject = ObjectManager::instance().getObject(*selectedEntity))
		{
			if (LightComponent* light = selectedObject->getComponent<LightComponent>(); light != nullptr && light->type == LightType::Directional)
			{
				ImGui::SameLine();
				if (ImGui::Button("Use Selected Light"))
				{
					settings.directionalLightObjectId = selectedObject->getUUID();
					changed = true;
				}
			}
		}
	}
	ImGui::EndDisabled();

	if (changed)
	{
		SceneManager::Instance().SetActiveSceneSkySettings(settings);
	}
}
