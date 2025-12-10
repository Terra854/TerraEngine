#pragma once

#include "Engine/Core/SystemBase.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"

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

	void RenderUI(void* cmdBuffer);
};
