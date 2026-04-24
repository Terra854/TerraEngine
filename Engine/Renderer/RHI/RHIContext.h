#pragma once

#include "RHIDefinitions.h"
#include "Renderer/RenderTypes.h"
#include "Core/Export.h"

class WindowSystem;

class TERRAENGINE_API RHIContext
{
public:
	virtual ~RHIContext() = default;

	virtual void Initialize(WindowSystem& windowSystem) = 0;
	virtual void BeginFrame() = 0;
	virtual void RenderFrame(const FrameRenderData& frameData) = 0;
	virtual void RenderFrame(const FrameRenderData& frameData, const RenderOutputRequest& outputRequest)
	{
		(void)outputRequest;
		RenderFrame(frameData);
	}
	virtual void EndFrame() = 0;
	virtual void SetGlobalTaskShaderCullingEnabled(bool enabled) {}
	virtual bool IsTaskShaderSupported() const { return false; }
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
	virtual void InvalidateMaterial(const TerraEngine::UUID& materialId) {}
	virtual void InvalidateMesh(const TerraEngine::UUID& meshId) {}
	virtual void InvalidateTexture(const TerraEngine::UUID& textureId) {}
	virtual void InvalidateResource(const TerraEngine::UUID& id) {}
	virtual void ClearResources() {}
	virtual void WaitIdle() = 0;
	virtual void Shutdown() = 0;
};
