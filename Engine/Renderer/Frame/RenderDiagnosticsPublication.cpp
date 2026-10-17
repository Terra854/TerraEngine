#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "RenderFrameSealing.h"
#include "RenderSnapshotPreparation.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
// Protected backend capability diagnostics use only actual completed evidence.
#include "RenderFrameState.h"

namespace TerraEngine::Renderer::Frame {

	using BackendStage = TerraEngine::Renderer::VirtualGeometryBackendStage;
	using CapabilityStage =
		TerraEngine::Renderer::VirtualGeometryCapabilityStage;

	CapabilityStage ToCapabilityStage(BackendStage stage)
	{
		switch(stage)
		{
		case BackendStage::Traversal: return CapabilityStage::Traversal;
		case BackendStage::Compaction: return CapabilityStage::Compaction;
		case BackendStage::IndirectGeneration: return CapabilityStage::Indirect;
		case BackendStage::PageDecode: return CapabilityStage::Decode;
		case BackendStage::HardwareRaster: return CapabilityStage::HardwareRaster;
		case BackendStage::TriangleSetup:
		case BackendStage::ComputeRaster: return CapabilityStage::ComputeRaster;
		case BackendStage::VisibilityResolve: return CapabilityStage::Resolve;
		case BackendStage::Shading: return CapabilityStage::Shading;
		case BackendStage::StreamingUpdate: return CapabilityStage::Streaming;
		case BackendStage::Deformation: return CapabilityStage::Deformation;
		case BackendStage::Shadow: return CapabilityStage::Shadow;
		case BackendStage::RayTracing: return CapabilityStage::RayTracing;
		default: return CapabilityStage::Capture;
		}
	}

	TerraEngine::Renderer::VirtualGeometryCapabilityRoute ToCapabilityRoute(
		TerraEngine::Renderer::VirtualGeometryBackendRoute route)
	{
		using BackendRoute =
			TerraEngine::Renderer::VirtualGeometryBackendRoute;
		using CapabilityRoute =
			TerraEngine::Renderer::VirtualGeometryCapabilityRoute;
		switch(route)
		{
		case BackendRoute::MeshShader: return CapabilityRoute::MeshShader;
		case BackendRoute::Compute: return CapabilityRoute::ComputeRaster;
		case BackendRoute::ProcessorOracle:
		case BackendRoute::Indexed: return CapabilityRoute::IndexedFallback;
		default: return CapabilityRoute::Unknown;
		}
	}

	TerraEngine::Renderer::VirtualGeometryMeasurementAvailability
		ToMeasurementAvailability(
			TerraEngine::Renderer::BackendMeasurementAvailability availability)
	{
		using BackendAvailability =
			TerraEngine::Renderer::BackendMeasurementAvailability;
		using Availability =
			TerraEngine::Renderer::VirtualGeometryMeasurementAvailability;
		switch(availability)
		{
		case BackendAvailability::Pending: return Availability::Pending;
		case BackendAvailability::Unavailable: return Availability::Unavailable;
		case BackendAvailability::Invalid: return Availability::Invalid;
		case BackendAvailability::Measured: return Availability::Measured;
		default: return Availability::Invalid;
		}
	}

	bool BackendSupportsStage(
		CapabilityStage stage,
		const TerraEngine::Renderer::BackendCapabilityProfile& profile,
		bool meshShaderRoute)
	{
		switch(stage)
		{
		case CapabilityStage::Compaction:
			return profile.supportsVirtualGeometryGpuCompaction;
		case CapabilityStage::Indirect:
			return profile.supportsVirtualGeometryGeneratedSubmissions;
		case CapabilityStage::ComputeRaster:
			return profile.supportsVirtualGeometryComputeRaster;
		case CapabilityStage::Resolve:
			return profile.supportsVirtualGeometryVisibilityResolve;
		case CapabilityStage::Shading:
			return profile.supportsVirtualGeometryShadingBins;
		case CapabilityStage::Streaming:
		case CapabilityStage::RayTracing:
			return false;
		case CapabilityStage::Capture:
			return profile.supportsImageReadback;
		default:
			return meshShaderRoute;
		}
	}

