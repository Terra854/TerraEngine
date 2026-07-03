#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanHiZ.h"
#include "Renderer/RenderSystem.h"

#include <algorithm>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryHardeningTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Hardening.VisibilitySnapshotsArePerViewAndPrepared", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			snapshot.frameId = 2901u;
			snapshot.sceneRevision = 7u;
			snapshot.resourceRevision = 11u;
			snapshot.viewRequests = {
				{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 1u },
				{ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(640u, 360u), 2u }
			};

			TerraEngine::Renderer::RenderFrameRenderableRecord renderable{};
			renderable.objectId = TerraEngine::GenerateUUID();
			renderable.meshId = TerraEngine::GenerateUUID();
			renderable.virtualGeometry.virtualGeometryActive = true;
			renderable.virtualGeometry.meshletCount = 12u;
			renderable.virtualGeometry.selectedLodIndex = 1u;
			snapshot.renderables.push_back(renderable);

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot> visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshots(snapshot);

			context.ExpectEqual(static_cast<std::size_t>(2u), visibility.size(), "Each requested view should receive an isolated virtual geometry visibility snapshot.");
			context.Expect(visibility[0].cameraSignature != visibility[1].cameraSignature, "Scene and Game visibility snapshots should not share one camera signature.");
			for (const TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot& view : visibility)
			{
				context.Expect(view.valid, "Generated visibility snapshots should inherit the frame snapshot validity flag.");
				context.Expect(view.source == VirtualGeometryCounterSource::Prepared, "Foundation counters should be labeled as prepared CPU estimates.");
				context.ExpectEqual(12u, view.counters.selectedMeshlets, "Prepared counters should count selected meshlets from copied draw ranges.");
				context.Expect(view.hiZValidationState.state == VirtualGeometryHiZState::Active, "Sized camera views should publish an active foundation Hi-Z state.");
			}
		});

		registry.Add("VirtualGeometry.Hardening.MissingPyramidFailsOpenInDiagnostics", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			snapshot.frameId = 2902u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.viewType = RenderViewType::Game;
			state.targetSize = glm::uvec2(0u);
			state.hiZOcclusionAllowed = true;

			const TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, state, VirtualGeometryCounterSource::Prepared);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(diagnostics.hiZState == VirtualGeometryHiZState::MissingPyramid, "A zero-sized view should not masquerade as an active Hi-Z pyramid.");
			context.Expect(!diagnostics.hiZActive, "Missing-pyramid diagnostics should fail open rather than cull.");
			context.Expect(diagnostics.hiZValidation.failOpen, "Missing-pyramid diagnostics should publish fail-open state.");
			context.Expect(!diagnostics.hiZInvalidReason.empty(), "Missing-pyramid diagnostics should explain why Hi-Z is unavailable.");
		});

		registry.Add("VirtualGeometry.Hardening.DelayedCounterSourceIsExplicit", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			snapshot.frameId = 2903u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.viewType = RenderViewType::Scene;
			state.outputKind = RenderOutputKind::SceneView;
			state.targetSize = glm::uvec2(800u, 600u);

			TerraEngine::Renderer::VirtualGeometryViewDrawRecord draw{};
			draw.objectId = TerraEngine::GenerateUUID();
			draw.meshId = TerraEngine::GenerateUUID();
			draw.range.virtualGeometryActive = true;
			draw.range.meshletCount = 5u;
			state.draws.push_back(draw);

			const TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, state, VirtualGeometryCounterSource::DelayedGPUReadback);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(diagnostics.counterSource == VirtualGeometryCounterSource::DelayedGPUReadback, "Delayed GPU counter data should keep a non-authoritative source label.");
			context.ExpectEqual(5u, diagnostics.selectedMeshlets, "Delayed counter diagnostics should still carry the latest prepared selection estimate.");
			context.Expect(!diagnostics.warnings.empty(), "Delayed counter diagnostics should warn that the values are not authoritative readback.");
		});

		registry.Add("VirtualGeometry.Hardening.CounterFramesReasonsAndToleranceAreExplicit", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			snapshot.frameId = 44u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.viewType = RenderViewType::Game;
			state.outputKind = RenderOutputKind::GameView;
			state.targetSize = glm::uvec2(1280u, 720u);
			TerraEngine::Renderer::VirtualGeometryViewDrawRecord draw{};
			draw.objectId = TerraEngine::GenerateUUID();
			draw.meshId = TerraEngine::GenerateUUID();
			draw.range.virtualGeometryActive = true;
			draw.range.meshletCount = 100u;
			state.draws.push_back(draw);

			const TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, state, VirtualGeometryCounterSource::DelayedGPUReadback);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(!visibility.counters.authoritative, "Delayed counters should not be marked as authoritative GPU readback.");
			context.ExpectEqual(44ull, visibility.counters.preparedFrameId, "Prepared counter frame should identify the frame that built the estimate.");
			context.ExpectEqual(43ull, visibility.counters.readbackFrameId, "Delayed readback should identify the older GPU frame when available.");
			context.ExpectEqual(1u, visibility.counters.readbackLatencyFrames, "Delayed readback should expose one-frame latency.");
			context.Expect(!visibility.counters.unavailableReason.empty(), "Delayed readback should explain why the GPU value is not current.");
			context.Expect(TerraEngine::Renderer::VirtualGeometryCounterWithinTolerance(100u, 101u, visibility.counters), "Default tolerance should allow one meshlet of counter drift.");
			context.Expect(TerraEngine::Renderer::VirtualGeometryCounterWithinTolerance(10000u, 10090u, visibility.counters), "Default tolerance should allow up to 1 percent counter drift.");
			context.Expect(!TerraEngine::Renderer::VirtualGeometryCounterWithinTolerance(100u, 103u, visibility.counters), "Counter drift beyond one meshlet and 1 percent should fail.");
			context.ExpectEqual(visibility.counters.unavailableReason, diagnostics.counterUnavailableReason, "Diagnostics should preserve the delayed-counter reason.");
			context.ExpectEqual(visibility.counters.toleranceMeshlets, diagnostics.counterToleranceMeshlets, "Diagnostics should publish the meshlet tolerance.");
		});

		registry.Add("VirtualGeometry.Hardening.UnavailableCounterOverlayDoesNotLookAuthoritative", [](TestContext& context) {
			const std::string text = TerraEngine::Renderer::FormatVirtualGeometryCounterOverlayValue(
				"Meshlets Selected",
				0u,
				VirtualGeometryCounterSource::Unavailable,
				"no completed view stats");

			context.Expect(text.find("unavailable") != std::string::npos, "Unavailable counter overlay text should name unavailable state.");
			context.Expect(text.find("no completed view stats") != std::string::npos, "Unavailable counter overlay text should include the reason.");
			context.Expect(text.find("[GPU Readback]") == std::string::npos, "Unavailable counters must not be presented as GPU readback.");
		});

		registry.Add("VirtualGeometry.Hardening.HiZRecoversAcrossMovingBlockerFrames", [](TestContext& context) {
			VulkanHiZHistoryMetadata history{};
			history.viewId = 7u;
			history.viewClass = VulkanHiZViewClass::Game;
			history.targetId = 3u;
			history.width = 1280u;
			history.height = 720u;
			history.projectionSignature = 0x1234u;
			history.depthSceneSignature = 0x4567u;
			history.frameId = 20u;
			history.warmupFrames = 1u;
			history.valid = true;
			history.invalidReason = VulkanHiZInvalidReason::None;

			VulkanHiZValidationInput input{};
			input.viewId = history.viewId;
			input.viewClass = history.viewClass;
			input.targetId = history.targetId;
			input.width = history.width;
			input.height = history.height;
			input.projectionSignature = history.projectionSignature;
			input.depthSceneSignature = history.depthSceneSignature + 1u;
			input.pyramidReady = true;
			input.qualityAllowsOcclusion = true;
			input.viewSupported = true;

			const VulkanHiZValidationResult unblocked = ValidateVulkanHiZHistory(history, input);
			context.Expect(unblocked.occlusionAllowed, "Moving an opaque blocker away should not latch Hi-Z off while the current view still has a pyramid.");
			context.Expect(!unblocked.depthSceneMatches, "The validation record should still explain that the blocker/depth scene changed.");
			context.Expect(unblocked.warmupFrames >= 2u, "A ready current-view pyramid should continue warming after blocker motion.");

			history.depthSceneSignature = input.depthSceneSignature;
			history.warmupFrames = unblocked.warmupFrames;
			input.depthSceneSignature += 1u;
			const VulkanHiZValidationResult blockedAgain = ValidateVulkanHiZHistory(history, input);
			context.Expect(blockedAgain.occlusionAllowed, "Moving the blocker back should resume Hi-Z immediately without an editor restart.");
			context.Expect(blockedAgain.warmupFrames >= unblocked.warmupFrames, "Repeated blocker moves should not reset the current-pyramid warm-up counter.");
		});

		registry.Add("VirtualGeometry.Hardening.US7.BatchedMovingWallRecoveryKeepsBaselineMatched", [](TestContext& context) {
			VulkanHiZHistoryMetadata history{};
			history.viewId = 17u;
			history.viewClass = VulkanHiZViewClass::Game;
			history.targetId = 5u;
			history.width = 1280u;
			history.height = 720u;
			history.projectionSignature = 0x5151u;
			history.depthSceneSignature = 0x6161u;
			history.frameId = 41u;
			history.warmupFrames = 2u;
			history.valid = true;

			VulkanHiZValidationInput input{};
			input.viewId = history.viewId;
			input.viewClass = history.viewClass;
			input.targetId = history.targetId;
			input.width = history.width;
			input.height = history.height;
			input.projectionSignature = history.projectionSignature;
			input.depthSceneSignature = history.depthSceneSignature + 1u;
			input.pyramidReady = true;
			const VulkanHiZValidationResult validation = ValidateVulkanHiZHistory(history, input);

			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.viewType = RenderViewType::Game;
			state.outputKind = RenderOutputKind::GameView;
			state.outputId = input.targetId;
			state.targetSize = glm::uvec2(input.width, input.height);
			state.hasHiZValidationState = true;
			state.hiZValidationState.state = validation.occlusionAllowed ? VirtualGeometryHiZState::Active : VirtualGeometryHiZState::FailOpen;
			state.hiZValidationState.pyramidReady = validation.pyramidReady;
			state.hiZValidationState.pyramidSize = state.targetSize;
			state.hiZValidationState.renderSize = state.targetSize;
			state.hiZValidationState.warmupFrames = validation.warmupFrames;
			state.hiZValidationState.failOpen = validation.failOpen;

			ClusterWorkBatch batch{};
			batch.batchId = 9u;
			batch.viewId = state.outputId;
			batch.viewType = state.viewType;
			batch.packetCount = 4u;
			batch.selectedMeshlets = 64u;
			batch.submittedMeshlets = 64u;
			state.clusterBatches.push_back(batch);

			HiZRegressionSnapshot baseline{};
			baseline.viewId = state.outputId;
			baseline.viewType = state.viewType;
			baseline.frameIndex = 42u;
			baseline.state = state.hiZValidationState.state;
			baseline.testedMeshlets = 64u;
			baseline.visibleMeshlets = 64u;
			baseline.recoveryFrames = validation.warmupFrames;
			baseline.baselineMatched = validation.occlusionAllowed;
			state.hiZRegressionSnapshots.push_back(baseline);

			TerraEngine::Renderer::RenderFrameSnapshot frame{};
			frame.frameId = 42u;
			const TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(frame, state, VirtualGeometryCounterSource::Prepared);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(validation.occlusionAllowed, "Moving the blocker in a batched fixture should keep Hi-Z active with the current pyramid.");
			context.Expect(diagnostics.hiZBaselineMatched, "Batched moving-wall diagnostics should keep the accepted Hi-Z baseline matched.");
			context.ExpectEqual(1u, diagnostics.submittedBatchCount, "Moving-wall recovery should not discard the batched cluster work record.");
			context.ExpectEqual(validation.warmupFrames, diagnostics.hiZWarmupFrames, "Diagnostics should preserve the backend warm-up/recovery counter.");
		});

		registry.Add("VirtualGeometry.Hardening.US5.GpuCompactionMovingWallRecoveryKeepsHiZActive", [](TestContext& context) {
			VulkanHiZHistoryMetadata history{};
			history.viewId = 21u;
			history.viewClass = VulkanHiZViewClass::Game;
			history.targetId = 8u;
			history.width = 1600u;
			history.height = 900u;
			history.projectionSignature = 0x71050010u;
			history.depthSceneSignature = 0x71050020u;
			history.frameId = 80u;
			history.warmupFrames = 3u;
			history.valid = true;
			history.invalidReason = VulkanHiZInvalidReason::None;

			VulkanHiZValidationInput input{};
			input.viewId = history.viewId;
			input.viewClass = history.viewClass;
			input.targetId = history.targetId;
			input.width = history.width;
			input.height = history.height;
			input.projectionSignature = history.projectionSignature;
			input.depthSceneSignature = history.depthSceneSignature + 1u;
			input.pyramidReady = true;
			const VulkanHiZValidationResult unblocked = ValidateVulkanHiZHistory(history, input);

			history.depthSceneSignature = input.depthSceneSignature;
			history.warmupFrames = unblocked.warmupFrames;
			input.depthSceneSignature += 1u;
			const VulkanHiZValidationResult blockedAgain = ValidateVulkanHiZHistory(history, input);

			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.outputKind = RenderOutputKind::GameView;
			state.viewType = RenderViewType::Game;
			state.outputId = history.targetId;
			state.targetSize = glm::uvec2(history.width, history.height);
			state.hasHiZValidationState = true;
			state.hiZValidationState.state = blockedAgain.occlusionAllowed ? VirtualGeometryHiZState::Active : VirtualGeometryHiZState::FailOpen;
			state.hiZValidationState.pyramidReady = blockedAgain.pyramidReady;
			state.hiZValidationState.pyramidSize = state.targetSize;
			state.hiZValidationState.renderSize = state.targetSize;
			state.hiZValidationState.depthSceneSignature = input.depthSceneSignature;
			state.hiZValidationState.warmupFrames = blockedAgain.warmupFrames;
			state.hiZValidationState.failOpen = blockedAgain.failOpen;

			GpuVirtualGeometryPacketRecord packet{};
			packet.packetId = 0u;
			packet.viewId = state.outputId;
			packet.viewType = state.viewType;
			packet.frameId = history.frameId + 2u;
			packet.meshId = TerraEngine::GenerateUUID();
			packet.materialId = TerraEngine::GenerateUUID();
			packet.clusterId = 44u;
			packet.meshletCount = 40u;
			state.gpuPacketRecords.push_back(packet);

			GpuVirtualGeometryPacketFrameSet frameSet{};
			frameSet.viewId = state.outputId;
			frameSet.viewType = state.viewType;
			frameSet.frameId = packet.frameId;
			frameSet.targetSize = state.targetSize;
			frameSet.packetCount = 1u;
			frameSet.batchCount = 1u;
			frameSet.compactedPacketCount = 1u;
			frameSet.generatedSubmissionCount = 1u;
			frameSet.validity = VirtualGeometryGpuFrameSetValidity::Valid;
			state.gpuPacketFrameSets.push_back(frameSet);

			GpuPacketCompactionDispatch dispatch{};
			dispatch.dispatchId = 3u;
			dispatch.viewId = state.outputId;
			dispatch.viewType = state.viewType;
			dispatch.frameId = packet.frameId;
			dispatch.candidatePacketCount = 1u;
			dispatch.candidateBatchCount = 1u;
			dispatch.outputCapacity = 1u;
			dispatch.hiZBaselineState = state.hiZValidationState.state;
			state.gpuCompactionDispatches.push_back(dispatch);

			GeneratedVirtualGeometrySubmissionRecord submission{};
			submission.recordId = 2u;
			submission.viewId = state.outputId;
			submission.viewType = state.viewType;
			submission.frameId = packet.frameId;
			submission.packetCount = 1u;
			submission.indirectTaskCount = 1u;
			submission.materialId = packet.materialId;
			submission.meshId = packet.meshId;
			submission.source = VirtualGeometryCounterSource::GPUReadback;
			submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::Valid;
			state.generatedSubmissions.push_back(submission);

			CompactedVisibilitySet compaction{};
			compaction.viewId = state.outputId;
			compaction.viewType = state.viewType;
			compaction.candidatePackets = 1u;
			compaction.visiblePackets = 1u;
			compaction.submittedBatches = 1u;
			compaction.source = VirtualGeometryCounterSource::GPUReadback;
			state.compactionResults.push_back(compaction);

			HiZRegressionSnapshot baseline{};
			baseline.viewId = state.outputId;
			baseline.viewType = state.viewType;
			baseline.frameIndex = packet.frameId;
			baseline.state = state.hiZValidationState.state;
			baseline.testedMeshlets = packet.meshletCount;
			baseline.visibleMeshlets = packet.meshletCount;
			baseline.recoveryFrames = blockedAgain.warmupFrames;
			baseline.baselineMatched = blockedAgain.occlusionAllowed;
			state.hiZRegressionSnapshots.push_back(baseline);

			TerraEngine::Renderer::RenderFrameSnapshot frame{};
			frame.frameId = packet.frameId;
			const TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(frame, state, VirtualGeometryCounterSource::GPUReadback);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(unblocked.occlusionAllowed, "Moving the wall away should keep Hi-Z active while GPU packet compaction is enabled.");
			context.Expect(blockedAgain.occlusionAllowed, "Moving the wall back should resume occlusion without restarting the editor.");
			context.Expect(diagnostics.hiZBaselineMatched, "GPU compaction diagnostics should preserve the accepted moving-wall Hi-Z baseline.");
			context.ExpectEqual(1u, diagnostics.gpuCompactionDispatchCount, "The moving-wall fixture should exercise the GPU compaction diagnostics path.");
			context.ExpectEqual(1u, diagnostics.validGeneratedSubmissionCount, "Valid generated submission work should survive the Hi-Z recovery cycle.");
		});

		registry.Add("VirtualGeometry.Hardening.US4.InvalidGeneratedSubmissionPublishesFallbackDiagnostics", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility;
			visibility.valid = true;
			visibility.frameId = 10003u;
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 2u;

			GeneratedVirtualGeometrySubmissionRecord submission;
			submission.recordId = 1u;
			submission.viewId = visibility.outputId;
			submission.viewType = visibility.viewType;
			submission.frameId = visibility.frameId;
			submission.packetOffset = 16u;
			submission.packetCount = 4u;
			submission.indirectTaskCount = 1u;
			submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::Mismatch;
			submission.fallbackReason = "generated submission range mismatch";
			visibility.generatedSubmissions.push_back(submission);
			visibility.gpuFallbackRecords.push_back(RenderSystem::BuildVirtualGeometryGpuFallbackRecord(
				visibility.viewType,
				visibility.outputId,
				visibility.frameId,
				submission.fallbackReason,
				submission.packetCount,
				1u,
				"CPU-expanded packet batches remain authoritative."));

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(1u, diagnostics.invalidGeneratedSubmissionCount, "Invalid generated submissions should be counted separately from valid generated work.");
			context.ExpectEqual(1u, diagnostics.gpuFallbackRecordCount, "Invalid generated submissions should publish a fallback record.");
			context.Expect(diagnostics.gpuFallbackReason.find("mismatch") != std::string::npos, "Fallback diagnostics should preserve the generated submission validation reason.");
		});

		registry.Add("VirtualGeometry.Hardening.ExplicitHiZStateTravelsWithVisibilitySnapshot", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			snapshot.frameId = 2904u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.viewType = RenderViewType::Game;
			state.outputKind = RenderOutputKind::GameView;
			state.outputId = 11u;
			state.targetSize = glm::uvec2(1920u, 1080u);
			state.hasHiZValidationState = true;
			state.hiZValidationState.state = VirtualGeometryHiZState::Active;
			state.hiZValidationState.pyramidSize = glm::uvec2(1920u, 1080u);
			state.hiZValidationState.renderSize = glm::uvec2(1920u, 1080u);
			state.hiZValidationState.cameraSignature = 0xaaaa5555u;
			state.hiZValidationState.depthSceneSignature = 0xbbbb7777u;
			state.hiZValidationState.warmupFrames = 4u;

			const TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, state, VirtualGeometryCounterSource::Prepared);

			context.Expect(visibility.hiZValidationState.state == VirtualGeometryHiZState::Active, "Visibility snapshots should preserve backend-published Hi-Z availability.");
			context.ExpectEqual(4u, visibility.hiZValidationState.warmupFrames, "Visibility snapshots should preserve current-view Hi-Z warm-up frames.");
			context.ExpectEqual(0xaaaa5555ull, visibility.hiZValidationState.cameraSignature, "Visibility snapshots should preserve the backend camera signature when one is provided.");
		});

		registry.Add("VirtualGeometry.Hardening.HiddenViewStateDoesNotPoisonGameView", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			snapshot.frameId = 2906u;
			snapshot.sceneRevision = 99u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState gameState{};
			gameState.outputKind = RenderOutputKind::GameView;
			gameState.viewType = RenderViewType::Game;
			gameState.outputId = 2u;
			gameState.targetSize = glm::uvec2(1920u, 1080u);
			gameState.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::HiZ;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState hiddenSceneState{};
			hiddenSceneState.outputKind = RenderOutputKind::SceneView;
			hiddenSceneState.viewType = RenderViewType::Scene;
			hiddenSceneState.outputId = 1u;
			hiddenSceneState.targetSize = glm::uvec2(0u);
			hiddenSceneState.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::Cluster;

			snapshot.virtualGeometryViewSelections = { gameState, hiddenSceneState };
			const std::vector<TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot> visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshots(snapshot);

			const auto gameIt = std::find_if(visibility.begin(), visibility.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Game;
			});
			const auto sceneIt = std::find_if(visibility.begin(), visibility.end(), [](const auto& state) {
				return state.viewType == RenderViewType::Scene;
			});

			context.Expect(gameIt != visibility.end(), "Game View visibility should still be published when Scene View is hidden.");
			context.Expect(sceneIt != visibility.end(), "Hidden Scene View should publish an explicit zero-sized state instead of reusing stale data.");
			context.Expect(gameIt->hiZValidationState.state == VirtualGeometryHiZState::Active, "Game View Hi-Z should remain active with its own sized target.");
			context.Expect(sceneIt->hiZValidationState.state == VirtualGeometryHiZState::MissingPyramid, "Hidden Scene View should fail open instead of borrowing Game View's pyramid.");
			context.Expect(gameIt->debugVisualizationMode == VirtualGeometryDebugVisualizationMode::HiZ, "Game View should keep its own visualizer mode.");
			context.Expect(sceneIt->debugVisualizationMode == VirtualGeometryDebugVisualizationMode::Cluster, "Hidden Scene View should keep its own visualizer mode for the next resize.");
		});

		registry.Add("VirtualGeometry.Hardening.SynthesizesOcclusionDebugSamplesFromDrawRanges", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			snapshot.frameId = 2905u;

			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.viewType = RenderViewType::Game;
			state.outputKind = RenderOutputKind::GameView;
			state.targetSize = glm::uvec2(1280u, 720u);
			state.hiZOcclusionAllowed = true;
			state.hasHiZValidationState = true;
			state.hiZValidationState.state = VirtualGeometryHiZState::Active;
			state.hiZValidationState.pyramidReady = true;
			state.hiZValidationState.pyramidSize = state.targetSize;
			state.hiZValidationState.renderSize = state.targetSize;
			state.hiZValidationState.warmupFrames = 2u;

			TerraEngine::Renderer::VirtualGeometryViewDrawRecord draw{};
			draw.objectId = TerraEngine::GenerateUUID();
			draw.meshId = TerraEngine::GenerateUUID();
			draw.range.virtualGeometryActive = true;
			draw.range.meshletOffset = 40u;
			draw.range.meshletCount = 3u;
			draw.range.failOpen = true;
			draw.range.fallbackReason = "projected-bounds-risk";
			state.draws.push_back(draw);

			const TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshot(snapshot, state, VirtualGeometryCounterSource::Prepared);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(!visibility.occlusionDebugSamples.empty(), "Visibility snapshots should synthesize representative occlusion explanation samples from draw ranges.");
			context.Expect(visibility.occlusionDebugSamples.front().outcome == VirtualGeometryCullOutcome::FailOpen, "Fail-open draw ranges should publish fail-open occlusion samples.");
			context.Expect(visibility.occlusionDebugSamples.front().meshletId == 40u, "The representative sample should identify the first meshlet in the draw range.");
			context.Expect(!diagnostics.occlusionDebugSamples.empty(), "Diagnostics should preserve synthesized occlusion explanation samples.");
			context.Expect(diagnostics.occlusionDebugSamples.front().reason.find("projected-bounds") != std::string::npos, "Synthesized samples should preserve the fail-open reason text.");
		});
	}
}
