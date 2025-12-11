#pragma once

#include "Engine/Core/SystemBase.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Transform.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Scene/MeshRenderComponent.h"
#include "Engine/Scene/CameraComponent.h"
#include "Engine/Utilities/UUID.h"
#include <entt/entt.hpp>
#include <unordered_map>
#include <vector>
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
	std::unordered_map<entt::entity, std::vector<std::string>> inspectorComponentOrder;
	std::unordered_map<entt::entity, glm::vec3> rotationEditCache;
	std::unordered_map<entt::entity, glm::quat> rotationLastQuat;

	void RenderUI(void* cmdBuffer);
	void DrawHierarchyWindow();
	void DrawHierarchyNode(const Object& object);
	void DrawInspectorWindow();
	void DrawComponentInspector(const std::string& componentName, const ComponentAccessFunctions& access, int index, std::function<void(int, int)> onReorder);
	void EnsureComponentOrder(Object& object);
	void ReorderComponent(std::vector<std::string>& order, int from, int to);
	bool DrawPropertyEditor(const std::string& label, rttr::type type, rttr::variant& value, entt::entity entityId);
	glm::quat EulerDegreesToQuaternion(const glm::vec3& euler) const;
};