	TerraEngine::Renderer::VirtualGeometryCapabilityDiagnosticsSnapshot
		CollectVirtualGeometryCapabilityDiagnostics(
			RHIContext& context,
			const TerraEngine::Renderer::BackendCapabilityProfile& profile,
			const TerraEngine::Renderer::VirtualGeometryFrameViewIdentity&
				currentPublication)
	{
		using namespace TerraEngine::Renderer;
		std::vector<VirtualGeometryBackendExecutionReceipt> receipts =
			context.PollVirtualGeometryExecutionReceipts();
		std::vector<VirtualGeometryBackendMeasurement> backendMeasurements =
			context.PollVirtualGeometryMeasurements();

		VirtualGeometryCapabilityDiagnosticsSnapshot diagnostics{};
		diagnostics.publicationIdentity = currentPublication;

		// Delayed queries keep the identity of the frame that owned the commands.
		// Publish the newest completed identity, then reject older/mismatched rows
		// through the shared generation-safe diagnostics applicator.
		std::optional<VirtualGeometryFrameViewIdentity> newestCompletedIdentity;
		const auto considerCompletedIdentity =
			[&newestCompletedIdentity](
				VirtualGeometryFrameViewIdentity candidate)
			{
				if(!newestCompletedIdentity.has_value()
					|| candidate.frameId > newestCompletedIdentity->frameId
					|| (candidate.frameId == newestCompletedIdentity->frameId
						&& candidate.viewId
							> newestCompletedIdentity->viewId))
				{
					newestCompletedIdentity = candidate;
				}
			};
		for(const VirtualGeometryBackendExecutionReceipt& receipt : receipts)
		{
			if(receipt.IsExecuted())
			{
				considerCompletedIdentity({
					receipt.frameNumber,
					receipt.viewId,
					receipt.rendererGeneration
				});
			}
		}
		for(const VirtualGeometryBackendMeasurement& measurement
			: backendMeasurements)
		{
			if(measurement.frameNumber != 0u
				&& measurement.rendererGeneration != 0u)
			{
				considerCompletedIdentity({
					measurement.frameNumber,
					measurement.viewId,
					measurement.rendererGeneration
				});
			}
		}
		if(newestCompletedIdentity.has_value())
		{
			diagnostics.publicationIdentity = *newestCompletedIdentity;
		}

		const bool meshShaderRoute =
			context.SupportsVirtualGeometryMeshShaderRoute();
		constexpr std::array<BackendStage, 13u> backendStages = {
			BackendStage::Traversal,
			BackendStage::Compaction,
			BackendStage::IndirectGeneration,
			BackendStage::PageDecode,
			BackendStage::HardwareRaster,
			BackendStage::ComputeRaster,
			BackendStage::VisibilityResolve,
			BackendStage::Shading,
			BackendStage::StreamingUpdate,
			BackendStage::Deformation,
			BackendStage::Shadow,
			BackendStage::Capture,
			BackendStage::RayTracing
		};
		diagnostics.capabilities.reserve(backendStages.size());
		for(const BackendStage backendStage : backendStages)
		{
			const CapabilityStage stage = ToCapabilityStage(backendStage);
			VirtualGeometryCapabilityKey key{};
			key.stage = stage;
			key.backend = profile.backendId == "vulkan"
				? VirtualGeometryCapabilityBackend::Vulkan
				: VirtualGeometryCapabilityBackend::Unknown;
			key.route = stage == CapabilityStage::Compaction
					|| stage == CapabilityStage::ComputeRaster
				? VirtualGeometryCapabilityRoute::ComputeRaster
				: VirtualGeometryCapabilityRoute::MeshShader;
			key.deviceIdentity = profile.backendId;
			VirtualGeometryCapabilityEvidence capability =
				MakePlannedVirtualGeometryCapability(std::move(key));
			const bool supported =
				BackendSupportsStage(stage, profile, meshShaderRoute);
			EvaluateVirtualGeometryCapabilitySupport(
				capability,
				supported,
				supported
					? "The active backend exposes the required live route."
					: "The active backend has not exposed a complete live route.",
				"profile-derived limits");
			capability.source = "backend-profile";
			if(!supported)
			{
				capability.fallbackRoute =
					VirtualGeometryCapabilityRoute::IndexedFallback;
				capability.fallbackReason =
					"The processor-expanded/indexed route remains authoritative.";
			}

			const auto completed = std::find_if(
				receipts.begin(),
				receipts.end(),
				[&](const VirtualGeometryBackendExecutionReceipt& receipt)
				{
					return receipt.stage == backendStage
						&& receipt.IsExecuted()
						&& VirtualGeometryFrameViewIdentity{
							receipt.frameNumber,
							receipt.viewId,
							receipt.rendererGeneration
						} == diagnostics.publicationIdentity;
				});
			if(completed != receipts.end())
			{
				VirtualGeometryCapabilityExecution execution{};
				execution.identity = diagnostics.publicationIdentity;
				execution.evidenceId = completed->evidenceId;
				execution.completedLiveCommand = true;
				execution.preparedOnly = false;
				(void)AttachVirtualGeometryExecutionEvidence(
					capability,
					std::move(execution));
				capability.source = "backend-completion-receipt";
			}
			diagnostics.capabilities.push_back(std::move(capability));
		}

		diagnostics.measurements.reserve(backendMeasurements.size());
		for(const VirtualGeometryBackendMeasurement& backendMeasurement
			: backendMeasurements)
		{
			VirtualGeometryMeasurementRecord measurement{};
			measurement.stage =
				ToCapabilityStage(backendMeasurement.stage);
			measurement.identity = {
				backendMeasurement.frameNumber,
				backendMeasurement.viewId,
				backendMeasurement.rendererGeneration
			};
			measurement.availability =
				ToMeasurementAvailability(backendMeasurement.availability);
			measurement.rejectionReason = backendMeasurement.reason;
			if(backendMeasurement.queryPoolGeneration != 0u
				&& backendMeasurement.queryCount != 0u)
			{
				measurement.queryIdentity =
					"pool-"
					+ std::to_string(
						backendMeasurement.queryPoolGeneration)
					+ "/"
					+ std::to_string(backendMeasurement.firstQuery)
					+ "+"
					+ std::to_string(backendMeasurement.queryCount);
			}

			const auto completed = std::find_if(
				receipts.begin(),
				receipts.end(),
				[&](const VirtualGeometryBackendExecutionReceipt& receipt)
				{
					return receipt.stage == backendMeasurement.stage
						&& receipt.frameNumber
							== backendMeasurement.frameNumber
						&& receipt.viewId == backendMeasurement.viewId
						&& receipt.rendererGeneration
							== backendMeasurement.rendererGeneration
						&& receipt.IsExecuted();
				});
			if(completed != receipts.end())
			{
				measurement.executionEvidenceId = completed->evidenceId;
			}
			if(backendMeasurement.availability
				== BackendMeasurementAvailability::Measured)
			{
				if(completed == receipts.end()
					|| !backendMeasurement.HasAuthoritativeValue())
				{
					measurement.availability =
						VirtualGeometryMeasurementAvailability::Invalid;
					measurement.rejectionReason =
						"measured backend timing lacks matching completed execution evidence";
				}
				else
				{
					measurement.rawTicks =
						backendMeasurement.rawTicks;
					measurement.durationNanoseconds =
						static_cast<std::uint64_t>(std::llround(
							backendMeasurement.durationMilliseconds
								* 1'000'000.0));
				}
			}
			diagnostics.measurements.push_back(std::move(measurement));
		}

		for(VirtualGeometryCapabilityEvidence& capability
			: diagnostics.capabilities)
		{
			if(!capability.CanReportExecuted())
			{
				continue;
			}
			const auto measured = std::find_if(
				diagnostics.measurements.begin(),
				diagnostics.measurements.end(),
				[&](const VirtualGeometryMeasurementRecord& measurement)
				{
					return measurement.stage == capability.key.stage
						&& measurement.identity
							== capability.execution.identity
						&& measurement.HasAuthoritativeDuration();
				});
			if(measured != diagnostics.measurements.end())
			{
				(void)AttachVirtualGeometryValidationEvidence(
					capability,
					{ measured->queryIdentity });
			}
		}
		return diagnostics;
	}

}

