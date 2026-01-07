#pragma once

#include "../RHIContext.h"
#include "Core/Export.h"
#include <memory>
#include <functional>

class TERRAENGINE_API VulkanContext : public RHIContext
{
public:
	VulkanContext();
	~VulkanContext() override;

	void Initialize(WindowSystem& windowSystem) override;
	void BeginFrame() override;
	void RenderFrame(const FrameRenderData& frameData) override;
	void EndFrame() override;
	void InvalidateMaterial(const TerraEngine::UUID& materialId) override;
	void InvalidateMesh(const TerraEngine::UUID& meshId) override;
	void InvalidateTexture(const TerraEngine::UUID& textureId) override;
	void WaitIdle() override;
	void Shutdown() override;

	void Resize(int width, int height);

	// Editor / ImGui Integration
	struct EditorHandles {
		void* instance = nullptr;
		void* physicalDevice = nullptr;
		void* device = nullptr;
		void* queue = nullptr;
		uint32_t queueFamily = 0;
		void* descriptorPool = nullptr;
		void* renderPass = nullptr;
		uint32_t minImageCount = 0;
		uint32_t imageCount = 0;
		void* sceneImageView = nullptr;
		void* sceneSampler = nullptr;
		void* vkGetInstanceProcAddr = nullptr;
	};
	void GetEditorHandles(EditorHandles& outHandles);
	
	void EnableEditorMode();
	
	// callback takes VkCommandBuffer (cast to void*)
	void PresentEditor(std::function<void(void*)> renderUI);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
