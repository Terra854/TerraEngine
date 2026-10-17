// RHIContext.h — Renderer Hardware Interface Context Definition
//
// Defines the base RHIContext interface, establishing the contract for graphics backend implementations.
// It provides abstract methods for resource management, frame submission, and state synchronization.
// This allows the high-level renderer to remain agnostic of the specific graphics API being used (e.g., Vulkan).

#pragma once

#include "RHIDefinitions.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/RendererRecovery.h"
#include "Renderer/VirtualGeometry/Diagnostics/VirtualGeometryReplay.h"
#include "Core/Export.h"
#include "CaptureReadbackLifetime.h"

#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <vector>

class WindowSystem;


class TERRAENGINE_API RHIContext
{
public:
	virtual ~RHIContext() = default;

	virtual void Initialize(WindowSystem& windowSystem) = 0;
	virtual void BeginFrame() = 0;
	virtual void RenderFrame(std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication, std::uint32_t viewIndex) = 0;

	virtual void EndFrame() = 0;

	virtual void SetGlobalTaskShaderCullingEnabled(bool enabled) {}
	// Backend capability data keeps high-level scheduling decisions portable.
	// The default profile is conservative so unknown backends remain correct.
	virtual TerraEngine::Renderer::BackendCapabilityProfile GetBackendCapabilityProfile() const
	{
		TerraEngine::Renderer::BackendCapabilityProfile profile;
		// Unknown backends start conservative: workers may prepare immutable data,
		// while final ownership transitions stay coordinator-owned until a backend
		// explicitly proves stronger queue and lifetime rules.
		profile.requiredSerialBoundaries = { "final-submit", "present", "swapchain", "recovery", "resource-retire" };
		profile.queueCapabilities = {
			{ "transfer", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, "Default backend profile uses a single safe queue." },
			{ "compute", TerraEngine::Renderer::RenderBackendQueueMode::Unavailable, false, "Default backend profile exposes no compute overlap." },
			{ "graphics", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, {} },
			{ "presentation", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, "Presentation remains coordinator-owned." }
		};
		return profile;
	}
	// A sealed publication is the backend-neutral counterpart to the execution
	// plan.  Backends retain this immutable root at their protected frame-slot
	// boundary and later resolve per-view selections without consulting live
	// scene or editor state.
	virtual bool ConsumeRenderFramePublication(
		std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication)
	{
		// Full selector/page validation is completed by SealRenderFramePublication
		// before the store exposes this root. Repeating a crowd scan at the protected
		// backend boundary would make submission scale with scene population twice.
		return publication != nullptr && publication->frameId != 0u
			&& publication->generation != 0u
			&& publication->rendererSessionGeneration != 0u
			&& publication->persistent != nullptr && publication->dynamic != nullptr
			&& publication->views.size() == publication->presentationInputs.size();
	}
	// Capture readback is a protected GPU-boundary operation. Backends that can
	// copy rendered color targets into CPU-visible memory override this method;
	// the base implementation reports a portable unsupported result so editor
	// automation can fall back without guessing about backend internals.
	virtual TerraEngine::Renderer::RenderCaptureResult ReadbackCapture(const TerraEngine::Renderer::RenderCaptureRequest& request)
	{
		TerraEngine::Renderer::RenderCaptureResult result;
		result.requestId = request.requestId;
		result.sourceKind = request.sourceKind;
		result.sourceName = request.sourceName;
		result.size = request.targetSize;
		result.errorCategory = "unsupported_readback";
		result.message = "The active RHI backend does not expose image readback.";
		return result;
	}
	// Backends publish completed readbacks at the protected render boundary.
	// Polling is non-blocking: an empty vector means GPU work is still pending.
	virtual std::vector<TerraEngine::Renderer::RenderCaptureResult> PollCaptureReadbacks()
	{
		return {};
	}
	// Probe support before submitting asynchronous work. Unsupported is never
	// retirement evidence; callers must fail/fallback before backend admission.
	virtual TerraEngine::Renderer::CaptureReadbackOwnership GetCaptureReadbackOwnership(std::string_view) const
	{
		return TerraEngine::Renderer::CaptureReadbackOwnership::Unsupported;
	}
	// A failed native idle proof may pause Shutdown. The caller must retain this
	// backend instance and retry rather than replacing its still-live device.
	virtual bool IsShutdownComplete() const { return true; }
	// Conventional visibility identity readback is intentionally a distinct
	// typed queue: color capture metadata must never be mistaken for GPU picking.
	virtual bool QueueConventionalVisibilityIdentityReadback(
		const TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackRequest&)
	{
		return false;
	}
	virtual std::optional<TerraEngine::Renderer::ConventionalVisibilityIdentityTargetMetadata>
		GetConventionalVisibilityIdentityTargetMetadata(
			RenderViewType, bool) const
	{
		return std::nullopt;
	}
	virtual std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult>
		PollConventionalVisibilityIdentityReadbacks()
	{
		return {};
	}

