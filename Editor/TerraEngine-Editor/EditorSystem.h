#pragma once

#include "Engine/Core/SystemBase.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Transform.h"
#include "Engine/Scene/MeshRenderComponent.h"
#include "Engine/Scene/CameraComponent.h"
#include "Engine/Utilities/UUID.h"
#include <entt/entt.hpp>
#include <optional>

// ImGui includes
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

class EditorSystem : public SystemsBase {
public:
	EditorSystem(Engine& engine);
	~EditorSystem() override;

	void Initialize() override;
	void Update() override;
	void Shutdown() override;
	std::string SystemName() override;

private:
	Engine& engine;
	VulkanContext* rhi = nullptr;
	WindowSystem* windowSystem = nullptr;
	VkDescriptorSet sceneDescriptorSet = VK_NULL_HANDLE;
	bool initialized = false;
	std::optional<Object> selectedObject;
	entt::entity cachedRotationEntity = entt::null;
	glm::vec3 cachedEulerDegrees{0.0f, 0.0f, 0.0f};
	glm::quat cachedRotation{1.0f, 0.0f, 0.0f, 0.0f};
	bool cachedRotationValid = false;

	void RenderUI(void* cmdBuffer);
	void DrawHierarchyWindow();
	void DrawHierarchyNode(const Object& object);
	void DrawInspectorWindow();
	void DrawTransformInspector(Transform& transform);
	void DrawCameraInspector(CameraComponent& camera);
	void DrawMeshRenderInspector(MeshRenderComponent& meshComponent);
	glm::vec3 QuaternionToEulerDegrees(const glm::quat& q) const;
	glm::quat EulerDegreesToQuaternion(const glm::vec3& euler) const;
};