// Enrich copied prepared diagnostics before acquiring the stats publication lock.
void RenderSystem::EnrichFrameVirtualGeometryDiagnostics(const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	const FrameVirtualGeometryPreparation& preparation,
	const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>& sealedPublication,
	TerraEngine::Renderer::RenderDiagnosticsSummary& renderGraphDiagnostics)
{
	const auto& gameVirtualGeometryDiagnosticsAssets = preparation.gameVirtualGeometryDiagnosticsAssets;
	const auto& sceneVirtualGeometryDiagnosticsAssets = preparation.sceneVirtualGeometryDiagnosticsAssets;
	const auto& shadowVirtualGeometryDiagnosticsAssets = preparation.shadowVirtualGeometryDiagnosticsAssets;
		std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> virtualGeometryViewSnapshots;
		const VirtualGeometryQualityProfile virtualGeometryQuality = renderFrameSnapshot.renderSettings.virtualGeometry;
		const bool detailedRenderDiagnosticsRequested = TerraEngine::Renderer::RenderDiagnosticsRequested();
		// The stats overlay only needs live frame-selection counters. Full asset
		// inventory diagnostics scan every cooked VG asset and allocate warning
		// strings, so keep that work behind the explicit diagnostics request.
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Compact Render Diagnostics",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			virtualGeometryViewSnapshots =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					renderFrameSnapshot,
					virtualGeometryQuality,
					GetVirtualGeometryInactiveReason(),
					// Ownership moved into the sealed backend publication above. Read the
					// retained immutable root instead of the moved-from selector; otherwise
					// diagnostics falsely report a missing deformation plan on every valid
					// animated frame even though Vulkan receives the completed work.
					sealedPublication && sealedPublication->backendWork
						? sealedPublication->backendWork->deformationWork.get()
						: nullptr,
					detailedRenderDiagnosticsRequested);
		}
		if (detailedRenderDiagnosticsRequested)
		{
			std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> detailedVirtualGeometryViewSnapshots;
			detailedVirtualGeometryViewSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, activeRenderFrameId, gameVirtualGeometryDiagnosticsAssets, virtualGeometryQuality));
			detailedVirtualGeometryViewSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Scene, activeRenderFrameId, sceneVirtualGeometryDiagnosticsAssets, virtualGeometryQuality));
			bool shadowDiagnosticsPublished = false;
			{
				TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot shadowSnapshot =
					TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Shadow, activeRenderFrameId, shadowVirtualGeometryDiagnosticsAssets, virtualGeometryQuality);
				shadowSnapshot.hiZActive = true;
				shadowSnapshot.hiZHistoryValid = true;
				shadowSnapshot.hiZInvalidReason.clear();
				shadowSnapshot.warnings.push_back("Shadow virtual geometry uses conservative visibility; camera Hi-Z ownership is diagnostic-only.");
				detailedVirtualGeometryViewSnapshots.push_back(std::move(shadowSnapshot));
				shadowDiagnosticsPublished = true;
			}
			const auto mergeSelectionDiagnostics = [](
				TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& target,
				const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& selection) {
				// The quality snapshot describes cooked assets and residency. The
				// selection snapshot describes what this frame actually dispatched.
				// Merge the live fields into the inventory view so editor overlays
				// do not stop on a stale zero-count Game/Scene snapshot.
				target.active = target.active || selection.active;
				target.inactiveReason = selection.inactiveReason;
				target.selectedMeshlets = selection.selectedMeshlets;
				target.dispatchedMeshlets = selection.dispatchedMeshlets;
				target.culledMeshlets = selection.culledMeshlets;
				target.culledByFrustum = selection.culledByFrustum;
				target.culledByCone = selection.culledByCone;
				target.culledByHiZ = selection.culledByHiZ;
				target.clusterPacketCount = selection.clusterPacketCount;
				target.candidateClusterCount = selection.candidateClusterCount;
				target.compactedClusterCount = selection.compactedClusterCount;
				target.rejectedClusterCount = selection.rejectedClusterCount;
				target.failOpenClusterCount = selection.failOpenClusterCount;
				target.submittedBatchCount = selection.submittedBatchCount;
				target.singlePacketFallbackBatchCount = selection.singlePacketFallbackBatchCount;
				target.hiZActive = selection.hiZActive;
				target.hiZHistoryValid = selection.hiZHistoryValid;
				target.hiZInvalidReason = selection.hiZInvalidReason;
				target.hiZState = selection.hiZState;
				target.hiZPyramidSize = selection.hiZPyramidSize;
				target.hiZRenderSize = selection.hiZRenderSize;
				target.hiZCameraSignature = selection.hiZCameraSignature;
				target.hiZDepthSceneSignature = selection.hiZDepthSceneSignature;
				target.hiZWarmupFrames = selection.hiZWarmupFrames;
				target.hiZBaselineMatched = target.hiZBaselineMatched && selection.hiZBaselineMatched;
				target.hiZFalseOcclusionRiskWarnings += selection.hiZFalseOcclusionRiskWarnings;
				target.clusterSelection = selection.clusterSelection;
				target.hiZValidation = selection.hiZValidation;
				// Animated-page facts come from the same live selection snapshot.
				// Preserve them when detailed cooked-asset inventory is enabled;
				// otherwise opening diagnostics would replace accepted compute work
				// with the inventory snapshot's default zero values.
				target.animatedPageDemandCount =
					selection.animatedPageDemandCount;
				target.animatedUniqueDispatchCount =
					selection.animatedUniqueDispatchCount;
				target.animatedReuseCount =
					selection.animatedReuseCount;
				target.animatedAnimationLods =
					selection.animatedAnimationLods;
				target.animatedSkippedWorkCount =
					selection.animatedSkippedWorkCount;
				target.animatedBoundsSource =
					selection.animatedBoundsSource;
				target.animatedFallbackCount =
					selection.animatedFallbackCount;
				target.deformationCurrentBytes =
					selection.deformationCurrentBytes;
				target.deformationHistoryBytes =
					selection.deformationHistoryBytes;
				target.deformationBoundsBytes =
					selection.deformationBoundsBytes;
				target.deformationHighWaterBytes =
					selection.deformationHighWaterBytes;
				target.animatedDiagnosticEmitted =
					selection.animatedDiagnosticEmitted;
				target.animatedDiagnosticCoalesced =
					selection.animatedDiagnosticCoalesced;
				target.animatedDiagnosticDropped =
					selection.animatedDiagnosticDropped;
				target.animatedShaderContractValid =
					selection.animatedShaderContractValid;
				target.animatedFallbackReason =
					selection.animatedFallbackReason;
				target.virtualGeometryPerformanceGates.insert(
					target.virtualGeometryPerformanceGates.end(),
					selection.virtualGeometryPerformanceGates.begin(),
					selection.virtualGeometryPerformanceGates.end());
				target.selectedLodHistogram = selection.selectedLodHistogram;
				target.occlusionDebugSamples = selection.occlusionDebugSamples;
				if (selection.sourceMeshlets > 0u)
				{
					target.sourceMeshlets = std::max(target.sourceMeshlets, selection.sourceMeshlets);
				}
				if (selection.requestedPages > 0u || selection.missingPages > 0u)
				{
					target.requestedPages = std::max(target.requestedPages, selection.requestedPages);
					target.missingPages = std::max(target.missingPages, selection.missingPages);
				}
				target.warnings.insert(target.warnings.end(), selection.warnings.begin(), selection.warnings.end());
			};
			for (TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& selectionSnapshot : virtualGeometryViewSnapshots)
			{
				auto existingSnapshot = std::find_if(
					detailedVirtualGeometryViewSnapshots.begin(),
					detailedVirtualGeometryViewSnapshots.end(),
					[viewType = selectionSnapshot.viewType](const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot) {
						return snapshot.viewType == viewType;
					});
				if (existingSnapshot != detailedVirtualGeometryViewSnapshots.end())
				{
					mergeSelectionDiagnostics(*existingSnapshot, selectionSnapshot);
				}
				else if (selectionSnapshot.viewType == RenderViewType::ExportRuntime
					|| (selectionSnapshot.viewType == RenderViewType::Shadow && !shadowDiagnosticsPublished))
				{
					detailedVirtualGeometryViewSnapshots.push_back(std::move(selectionSnapshot));
				}
			}
			virtualGeometryViewSnapshots = std::move(detailedVirtualGeometryViewSnapshots);
			for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& snapshot : virtualGeometryViewSnapshots)
			{
				for (const PerformanceGateResult& gate : snapshot.virtualGeometryPerformanceGates)
				{
					if (gate.status != VirtualGeometryPerformanceGateStatus::Fail)
					{
						continue;
					}
					renderGraphDiagnostics.backendFallbacks.push_back(
						"Virtual geometry gate failed for "
						+ gate.fixtureId
						+ " "
						+ ToString(gate.viewType)
						+ "#"
						+ std::to_string(gate.viewId)
						+ "/"
						+ gate.metricName
						+ ": expected "
						+ std::to_string(gate.threshold)
						+ ", actual "
						+ std::to_string(gate.actual));
				}
			}
			const TerraEngine::Scheduling::SystemDeclaration schedulerDeclaration = SchedulerDeclaration();
			std::vector<TerraEngine::Renderer::VirtualGeometrySchedulerDiagnostics> virtualGeometrySchedulerSnapshots;
			virtualGeometrySchedulerSnapshots.push_back(TerraEngine::Renderer::BuildVirtualGeometrySchedulerDiagnostics(
				schedulerDeclaration.name,
				schedulerDeclaration.phase,
				schedulerDeclaration.reads,
				schedulerDeclaration.writes,
				schedulerDeclaration.safeSubwork,
				schedulerDeclaration.protectedCommit,
				schedulerDeclaration.requiresFlushedSceneMutations
					? "SceneMutationQueue flush before render snapshot reads; virtual geometry diagnostics publish after protected RHI work."
					: "Virtual geometry diagnostics publish after protected RHI work."));
			TerraEngine::Renderer::ApplyVirtualGeometryDiagnostics(renderGraphDiagnostics, std::move(virtualGeometryViewSnapshots), std::move(virtualGeometrySchedulerSnapshots));
			TerraEngine::Renderer::PublishRenderDiagnostics(renderGraphDiagnostics);
		}
		else
		{
			// Keep latestRenderGraphDiagnostics useful for overlays and MCP stats,
			// but avoid publishing or enriching detailed diagnostics while closed.
			renderGraphDiagnostics.virtualGeometryViews = std::move(virtualGeometryViewSnapshots);
		}
}

