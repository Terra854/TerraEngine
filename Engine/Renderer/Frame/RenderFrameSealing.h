#include <chrono>
// View lowering transfers owned arrays into typed presentation inputs before publication sealing.
#pragma once
#include "RenderViewPreparation.h"
	struct RenderSystem::PreparedRenderSubmission
	{
		// Commands and RenderViewBuildState are temporary worker scratch. Queueing
		// lowers commands into compact selectors and transfers every owned vector
		// directly into this immutable presentation. Keeping a second frame copy here
		// would duplicate the complete grouped-instance stream for every camera.
		RenderOutputRequest output;
		TerraEngine::Renderer::RenderViewSelection selection;
		TerraEngine::Renderer::RenderViewPresentationInput presentation;
	};

// These view outputs remain coroutine-owned until the protected backend commit.
struct RenderSystem::FrameViewBuildOutputs {
	std::vector<PreparedRenderSubmission> preparedRenderSubmissions;
	std::vector<std::uint8_t> visibleAnimatedRenderableFlags;
	TerraEngine::Scene::SpatialDiagnosticsSnapshot sceneSpatialDiagnostics{}, gameSpatialDiagnostics{};
	TerraEngine::Renderer::VisibleCommandReuseTelemetry sceneVisibleCommandTelemetry{}, gameVisibleCommandTelemetry{};
};
// Borrowed immutable selection inputs shared by scene, game and dedicated-view builders.
struct RenderSystem::FrameViewSelectionSettings {
	std::span<const RenderItemRecord* const> renderableDrawCommandsByOrdinal;
	std::uint64_t renderPreparationSettingsSignature, renderableSubmissionSignature;
	std::size_t renderGraphWorkerCount;
	bool renderTopologyRebuilt;
	glm::uvec2 effectiveSceneTargetSize, effectiveGameTargetSize;
};

// Clock values preserve the existing disjoint profiling boundaries across compiled helpers.
struct RenderSystem::FramePublicationTimings {
	std::chrono::steady_clock::time_point backendPreparationStart, backendPreparationEnd;
	std::chrono::steady_clock::time_point publicationSealStart, persistentPublicationEnd;
	std::chrono::steady_clock::time_point dynamicPublicationEnd, viewPublicationEnd;
};

// Copied per-output evidence survives through protected target metadata and final diagnostics.
struct RenderSystem::FrameViewObservations {
	std::vector<GpuVirtualGeometryPacketRecord> completedSceneVisibilityRecords;
	std::vector<GpuVirtualGeometryPacketRecord> completedGameVisibilityRecords;
	std::vector<GpuVirtualGeometryPacketRecord> completedSceneCaptureVisibilityRecords;
	std::vector<GpuVirtualGeometryPacketRecord> completedGameCaptureVisibilityRecords;
	CaptureConventionalVisibilityIdentitySnapshot completedSceneCaptureIdentitySnapshot;
	CaptureConventionalVisibilityIdentitySnapshot completedGameCaptureIdentitySnapshot;
	bool sceneCaptureProcessed = false;
	bool gameCaptureProcessed = false;
};
