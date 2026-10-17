// Per-view visibility borrows retained base/spatial caches; resource demand uses copied terrain values.
#pragma once
#include "RenderFrameState.h"
#include "RenderFrameCapture.h"
#include "RenderVirtualGeometrySelection.h"
struct RenderSystem::FrameVisibilityInputs {
	std::span<const RenderableEntry> renderables;
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot;
	const TerraEngine::Renderer::Frame::SpatialPreparationCacheState& spatialCache;
	const std::vector<RenderItemRecord>& renderableDrawCommands;
	const TerraEngine::Renderer::Frame::RetainedRenderTopologyState& retainedRenderTopology;
	const TerraEngine::Renderer::Frame::RenderableFrameIdentityCacheState& identityCache;
	const TerraEngine::Scene::SpatialDiagnosticsSnapshot& latestSpatialRefreshDiagnostics;
	std::uint64_t renderableVisibilitySignature, snapshotRendererGeneration, virtualGeometryResidencyGeneration;
};
struct RenderSystem::FrameVisibleStreamingInputs {
	const std::unordered_map<TerraEngine::UUID, const Material*>& frameMaterialResources;
	const FrameTerrainCapture& terrain;
};
