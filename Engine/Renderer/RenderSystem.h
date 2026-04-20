#pragma once

#include "Core/SystemBase.h"
#include "RHI/RHIContext.h"
#include "Core/Object/ObjectManager.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <entt/entt.hpp>
#include "Components/Transform.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/LightComponent.h"

class RenderSystem : public SystemsBase
{
public:
	static RenderSystem& Instance();

	RenderSystem(const RenderSystem&) = delete;
	RenderSystem& operator=(const RenderSystem&) = delete;
	RenderSystem(RenderSystem&&) = delete;
	RenderSystem& operator=(RenderSystem&&) = delete;
	~RenderSystem() override = default;

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

	void ReloadMaterial(const TerraEngine::UUID& materialId);
	void ReloadMesh(const TerraEngine::UUID& meshId);
	void ReloadTexture(const TerraEngine::UUID& textureId);

	void DeleteResource(const TerraEngine::UUID& id);
    
	void Clear();
	void SetTaskShaderCullingEnabled(bool enabled);
	bool IsTaskShaderCullingEnabled() const { return taskShaderCullingEnabled; }
	RenderStatsSnapshot GetLatestRenderStats(RenderViewType viewType) const;
	FrameBudgetDiagnosticsSnapshot GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const;
	LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const;

	void SetQualityPreset(QualityPreset preset);
	QualityPreset GetQualityPreset() const { return currentPreset; }
	void SetRenderSettings(const RenderSettings& settings);
	const RenderSettings& GetRenderSettings() const { return currentSettings; }
	SkyRuntimeState GetLatestSkyRuntimeState() const;
	void MarkSkyStateDirty();

	void LoadSettings();
	void SaveSettings();

	RHIContext* GetContext() const { return rhi.get(); }

    struct OverrideCameraData {
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec3 pos;
    };
	void SetSceneViewCamera(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& pos) {
		sceneViewCamera = OverrideCameraData{view, proj, pos};
		hasSceneViewCamera = true;
	}
	void SetEditorModeEnabled(bool enabled) { editorModeEnabled = enabled; }
    void SetGameViewCamera(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& pos) {
        gameViewCameras.push_back({view, proj, pos});
    }
    void ClearOverrideCameras() {
        hasSceneViewCamera = false;
        gameViewCameras.clear();
    }

private:
	RenderSystem();
	std::unique_ptr<RHIContext> rhi;
	ObjectManager& objManager = ObjectManager::instance();
	bool initialized = false;
	bool taskShaderCullingEnabled = true;
	bool editorModeEnabled = false;

    bool hasSceneViewCamera = false;
    OverrideCameraData sceneViewCamera;
    std::vector<OverrideCameraData> gameViewCameras;

    // Internal structs for data gathering
    struct CameraEntry
    {
        entt::entity entity;
        CameraComponent camera;
    };
    struct RenderableEntry
    {
        entt::entity entity;
        MeshRenderComponent mesh;
    };
	struct LightEntry
	{
		entt::entity entity;
		TerraEngine::UUID lightId;
		LightComponent light;
	};

	mutable std::mutex statsMutex;
	RenderStatsSnapshot latestGameStats{};
	RenderStatsSnapshot latestSceneStats{};
	FrameBudgetDiagnosticsSnapshot latestGameBudgetDiagnostics{};
	FrameBudgetDiagnosticsSnapshot latestSceneBudgetDiagnostics{};
	LightingDiagnosticsSnapshot latestGameLightingDiagnostics{};
	LightingDiagnosticsSnapshot latestSceneLightingDiagnostics{};
	SkyRuntimeState latestSkyRuntimeState{};
	bool skyStateDirty = true;
	std::string lastLoggedSkyIssueKey;

	QualityPreset currentPreset = QualityPreset::High;
	RenderSettings currentSettings;
};
