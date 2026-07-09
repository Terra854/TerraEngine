// VulkanContext.h — Vulkan RHI Context Definition
//
// Defines the Vulkan-specific implementation of the RHIContext interface.
// It provides the public API for the Vulkan backend, including frame rendering entry points and resource invalidation hooks.
// The class serves as the primary interface between the high-level RenderSystem and the Vulkan graphics API.

#pragma once

#include "../RHIContext.h"
#include "Core/Export.h"
#include <memory>
#include <functional>
#include <string_view>

class TERRAENGINE_API VulkanContext : public RHIContext
{
public:
	VulkanContext();
	~VulkanContext() override;

	void Initialize(WindowSystem& windowSystem) override;
	void BeginFrame() override;
	// RenderFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void RenderFrame(const FrameRenderData& frameData) override {
		RenderFrame(frameData, ViewType::Game);
	}
	void RenderFrame(const FrameRenderData& frameData, const RenderOutputRequest& outputRequest) override;
	void EndFrame() override;
	void SetGlobalTaskShaderCullingEnabled(bool enabled) override;
	TerraEngine::Renderer::BackendCapabilityProfile GetBackendCapabilityProfile() const override;
	bool ConsumeRenderSubmissionPlan(const TerraEngine::Renderer::RenderSubmissionPlan& plan) override;
	TerraEngine::Renderer::RenderCaptureResult ReadbackCapture(const TerraEngine::Renderer::RenderCaptureRequest& request) override;
	TerraEngine::Renderer::BackendFrameBreakdown GetLatestBackendFrameBreakdown() const override;
	bool IsTaskShaderSupported() const override;
	bool SupportsVirtualGeometryMeshShaderRoute() const override;
	RenderStatsSnapshot GetLatestRenderStats(RenderViewType viewType) const override;
	FrameBudgetDiagnosticsSnapshot GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const override;
	LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const override;
	RenderSnapshotDiagnosticsSnapshot GetLatestRenderSnapshotDiagnostics() const override;
	TerraEngine::Renderer::RendererRecoverySnapshot GetLatestRendererRecoverySnapshot() const override;
	bool SupportsRendererRecoveryRebuild() const override;
	TerraEngine::Renderer::RendererRecoverySnapshot BeginRendererRecoverySession(
		std::uint32_t attemptNumber,
		std::uint32_t maxAttempts,
		TerraEngine::Renderer::RendererRecoveryAction requestedBy) override;
	bool RebuildRendererSession(
		WindowSystem& windowSystem,
		const TerraEngine::Renderer::ResourceRestorationPlan& restorationPlan,
		TerraEngine::Renderer::RendererRecoverySnapshot* outSnapshot,
		std::string* outError) override;
	void PublishRendererRecoverySnapshot(TerraEngine::Renderer::RendererRecoverySnapshot snapshot) override;
	struct LiveGpuSubmissionBoundary
	{
		std::string classification;
		std::string workerSafePreparation;
		std::string protectedSubmission;
	};
	[[nodiscard]] LiveGpuSubmissionBoundary GetLiveGpuSubmissionBoundary() const;
	void InvalidateMaterial(const TerraEngine::UUID& materialId) override;
	void InvalidateMesh(const TerraEngine::UUID& meshId) override;
	void InvalidateTexture(const TerraEngine::UUID& textureId) override;
	void InvalidateResource(const TerraEngine::UUID& id) override;
	void RetireLoadedResource(const TerraEngine::UUID& id, std::uint64_t frameId, std::string_view debugName) override;
	void WaitIdle() override;
	void Shutdown() override;
	void ClearResources() override;
	void SetValidationLayersRequested(bool enabled);
	void SetVariableRateShadingEnabled(bool enabled);

	// ViewType names the supported states explicitly so serialization and switches stay readable.
	enum class ViewType {
		Game,
		Scene
	};

	void Resize(int width, int height, ViewType view = ViewType::Game);

	virtual void RenderFrame(const FrameRenderData& frameData, ViewType view);
	virtual void RenderFrame(const FrameRenderData& frameData, ViewType view, const RenderOutputRequest& outputRequest);

	// Editor / ImGui Integration
	// struct EditorHandles groups related state and operations so callers have one documented contract.
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
	// struct Impl groups related state and operations so callers have one documented contract.
	struct Impl;
	std::unique_ptr<Impl> impl;
};
