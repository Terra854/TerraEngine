// RHIContext.h — Renderer Hardware Interface Context Definition
//
// Defines the base RHIContext interface, establishing the contract for graphics backend implementations.
// It provides abstract methods for resource management, frame submission, and state synchronization.
// This allows the high-level renderer to remain agnostic of the specific graphics API being used (e.g., Vulkan).

#pragma once

#include "RHIDefinitions.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Core/Export.h"

#include <string_view>

class WindowSystem;

// class TERRAENGINE_API groups related state and operations so callers have one documented contract.
class TERRAENGINE_API RHIContext
{
public:
	virtual ~RHIContext() = default;

	virtual void Initialize(WindowSystem& windowSystem) = 0;
	virtual void BeginFrame() = 0;
	virtual void RenderFrame(const FrameRenderData& frameData) = 0;
	// RenderFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void RenderFrame(const FrameRenderData& frameData, const RenderOutputRequest& outputRequest)
	{
		(void)outputRequest;
		RenderFrame(frameData);
	}
	virtual void EndFrame() = 0;
	// SetGlobalTaskShaderCullingEnabled is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void SetGlobalTaskShaderCullingEnabled(bool enabled) {}
	// Backend capability data keeps high-level scheduling decisions portable.
	// The default profile is conservative so unknown backends remain correct.
	virtual TerraEngine::Renderer::BackendCapabilityProfile GetBackendCapabilityProfile() const
	{
		TerraEngine::Renderer::BackendCapabilityProfile profile;
		// Unknown backends start conservative: workers may prepare immutable
		// data, but final ownership transitions stay coordinator-owned until the
		// backend explicitly proves a stronger contract. New APIs such as DX12
		// should fill this profile from their real queue and lifetime rules
		// instead of changing high-level render graph code.
		profile.requiredSerialBoundaries = { "final-submit", "present", "swapchain", "recovery", "resource-retire" };
		profile.queueCapabilities = {
			{ "transfer", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, "Default backend profile uses a single safe queue." },
			{ "compute", TerraEngine::Renderer::RenderBackendQueueMode::Unavailable, false, "Default backend profile exposes no compute overlap." },
			{ "graphics", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, {} },
			{ "presentation", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, "Presentation remains coordinator-owned." }
		};
		return profile;
	}
	// Backends consume a validated plan only at the coordinator boundary. The
	// current Vulkan path still records from FrameRenderData, so this hook is a
	// compatibility bridge for tests and future RHI implementations.
	virtual bool ConsumeRenderSubmissionPlan(const TerraEngine::Renderer::RenderSubmissionPlan& plan)
	{
		return plan.valid;
	}
	// IsTaskShaderSupported is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual bool IsTaskShaderSupported() const { return false; }
	virtual TerraEngine::Renderer::RenderDiagnosticsSummary GetLatestRenderGraphDiagnostics() const
	{
		return TerraEngine::Renderer::LatestRenderDiagnostics();
	}
	virtual TerraEngine::Renderer::BackendFrameBreakdown GetLatestBackendFrameBreakdown() const
	{
		return {};
	}
	// GetLatestRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual RenderStatsSnapshot GetLatestRenderStats(RenderViewType viewType) const
	{
		RenderStatsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}
	// GetLatestFrameBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual FrameBudgetDiagnosticsSnapshot GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const
	{
		FrameBudgetDiagnosticsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}
	// GetLatestLightingDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const
	{
		LightingDiagnosticsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}
	// GetLatestRenderSnapshotDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual RenderSnapshotDiagnosticsSnapshot GetLatestRenderSnapshotDiagnostics() const
	{
		return {};
	}
	// InvalidateMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void InvalidateMaterial(const TerraEngine::UUID& materialId) {}
	// InvalidateMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void InvalidateMesh(const TerraEngine::UUID& meshId) {}
	// InvalidateTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void InvalidateTexture(const TerraEngine::UUID& textureId) {}
	// InvalidateResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void InvalidateResource(const TerraEngine::UUID& id) {}
	// RetireLoadedResource is used by RAM/VRAM residency cleanup. The default
	// backend implementation is conservative and simply invalidates now; APIs
	// with explicit frame fences can override this to defer physical release.
	virtual void RetireLoadedResource(const TerraEngine::UUID& id, std::uint64_t frameId, std::string_view debugName)
	{
		(void)frameId;
		(void)debugName;
		InvalidateResource(id);
	}
	// ClearResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void ClearResources() {}
	virtual void WaitIdle() = 0;
	virtual void Shutdown() = 0;
};