// Atomically publish completed diagnostics and transfer snapshot owners under the existing stats lock.
void RenderSystem::PublishFrameDiagnostics(TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	const FrameVirtualGeometryPreparation& virtualGeometryPreparation, FrameViewBuildOutputs& outputs,
	FrameViewObservations& observations, TerraEngine::Renderer::Frame::GeometryRouteStatsAggregate& geometryRouteStats,
	std::uint64_t geometryRouteSelectionSignature, std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof>& fixtureTargets,
	const SkyRuntimeState& skyRuntimeState, const TerraEngine::Renderer::RendererRecoverySnapshot& recoverySnapshot,
	const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>& sealedPublication,
	TerraEngine::Renderer::RenderDiagnosticsSummary& renderGraphDiagnostics)
{
	auto& sceneSpatialDiagnostics = outputs.sceneSpatialDiagnostics;
	auto& gameSpatialDiagnostics = outputs.gameSpatialDiagnostics;
	auto& sceneVisibleCommandTelemetry = outputs.sceneVisibleCommandTelemetry;
	auto& gameVisibleCommandTelemetry = outputs.gameVisibleCommandTelemetry;
	auto& completedSceneVisibilityRecords = observations.completedSceneVisibilityRecords;
	auto& completedGameVisibilityRecords = observations.completedGameVisibilityRecords;
	auto& completedSceneCaptureVisibilityRecords = observations.completedSceneCaptureVisibilityRecords;
	auto& completedGameCaptureVisibilityRecords = observations.completedGameCaptureVisibilityRecords;
	auto& completedSceneCaptureIdentitySnapshot = observations.completedSceneCaptureIdentitySnapshot;
	auto& completedGameCaptureIdentitySnapshot = observations.completedGameCaptureIdentitySnapshot;
	auto& sceneCaptureProcessed = observations.sceneCaptureProcessed;
	auto& gameCaptureProcessed = observations.gameCaptureProcessed;
	const auto& fixtureShadowSelectionApplied = virtualGeometryPreparation.fixtureShadowSelectionApplied;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Publish Render Stats", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		EnrichFrameVirtualGeometryDiagnostics(renderFrameSnapshot, virtualGeometryPreparation,
			sealedPublication, renderGraphDiagnostics);
		std::unique_lock<std::mutex> lock(statsMutex, std::defer_lock);
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Acquire Stats Publication Lock",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			lock.lock();
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Read Backend Render Stats",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			latestGameStats = rhi->GetLatestRenderStats(RenderViewType::Game);
			latestSceneStats = rhi->GetLatestRenderStats(RenderViewType::Scene);
		}
		const auto applyVirtualGeometryStats = [](RenderStatsSnapshot& stats, const TerraEngine::Renderer::RenderDiagnosticsSummary& diagnostics) {
			for (const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot& view : diagnostics.virtualGeometryViews)
			{
				if (view.viewType != stats.viewType)
				{
					continue;
				}
				stats.hiZState = stats.hiZActive
					? view.hiZState
					: (view.hiZState == VirtualGeometryHiZState::Active ? VirtualGeometryHiZState::MissingPyramid : view.hiZState);
				stats.hiZStatusReason = view.hiZInvalidReason;
				if (!stats.hiZActive && stats.hiZStatusReason.empty())
				{
					stats.hiZStatusReason = "backend pyramid unavailable for completed view stats";
				}
				stats.hiZWarmupFrames = view.hiZWarmupFrames;
				stats.hiZBaselineMatched = view.hiZBaselineMatched;
				stats.hiZFalseOcclusionRiskWarnings = view.hiZFalseOcclusionRiskWarnings;
				stats.virtualGeometryClusterPackets = view.clusterPacketCount;
				stats.virtualGeometrySubmittedBatches = view.submittedBatchCount;
				stats.virtualGeometrySinglePacketFallbackBatches = view.singlePacketFallbackBatchCount;
				stats.virtualGeometryCandidatePackets = view.candidateClusterCount;
				stats.virtualGeometryCompactedPackets = view.compactedClusterCount;
				stats.virtualGeometryRejectedPackets = view.rejectedClusterCount;
				stats.virtualGeometryFailOpenPackets = view.failOpenClusterCount;
				stats.virtualGeometryResidentPages = view.residentPages;
				stats.virtualGeometryRequestedPages = view.requestedPages;
				stats.virtualGeometryMissingPages = view.missingPages;
				stats.virtualGeometryFailedPages = view.failedPages;
				stats.virtualGeometryEvictedPages = view.evictedPages;
				stats.virtualGeometryPinnedFallbackPages = view.pinnedFallbackPages;
				// Copy the compact page-source summary into the stats snapshot so
				// UI, automation, and MCP readers can explain page-streamed geometry
				// without dereferencing the heavier render diagnostics graph.
				stats.virtualGeometryResidentPagePayloads = view.pageSources.residentPayloads;
				stats.virtualGeometryHighDetailPagePayloads = view.pageSources.highDetailPayloads;
				stats.virtualGeometryFallbackPagePayloads = view.pageSources.fallbackPayloads;
				stats.virtualGeometryEmbeddedFallbackPagePayloads = view.pageSources.embeddedFallbackPayloads;
				stats.virtualGeometryEmittedHighDetailPages = view.pageSources.emittedHighDetailSources;
				stats.virtualGeometryEmittedFallbackPages = view.pageSources.emittedFallbackSources;
				stats.virtualGeometryEmittedEmbeddedFallbackPages = view.pageSources.emittedEmbeddedFallbackSources;
				stats.virtualGeometryMissingPageSources = view.pageSources.missingSources;
				stats.virtualGeometryPageSourceRequests = view.pageSources.pageRequests;
				stats.virtualGeometryPageSourceRetirements = view.pageSources.pageRetirements;
				stats.virtualGeometryResidentPagePayloadBytes = view.pageSources.residentPayloadBytes;
				stats.virtualGeometryFallbackPagePayloadBytes = view.pageSources.fallbackPayloadBytes;
				stats.virtualGeometryEmbeddedFallbackBytes = view.pageSources.embeddedFallbackBytes;
				stats.virtualGeometryGpuPacketRecords = view.gpuPacketRecordCount;
				stats.virtualGeometryGpuBatchDescriptors = view.gpuBatchDescriptorCount;
				stats.virtualGeometryGpuPacketFrameSets = view.gpuPacketFrameSetCount;
				stats.virtualGeometryGpuValidFrameSets = view.gpuValidFrameSetCount;
				stats.virtualGeometryGpuInvalidFrameSets = view.gpuInvalidFrameSetCount;
				stats.virtualGeometryGpuCompactionDispatches = view.gpuCompactionDispatchCount;
				stats.virtualGeometryGeneratedSubmissions = view.generatedSubmissionCount;
				stats.virtualGeometryValidGeneratedSubmissions = view.validGeneratedSubmissionCount;
				stats.virtualGeometryInvalidGeneratedSubmissions = view.invalidGeneratedSubmissionCount;
				stats.virtualGeometryFallbackGeneratedSubmissions = view.fallbackGeneratedSubmissionCount;
				stats.virtualGeometryGpuCounterReadbacks = view.gpuCounterReadbackCount;
				stats.virtualGeometryGpuCounterLatencyFrames = view.gpuCounterReadbackLatencyFrames;
				stats.virtualGeometryGpuFallbackRecords = view.gpuFallbackRecordCount;
				stats.virtualGeometryGpuFallbackAffectedPackets = view.gpuFallbackAffectedPackets;
				stats.virtualGeometryGpuFallbackAffectedBatches = view.gpuFallbackAffectedBatches;
				stats.virtualGeometryGpuFallbackReason = view.gpuFallbackReason;
				stats.virtualGeometryPerformanceGateCount = static_cast<std::uint32_t>(view.virtualGeometryPerformanceGates.size());
				stats.virtualGeometryPerformanceGateFailures = 0u;
				stats.virtualGeometryPerformanceGateUnsupported = 0u;
				stats.virtualGeometryFirstFailedPerformanceGate.clear();
				for (const PerformanceGateResult& gate : view.virtualGeometryPerformanceGates)
				{
					if (gate.status == VirtualGeometryPerformanceGateStatus::Fail)
					{
						++stats.virtualGeometryPerformanceGateFailures;
						if (stats.virtualGeometryFirstFailedPerformanceGate.empty())
						{
							stats.virtualGeometryFirstFailedPerformanceGate =
								gate.fixtureId + "/" + gate.metricName;
						}
					}
					else if (gate.status == VirtualGeometryPerformanceGateStatus::Unsupported)
					{
						++stats.virtualGeometryPerformanceGateUnsupported;
					}
				}
				if (stats.meshletCounterReason.empty() && stats.meshletCounterSource == VirtualGeometryCounterSource::Prepared)
				{
					stats.meshletCounterSource = view.counterSource;
					stats.meshletCounterFrameId = view.countersAuthoritative
						? view.counterReadbackFrameId
						: view.counterPreparedFrameId;
					stats.meshletCounterLatencyFrames = view.counterReadbackLatencyFrames;
					stats.meshletCounterReason = view.counterUnavailableReason;
				}
				break;
			}
		};
		applyVirtualGeometryStats(latestGameStats, renderGraphDiagnostics);
		applyVirtualGeometryStats(latestSceneStats, renderGraphDiagnostics);
		// Full explanations exist only while diagnostics are requested. They are
		// not needed to count routes, but preserve the most useful fallback string
		// for the overlay when a diagnostic consumer explicitly asked for it.
		for(const TerraEngine::Renderer::GeometryRouteDecision& decision :
			renderFrameSnapshot.geometryRouteDecisions)
		{
			if(!decision.fallbackDiagnostic.empty())
			{
				geometryRouteStats.fallbackReason = decision.fallbackDiagnostic;
			}
		}
		latestGeometryRouteSelectionSignature = geometryRouteSelectionSignature;
		latestGeometryRouteSelectionCount =
			renderFrameSnapshot.renderables.size();
		const auto applyGeometryRouteStats = [&renderFrameSnapshot,
			&geometryRouteStats](RenderStatsSnapshot& stats) {
			stats.geometryRoutePolicyIdentity = geometryRouteStats.policyIdentity;
			stats.geometryRouteCalibrationIdentity = geometryRouteStats.calibrationIdentity;
			stats.geometryRouteHysteresisFrames = std::max(
				stats.geometryRouteHysteresisFrames,
				geometryRouteStats.hysteresisFrames);
			stats.geometryRouteFallbackReason = geometryRouteStats.fallbackReason;
			stats.geometryRouteSource = geometryRouteStats.source
				== TerraEngine::Renderer::GeometryRouteSelectionSource::AuthorOverride
				? "author_override" : geometryRouteStats.source
					== TerraEngine::Renderer::GeometryRouteSelectionSource::CapabilityFallback
				? "capability_fallback" : geometryRouteStats.source
					== TerraEngine::Renderer::GeometryRouteSelectionSource::CorrectnessFallback
				? "correctness_fallback" : "automatic";
			stats.geometryRouteDirectBatchCount = static_cast<std::uint32_t>(renderFrameSnapshot.directMeshletBatches.size());
			stats.geometryRouteSharedInstanceCount = geometryRouteStats.sharedInstances;
			stats.geometryRouteSuppressedPageRequests = geometryRouteStats.directCount;
			stats.geometryRouteSuppressedDecodeBlocks = geometryRouteStats.directCount;
			stats.geometryRouteSuppressedResidencyRecords = geometryRouteStats.directCount;
			// An empty route envelope means this frame contained no routable geometry;
			// do not label that absence as a successful direct-meshlet decision.
			stats.geometryRoute = renderFrameSnapshot.renderables.empty() ? ""
				: geometryRouteStats.virtualCount == 0U ? "direct_meshlet"
				: geometryRouteStats.directCount == 0U ? "virtual_geometry" : "mixed";
		};
		applyGeometryRouteStats(latestGameStats);
		applyGeometryRouteStats(latestSceneStats);
		latestGameStats.spatialIndexedObjectCount = gameSpatialDiagnostics.indexedObjectCount;
		latestGameStats.spatialVisibleCandidateCount = gameSpatialDiagnostics.visibleCandidateCount;
		latestGameStats.spatialRejectedObjectCount = gameSpatialDiagnostics.rejectedObjectCount;
		latestGameStats.spatialFallbackObjectCount = gameSpatialDiagnostics.fallbackObjectCount;
		latestGameStats.spatialRefreshMode = gameSpatialDiagnostics.refreshMode;
		latestGameStats.spatialAddedObjectCount = gameSpatialDiagnostics.addedObjectCount;
		latestGameStats.spatialUpdatedObjectCount = gameSpatialDiagnostics.updatedObjectCount;
		latestGameStats.spatialRemovedObjectCount = gameSpatialDiagnostics.removedObjectCount;
		latestGameStats.spatialUnchangedObjectCount = gameSpatialDiagnostics.unchangedObjectCount;
		latestGameStats.spatialFullRefreshObjectCount = gameSpatialDiagnostics.fullRefreshObjectCount;
		latestGameStats.transformDirtyRootCount = gameSpatialDiagnostics.transformDirtyRootCount;
		latestGameStats.transformProcessedObjectCount = gameSpatialDiagnostics.transformProcessedObjectCount;
		latestGameStats.boundsRecomputedObjectCount = gameSpatialDiagnostics.boundsRecomputedObjectCount;
		latestGameStats.visibleCommandCacheHit = gameVisibleCommandTelemetry.cacheHit;
		latestGameStats.visibleCommandConservativeRefresh = gameVisibleCommandTelemetry.conservativeRefresh;
		latestGameStats.detailedRenderDiagnosticsRequested = gameVisibleCommandTelemetry.detailedDiagnosticsRequested;
		latestGameStats.visibleCommandCount = gameVisibleCommandTelemetry.commandCount;
		latestGameStats.visibleCommandCopiedCount = gameVisibleCommandTelemetry.copiedCommandCount;
		latestGameStats.visibleStreamingUniqueMeshes = gameVisibleCommandTelemetry.uniqueMeshTouches;
		latestGameStats.visibleStreamingUniqueMaterials = gameVisibleCommandTelemetry.uniqueMaterialTouches;
		latestGameStats.visibleStreamingUniqueTextures = gameVisibleCommandTelemetry.uniqueTextureTouches;
		latestGameStats.visibleStreamingDuplicateTouchesEliminated = gameVisibleCommandTelemetry.duplicateResourceTouchesEliminated;
		latestGameStats.visibleCommandReuseFallbackReason = gameVisibleCommandTelemetry.fallbackReason;
		latestSceneStats.spatialIndexedObjectCount = sceneSpatialDiagnostics.indexedObjectCount;
		latestSceneStats.spatialVisibleCandidateCount = sceneSpatialDiagnostics.visibleCandidateCount;
		latestSceneStats.spatialRejectedObjectCount = sceneSpatialDiagnostics.rejectedObjectCount;
		latestSceneStats.spatialFallbackObjectCount = sceneSpatialDiagnostics.fallbackObjectCount;
		latestSceneStats.spatialRefreshMode = sceneSpatialDiagnostics.refreshMode;
		latestSceneStats.spatialAddedObjectCount = sceneSpatialDiagnostics.addedObjectCount;
		latestSceneStats.spatialUpdatedObjectCount = sceneSpatialDiagnostics.updatedObjectCount;
		latestSceneStats.spatialRemovedObjectCount = sceneSpatialDiagnostics.removedObjectCount;
		latestSceneStats.spatialUnchangedObjectCount = sceneSpatialDiagnostics.unchangedObjectCount;
		latestSceneStats.spatialFullRefreshObjectCount = sceneSpatialDiagnostics.fullRefreshObjectCount;
		latestSceneStats.transformDirtyRootCount = sceneSpatialDiagnostics.transformDirtyRootCount;
		latestSceneStats.transformProcessedObjectCount = sceneSpatialDiagnostics.transformProcessedObjectCount;
		latestSceneStats.boundsRecomputedObjectCount = sceneSpatialDiagnostics.boundsRecomputedObjectCount;
		latestSceneStats.visibleCommandCacheHit = sceneVisibleCommandTelemetry.cacheHit;
		latestSceneStats.visibleCommandConservativeRefresh = sceneVisibleCommandTelemetry.conservativeRefresh;
		latestSceneStats.detailedRenderDiagnosticsRequested = sceneVisibleCommandTelemetry.detailedDiagnosticsRequested;
		latestSceneStats.visibleCommandCount = sceneVisibleCommandTelemetry.commandCount;
		latestSceneStats.visibleCommandCopiedCount = sceneVisibleCommandTelemetry.copiedCommandCount;
		latestSceneStats.visibleStreamingUniqueMeshes = sceneVisibleCommandTelemetry.uniqueMeshTouches;
		latestSceneStats.visibleStreamingUniqueMaterials = sceneVisibleCommandTelemetry.uniqueMaterialTouches;
		latestSceneStats.visibleStreamingUniqueTextures = sceneVisibleCommandTelemetry.uniqueTextureTouches;
		latestSceneStats.visibleStreamingDuplicateTouchesEliminated = sceneVisibleCommandTelemetry.duplicateResourceTouchesEliminated;
		latestSceneStats.visibleCommandReuseFallbackReason = sceneVisibleCommandTelemetry.fallbackReason;
		latestGameSpatialDiagnostics = gameSpatialDiagnostics;
		latestSceneSpatialDiagnostics = sceneSpatialDiagnostics;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Read Backend Diagnostic Snapshots",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			latestGameBudgetDiagnostics = rhi->GetLatestFrameBudgetDiagnostics(RenderViewType::Game);
			latestSceneBudgetDiagnostics = rhi->GetLatestFrameBudgetDiagnostics(RenderViewType::Scene);
			latestGameLightingDiagnostics = rhi->GetLatestLightingDiagnostics(RenderViewType::Game);
			latestSceneLightingDiagnostics = rhi->GetLatestLightingDiagnostics(RenderViewType::Scene);
			latestRenderSnapshotDiagnostics = rhi->GetLatestRenderSnapshotDiagnostics();
			latestRendererRecoverySnapshot = rhi ? rhi->GetLatestRendererRecoverySnapshot() : recoverySnapshot;
			latestAnimationGpuBackendExecution = rhi
				? rhi->GetLatestAnimationGpuBackendExecution()
				: TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot{};
			latestRenderGraphDiagnostics = renderGraphDiagnostics;
		}
		// Capture identity without inventing backend measurements. Story-specific
		// receipts replace the explicit unavailable counters as stages are added.
		auto completedEfficiencyRows = rhi->PollRendererEfficiencyEvidence();
		std::erase_if(completedEfficiencyRows, [&](const auto& row) {
			return !TerraEngine::Renderer::IsCurrentRendererEfficiencyFixtureEvidence(row,
				renderFrameSnapshot.efficiencyOptions, renderFrameSnapshot.efficiencyFixtureIntent);
		});
		if(!completedEfficiencyRows.empty())
		{
			// One cursor owns the entire completed batch. View and face counts do
			// not advance fixture frame progression independently.
			++latestRendererEfficiencyEvidenceBatch.batchSequence;
			latestRendererEfficiencyEvidenceBatch.rows = std::move(completedEfficiencyRows);
			latestRendererEfficiencyEvidence = latestRendererEfficiencyEvidenceBatch.rows;
		}
		else if(latestRendererEfficiencyEvidence.empty())
		{
		for (const auto& view : renderFrameSnapshot.viewRequests)
		{
			TerraEngine::Renderer::RendererEfficiencyEvidence evidence;
			evidence.viewType = view.viewType;
			evidence.viewId = view.outputId;
			evidence.frameSerial = renderFrameSnapshot.frameId;
			evidence.resolution = view.targetSize;
			evidence.effectiveOptions = renderFrameSnapshot.efficiencyOptions;
			evidence.staticCacheBudgetRequestedBytes = evidence.effectiveOptions.options.staticShadowCacheBudgetBytes;
			evidence.staticCacheBudgetEffectiveBytes = evidence.staticCacheBudgetRequestedBytes;
			evidence.fallbackReason = "Optimized stage support has not been published by the backend.";
			latestRendererEfficiencyEvidence.push_back(std::move(evidence));
		}
		}
		// The renderer has finished every backend and statistics read of the local
		// snapshot. Transfer its owned vectors into the protected publication instead
		// of deep-copying thousands of renderables, route decisions, and batch members.
		// The feedback pass below only reads this completed publication.
		if (renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied && renderFrameSnapshot.efficiencyFixtureIntent.shadowSelection
			&& !fixtureShadowSelectionApplied.load(std::memory_order_relaxed))
		{
			renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied = false;
			renderFrameSnapshot.efficiencyFixtureIntent.controlReason = "Requested target did not appear in an emitted shadow owner-range command.";
		}
		latestRendererEfficiencyFixtureTargets = std::move(fixtureTargets);
		std::swap(latestRenderFrameSnapshot, renderFrameSnapshot);
		recycledRenderFrameSnapshot = std::move(renderFrameSnapshot);
		latestSceneVirtualGeometryVisibilityRecords =
			std::move(completedSceneVisibilityRecords);
		latestGameVirtualGeometryVisibilityRecords =
			std::move(completedGameVisibilityRecords);
		// An ordinary frame must not erase valid custom-capture proof before its
		// explicit identity query reaches the fence-owned Vulkan capture target.
		if(sceneCaptureProcessed)
			latestSceneCaptureVisibilityRecords = std::move(completedSceneCaptureVisibilityRecords);
		if(gameCaptureProcessed)
			latestGameCaptureVisibilityRecords = std::move(completedGameCaptureVisibilityRecords);
		if(sceneCaptureProcessed)
			latestSceneCaptureIdentitySnapshot = std::move(completedSceneCaptureIdentitySnapshot);
		if(gameCaptureProcessed)
			latestGameCaptureIdentitySnapshot = std::move(completedGameCaptureIdentitySnapshot);
		latestSkyRuntimeState = skyRuntimeState;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Record Profiler Render Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			TerraEngine::Profiling::Profiler::Instance().RecordRenderStats(
				latestGameStats, latestSceneStats);
		}
	}

}
