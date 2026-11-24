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

namespace TerraEngine::Tests::Review { struct VulkanLifecycleReviewAccess; struct VulkanTextureReviewAccess; struct VulkanDiscardedPrimaryReviewAccess; }
namespace TerraEngine::Renderer { struct PublishedGraphicsCameraView; }
namespace TerraEngine::Renderer::GraphicsSettings {
	struct PostProcessTimingSample;
	struct PostProcessDiagnosticsSnapshot;
	struct GraphicsRequestReceipt;
	struct GraphicsCatalogCapabilities;
}

class TERRAENGINE_API VulkanContext : public RHIContext
{
public:
	VulkanContext();
	~VulkanContext() override;

	void Initialize(WindowSystem& windowSystem) override;
	// A minimized startup waits for a drawable extent at the protected boundary.
	bool IsReady() const;
	bool IsInitializationDeferred() const;
	// Query the selected device and the actual instance/pass state once per
	// startup or recovery. Requested startup flags are not effective values.
	TerraEngine::Renderer::GraphicsSettings::GraphicsCatalogCapabilities ReadGraphicsCatalogCapabilities(
		bool runtimeViews) const;
	// Return exact native spatial options for the supplied view extent.
	std::uint32_t ReadSpatialAAModeMask(glm::uvec2 extent) const;
	std::pair<bool, bool> ReadGraphicsStartupFlags() const;
	struct RecordedGraphicsView
	{
		std::uint32_t publicationIndex = 0;
		std::uint32_t width = 0, height = 0;
		std::uint32_t indexedDrawCount = 0;
		// This comes from the actual recorded camera pipeline and pass shape.
		// It is independent of the settings snapshot's requested samples.
		std::uint32_t rasterSampleCount = 1;
		// Join these command-side facts to a successful native capture before
		// drawing conclusions about an actually submitted reconstruction.
		std::uint32_t recordedMotionDrawCount = 0u;
		std::uint64_t temporalRecordingId = 0u;
		std::uint64_t temporalPreviousFrameId = 0u;
		std::uint32_t internalWidth = 0u, internalHeight = 0u;
		std::uint32_t temporalJitterPhase = 0u;
	};
	std::vector<RecordedGraphicsView> ReadRecordedGraphicsViews(std::uint64_t frameId) const;
	// Native fixture diagnostics can validate the immutable inputs to a known
	// completed capture. Combine this with the capture receipt for submission proof.
	std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> ReadAcceptedRenderFramePublication(
		std::uint64_t expectedFrameId) const;
	void RetireInactiveCameraPostProcessTargets(std::span<const TerraEngine::Renderer::PublishedGraphicsCameraView> activeViews);
	void BeginFrame() override;

	void RenderFrame(std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication, std::uint32_t viewIndex) override;
	// Protected backend preparation for one pending fixed-exposure candidate.
	// Success means actual images and pipelines exist; it does not publish any
	// requested setting or render a frame on the caller's behalf.
	bool PrepareFixedExposureView(const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
		const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot& candidate,
		std::string& reason);
	bool PreparePostProcessCandidate(const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
		const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot& candidate,
		std::string& reason);
	// Prepare the exact affected view before an indexed settings publication.
	bool PrepareIndexedRasterCandidate(const TerraEngine::Renderer::RenderFramePublication& publication,
		std::uint32_t viewIndex, const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot& candidate,
		std::string& reason, bool& preparationPending);
	void DiscardPostProcessCandidates();
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
	// Query copies bounded, completed per-output samples. Pending, unavailable,
	// and abandoned records retain their status and never imply a zero duration.
	std::vector<TerraEngine::Renderer::GraphicsSettings::PostProcessTimingSample> GetCompletedPostProcessTimings() const;
	// Query one immutable view and optionally its caller-correlated rejected
	// request. Effect state never borrows timing from another view or revision.
	TerraEngine::Renderer::GraphicsSettings::PostProcessDiagnosticsSnapshot GetPostProcessDiagnostics(
		const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
		const TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReceipt* correlatedRequest = nullptr) const;
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
	struct SceneViewPostProcessPreview
	{
		bool requestedRaw = false;
		bool effectiveRaw = false;
		bool postProcessActive = false;
		std::uint64_t frameId = 0u;
		glm::uvec2 extent{0u};
	};
	// The request affects the next Scene output. Query reports the last
	// accepted output so the editor never labels pending pixels as ready.
	void SetSceneViewRawPreview(bool raw);
	[[nodiscard]] SceneViewPostProcessPreview GetSceneViewPostProcessPreview() const;
	// Match the exact sampled view and its retained offscreen generation. A
	// merely recorded or rejected primary never supplies a content publication.
	[[nodiscard]] std::optional<TerraEngine::Renderer::Vulkan::SceneViewContentStamp>
		GetSceneViewContentStamp(void* imageView, std::uint64_t targetGeneration) const;
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
		std::function<void(void*)> renderUI, bool captureWindowProof = false);
	[[nodiscard]] TerraEngine::Renderer::Vulkan::EditorSubmissionProgress GetEditorSubmissionProgress() const;
	// Frees one editor-owned descriptor through TerraEngine's checked and
	// externally synchronized descriptor-pool boundary.
	[[nodiscard]] std::int32_t FreeEditorDescriptorSet(void* descriptorSet);
	void AcknowledgeViewportTextureGeneration(ViewType view, std::uint64_t generation,
		std::uint64_t finalUseSerial);

private:
	bool PreparePostProcessView(const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
		const TerraEngine::Renderer::GraphicsSettings::GraphicsSettingsSnapshot& candidate,
		bool prepareOnly, std::string& reason);
	friend struct TerraEngine::Tests::Review::VulkanLifecycleReviewAccess;
	friend struct TerraEngine::Tests::Review::VulkanTextureReviewAccess;
	friend struct TerraEngine::Tests::Review::VulkanDiscardedPrimaryReviewAccess;
	// Publishes the compute/deformation work sealed inside a frame publication.
	// Keeping this helper private prevents a second externally callable frame
	// hand-off from drifting away from the immutable publication root.
	bool ConsumePublishedWorkPlans(
		const TerraEngine::Renderer::RenderBackendWorkPublication& plan);

	struct Impl;
	std::unique_ptr<Impl> impl;
	bool efficiencyDeviceRecreationPrepared = false;
};
