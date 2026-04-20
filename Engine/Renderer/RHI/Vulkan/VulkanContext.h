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
	void RenderFrame(const FrameRenderData& frameData) override {
        RenderFrame(frameData, ViewType::Game);
    }
	void EndFrame() override;
	void SetGlobalTaskShaderCullingEnabled(bool enabled) override;
	bool IsTaskShaderSupported() const override;
	RenderStatsSnapshot GetLatestRenderStats(RenderViewType viewType) const override;
	FrameBudgetDiagnosticsSnapshot GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const override;
	LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const override;
	void InvalidateMaterial(const TerraEngine::UUID& materialId) override;
	void InvalidateMesh(const TerraEngine::UUID& meshId) override;
	void InvalidateTexture(const TerraEngine::UUID& textureId) override;
	void InvalidateResource(const TerraEngine::UUID& id) override;
	void WaitIdle() override;
	void Shutdown() override;
    void ClearResources() override;

    enum class ViewType {
        Game,
        Scene
    };

	void Resize(int width, int height, ViewType view = ViewType::Game);

    virtual void RenderFrame(const FrameRenderData& frameData, ViewType view);

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
        void* gameImageView = nullptr;
        void* gameSampler = nullptr;
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