	virtual bool IsTaskShaderSupported() const { return false; }
	// Virtual geometry can still render through TerraEngine's mesh-only shader
	// collection when task shaders are absent, so this capability is tracked
	// separately from task-shader culling support.
	virtual bool SupportsVirtualGeometryMeshShaderRoute() const { return false; }
	virtual TerraEngine::Renderer::RenderDiagnosticsSummary GetLatestRenderGraphDiagnostics() const
	{
		return TerraEngine::Renderer::LatestRenderDiagnostics();
	}
	virtual TerraEngine::Renderer::BackendFrameBreakdown GetLatestBackendFrameBreakdown() const
	{
		return {};
	}
	// This compact receipt is independent from render timing diagnostics. A
	// backend returns a non-empty record only after it recorded animation compute
	// commands and successfully submitted their owning graphics command buffer.
	virtual TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
		GetLatestAnimationGpuBackendExecution() const
	{
		return {};
	}
	// Completed receipts and measurements are non-blocking protected-boundary
	// publications. Empty vectors mean that the backend has no completed data;
	// callers must not translate that absence into executed support or zero time.
	virtual std::vector<TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt>
		PollVirtualGeometryExecutionReceipts()
	{
		return {};
	}
	virtual std::vector<TerraEngine::Renderer::VirtualGeometryBackendMeasurement>
		PollVirtualGeometryMeasurements()
	{
		return {};
	}
	// Counter data is a separate delayed publication from timings and execution
	// receipts. The default preserves the conservative contract: callers retain
	// pending replay artifacts until a backend proves a completed counter copy.
	virtual std::vector<TerraEngine::Renderer::VirtualGeometryAuthoritativeCounterReadback>
		PollVirtualGeometryReplayCounters()
	{
		return {};
	}
	// Protected diagnostics consume owned, completed validation observations.
	// Backends without GPU evidence return an empty batch, never a measured zero.
	virtual std::vector<TerraEngine::Renderer::RendererEfficiencyEvidence> PollRendererEfficiencyEvidence()
	{
		return {};
	}
	// Validation reads current physical references at the protected boundary.
	// This CPU snapshot does not wait, complete commands, or authorize release.
	virtual std::optional<TerraEngine::Renderer::RendererEfficiencyStaticShadowExecution>
		GetRendererEfficiencyStaticLifetimeSnapshot() const
	{
		return std::nullopt;
	}

	virtual RenderStatsSnapshot GetLatestRenderStats(RenderViewType viewType) const
	{
		RenderStatsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}

	virtual FrameBudgetDiagnosticsSnapshot GetLatestFrameBudgetDiagnostics(RenderViewType viewType) const
	{
		FrameBudgetDiagnosticsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}

	virtual LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const
	{
		LightingDiagnosticsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}

	virtual RenderSnapshotDiagnosticsSnapshot GetLatestRenderSnapshotDiagnostics() const
	{
		return {};
	}
	// Recovery state is CPU-only and safe to read after a backend failure. The
	// base implementation reports an active renderer so simple/mock backends do
	// not need to implement recovery plumbing until they can lose a real device.
	virtual TerraEngine::Renderer::RendererRecoverySnapshot GetLatestRendererRecoverySnapshot() const
	{
		return TerraEngine::Renderer::MakeActiveRecoverySnapshot();
	}
	// This identity changes when a backend replaces presentation-compatible GPU
	// objects while retaining its renderer session. The conservative default is
	// stable so backends without explicit presentation recovery remain usable.
	virtual std::uint64_t GetBackendResourceGeneration() const { return 1u; }
	virtual void PublishRendererRecoverySnapshot(TerraEngine::Renderer::RendererRecoverySnapshot snapshot) { (void)snapshot; }

	virtual void InvalidateMaterial(const TerraEngine::UUID& materialId) {}

	virtual void InvalidateMesh(const TerraEngine::UUID& meshId) {}

	virtual void InvalidateTexture(const TerraEngine::UUID& textureId) {}

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

	virtual void ClearResources() {}
	virtual void WaitIdle() = 0;
	virtual void Shutdown() = 0;
};
