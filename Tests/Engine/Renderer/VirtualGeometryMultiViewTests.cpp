#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	namespace
	{
		VirtualGeometryAssetRuntime MakeModeSwitchTransitionAsset()
		{
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = TerraEngine::GenerateUUID();
			asset.clusterCoverageValidated = true;
			asset.detailLevels = {
				{ 0u, 0u, 4u, 256u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 4u, 4u, 128u, 0.015f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			asset.cullableGroups.resize(8u);
			GeometryPageRuntime page{};
			page.pageId = 0u;
			page.firstLodIndex = 0u;
			page.lodCount = 2u;
			page.groupCount = 8u;
			page.state = GeometryPageState::Resident;
			page.fallbackPage = true;
			asset.pages = { page };

			VirtualGeometryClusterRuntime root{};
			root.clusterId = 0u;
			root.parentClusterId = kVirtualGeometryInvalidIndex;
			root.firstChildClusterId = 1u;
			root.childCount = 1u;
			root.lodIndex = 1u;
			root.firstGroupIndex = 4u;
			root.groupCount = 4u;
			root.pageId = 0u;
			root.fallbackClusterId = 0u;
			root.geometricError = 0.01f;
			root.boundsRadius = 1.0f;

			VirtualGeometryClusterRuntime child{};
			child.clusterId = 1u;
			child.parentClusterId = 0u;
			child.lodIndex = 0u;
			child.firstGroupIndex = 0u;
			child.groupCount = 4u;
			child.pageId = 0u;
			child.fallbackClusterId = 0u;
			child.boundsRadius = 1.0f;
			asset.clusters = { root, child };
			return asset;
		}
	}

	void RegisterVirtualGeometryMultiViewTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Render.MultiView.KeepsSelectionStateIsolatedPerView", [](TestContext& context) {
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();

			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 501u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState gameState;
			gameState.outputKind = RenderOutputKind::GameView;
			gameState.viewType = RenderViewType::Game;
			gameState.outputId = 2u;
			gameState.targetSize = glm::uvec2(1920u, 1080u);
			gameState.draws.push_back({ objectId, meshId, { 0u, 0u, 16u, true, false, {}, true }, true });

			TerraEngine::Renderer::VirtualGeometryViewSelectionState sceneState;
			sceneState.outputKind = RenderOutputKind::SceneView;
			sceneState.viewType = RenderViewType::Scene;
			sceneState.outputId = 1u;
			sceneState.targetSize = glm::uvec2(1280u, 720u);
			sceneState.draws.push_back({ objectId, meshId, { 2u, 24u, 4u, true, false, {}, true }, true });

			snapshot.virtualGeometryViewSelections = { gameState, sceneState };
			const std::vector<TerraEngine::Renderer::VirtualGeometryViewSelectionState> states =
				TerraEngine::Renderer::BuildVirtualGeometryViewSelectionStates(snapshot);

			const auto gameIt = std::find_if(states.begin(), states.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Game;
			});
			const auto sceneIt = std::find_if(states.begin(), states.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Scene;
			});

			context.Expect(gameIt != states.end(), "Game view virtual geometry state should be present.");
			context.Expect(sceneIt != states.end(), "Scene view virtual geometry state should be present.");
			context.ExpectEqual(16u, gameIt->draws.front().range.meshletCount, "Game view should keep its own selected meshlet range.");
			context.ExpectEqual(4u, sceneIt->draws.front().range.meshletCount, "Scene view should keep its own selected meshlet range.");
			context.Expect(gameIt->draws.front().range.meshletOffset != sceneIt->draws.front().range.meshletOffset, "Different views should not share one mutable LOD range.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.BuildsExportRuntimeDiagnosticsSeparately", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 777u;
			snapshot.viewRequests.push_back({ RenderOutputKind::MainRuntimeView, RenderViewType::Game, glm::uvec2(1280u, 720u), 9u });
			snapshot.renderables.push_back({
				TerraEngine::GenerateUUID(),
				TerraEngine::GenerateUUID(),
				TerraEngine::GenerateUUID(),
				glm::mat4(1.0f),
				{ 1u, 12u, 6u, true, false, {}, true },
				true,
				false
			});

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.hiZOcclusionEnabled = true;
			const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					quality,
					VirtualGeometryInactiveReason::None);

			context.ExpectEqual(static_cast<std::size_t>(1u), diagnostics.size(), "One runtime output should create one virtual geometry diagnostics snapshot.");
			context.Expect(diagnostics.front().viewType == RenderViewType::ExportRuntime, "Main runtime output should be reported separately from editor Game View.");
			context.ExpectEqual(6u, diagnostics.front().selectedMeshlets, "Export runtime diagnostics should use the runtime view's selected range.");
			context.Expect(!diagnostics.front().warnings.empty(), "Export runtime diagnostics should explain that the state is isolated from editor views.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.KeepsCounterSourcesPerView", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 888u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState gameState;
			gameState.outputKind = RenderOutputKind::GameView;
			gameState.viewType = RenderViewType::Game;
			gameState.outputId = 4u;
			gameState.targetSize = glm::uvec2(1920u, 1080u);
			gameState.draws.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), { 0u, 0u, 12u, true, false, {}, true }, true });

			TerraEngine::Renderer::VirtualGeometryViewSelectionState sceneState = gameState;
			sceneState.outputKind = RenderOutputKind::SceneView;
			sceneState.viewType = RenderViewType::Scene;
			sceneState.outputId = 5u;
			sceneState.draws.front().range.meshletCount = 3u;

			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, gameState, VirtualGeometryCounterSource::DelayedGPUReadback));
			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, sceneState, VirtualGeometryCounterSource::GPUReadback));

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					MakeDefaultVirtualGeometryQualityProfile(),
					VirtualGeometryInactiveReason::None);

			const auto gameIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& snapshot) {
				return snapshot.viewType == RenderViewType::Game;
			});
			const auto sceneIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& snapshot) {
				return snapshot.viewType == RenderViewType::Scene;
			});

			context.Expect(gameIt != diagnostics.end(), "Game view diagnostics should be present.");
			context.Expect(sceneIt != diagnostics.end(), "Scene view diagnostics should be present.");
			context.Expect(gameIt->counterSource == VirtualGeometryCounterSource::DelayedGPUReadback, "Game view should keep its delayed counter source.");
			context.Expect(sceneIt->counterSource == VirtualGeometryCounterSource::GPUReadback, "Scene view should keep its GPU readback counter source.");
			context.ExpectEqual(12u, gameIt->selectedMeshlets, "Game view counter values should stay tied to its draw range.");
			context.ExpectEqual(3u, sceneIt->selectedMeshlets, "Scene view counter values should stay tied to its draw range.");
			context.Expect(!gameIt->countersAuthoritative, "Delayed Game View counters should not be authoritative.");
			context.Expect(sceneIt->countersAuthoritative, "GPU Scene View counters should be authoritative.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.KeepsDebugModesPerView", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 889u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState gameState;
			gameState.outputKind = RenderOutputKind::GameView;
			gameState.viewType = RenderViewType::Game;
			gameState.outputId = 2u;
			gameState.targetSize = glm::uvec2(1920u, 1080u);
			gameState.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::HiZ;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState sceneState = gameState;
			sceneState.outputKind = RenderOutputKind::SceneView;
			sceneState.viewType = RenderViewType::Scene;
			sceneState.outputId = 1u;
			sceneState.targetSize = glm::uvec2(1280u, 720u);
			sceneState.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::Cluster;

			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, gameState, VirtualGeometryCounterSource::Prepared));
			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, sceneState, VirtualGeometryCounterSource::Prepared));

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot> visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshots(snapshot);
			const auto gameIt = std::find_if(visibility.begin(), visibility.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Game;
			});
			const auto sceneIt = std::find_if(visibility.begin(), visibility.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Scene;
			});

			context.Expect(gameIt != visibility.end(), "Game view visibility should be present.");
			context.Expect(sceneIt != visibility.end(), "Scene view visibility should be present.");
			context.Expect(gameIt->debugVisualizationMode == VirtualGeometryDebugVisualizationMode::HiZ, "Game View should preserve its own VG visualizer mode.");
			context.Expect(sceneIt->debugVisualizationMode == VirtualGeometryDebugVisualizationMode::Cluster, "Scene View should preserve its own VG visualizer mode.");
			context.Expect(gameIt->cameraSignature != sceneIt->cameraSignature, "Different debug modes should still remain tied to separate view snapshots.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.US7.KeepsHiZBaselineDiagnosticsPerView", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 890u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState gameState;
			gameState.outputKind = RenderOutputKind::GameView;
			gameState.viewType = RenderViewType::Game;
			gameState.outputId = 2u;
			gameState.targetSize = glm::uvec2(1920u, 1080u);
			gameState.hasHiZValidationState = true;
			gameState.hiZValidationState.state = VirtualGeometryHiZState::Active;
			gameState.hiZValidationState.pyramidReady = true;
			gameState.hiZValidationState.pyramidSize = gameState.targetSize;
			gameState.hiZValidationState.renderSize = gameState.targetSize;
			gameState.hiZValidationState.warmupFrames = 3u;
			gameState.hiZValidationState.failOpen = false;
			HiZRegressionSnapshot gameBaseline{};
			gameBaseline.viewId = gameState.outputId;
			gameBaseline.viewType = gameState.viewType;
			gameBaseline.frameIndex = snapshot.frameId;
			gameBaseline.state = VirtualGeometryHiZState::Active;
			gameBaseline.baselineMatched = true;
			gameState.hiZRegressionSnapshots.push_back(gameBaseline);

			TerraEngine::Renderer::VirtualGeometryViewSelectionState sceneState = gameState;
			sceneState.outputKind = RenderOutputKind::SceneView;
			sceneState.viewType = RenderViewType::Scene;
			sceneState.outputId = 1u;
			sceneState.hiZRegressionSnapshots.clear();
			HiZRegressionSnapshot sceneBaseline = gameBaseline;
			sceneBaseline.viewId = sceneState.outputId;
			sceneBaseline.viewType = sceneState.viewType;
			sceneBaseline.baselineMatched = false;
			sceneState.hiZRegressionSnapshots.push_back(sceneBaseline);

			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, gameState, VirtualGeometryCounterSource::Prepared));
			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, sceneState, VirtualGeometryCounterSource::Prepared));

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					MakeDefaultVirtualGeometryQualityProfile(),
					VirtualGeometryInactiveReason::None);
			const auto gameIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& view) {
				return view.viewType == RenderViewType::Game;
			});
			const auto sceneIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& view) {
				return view.viewType == RenderViewType::Scene;
			});

			context.Expect(gameIt != diagnostics.end(), "Game view Hi-Z diagnostics should be present.");
			context.Expect(sceneIt != diagnostics.end(), "Scene view Hi-Z diagnostics should be present.");
			context.Expect(gameIt->hiZBaselineMatched, "Game View should keep its own matched baseline state.");
			context.Expect(!sceneIt->hiZBaselineMatched, "Scene View should keep its own baseline mismatch state.");
			context.ExpectEqual(0u, gameIt->hiZFalseOcclusionRiskWarnings, "Game View should not inherit Scene View false-occlusion risk warnings.");
			context.ExpectEqual(1u, sceneIt->hiZFalseOcclusionRiskWarnings, "Scene View should report only its own baseline risk warning.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.US5.GpuPacketsKeepHiZStatePerView", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 891u;

			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();

			TerraEngine::Renderer::VirtualGeometryViewSelectionState gameState;
			gameState.outputKind = RenderOutputKind::GameView;
			gameState.viewType = RenderViewType::Game;
			gameState.outputId = 2u;
			gameState.targetSize = glm::uvec2(1920u, 1080u);
			gameState.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::HiZ;
			gameState.hasHiZValidationState = true;
			gameState.hiZValidationState.state = VirtualGeometryHiZState::Active;
			gameState.hiZValidationState.pyramidReady = true;
			gameState.hiZValidationState.pyramidSize = gameState.targetSize;
			gameState.hiZValidationState.renderSize = gameState.targetSize;
			gameState.hiZValidationState.cameraSignature = 0x89120001u;
			gameState.hiZValidationState.depthSceneSignature = 0x89120002u;
			gameState.hiZValidationState.warmupFrames = 5u;

			GpuVirtualGeometryPacketRecord gamePacket{};
			gamePacket.packetId = 0u;
			gamePacket.viewId = gameState.outputId;
			gamePacket.viewType = gameState.viewType;
			gamePacket.frameId = snapshot.frameId;
			gamePacket.meshId = meshId;
			gamePacket.materialId = materialId;
			gamePacket.clusterId = 10u;
			gamePacket.meshletOffset = 0u;
			gamePacket.meshletCount = 32u;
			gameState.gpuPacketRecords.push_back(gamePacket);

			GpuVirtualGeometryPacketFrameSet gameFrameSet{};
			gameFrameSet.viewId = gameState.outputId;
			gameFrameSet.viewType = gameState.viewType;
			gameFrameSet.frameId = snapshot.frameId;
			gameFrameSet.targetSize = gameState.targetSize;
			gameFrameSet.packetCount = 1u;
			gameFrameSet.batchCount = 1u;
			gameFrameSet.validity = VirtualGeometryGpuFrameSetValidity::Valid;
			gameState.gpuPacketFrameSets.push_back(gameFrameSet);

			HiZRegressionSnapshot gameBaseline{};
			gameBaseline.viewId = gameState.outputId;
			gameBaseline.viewType = gameState.viewType;
			gameBaseline.frameIndex = snapshot.frameId;
			gameBaseline.state = VirtualGeometryHiZState::Active;
			gameBaseline.baselineMatched = true;
			gameState.hiZRegressionSnapshots.push_back(gameBaseline);

			TerraEngine::Renderer::VirtualGeometryViewSelectionState sceneState = gameState;
			sceneState.outputKind = RenderOutputKind::SceneView;
			sceneState.viewType = RenderViewType::Scene;
			sceneState.outputId = 1u;
			sceneState.targetSize = glm::uvec2(1280u, 720u);
			sceneState.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::Cluster;
			sceneState.hiZValidationState.pyramidSize = sceneState.targetSize;
			sceneState.hiZValidationState.renderSize = sceneState.targetSize;
			sceneState.hiZValidationState.cameraSignature = 0x89110001u;
			sceneState.hiZValidationState.depthSceneSignature = 0x89110002u;
			sceneState.gpuPacketRecords.clear();
			GpuVirtualGeometryPacketRecord scenePacket = gamePacket;
			scenePacket.packetId = 0u;
			scenePacket.viewId = sceneState.outputId;
			scenePacket.viewType = sceneState.viewType;
			scenePacket.clusterId = 22u;
			scenePacket.meshletOffset = 64u;
			scenePacket.meshletCount = 12u;
			sceneState.gpuPacketRecords.push_back(scenePacket);
			sceneState.gpuPacketFrameSets.clear();
			GpuVirtualGeometryPacketFrameSet sceneFrameSet = gameFrameSet;
			sceneFrameSet.viewId = sceneState.outputId;
			sceneFrameSet.viewType = sceneState.viewType;
			sceneFrameSet.targetSize = sceneState.targetSize;
			sceneState.gpuPacketFrameSets.push_back(sceneFrameSet);
			sceneState.hiZRegressionSnapshots.clear();
			HiZRegressionSnapshot sceneBaseline = gameBaseline;
			sceneBaseline.viewId = sceneState.outputId;
			sceneBaseline.viewType = sceneState.viewType;
			sceneBaseline.baselineMatched = true;
			sceneState.hiZRegressionSnapshots.push_back(sceneBaseline);

			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, gameState, VirtualGeometryCounterSource::Prepared));
			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, sceneState, VirtualGeometryCounterSource::Prepared));

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					MakeDefaultVirtualGeometryQualityProfile(),
					VirtualGeometryInactiveReason::None);
			const auto gameIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& view) {
				return view.viewType == RenderViewType::Game;
			});
			const auto sceneIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& view) {
				return view.viewType == RenderViewType::Scene;
			});

			context.Expect(gameIt != diagnostics.end(), "Game View GPU packet diagnostics should be present.");
			context.Expect(sceneIt != diagnostics.end(), "Scene View GPU packet diagnostics should be present.");
			context.Expect(gameIt->hiZBaselineMatched, "Game View should keep its own matched Hi-Z baseline.");
			context.Expect(sceneIt->hiZBaselineMatched, "Scene View should keep its own matched Hi-Z baseline.");
			context.Expect(gameIt->hiZCameraSignature != sceneIt->hiZCameraSignature, "Scene View and Game View should not share Hi-Z camera signatures.");
			context.ExpectEqual(1u, gameIt->gpuPacketRecordCount, "Game View should report only its own GPU packet table.");
			context.ExpectEqual(1u, sceneIt->gpuPacketRecordCount, "Scene View should report only its own GPU packet table.");
			context.ExpectEqual(1920u, gameIt->hiZPyramidSize.x, "Game View should keep its own Hi-Z pyramid width.");
			context.ExpectEqual(1080u, gameIt->hiZPyramidSize.y, "Game View should keep its own Hi-Z pyramid height.");
			context.ExpectEqual(1280u, sceneIt->hiZPyramidSize.x, "Scene View should keep its own Hi-Z pyramid width.");
			context.ExpectEqual(720u, sceneIt->hiZPyramidSize.y, "Scene View should keep its own Hi-Z pyramid height.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.US6.KeepsGpuPacketCountersPerView", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 892u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState gameState;
			gameState.outputKind = RenderOutputKind::GameView;
			gameState.viewType = RenderViewType::Game;
			gameState.outputId = 2u;
			gameState.targetSize = glm::uvec2(1920u, 1080u);
			for (std::uint32_t index = 0u; index < 3u; ++index)
			{
				GpuVirtualGeometryPacketRecord packet{};
				packet.packetId = index;
				packet.viewId = gameState.outputId;
				packet.viewType = gameState.viewType;
				packet.frameId = snapshot.frameId;
				packet.meshletCount = 8u;
				gameState.gpuPacketRecords.push_back(packet);
			}
			GpuPacketCompactionDispatch gameDispatch{};
			gameDispatch.viewId = gameState.outputId;
			gameDispatch.viewType = gameState.viewType;
			gameDispatch.frameId = snapshot.frameId;
			gameDispatch.candidatePacketCount = 3u;
			gameDispatch.outputCapacity = 3u;
			gameState.gpuCompactionDispatches.push_back(gameDispatch);
			GeneratedVirtualGeometrySubmissionRecord gameSubmission{};
			gameSubmission.viewId = gameState.outputId;
			gameSubmission.viewType = gameState.viewType;
			gameSubmission.frameId = snapshot.frameId;
			gameSubmission.packetCount = 3u;
			gameSubmission.validationState = VirtualGeometryGeneratedSubmissionValidationState::Valid;
			gameState.generatedSubmissions.push_back(gameSubmission);

			TerraEngine::Renderer::VirtualGeometryViewSelectionState sceneState = gameState;
			sceneState.outputKind = RenderOutputKind::SceneView;
			sceneState.viewType = RenderViewType::Scene;
			sceneState.outputId = 1u;
			sceneState.targetSize = glm::uvec2(1280u, 720u);
			sceneState.gpuPacketRecords.resize(1u);
			sceneState.gpuPacketRecords.front().viewId = sceneState.outputId;
			sceneState.gpuPacketRecords.front().viewType = sceneState.viewType;
			sceneState.gpuCompactionDispatches.front().viewId = sceneState.outputId;
			sceneState.gpuCompactionDispatches.front().viewType = sceneState.viewType;
			sceneState.gpuCompactionDispatches.front().candidatePacketCount = 1u;
			sceneState.generatedSubmissions.front().viewId = sceneState.outputId;
			sceneState.generatedSubmissions.front().viewType = sceneState.viewType;
			sceneState.generatedSubmissions.front().packetCount = 1u;

			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, gameState, VirtualGeometryCounterSource::GPUReadback));
			snapshot.virtualGeometryVisibilitySnapshots.push_back(
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, sceneState, VirtualGeometryCounterSource::DelayedGPUReadback));

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					MakeDefaultVirtualGeometryQualityProfile(),
					VirtualGeometryInactiveReason::None);
			const auto gameIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& view) {
				return view.viewType == RenderViewType::Game;
			});
			const auto sceneIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& view) {
				return view.viewType == RenderViewType::Scene;
			});

			context.Expect(gameIt != diagnostics.end(), "Game View diagnostics should be present.");
			context.Expect(sceneIt != diagnostics.end(), "Scene View diagnostics should be present.");
			context.ExpectEqual(3u, gameIt->gpuPacketRecordCount, "Game View should keep its own GPU packet count.");
			context.ExpectEqual(1u, sceneIt->gpuPacketRecordCount, "Scene View should keep its own GPU packet count.");
			context.ExpectEqual(1u, gameIt->gpuCompactionDispatchCount, "Game View should report its own GPU compaction dispatch.");
			context.ExpectEqual(1u, sceneIt->gpuCompactionDispatchCount, "Scene View should report its own GPU compaction dispatch.");
			context.Expect(gameIt->counterSource == VirtualGeometryCounterSource::GPUReadback, "Game View should keep its GPU readback source.");
			context.Expect(sceneIt->counterSource == VirtualGeometryCounterSource::DelayedGPUReadback, "Scene View should keep its delayed source.");
			context.ExpectEqual(1u, gameIt->validGeneratedSubmissionCount, "Game View should report its generated submission independently.");
			context.ExpectEqual(1u, sceneIt->validGeneratedSubmissionCount, "Scene View should report its generated submission independently.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.US7.KeepsPerformanceGateViewIdentityFor300Frames", [](TestContext& context) {
			std::uint32_t checkedGatePairs = 0u;
			for (std::uint64_t frame = 0u; frame < 300u; ++frame)
			{
				TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot gameVisibility{};
				gameVisibility.viewType = RenderViewType::Game;
				gameVisibility.outputId = 2u;
				gameVisibility.frameId = 9000u + frame;
				gameVisibility.valid = true;
				gameVisibility.source = VirtualGeometryCounterSource::GPUReadback;
				gameVisibility.counters.source = gameVisibility.source;
				gameVisibility.counters.preparedFrameId = gameVisibility.frameId;
				gameVisibility.counters.readbackFrameId = gameVisibility.frameId;
				gameVisibility.clusterPackets.resize(600u);
				gameVisibility.gpuPacketRecords.resize(600u);
				gameVisibility.gpuBatchDescriptors.resize(30u);
				CompactedVisibilitySet gameCompaction{};
				gameCompaction.viewId = gameVisibility.outputId;
				gameCompaction.viewType = gameVisibility.viewType;
				gameCompaction.candidatePackets = 600u;
				gameCompaction.visiblePackets = 420u;
				gameCompaction.rejectedByHiZ = 180u;
				gameCompaction.submittedBatches = 30u;
				gameVisibility.compactionResults.push_back(gameCompaction);

				TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot sceneVisibility = gameVisibility;
				sceneVisibility.viewType = RenderViewType::Scene;
				sceneVisibility.outputId = 1u;
				sceneVisibility.source = VirtualGeometryCounterSource::DelayedGPUReadback;
				sceneVisibility.counters.source = sceneVisibility.source;
				sceneVisibility.counters.unavailableReason = "delayed scene readback";

				const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot gameDiagnostics =
					TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(gameVisibility);
				const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot sceneDiagnostics =
					TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(sceneVisibility);
				const auto gameGate = std::find_if(
					gameDiagnostics.virtualGeometryPerformanceGates.begin(),
					gameDiagnostics.virtualGeometryPerformanceGates.end(),
					[](const PerformanceGateResult& gate) {
						return gate.gateId == "gpu-packet-batch-ratio";
					});
				const auto sceneGate = std::find_if(
					sceneDiagnostics.virtualGeometryPerformanceGates.begin(),
					sceneDiagnostics.virtualGeometryPerformanceGates.end(),
					[](const PerformanceGateResult& gate) {
						return gate.gateId == "gpu-packet-batch-ratio";
					});

				context.Expect(gameGate != gameDiagnostics.virtualGeometryPerformanceGates.end(), "Game View should publish a GPU packet performance gate each frame.");
				context.Expect(sceneGate != sceneDiagnostics.virtualGeometryPerformanceGates.end(), "Scene View should publish a GPU packet performance gate each frame.");
				context.ExpectEqual(2u, gameGate->viewId, "Game View performance gates should keep the Game output id.");
				context.ExpectEqual(1u, sceneGate->viewId, "Scene View performance gates should keep the Scene output id.");
				context.Expect(gameGate->source == VirtualGeometryPerformanceGateSource::GPUReadback, "Game View gate source should remain GPU readback.");
				context.Expect(sceneGate->source == VirtualGeometryPerformanceGateSource::DelayedGPUReadback, "Scene View gate source should remain delayed GPU readback.");
				++checkedGatePairs;
			}

			context.ExpectEqual(300u, checkedGatePairs, "The multi-view gate fixture should exercise 300 independent frames.");
		});

		registry.Add("VirtualGeometry.Render.MultiView.ModeSwitchResetsLodTransitionState", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeModeSwitchTransitionAsset();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::LOD;
			TerraEngine::Renderer::VirtualGeometryClusterSelectionResult previous =
				RenderSystem::SelectVirtualGeometryStableClusterCut(
					asset,
					meshId,
					objectId,
					quality,
					220.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f),
					glm::uvec2(0u),
					true,
					true,
					nullptr,
					RenderViewType::Game,
					2u);
			context.ExpectEqual(1u, previous.selectedClusters.front().clusterId, "Setup should begin from the high-detail child cut.");

			quality.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::HiZ;
			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult current =
				RenderSystem::SelectVirtualGeometryStableClusterCut(
					asset,
					meshId,
					objectId,
					quality,
					146.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f),
					glm::uvec2(0u),
					true,
					true,
					&previous,
					RenderViewType::Game,
					2u);

			context.Expect(current.transitionState.resetThisFrame, "Changing visualizer modes should reset transition history for the target view.");
			context.Expect(!current.transitionState.heldPreviousSelection, "Mode reset should not hold the previous high-detail cut.");
			context.ExpectEqual(0u, current.selectedClusters.front().clusterId, "After reset, the current camera threshold should select the coarse root.");
		});
	}
}
