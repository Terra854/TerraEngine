#pragma once

#include "Core/SystemBase.h"
#include "RHI/RHIContext.h"
#include "Core/Object/ObjectManager.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <entt/entt.hpp>
#include "Core/Object/Transform.h"
#include "Scene/CameraComponent.h"
#include "Scene/MeshRenderComponent.h"

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
    
    void Clear();

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
};
