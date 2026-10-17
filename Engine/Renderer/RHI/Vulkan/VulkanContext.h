// VulkanContext.h — Vulkan RHI Context Definition
//
// Defines the Vulkan-specific implementation of the RHIContext interface.
// It provides the public API for the Vulkan backend, including frame rendering entry points and resource invalidation hooks.
// The class serves as the primary interface between the high-level RenderSystem and the Vulkan graphics API.

#pragma once

#include "../RHIContext.h"
#include "Core/Export.h"
#include <cstdint>
#include <memory>
#include <functional>
#include <string_view>

#include "VulkanPresentationLifecycle.h"
#include "VulkanEmptyFramePolicy.h"
#include "VulkanReadbackValidation.h"

class TERRAENGINE_API VulkanContext : public RHIContext
{
public:
	VulkanContext();
	~VulkanContext() override;

	void Initialize(WindowSystem& windowSystem) override;
	void BeginFrame() override;

	void RenderFrame(std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication, std::uint32_t viewIndex) override;
	void EndFrame() override;
	void SetGlobalTaskShaderCullingEnabled(bool enabled) override;
	TerraEngine::Renderer::BackendCapabilityProfile GetBackendCapabilityProfile() const override;
	bool ConsumeRenderFramePublication(
		std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication) override;
	TerraEngine::Renderer::RenderCaptureResult ReadbackCapture(const TerraEngine::Renderer::RenderCaptureRequest& request) override;
	std::vector<TerraEngine::Renderer::RenderCaptureResult> PollCaptureReadbacks() override;
	TerraEngine::Renderer::CaptureReadbackOwnership GetCaptureReadbackOwnership(std::string_view requestId) const override;
	bool IsShutdownComplete() const override;
	// Quiescent native validation only; these return unsupported in production
	// builds and are deliberately absent from editor/MCP command dispatch.
	bool ArmReadbackLifetimeGate(std::string_view requestId);
	// Inject one C++ allocation failure at the real completed-payload allocation
	// checkpoint. Requires this same native validation gate before submission.
	bool ArmReadbackPayloadAllocationFailure(std::string_view requestId);
	bool ReleaseReadbackLifetimeGate();
	TerraEngine::Renderer::Vulkan::ReadbackLifetimeValidationSnapshot GetReadbackLifetimeValidationSnapshot() const;
	// Four bounded slots admit scene/game x ordinary/dedicated selectors at
	// once. Additional pending work returns false; drain results before retrying.
	bool QueueConventionalVisibilityIdentityReadback(
		const TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackRequest& request) override;
	std::optional<TerraEngine::Renderer::ConventionalVisibilityIdentityTargetMetadata>
		GetConventionalVisibilityIdentityTargetMetadata(
			RenderViewType viewType,
			bool dedicatedCaptureTarget) const override;
	std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult>
		PollConventionalVisibilityIdentityReadbacks() override;
	std::vector<TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt>
		PollVirtualGeometryExecutionReceipts() override;
	std::vector<TerraEngine::Renderer::VirtualGeometryBackendMeasurement>
		PollVirtualGeometryMeasurements() override;
	TerraEngine::Renderer::BackendFrameBreakdown GetLatestBackendFrameBreakdown() const override;
	TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
		GetLatestAnimationGpuBackendExecution() const override;
	bool IsTaskShaderSupported() const override;
	bool SupportsVirtualGeometryMeshShaderRoute() const override;
	RenderStatsSnapshot GetLatestRenderStats(RenderViewType viewType) const override;
	std::vector<TerraEngine::Renderer::RendererEfficiencyEvidence> PollRendererEfficiencyEvidence() override;
	std::optional<TerraEngine::Renderer::RendererEfficiencyStaticShadowExecution>
		GetRendererEfficiencyStaticLifetimeSnapshot() const override;
	FrameBudgetDiagnosticsSnapshot GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const override;
	LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const override;
	RenderSnapshotDiagnosticsSnapshot GetLatestRenderSnapshotDiagnostics() const override;
	TerraEngine::Renderer::RendererRecoverySnapshot GetLatestRendererRecoverySnapshot() const override;
	std::uint64_t GetBackendResourceGeneration() const override;
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
	// Opt-in validation handshake. Editor releases all old device bindings between
	// these calls; healthy recreation never fabricates a device-loss result.
	bool PrepareRendererEfficiencyDeviceRecreation();
	bool RecreateRendererEfficiencyDevice();
	void SetValidationLayersRequested(bool enabled);
	// Quiescent native-fixture hook. It rejects scene submission for one acquired
	// runtime frame; receipts describe only operations actually attempted.
	bool ArmEmptyFrameValidation(TerraEngine::Renderer::Vulkan::EmptyFrameValidationRequest request);
	TerraEngine::Renderer::Vulkan::EmptyFrameValidationReceipt GetEmptyFrameValidationReceipt() const;
	bool ConfigureEmptyFrameValidation(bool disableMaintenance);
	bool RecreateEmptyFrameValidation(bool rejectCandidate);
	bool RecreateEmptyFrameValidationAt(TerraEngine::Renderer::Vulkan::EmptyFrameCandidateCheckpoint checkpoint);
	void SetVariableRateShadingEnabled(bool enabled);

	// ViewType names the supported states explicitly so serialization and switches stay readable.
	enum class ViewType {
		Game,
		Scene
	};

	void Resize(int width, int height, ViewType view = ViewType::Game);


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
		std::uint64_t sceneImageGeneration = 0u;
		void* gameImageView = nullptr;
		void* gameSampler = nullptr;
		std::uint64_t gameImageGeneration = 0u;
		void* vkGetInstanceProcAddr = nullptr;
	};
	void GetEditorHandles(EditorHandles& outHandles);
	// ImGui owns a few internal Vulkan queue submissions for font/texture
	// uploads. Run those calls inside the same queue-wide synchronization used
	// by engine frames and background asset uploads.
	void RunWithQueueAccess(const std::function<void()>& operation);
	
	void EnableEditorMode();
	
	// prepareUI runs after the frame fence but before Vulkan begins the UI command
	// buffer/render pass. renderUI then receives VkCommandBuffer (cast to void*).
	// Both callbacks share the graphics queue synchronization boundary.
	// PresentEditor reports whether draw data reached the queue; editor-owned
	// descriptor retirement must never infer submission from a void callback.
	[[nodiscard]] TerraEngine::Renderer::Vulkan::EditorPresentResult PresentEditor(
		std::function<void()> prepareUI,
		std::function<void(void*)> renderUI);
	[[nodiscard]] TerraEngine::Renderer::Vulkan::EditorSubmissionProgress GetEditorSubmissionProgress() const;
	// Frees one editor-owned descriptor through TerraEngine's checked and
	// externally synchronized descriptor-pool boundary.
	[[nodiscard]] std::int32_t FreeEditorDescriptorSet(void* descriptorSet);
	void AcknowledgeViewportTextureGeneration(ViewType view, std::uint64_t generation,
		std::uint64_t finalUseSerial);

private:
	// Publishes the compute/deformation work sealed inside a frame publication.
	// Keeping this helper private prevents a second externally callable frame
	// hand-off from drifting away from the immutable publication root.
	bool ConsumePublishedWorkPlans(
		const TerraEngine::Renderer::RenderBackendWorkPublication& plan);

	struct Impl;
	std::unique_ptr<Impl> impl;
	bool efficiencyDeviceRecreationPrepared = false;
};
