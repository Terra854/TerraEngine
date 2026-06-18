// RenderSystem.h — Render System Definition
//
// Defines the RenderSystem class, the primary high-level coordinator for all engine rendering operations.
// It manages the frame lifecycle, integrates with the RHI to submit commands to the GPU, and handles scene-to-renderer data synchronization.
// The system oversees specialized features like deferred shading, shadow mapping, and post-processing.

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
#include "Scene/SpatialDiagnostics.h"
#include "Scene/SpatialIndex.h"

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
	// IsTaskShaderCullingEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsTaskShaderCullingEnabled() const { return taskShaderCullingEnabled; }
	RenderStatsSnapshot GetLatestRenderStats(RenderViewType viewType) const;
	TerraEngine::Scene::SpatialDiagnosticsSnapshot GetLatestSpatialDiagnostics(RenderViewType viewType) const;
	FrameBudgetDiagnosticsSnapshot GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const;
	LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const;
	RenderSnapshotDiagnosticsSnapshot GetLatestRenderSnapshotDiagnostics() const;

	void SetQualityPreset(QualityPreset preset);
	// GetQualityPreset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	QualityPreset GetQualityPreset() const { return currentPreset; }
	void SetRenderSettings(const RenderSettings& settings);
	const RenderSettings& GetRenderSettings() const { return currentSettings; }
	SkyRuntimeState GetLatestSkyRuntimeState() const;
	void MarkSkyStateDirty();

	void LoadSettings();
	void SaveSettings();

	// GetContext is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	RHIContext* GetContext() const { return rhi.get(); }

	struct OverrideCameraData {
		glm::mat4 view;
		glm::mat4 proj;
		glm::vec3 pos;
	};
	// SetSceneViewCamera is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetSceneViewCamera(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& pos) {
		sceneViewCamera = OverrideCameraData{view, proj, pos};
		hasSceneViewCamera = true;
	}
	void SetSceneViewContextTint(const std::unordered_set<TerraEngine::UUID>& focusedObjectIds);
	// SetEditorModeEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetEditorModeEnabled(bool enabled) { editorModeEnabled = enabled; }
	// SetGameViewCamera is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetGameViewCamera(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& pos) {
		gameViewCameras.push_back({view, proj, pos});
	}
	// Isolated prefab asset editing does not have an active game scene, so the
	// editor can suppress Game View submission for that frame and leave GPU time
	// available for Scene View and the rest of the desktop.
	void SetGameViewSuppressed(bool suppressed) { gameViewSuppressed = suppressed; }
	// ClearOverrideCameras is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ClearOverrideCameras() {
		hasSceneViewCamera = false;
		gameViewCameras.clear();
		gameViewSuppressed = false;
		sceneViewContextTintEnabled = false;
		sceneViewContextFocusedObjectIds.clear();
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
	bool gameViewSuppressed = false;
	// Scene View prefab-context editing keeps the selected prefab branch normal
	// and submits a light tint for every other draw command in that view only.
	std::unordered_set<TerraEngine::UUID> sceneViewContextFocusedObjectIds;
	bool sceneViewContextTintEnabled = false;

	// Internal structs for data gathering
	// struct CameraEntry groups related state and operations so callers have one documented contract.
	struct CameraEntry
	{
		entt::entity entity;
		CameraComponent camera;
	};
	// struct RenderableEntry groups related state and operations so callers have one documented contract.
	struct RenderableEntry
	{
		entt::entity entity;
		TerraEngine::UUID objectId;
		bool isStatic = false;
		MeshRenderComponent mesh;
	};
	// struct LightEntry groups related state and operations so callers have one documented contract.
	struct LightEntry
	{
		entt::entity entity;
		TerraEngine::UUID lightId;
		LightComponent light;
	};

	mutable std::mutex statsMutex;
	TerraEngine::Scene::SpatialIndex spatialIndex;
	RenderStatsSnapshot latestGameStats{};
	RenderStatsSnapshot latestSceneStats{};
	TerraEngine::Scene::SpatialDiagnosticsSnapshot latestGameSpatialDiagnostics{};
	TerraEngine::Scene::SpatialDiagnosticsSnapshot latestSceneSpatialDiagnostics{};
	FrameBudgetDiagnosticsSnapshot latestGameBudgetDiagnostics{};
	FrameBudgetDiagnosticsSnapshot latestSceneBudgetDiagnostics{};
	LightingDiagnosticsSnapshot latestGameLightingDiagnostics{};
	LightingDiagnosticsSnapshot latestSceneLightingDiagnostics{};
	RenderSnapshotDiagnosticsSnapshot latestRenderSnapshotDiagnostics{};
	SkyRuntimeState latestSkyRuntimeState{};
	bool skyStateDirty = true;
	std::string lastLoggedSkyIssueKey;

	QualityPreset currentPreset = QualityPreset::High;
	RenderSettings currentSettings;
};
