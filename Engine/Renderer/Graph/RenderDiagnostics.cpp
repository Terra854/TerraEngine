#include "RenderDiagnostics.h"
#include "RenderFrameSnapshot.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryVisibility.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Renderer {

	namespace {
		constexpr std::size_t kRecoveryRecentPhaseLimit = 8u;

		std::atomic<bool> g_renderDiagnosticsRequested = false;
		std::mutex g_renderDiagnosticsMutex;
		RenderDiagnosticsSummary g_latestRenderDiagnostics;
		std::mutex g_backendResourceResidencyMutex;
		BackendResourceResidencySnapshot g_latestBackendResourceResidency;

		std::vector<std::string> BuildRecentRendererPhaseRing(const BackendFrameBreakdown& breakdown)
		{
			std::vector<std::string> phases;
			const std::size_t begin = breakdown.stageTimings.size() > kRecoveryRecentPhaseLimit
				? breakdown.stageTimings.size() - kRecoveryRecentPhaseLimit
				: 0u;
			for (std::size_t index = begin; index < breakdown.stageTimings.size(); ++index)
			{
				const BackendStageTiming& timing = breakdown.stageTimings[index];
				const std::string label = timing.label.empty()
					? std::string(ToString(timing.stage))
					: timing.label;
				phases.push_back(
					std::to_string(timing.frameId)
					+ ": "
					+ ToString(timing.stage)
					+ " "
					+ label
					+ " "
					+ std::to_string(timing.milliseconds)
					+ "ms");
			}
			return phases;
		}

		std::uint64_t ResolvePageResidencyBytes(const PageResidencyRecord& page)
		{
			// Older and test-built page records may carry only a payload size,
			// while residency-budget records carry an explicit byte cost. Treat
			// the payload size as the conservative renderable-byte fallback so
			// editor residency tools do not show visible VG pages as zero-byte.
			return page.byteCost != 0u ? page.byteCost : page.payloadByteSize;
		}
	}

	RenderBudgetDiagnostic BuildRenderBudgetDiagnostic(const RenderWorkBudget& budget)
	{
		RenderBudgetDiagnostic diagnostic;
		diagnostic.category = budget.category;
		diagnostic.limit = budget.limit;
		diagnostic.currentUsage = budget.currentUsage;
		diagnostic.overflowPolicy = budget.overflowPolicy;
		diagnostic.overBudget = IsRenderWorkOverBudget(budget);
		diagnostic.label = budget.diagnosticLabel.empty() ? ToString(budget.category) : budget.diagnosticLabel;
		return diagnostic;
	}

	RenderPerformanceGateResult EvaluateRenderPerformanceGate(std::string workloadName, float baselineValue, float latestValue, float thresholdPercent)
	{
		RenderPerformanceGateResult result;
		result.workloadName = std::move(workloadName);
		result.baselineValue = baselineValue;
		result.latestValue = latestValue;
		result.thresholdPercent = thresholdPercent;
		const float allowed = baselineValue * (1.0f + thresholdPercent / 100.0f);
		result.passed = latestValue <= allowed;
		return result;
	}

	PerformanceGateResult EvaluateVirtualGeometryPerformanceGate(
		std::string gateId,
		std::string fixtureId,
		std::string metricName,
		float threshold,
		float actual,
		std::string unit,
		bool lowerIsBetter,
		VirtualGeometryPerformanceGateSource source,
		RenderViewType viewType,
		std::uint32_t viewId)
	{
		PerformanceGateResult result;
		result.gateId = std::move(gateId);
		result.fixtureId = std::move(fixtureId);
		result.viewType = viewType;
		result.viewId = viewId;
		result.metricName = std::move(metricName);
		result.threshold = threshold;
		result.actual = actual;
		result.unit = std::move(unit);
		result.source = source;
		const bool pass = lowerIsBetter ? actual <= threshold : actual >= threshold;
		result.status = pass ? VirtualGeometryPerformanceGateStatus::Pass : VirtualGeometryPerformanceGateStatus::Fail;
		result.diagnostic =
			result.fixtureId
			+ " "
			+ result.metricName
			+ (pass ? " passed " : " failed ")
			+ (lowerIsBetter ? "max " : "min ")
			+ std::to_string(threshold)
			+ " with actual "
			+ std::to_string(actual)
			+ " "
			+ result.unit;
		return result;
	}

	std::vector<PerformanceGateResult> BuildVirtualGeometryPerformanceGates(const VirtualGeometryViewDiagnosticsSnapshot& snapshot)
	{
		std::vector<PerformanceGateResult> gates;
		const VirtualGeometryPerformanceGateSource source =
			snapshot.counterSource == VirtualGeometryCounterSource::GPUReadback
				? VirtualGeometryPerformanceGateSource::GPUReadback
				: (snapshot.counterSource == VirtualGeometryCounterSource::DelayedGPUReadback
					? VirtualGeometryPerformanceGateSource::DelayedGPUReadback
					: ((snapshot.counterSource == VirtualGeometryCounterSource::Unsupported
						|| snapshot.counterSource == VirtualGeometryCounterSource::Unavailable)
						? VirtualGeometryPerformanceGateSource::Unsupported
						: VirtualGeometryPerformanceGateSource::Prepared));
		const auto addUnsupported = [&](std::string gateId, std::string fixtureId, std::string metricName, std::string unit, std::string reason) {
			PerformanceGateResult gate;
			gate.gateId = std::move(gateId);
			gate.fixtureId = std::move(fixtureId);
			gate.viewType = snapshot.viewType;
			gate.viewId = snapshot.viewId;
			gate.metricName = std::move(metricName);
			gate.unit = std::move(unit);
			gate.status = VirtualGeometryPerformanceGateStatus::Unsupported;
			gate.source = source;
			gate.diagnostic = std::move(reason);
			gates.push_back(std::move(gate));
		};
		const auto addGate = [&](
			std::string gateId,
			std::string fixtureId,
			std::string metricName,
			float threshold,
			float actual,
			std::string unit,
			bool lowerIsBetter) {
			gates.push_back(EvaluateVirtualGeometryPerformanceGate(
				std::move(gateId),
				std::move(fixtureId),
				std::move(metricName),
				threshold,
				actual,
				std::move(unit),
				lowerIsBetter,
				source,
				snapshot.viewType,
				snapshot.viewId));
		};
		const auto attachPageStreamingScope = [&]() {
			const TerraEngine::UUID scopedAssetId = snapshot.budgetContributors.empty()
				? TerraEngine::EMPTY_UUID
				: snapshot.budgetContributors.front().assetId;
			const bool hasPageScope =
				snapshot.pageSources.residentPayloads > 0u
				|| snapshot.pageSources.highDetailPayloads > 0u
				|| snapshot.pageSources.fallbackPayloads > 0u
				|| snapshot.pageSources.embeddedFallbackPayloads > 0u
				|| snapshot.pageSources.emittedHighDetailSources > 0u
				|| snapshot.pageSources.emittedFallbackSources > 0u
				|| snapshot.pageSources.emittedEmbeddedFallbackSources > 0u
				|| snapshot.pageSources.missingSources > 0u;
			for (PerformanceGateResult& gate : gates)
			{
				if (gate.fixtureId != "virtual-geometry-page-streaming-resident")
				{
					continue;
				}
				gate.assetId = scopedAssetId;
				if (hasPageScope)
				{
					gate.pageId = 0u;
				}
			}
		};
		const std::uint64_t exactResidentPageBytes =
			snapshot.pageResidency.residentHighDetailBytes
			+ snapshot.pageResidency.residentFallbackBytes;
		const std::uint64_t residentPageBytesForGates =
			exactResidentPageBytes > 0u
				? exactResidentPageBytes
				: snapshot.pageSources.residentPayloadBytes;
		const std::uint64_t owningMeshFallbackBytes =
			snapshot.pageResidency.owningMeshFallbackBytes
			+ snapshot.pageSources.embeddedFallbackBytes;

		if (snapshot.counterSource == VirtualGeometryCounterSource::Unsupported
			|| snapshot.counterSource == VirtualGeometryCounterSource::Unavailable)
		{
			const std::string reason = snapshot.counterUnavailableReason.empty()
				? std::string("counter source is ") + ToString(snapshot.counterSource)
				: snapshot.counterUnavailableReason;
			addUnsupported(
				"virtual-geometry-counter-source-available",
				"gpu-packet-compaction-generated-submit",
				"counterSource",
				"state",
				"Performance gates require source-labeled counters; " + reason);
		}

		if (snapshot.clusterPacketCount >= 500u)
		{
			const float batchRatio = snapshot.clusterPacketCount == 0u
				? 0.0f
				: static_cast<float>(snapshot.submittedBatchCount) / static_cast<float>(snapshot.clusterPacketCount);
			addGate(
				"dense-cluster-batch-ratio",
				"virtual-geometry-dense-batching",
				"submittedBatchRatio",
				0.1f,
				batchRatio,
				"ratio",
				true);
		}
		else
		{
			addUnsupported(
				"dense-cluster-batch-ratio",
				"virtual-geometry-dense-batching",
				"submittedBatchRatio",
				"ratio",
				"Dense batch-ratio gate requires at least 500 prepared cluster packets.");
		}

		const float estimatedPrepMs =
			static_cast<float>(snapshot.clusterPacketCount) * 0.001f
			+ static_cast<float>(snapshot.submittedBatchCount) * 0.01f
			+ static_cast<float>(snapshot.requestedPages) * 0.002f;
		addGate(
			"virtual-geometry-prep-cost",
			"virtual-geometry-dense-batching",
			"estimatedPreparedMilliseconds",
			5.0f,
			estimatedPrepMs,
			"ms",
			true);

		if (snapshot.gpuPacketRecordCount >= 500u)
		{
			const float gpuBatchRatio = static_cast<float>(snapshot.gpuBatchDescriptorCount) / static_cast<float>(snapshot.gpuPacketRecordCount);
			addGate(
				"gpu-packet-batch-ratio",
				"gpu-packet-buffer-batching",
				"gpuBatchDescriptorRatio",
				0.1f,
				gpuBatchRatio,
				"ratio",
				true);
		}
		else
		{
			addUnsupported(
				"gpu-packet-batch-ratio",
				"gpu-packet-buffer-batching",
				"gpuBatchDescriptorRatio",
				"ratio",
				"GPU packet batch-ratio gate requires at least 500 GPU packet records.");
		}

		if (snapshot.geometryBudgetBytes > 0u)
		{
			addGate(
				"virtual-geometry-page-pressure",
				"streaming-pressure",
				"budgetPressureBytes",
				static_cast<float>(snapshot.geometryBudgetBytes),
				static_cast<float>(snapshot.budgetPressureBytes),
				"bytes",
				true);
		}
		else
		{
			addUnsupported(
				"virtual-geometry-page-pressure",
				"streaming-pressure",
				"budgetPressureBytes",
				"bytes",
				"Page-pressure gate requires a configured virtual geometry byte budget.");
		}

		if (snapshot.geometryBudgetBytes > 0u && residentPageBytesForGates > 0u)
		{
			const float residentBudgetRatio =
				static_cast<float>(residentPageBytesForGates)
				/ static_cast<float>(snapshot.geometryBudgetBytes);
			addGate(
				"virtual-geometry-resident-page-budget-ratio",
				"virtual-geometry-page-streaming-resident",
				"residentPayloadBudgetRatio",
				1.0f,
				residentBudgetRatio,
				"ratio",
				true);
		}
		else
		{
			addUnsupported(
				"virtual-geometry-resident-page-budget-ratio",
				"virtual-geometry-page-streaming-resident",
				"residentPayloadBudgetRatio",
				"ratio",
				"Resident page-budget gate requires resident payload bytes and a configured page budget.");
		}

		if (snapshot.estimatedGeometryBytes > 0u && residentPageBytesForGates > 0u)
		{
			const float owningMeshResidencyRatio =
				static_cast<float>(residentPageBytesForGates)
				/ static_cast<float>(snapshot.estimatedGeometryBytes);
			addGate(
				"virtual-geometry-full-owning-mesh-residency",
				"virtual-geometry-page-streaming-resident",
				"residentPayloadVsFullMeshRatio",
				0.95f,
				owningMeshResidencyRatio,
				"ratio",
				true);
		}
		else
		{
			addUnsupported(
				"virtual-geometry-full-owning-mesh-residency",
				"virtual-geometry-page-streaming-resident",
				"residentPayloadVsFullMeshRatio",
				"ratio",
				"Full-owning-mesh regression gate requires estimated full geometry bytes and resident payload bytes.");
		}

		addGate(
			"virtual-geometry-owning-mesh-fallback-bytes",
			"virtual-geometry-page-streaming-resident",
			"owningMeshFallbackBytes",
			0.0f,
			static_cast<float>(owningMeshFallbackBytes),
			"bytes",
			true);

		// True page streaming must not hide holes or the legacy embedded mesh
		// path behind normal-looking counters. These gates intentionally read
		// emitted page-source diagnostics rather than visual output.
		addGate(
			"virtual-geometry-missing-page-coverage",
			"virtual-geometry-page-streaming-resident",
			"missingPageSources",
			0.0f,
			static_cast<float>(snapshot.pageSources.missingSources),
			"sources",
			true);
		addGate(
			"virtual-geometry-embedded-fallback-uses",
			"virtual-geometry-page-streaming-resident",
			"embeddedFallbackSources",
			0.0f,
			static_cast<float>(snapshot.pageSources.emittedEmbeddedFallbackSources),
			"sources",
			true);
		attachPageStreamingScope();

		if (snapshot.selectedMeshlets > 0u)
		{
			const float dispatchRatio = static_cast<float>(snapshot.dispatchedMeshlets) / static_cast<float>(snapshot.selectedMeshlets);
			addGate(
				"virtual-geometry-selected-dispatched-ratio",
				"runtime-packet-compaction",
				"dispatchedToSelectedRatio",
				1.0f,
				dispatchRatio,
				"ratio",
				true);
		}
		else
		{
			addUnsupported(
				"virtual-geometry-selected-dispatched-ratio",
				"runtime-packet-compaction",
				"dispatchedToSelectedRatio",
				"ratio",
				"Selected-to-dispatched ratio gate requires selected meshlet counters.");
		}

		if (snapshot.candidateClusterCount > 0u)
		{
			const std::uint32_t accountedPackets = snapshot.compactedClusterCount + snapshot.rejectedClusterCount;
			addGate(
				"gpu-compaction-accounted-packets",
				"gpu-packet-compaction-generated-submit",
				"accountedPackets",
				static_cast<float>(snapshot.candidateClusterCount),
				static_cast<float>(accountedPackets),
				"packets",
				false);
			addGate(
				"gpu-compaction-overflow-packets",
				"gpu-packet-compaction-generated-submit",
				"accountedMinusCandidate",
				0.0f,
				accountedPackets > snapshot.candidateClusterCount
					? static_cast<float>(accountedPackets - snapshot.candidateClusterCount)
					: 0.0f,
				"packets",
				true);
		}
		else
		{
			addUnsupported(
				"gpu-compaction-accounted-packets",
				"gpu-packet-compaction-generated-submit",
				"accountedPackets",
				"packets",
				"Compaction truth gate requires candidate packet counters.");
		}

		if (snapshot.generatedSubmissionCount > 0u)
		{
			addGate(
				"generated-submission-invalid-count",
				"gpu-packet-compaction-generated-submit",
				"invalidGeneratedSubmissions",
				0.0f,
				static_cast<float>(snapshot.invalidGeneratedSubmissionCount),
				"submissions",
				true);
			if (snapshot.compactedClusterCount > 0u)
			{
				addGate(
					"generated-submission-packet-coverage",
					"gpu-packet-compaction-generated-submit",
					"generatedSubmissionsVsCompactedPackets",
					static_cast<float>(snapshot.compactedClusterCount),
					static_cast<float>(snapshot.validGeneratedSubmissionCount),
					"submissions",
					true);
			}
		}
		else if (snapshot.gpuPacketRecordCount > 0u)
		{
			addUnsupported(
				"generated-submission-invalid-count",
				"gpu-packet-compaction-generated-submit",
				"invalidGeneratedSubmissions",
				"submissions",
				"Generated submission validity gate requires generated submission records.");
		}

		addGate(
			"virtual-geometry-hiz-baseline-match",
			"thin-hiz-occluder",
			"hizBaselineMatched",
			1.0f,
			snapshot.hiZBaselineMatched ? 1.0f : 0.0f,
			"bool",
			false);
		addGate(
			"virtual-geometry-hiz-false-occlusion",
			"thin-hiz-occluder",
			"falseOcclusionRiskWarnings",
			0.0f,
			static_cast<float>(snapshot.hiZFalseOcclusionRiskWarnings),
			"warnings",
			true);
		return gates;
	}

	std::vector<PerformanceGateResult> BuildVisibleCommandReusePerformanceGates(const RenderStatsSnapshot& stats)
	{
		std::vector<PerformanceGateResult> gates;
		const float copiedCommands = static_cast<float>(stats.visibleCommandCopiedCount);
		const float visibleCommands = static_cast<float>(stats.visibleCommandCount);
		// Cache-hit reuse should never copy more commands than the completed
		// view owns. This is a count gate rather than a timing gate so it can run
		// in deterministic unit tests and still catch accidental payload churn.
		gates.push_back(EvaluateVirtualGeometryPerformanceGate(
			"visible-command-copy-count",
			"visible-command-reuse-static",
			"copiedCommands",
			visibleCommands,
			copiedCommands,
			"commands",
			true,
			VirtualGeometryPerformanceGateSource::Prepared,
			stats.viewType,
			stats.viewType == RenderViewType::Scene ? 1u : 2u));
		if (stats.visibleCommandConservativeRefresh)
		{
			PerformanceGateResult gate;
			gate.gateId = "visible-command-conservative-refresh";
			gate.fixtureId = "visible-command-reuse-static";
			gate.viewType = stats.viewType;
			gate.viewId = stats.viewType == RenderViewType::Scene ? 1u : 2u;
			gate.metricName = "conservativeRefresh";
			gate.threshold = 0.0f;
			gate.actual = 1.0f;
			gate.unit = "state";
			gate.status = VirtualGeometryPerformanceGateStatus::Warning;
			gate.source = VirtualGeometryPerformanceGateSource::Prepared;
			gate.diagnostic = stats.visibleCommandReuseFallbackReason.empty()
				? "Visible-command reuse fell back to conservative refresh."
				: stats.visibleCommandReuseFallbackReason;
			gates.push_back(std::move(gate));
		}
		return gates;
	}

	VirtualGeometryViewDiagnosticsSnapshot BuildInactiveVirtualGeometryDiagnostics(RenderViewType viewType, std::uint64_t frameId, VirtualGeometryInactiveReason reason)
	{
		VirtualGeometryViewDiagnosticsSnapshot snapshot;
		snapshot.viewType = viewType;
		snapshot.viewId = 0u;
		snapshot.frameId = frameId;
		snapshot.active = false;
		snapshot.inactiveReason = reason;
		snapshot.warnings.push_back("Virtual geometry is inactive for this view; rendering must use the conservative fallback path.");
		return snapshot;
	}

	VirtualGeometryAdaptiveSettingsDiagnostics BuildVirtualGeometryAdaptiveSettingsDiagnostics(const AdaptiveVirtualGeometryCookProfile& profile)
	{
		const AdaptiveVirtualGeometryCookProfile sanitized = SanitizeAdaptiveVirtualGeometryCookProfile(profile);
		VirtualGeometryAdaptiveSettingsDiagnostics diagnostics;
		diagnostics.settingsSource = sanitized.settingsSource;
		diagnostics.materialSafetyPolicy = sanitized.materialSafetyPolicy;
		diagnostics.fallbackPolicy = sanitized.fallbackPolicy;
		diagnostics.requestedTargetCount = sanitized.targetRatioCount;
		diagnostics.minimumSourceTriangles = sanitized.minimumSourceTriangles;
		diagnostics.minimumGeneratedTriangles = sanitized.minimumGeneratedTriangles;
		diagnostics.minimumUsefulTriangleReduction = sanitized.minimumUsefulTriangleReduction;
		diagnostics.minimumUsefulMeshletReduction = sanitized.minimumUsefulMeshletReduction;
		return diagnostics;
	}

	VirtualGeometryCookOutputDiagnostics BuildVirtualGeometryCookOutputDiagnostics(const std::vector<VirtualGeometryAssetRuntime>& assets)
	{
		VirtualGeometryCookOutputDiagnostics diagnostics;
		for (const VirtualGeometryAssetRuntime& asset : assets)
		{
			diagnostics.generatedLevelCount += static_cast<std::uint32_t>(asset.generatedLevels.empty() ? asset.detailLevels.size() : asset.generatedLevels.size());
			diagnostics.skippedDetailCount += static_cast<std::uint32_t>(asset.skippedDetails.size());
			diagnostics.clusterCount += static_cast<std::uint32_t>(asset.clusters.size());
			diagnostics.pageCount += static_cast<std::uint32_t>(asset.pages.size());
			for (const VirtualGeometrySkippedDetailRuntime& skipped : asset.skippedDetails)
			{
				if (skipped.status == VirtualGeometryDetailTargetStatus::Merged)
				{
					++diagnostics.mergedDetailCount;
				}
				if (skipped.status == VirtualGeometryDetailTargetStatus::Rejected)
				{
					++diagnostics.rejectedDetailCount;
				}
			}
		}
		return diagnostics;
	}

	VirtualGeometryViewDiagnosticsSnapshot BuildDefaultAdaptiveVirtualGeometryDiagnostics(
		RenderViewType viewType,
		std::uint64_t frameId,
		const AdaptiveVirtualGeometryCookProfile& profile)
	{
		VirtualGeometryViewDiagnosticsSnapshot snapshot;
		snapshot.viewType = viewType;
		snapshot.viewId = 0u;
		snapshot.frameId = frameId;
		snapshot.active = true;
		snapshot.adaptiveSettings = BuildVirtualGeometryAdaptiveSettingsDiagnostics(profile);
		snapshot.cookOutput = {};
		snapshot.clusterSelection = {};
		snapshot.pageResidency = {};
		snapshot.hiZValidation = {};
		snapshot.hiZValidation.ownerViewType = viewType;
		snapshot.hiZActive = true;
		snapshot.hiZHistoryValid = true;
		snapshot.counterSource = VirtualGeometryCounterSource::Prepared;
		snapshot.hiZState = VirtualGeometryHiZState::Active;
		// This configuration-only snapshot is not tied to a concrete pyramid
		// resource. Leave inactive state empty so default diagnostics cannot
		// masquerade as a runtime Hi-Z disable path.
		snapshot.hiZValidation.failOpen = false;
		snapshot.hiZValidation.inactiveReason.clear();
		return snapshot;
	}

	void ApplyVirtualGeometryAdaptiveDiagnostics(
		VirtualGeometryViewDiagnosticsSnapshot& snapshot,
		VirtualGeometryAdaptiveSettingsDiagnostics settings,
		VirtualGeometryCookOutputDiagnostics cookOutput,
		VirtualGeometryClusterSelectionDiagnostics clusterSelection,
		VirtualGeometryPageResidencyDiagnostics pageResidency,
		VirtualGeometryHiZValidationDiagnostics hiZValidation)
	{
		// This helper is deliberately a value copy. Worker jobs can build these
		// small diagnostic records from immutable snapshots, and the protected
		// publication step swaps the finished summary without sharing live state.
		snapshot.adaptiveSettings = std::move(settings);
		snapshot.cookOutput = std::move(cookOutput);
		snapshot.clusterSelection = std::move(clusterSelection);
		snapshot.pageResidency = std::move(pageResidency);
		snapshot.hiZValidation = std::move(hiZValidation);
	}

	VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryLodQualityDiagnostics(
		RenderViewType viewType,
		std::uint64_t frameId,
		const std::vector<VirtualGeometryAssetRuntime>& assets,
		const VirtualGeometryQualityProfile& qualityProfile)
	{
		const VirtualGeometryQualityProfile quality = SanitizeVirtualGeometryQualityProfile(qualityProfile);
		VirtualGeometryViewDiagnosticsSnapshot snapshot;
		snapshot.viewType = viewType;
		snapshot.viewId = 0u;
		snapshot.frameId = frameId;
		snapshot.active = true;
		snapshot.debugVisualizationMode = quality.debugVisualizationMode;
		snapshot.effectiveLodErrorScale = quality.lodErrorScale;
		snapshot.configuredMaxLodDrop = quality.maxLodDrop;
		snapshot.lodTransitionHysteresisPixels = quality.lodTransitionHysteresisPixels;
		snapshot.lodTransitionStableFrames = quality.lodTransitionStableFrames;
		snapshot.geometryBudgetBytes = quality.geometryBudgetBytes;
		snapshot.cookOutput = BuildVirtualGeometryCookOutputDiagnostics(assets);
		snapshot.counterSource = VirtualGeometryCounterSource::Prepared;
		snapshot.hiZState = VirtualGeometryHiZState::Active;

		for (const VirtualGeometryAssetRuntime& asset : assets)
		{
			snapshot.adaptiveSettings = BuildVirtualGeometryAdaptiveSettingsDiagnostics(asset.adaptiveCookProfile);
			VirtualGeometryBudgetContributor contributor;
			contributor.assetId = asset.assetId;
			contributor.viewType = viewType;
			snapshot.sourceMeshlets += static_cast<std::uint32_t>(asset.cullableGroups.size());
			snapshot.estimatedGeometryBytes +=
				static_cast<std::uint64_t>(asset.detailLevels.size() * sizeof(VirtualGeometryDetailRuntime))
				+ static_cast<std::uint64_t>(asset.cullableGroups.size() * sizeof(VirtualGeometryCullableGroupRuntime))
				+ static_cast<std::uint64_t>(asset.pages.size() * sizeof(GeometryPageRuntime));
			for (const GeometryPageRuntime& page : asset.pages)
			{
				snapshot.estimatedGeometryBytes += page.byteSize;
				switch (page.state)
				{
				case GeometryPageState::Resident:
				case GeometryPageState::Pinned:
				case GeometryPageState::PendingRetirement:
					++snapshot.residentPages;
					++contributor.residentPages;
					contributor.residentBytes += page.byteSize;
					if (page.state == GeometryPageState::PendingRetirement)
					{
						++snapshot.pageResidency.retiringPages;
						snapshot.pageResidency.retiringBytes += page.byteSize;
						contributor.retiringBytes += page.byteSize;
					}
					if (page.fallbackPage || page.pinnedFallback || page.state == GeometryPageState::Pinned)
					{
						++snapshot.pinnedFallbackPages;
						++snapshot.pageResidency.pinnedFallbackPages;
						++contributor.pinnedFallbackPages;
						snapshot.pageResidency.pinnedFallbackBytes += page.byteSize;
						snapshot.pageResidency.residentFallbackBytes += page.byteSize;
						contributor.pinnedFallbackBytes += page.byteSize;
						contributor.residentFallbackBytes += page.byteSize;
					}
					if (!page.fallbackPage && !page.pinnedFallback && page.state != GeometryPageState::Pinned)
					{
						snapshot.pageResidency.residentHighDetailBytes += page.byteSize;
						contributor.reclaimableBytes += page.byteSize;
						contributor.residentHighDetailBytes += page.byteSize;
					}
					break;
				case GeometryPageState::Requested:
				case GeometryPageState::Loading:
					++snapshot.requestedPages;
					snapshot.pageResidency.requestedBytes += page.byteSize;
					contributor.requestedBytes += page.byteSize;
					break;
				case GeometryPageState::PendingIO:
					++snapshot.requestedPages;
					++snapshot.pageResidency.pendingIoPages;
					snapshot.pageResidency.requestedBytes += page.byteSize;
					snapshot.pageResidency.pendingIoBytes += page.byteSize;
					contributor.requestedBytes += page.byteSize;
					break;
				case GeometryPageState::PendingUpload:
					++snapshot.requestedPages;
					++snapshot.pageResidency.pendingUploadPages;
					snapshot.pageResidency.requestedBytes += page.byteSize;
					snapshot.pageResidency.pendingUploadBytes += page.byteSize;
					contributor.requestedBytes += page.byteSize;
					break;
				case GeometryPageState::Evicting:
					++snapshot.evictingPages;
					++snapshot.pageResidency.evictingPages;
					++snapshot.missingPages;
					++contributor.missingPages;
					snapshot.pageResidency.evictingBytes += page.byteSize;
					break;
				case GeometryPageState::NotResident:
					++snapshot.missingPages;
					++contributor.missingPages;
					break;
				case GeometryPageState::Failed:
					++snapshot.failedPages;
					++snapshot.missingPages;
					snapshot.pageResidency.failedBytes += page.byteSize;
					if (!page.failureReason.empty())
					{
						++snapshot.pageResidency.failedPageReasons;
					}
					++contributor.missingPages;
					break;
				case GeometryPageState::Evicted:
				case GeometryPageState::Retired:
					++snapshot.evictedPages;
					++snapshot.missingPages;
					snapshot.pageResidency.evictedBytes += page.byteSize;
					++contributor.missingPages;
					break;
				}
			}
			if (!asset.pages.empty() && snapshot.missingPages > 0u)
			{
				snapshot.warnings.push_back("Virtual geometry has missing or failed high-detail pages; renderer must use resident fallback detail until streaming catches up.");
			}

			if (asset.detailLevels.empty())
			{
				++snapshot.poppingRiskWarnings;
				snapshot.warnings.push_back("Virtual geometry asset has no detail levels; renderer must use a conservative fallback.");
				if (contributor.residentBytes > 0u || contributor.reclaimableBytes > 0u || contributor.missingPages > 0u)
				{
					snapshot.budgetContributors.push_back(contributor);
				}
				continue;
			}

			snapshot.selectedMeshlets += asset.detailLevels.front().meshletCount;
			snapshot.dispatchedMeshlets += asset.detailLevels.front().meshletCount;
			if (asset.detailLevels.size() > 1u)
			{
				snapshot.lodTransitionCandidates += static_cast<std::uint32_t>(asset.detailLevels.size() - 1u);
			}
			snapshot.clusterSelection.selectedClusters += static_cast<std::uint32_t>(asset.clusters.size());

			if (quality.maxLodDrop > 1u && asset.detailLevels.size() > 2u)
			{
				++snapshot.poppingRiskWarnings;
				snapshot.warnings.push_back("Virtual geometry max LOD drop permits skipping intermediate detail levels; popping risk increases during fast camera motion.");
			}

			float previousTargetRatio = 2.0f;
			std::uint32_t previousTriangleCount = std::numeric_limits<std::uint32_t>::max();
			for (const VirtualGeometryDetailRuntime& detail : asset.detailLevels)
			{
				if (detail.lodIndex >= snapshot.selectedLodHistogram.size())
				{
					snapshot.selectedLodHistogram.resize(static_cast<std::size_t>(detail.lodIndex) + 1u, 0u);
				}
				snapshot.selectedLodHistogram[detail.lodIndex] += detail.meshletCount;
				snapshot.maxGeometricError = std::max(snapshot.maxGeometricError, detail.geometricError);

				const float effectiveError = detail.geometricError * quality.lodErrorScale;
				if (!std::isfinite(detail.geometricError)
					|| !std::isfinite(detail.targetRatio)
					|| detail.targetRatio <= 0.0f
					|| detail.targetRatio > previousTargetRatio
					|| detail.triangleCount >= previousTriangleCount
					|| effectiveError > 0.1f)
				{
					++snapshot.poppingRiskWarnings;
					snapshot.warnings.push_back("Virtual geometry LOD transition has popping-risk metadata; inspect geometric error, target ratio, and section fallback.");
				}

				previousTargetRatio = detail.targetRatio;
				previousTriangleCount = detail.triangleCount;
			}

			if (contributor.residentBytes > 0u || contributor.reclaimableBytes > 0u || contributor.missingPages > 0u)
			{
				snapshot.budgetContributors.push_back(contributor);
			}
		}

		if (snapshot.geometryBudgetBytes > 0u && snapshot.estimatedGeometryBytes > snapshot.geometryBudgetBytes)
		{
			snapshot.budgetPressureBytes = snapshot.estimatedGeometryBytes - snapshot.geometryBudgetBytes;
			snapshot.warnings.push_back("Virtual geometry estimated memory exceeds the configured geometry budget.");
		}

		std::sort(snapshot.budgetContributors.begin(), snapshot.budgetContributors.end(), [](const VirtualGeometryBudgetContributor& lhs, const VirtualGeometryBudgetContributor& rhs) {
			if (lhs.reclaimableBytes != rhs.reclaimableBytes)
			{
				return lhs.reclaimableBytes > rhs.reclaimableBytes;
			}
			if (lhs.residentBytes != rhs.residentBytes)
			{
				return lhs.residentBytes > rhs.residentBytes;
			}
			return TerraEngine::ToHexString(lhs.assetId) < TerraEngine::ToHexString(rhs.assetId);
		});

		return snapshot;
	}

	VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(const VirtualGeometryViewVisibilitySnapshot& visibility)
	{
		VirtualGeometryViewDiagnosticsSnapshot snapshot;
		snapshot.viewType = visibility.viewType;
		snapshot.viewId = visibility.outputId;
		snapshot.frameId = visibility.frameId;
		snapshot.active = visibility.valid;
		snapshot.debugVisualizationMode = NormalizeVirtualGeometryDebugVisualizationMode(visibility.debugVisualizationMode);
		snapshot.counterSource = visibility.source;
		snapshot.counterPreparedFrameId = visibility.counters.preparedFrameId;
		snapshot.counterReadbackFrameId = visibility.counters.readbackFrameId;
		snapshot.counterReadbackLatencyFrames = visibility.counters.readbackLatencyFrames;
		snapshot.counterUnavailableReason = visibility.counters.unavailableReason;
		snapshot.counterToleranceMeshlets = visibility.counters.toleranceMeshlets;
		snapshot.counterTolerancePercent = visibility.counters.tolerancePercent;
		snapshot.countersAuthoritative = visibility.counters.authoritative;
		snapshot.sourceMeshlets = visibility.counters.sourceMeshlets;
		snapshot.selectedMeshlets = visibility.counters.selectedMeshlets;
		snapshot.submittedMeshlets = visibility.clusterBatches.empty() ? visibility.counters.selectedMeshlets : 0u;
		snapshot.dispatchedMeshlets = visibility.counters.dispatchedMeshlets;
		snapshot.culledMeshlets = visibility.counters.culledMeshlets;
		snapshot.culledByFrustum = visibility.counters.culledByFrustum;
		snapshot.culledByCone = visibility.counters.culledByCone;
		snapshot.culledByHiZ = visibility.counters.culledByHiZ;
		snapshot.failOpenMeshlets = visibility.counters.fallbackMeshlets;
		snapshot.requestedPages = visibility.counters.requestedPages;
		snapshot.missingPages = visibility.counters.missingPages;
		snapshot.clusterPacketCount = static_cast<std::uint32_t>(visibility.clusterPackets.size());
		for(const VirtualGeometryClusterPacket& packet : visibility.clusterPackets)
		{
			switch(static_cast<VirtualGeometryRasterRoute>(packet.rasterRoute))
			{
			case VirtualGeometryRasterRoute::Hardware:
				++snapshot.hardwareRasterPacketCount;
				break;
			case VirtualGeometryRasterRoute::Compute:
				++snapshot.computeRasterPacketCount;
				break;
			case VirtualGeometryRasterRoute::Indexed:
			case VirtualGeometryRasterRoute::Fallback:
				// Fallback is counted with Indexed because Vulkan recording
				// normalizes unsupported work to the visible indexed path.
				++snapshot.indexedRasterPacketCount;
				break;
			}
		}
		snapshot.clusterSelection.selectedClusters = static_cast<std::uint32_t>(visibility.selectedClusters.size());
		snapshot.clusterSelection.fallbackClusters = visibility.counters.fallbackSubstitutions;
		for (const ClusterWorkBatch& batch : visibility.clusterBatches)
		{
			++snapshot.submittedBatchCount;
			snapshot.submittedMeshlets += batch.submittedMeshlets;
			if (batch.fallbackMode == VirtualGeometryBatchFallbackMode::SinglePacketDraw)
			{
				++snapshot.singlePacketFallbackBatchCount;
			}
		}
		for (const CompactedVisibilitySet& compaction : visibility.compactionResults)
		{
			snapshot.candidateClusterCount += compaction.candidatePackets;
			snapshot.compactedClusterCount += compaction.visiblePackets;
			snapshot.rejectedClusterCount += compaction.rejectedByFrustum
				+ compaction.rejectedByHiZ
				+ compaction.rejectedByResidency
				+ compaction.rejectedByCone;
			snapshot.failOpenClusterCount += compaction.failOpenPackets;
			snapshot.culledByFrustum += compaction.rejectedByFrustum;
			snapshot.culledByHiZ += compaction.rejectedByHiZ;
			snapshot.culledByCone += compaction.rejectedByCone;
			snapshot.culledByResidency += compaction.rejectedByResidency;
			snapshot.submittedBatchCount = std::max(snapshot.submittedBatchCount, compaction.submittedBatches);
		}
		snapshot.gpuPacketRecordCount = static_cast<std::uint32_t>(visibility.GpuPacketRecords().size());
		snapshot.gpuBatchDescriptorCount = static_cast<std::uint32_t>(visibility.GpuBatchDescriptors().size());
		snapshot.gpuPacketFrameSetCount = static_cast<std::uint32_t>(visibility.GpuPacketFrameSets().size());
		snapshot.gpuCompactionDispatchCount = static_cast<std::uint32_t>(visibility.GpuCompactionDispatches().size());
		snapshot.generatedSubmissionCount = static_cast<std::uint32_t>(visibility.GeneratedSubmissions().size());
		snapshot.gpuCounterReadbackCount = static_cast<std::uint32_t>(visibility.GpuCounterReadbacks().size());
		snapshot.gpuFallbackRecordCount = static_cast<std::uint32_t>(visibility.GpuFallbackRecords().size());
		snapshot.residentPagePayloadCount = static_cast<std::uint32_t>(visibility.residentPagePayloads.size());
		snapshot.emittedPageSourceCount = static_cast<std::uint32_t>(visibility.emittedPageSources.size());
		snapshot.pageRequestCount = static_cast<std::uint32_t>(visibility.pageRequests.size());
		snapshot.pageRetirementCount = static_cast<std::uint32_t>(visibility.pageRetirements.size());
		for (const GpuVirtualGeometryPacketFrameSet& frameSet : visibility.GpuPacketFrameSets())
		{
			switch (frameSet.validity)
			{
			case VirtualGeometryGpuFrameSetValidity::Valid:
				++snapshot.gpuValidFrameSetCount;
				break;
			case VirtualGeometryGpuFrameSetValidity::Delayed:
				++snapshot.gpuDelayedFrameSetCount;
				break;
			case VirtualGeometryGpuFrameSetValidity::Invalid:
			case VirtualGeometryGpuFrameSetValidity::Unsupported:
				++snapshot.gpuInvalidFrameSetCount;
				break;
			case VirtualGeometryGpuFrameSetValidity::FailOpen:
				++snapshot.gpuFailOpenFrameSetCount;
				break;
			}
			if (!frameSet.fallbackReason.empty() && snapshot.gpuFallbackReason.empty())
			{
				snapshot.gpuFallbackReason = frameSet.fallbackReason;
			}
		}
		for (const GeneratedVirtualGeometrySubmissionRecord& submission : visibility.GeneratedSubmissions())
		{
			switch (submission.validationState)
			{
			case VirtualGeometryGeneratedSubmissionValidationState::Valid:
				++snapshot.validGeneratedSubmissionCount;
				break;
			case VirtualGeometryGeneratedSubmissionValidationState::Delayed:
			case VirtualGeometryGeneratedSubmissionValidationState::Mismatch:
			case VirtualGeometryGeneratedSubmissionValidationState::Overflow:
			case VirtualGeometryGeneratedSubmissionValidationState::Unsupported:
				++snapshot.invalidGeneratedSubmissionCount;
				break;
			case VirtualGeometryGeneratedSubmissionValidationState::FailOpen:
				++snapshot.fallbackGeneratedSubmissionCount;
				break;
			}
			if (!submission.fallbackReason.empty() && snapshot.gpuFallbackReason.empty())
			{
				snapshot.gpuFallbackReason = submission.fallbackReason;
			}
		}
		for (const VirtualGeometryCounterReadbackRecord& readback : visibility.GpuCounterReadbacks())
		{
			snapshot.gpuCounterReadbackLatencyFrames = std::max(snapshot.gpuCounterReadbackLatencyFrames, readback.latencyFrames);
			if (!readback.unavailableReason.empty() && snapshot.counterUnavailableReason.empty())
			{
				snapshot.counterUnavailableReason = readback.unavailableReason;
			}
		}
		for (const VirtualGeometryGpuFallbackRecord& fallback : visibility.GpuFallbackRecords())
		{
			snapshot.gpuFallbackAffectedPackets += fallback.affectedPackets;
			snapshot.gpuFallbackAffectedBatches += fallback.affectedBatches;
			if (!fallback.reason.empty() && snapshot.gpuFallbackReason.empty())
			{
				snapshot.gpuFallbackReason = fallback.reason;
			}
			if (!fallback.diagnostic.empty())
			{
				snapshot.warnings.push_back(fallback.diagnostic);
			}
		}
		if (!snapshot.gpuFallbackReason.empty())
		{
			snapshot.warnings.push_back("Virtual geometry GPU packet path used fallback: " + snapshot.gpuFallbackReason);
		}
		const bool deriveResidencyBytesFromPayloads = visibility.pageResidency.empty();
		for (const VirtualGeometryPagePayloadRecord& payload : visibility.residentPagePayloads)
		{
			const bool embeddedFallbackPayload =
				payload.source == VirtualGeometryPagePayloadSource::EmbeddedMeshPayload
				|| payload.source == VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
			if (payload.source == VirtualGeometryPagePayloadSource::ResidentPagePayload)
			{
				++snapshot.pageSources.highDetailPayloads;
				if (deriveResidencyBytesFromPayloads)
				{
					snapshot.pageResidency.residentHighDetailBytes += payload.payloadByteSize;
				}
			}
			if (payload.source == VirtualGeometryPagePayloadSource::ResidentFallbackPayload || payload.pinnedFallback)
			{
				++snapshot.pageSources.fallbackPayloads;
				snapshot.pageSources.fallbackPayloadBytes += payload.payloadByteSize;
				if (deriveResidencyBytesFromPayloads)
				{
					snapshot.pageResidency.residentFallbackBytes += payload.payloadByteSize;
				}
			}
			if (embeddedFallbackPayload)
			{
				++snapshot.pageSources.embeddedFallbackPayloads;
				++snapshot.embeddedFallbackPayloadCount;
				snapshot.pageSources.embeddedFallbackBytes += payload.payloadByteSize;
				if (deriveResidencyBytesFromPayloads)
				{
					snapshot.pageResidency.embeddedFallbackBytes += payload.payloadByteSize;
					snapshot.pageResidency.owningMeshFallbackBytes += payload.payloadByteSize;
				}
			}
			if (payload.resident || embeddedFallbackPayload)
			{
				++snapshot.pageSources.residentPayloads;
				snapshot.pageSources.residentPayloadBytes += payload.payloadByteSize;
			}
		}
		std::vector<std::pair<std::uint32_t, TerraEngine::UUID>> emittedMaterialPagePairs;
		std::vector<std::uint32_t> emittedPagesWithMultipleMaterials;
		const auto containsPage = [](const std::vector<std::uint32_t>& pages, std::uint32_t pageId) {
			return std::find(pages.begin(), pages.end(), pageId) != pages.end();
		};
		const auto containsMaterialPagePair = [](const std::vector<std::pair<std::uint32_t, TerraEngine::UUID>>& pairs, std::uint32_t pageId, const TerraEngine::UUID& materialId) {
			return std::any_of(pairs.begin(), pairs.end(), [pageId, &materialId](const std::pair<std::uint32_t, TerraEngine::UUID>& pair) {
				return pair.first == pageId && pair.second == materialId;
			});
		};
		for (const VirtualGeometryEmittedPageSourceRecord& emitted : visibility.emittedPageSources)
		{
			if (!containsMaterialPagePair(emittedMaterialPagePairs, emitted.pageId, emitted.materialId))
			{
				const bool pageAlreadyHadAnotherMaterial = std::any_of(
					emittedMaterialPagePairs.begin(),
					emittedMaterialPagePairs.end(),
					[pageId = emitted.pageId, &materialId = emitted.materialId](const std::pair<std::uint32_t, TerraEngine::UUID>& pair) {
						return pair.first == pageId && pair.second != materialId;
					});
				emittedMaterialPagePairs.emplace_back(emitted.pageId, emitted.materialId);
				if (pageAlreadyHadAnotherMaterial && !containsPage(emittedPagesWithMultipleMaterials, emitted.pageId))
				{
					emittedPagesWithMultipleMaterials.push_back(emitted.pageId);
				}
			}
			switch (emitted.emittedSource)
			{
			case VirtualGeometryEmittedPageSource::HighDetailPage:
				++snapshot.pageSources.emittedHighDetailSources;
				break;
			case VirtualGeometryEmittedPageSource::FallbackPage:
				++snapshot.pageSources.emittedFallbackSources;
				if (!emitted.reason.empty())
				{
					snapshot.warnings.push_back("Virtual geometry emitted fallback page coverage: " + emitted.reason);
				}
				break;
			case VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback:
				++snapshot.pageSources.emittedEmbeddedFallbackSources;
				break;
			case VirtualGeometryEmittedPageSource::NoSubmittedWork:
			case VirtualGeometryEmittedPageSource::Invalid:
				++snapshot.pageSources.missingSources;
				break;
			case VirtualGeometryEmittedPageSource::None:
				break;
			}
			if ((emitted.conservativeEmbeddedFallback || emitted.emittedSource == VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback) && !emitted.reason.empty())
			{
				snapshot.warnings.push_back("Virtual geometry emitted conservative embedded fallback: " + emitted.reason);
			}
		}
		snapshot.pageSources.emittedMaterialPagePairs = static_cast<std::uint32_t>(emittedMaterialPagePairs.size());
		snapshot.pageSources.emittedMultiMaterialPages = static_cast<std::uint32_t>(emittedPagesWithMultipleMaterials.size());
		snapshot.pageSources.pageRequests = snapshot.pageRequestCount;
		snapshot.pageSources.pageRetirements = snapshot.pageRetirementCount;
		std::unordered_map<TerraEngine::UUID, std::size_t> budgetContributorIndex;
		const auto contributorForAsset = [&](const TerraEngine::UUID& meshId) -> VirtualGeometryBudgetContributor* {
			if (meshId == TerraEngine::EMPTY_UUID)
			{
				return nullptr;
			}

			const auto found = budgetContributorIndex.find(meshId);
			if (found != budgetContributorIndex.end())
			{
				return &snapshot.budgetContributors[found->second];
			}

			// Page residency records are already view-local immutable data, so
			// folding them into a per-mesh contributor here keeps editor tools
			// from having to inspect live renderer resources.
			const std::size_t contributorIndex = snapshot.budgetContributors.size();
			VirtualGeometryBudgetContributor contributor;
			contributor.assetId = meshId;
			contributor.viewType = visibility.viewType;
			snapshot.budgetContributors.push_back(contributor);
			budgetContributorIndex.emplace(meshId, contributorIndex);
			return &snapshot.budgetContributors[contributorIndex];
		};

		if (visibility.pageResidency.empty())
		{
			for (const VirtualGeometryPagePayloadRecord& payload : visibility.residentPagePayloads)
			{
				if (!payload.resident && payload.source != VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback)
				{
					continue;
				}

				VirtualGeometryBudgetContributor* contributor = contributorForAsset(payload.meshId);
				if (contributor == nullptr)
				{
					continue;
				}

				const std::uint64_t payloadBytes = payload.payloadByteSize;
				++snapshot.residentPages;
				++contributor->residentPages;
				contributor->residentBytes += payloadBytes;
				if (payload.pinnedFallback || payload.source == VirtualGeometryPagePayloadSource::ResidentFallbackPayload)
				{
					++snapshot.pinnedFallbackPages;
					++snapshot.pageResidency.pinnedFallbackPages;
					snapshot.pageResidency.pinnedFallbackBytes += payloadBytes;
					++contributor->pinnedFallbackPages;
					contributor->pinnedFallbackBytes += payloadBytes;
					contributor->residentFallbackBytes += payloadBytes;
				}
				else
				{
					contributor->reclaimableBytes += payloadBytes;
					contributor->residentHighDetailBytes += payloadBytes;
				}
				if (payload.source == VirtualGeometryPagePayloadSource::EmbeddedMeshPayload
					|| payload.source == VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback)
				{
					contributor->embeddedFallbackBytes += payloadBytes;
					contributor->owningMeshFallbackBytes += payloadBytes;
				}
			}
		}

		for (const PageResidencyRecord& page : visibility.pageResidency)
		{
			VirtualGeometryBudgetContributor* contributor = contributorForAsset(page.meshId);
			const std::uint64_t pageBytes = ResolvePageResidencyBytes(page);
			switch (page.state)
			{
			case GeometryPageState::Resident:
				++snapshot.residentPages;
				if (contributor != nullptr)
				{
					++contributor->residentPages;
					contributor->residentBytes += pageBytes;
					if (page.isFallback || page.protectedFallback)
					{
						++contributor->pinnedFallbackPages;
						contributor->pinnedFallbackBytes += pageBytes;
						contributor->residentFallbackBytes += pageBytes;
						snapshot.pageResidency.residentFallbackBytes += pageBytes;
					}
					else
					{
						contributor->reclaimableBytes += pageBytes;
						contributor->residentHighDetailBytes += pageBytes;
						snapshot.pageResidency.residentHighDetailBytes += pageBytes;
					}
				}
				break;
			case GeometryPageState::Pinned:
				++snapshot.residentPages;
				++snapshot.pinnedFallbackPages;
				++snapshot.pageResidency.pinnedFallbackPages;
				snapshot.pageResidency.pinnedFallbackBytes += pageBytes;
				snapshot.pageResidency.residentFallbackBytes += pageBytes;
				if (contributor != nullptr)
				{
					++contributor->residentPages;
					++contributor->pinnedFallbackPages;
					contributor->residentBytes += pageBytes;
					contributor->pinnedFallbackBytes += pageBytes;
					contributor->residentFallbackBytes += pageBytes;
				}
				break;
			case GeometryPageState::PendingRetirement:
				++snapshot.residentPages;
				++snapshot.pageResidency.retiringPages;
				snapshot.pageResidency.retiringBytes += pageBytes;
				if (contributor != nullptr)
				{
					++contributor->residentPages;
					contributor->residentBytes += pageBytes;
					contributor->reclaimableBytes += pageBytes;
					contributor->retiringBytes += pageBytes;
				}
				break;
			case GeometryPageState::Requested:
			case GeometryPageState::Loading:
				++snapshot.requestedPages;
				snapshot.pageResidency.requestedBytes += pageBytes;
				if (contributor != nullptr)
				{
					contributor->requestedBytes += pageBytes;
				}
				break;
			case GeometryPageState::PendingIO:
				++snapshot.requestedPages;
				++snapshot.pageResidency.pendingIoPages;
				snapshot.pageResidency.pendingIoBytes += pageBytes;
				if (contributor != nullptr)
				{
					contributor->requestedBytes += pageBytes;
				}
				break;
			case GeometryPageState::PendingUpload:
				++snapshot.requestedPages;
				++snapshot.pageResidency.pendingUploadPages;
				snapshot.pageResidency.pendingUploadBytes += pageBytes;
				if (contributor != nullptr)
				{
					contributor->requestedBytes += pageBytes;
				}
				break;
			case GeometryPageState::Failed:
				++snapshot.failedPages;
				snapshot.pageResidency.failedBytes += pageBytes;
				if (contributor != nullptr)
				{
					++contributor->missingPages;
				}
				if (!page.failureReason.empty())
				{
					++snapshot.pageResidency.failedPageReasons;
					snapshot.warnings.push_back("Virtual geometry page failed and requires fallback coverage: " + page.failureReason);
				}
				break;
			case GeometryPageState::Evicting:
				++snapshot.evictingPages;
				++snapshot.pageResidency.evictingPages;
				snapshot.pageResidency.evictingBytes += pageBytes;
				if (contributor != nullptr)
				{
					++contributor->missingPages;
				}
				break;
			case GeometryPageState::Evicted:
			case GeometryPageState::Retired:
				++snapshot.evictedPages;
				snapshot.pageResidency.evictedBytes += pageBytes;
				if (contributor != nullptr)
				{
					++contributor->missingPages;
				}
				break;
			case GeometryPageState::NotResident:
				++snapshot.missingPages;
				if (contributor != nullptr)
				{
					++contributor->missingPages;
				}
				break;
			}
		}
		std::sort(snapshot.budgetContributors.begin(), snapshot.budgetContributors.end(), [](const VirtualGeometryBudgetContributor& lhs, const VirtualGeometryBudgetContributor& rhs) {
			if (lhs.reclaimableBytes != rhs.reclaimableBytes)
			{
				return lhs.reclaimableBytes > rhs.reclaimableBytes;
			}
			if (lhs.residentBytes != rhs.residentBytes)
			{
				return lhs.residentBytes > rhs.residentBytes;
			}
			return TerraEngine::ToHexString(lhs.assetId) < TerraEngine::ToHexString(rhs.assetId);
		});
		snapshot.lodTransitionStates = static_cast<std::uint32_t>(visibility.lodTransitionStates.size());
		for (const VirtualGeometryLODTransitionState& transitionState : visibility.lodTransitionStates)
		{
			if (transitionState.heldPreviousSelection)
			{
				++snapshot.lodTransitionHeldSelections;
			}
			if (!transitionState.heldPreviousSelection && transitionState.valid)
			{
				++snapshot.lodTransitionAcceptedSelections;
			}
			if (transitionState.resetThisFrame)
			{
				++snapshot.lodTransitionResets;
			}
			snapshot.lodTransitionHysteresisPixels = transitionState.hysteresisBandPixels;
		}
		snapshot.hiZState = visibility.hiZValidationState.state;
		for (const HiZRegressionSnapshot& hiZRegression : visibility.hiZRegressionSnapshots)
		{
			// Hi-Z baseline records are copied from the current view snapshot.
			// A mismatch is a diagnostics problem for tests and tools; it must
			// not become a hidden quality tier or a render-state mutation.
			snapshot.hiZBaselineMatched = snapshot.hiZBaselineMatched && hiZRegression.baselineMatched;
			snapshot.hiZFalseOcclusionRiskWarnings += hiZRegression.baselineMatched ? 0u : 1u;
			if (!hiZRegression.baselineMatched)
			{
				const std::string reason = hiZRegression.reason.empty()
					? std::string("current view Hi-Z baseline mismatch")
					: hiZRegression.reason;
				snapshot.warnings.push_back("Virtual geometry Hi-Z baseline mismatch: " + reason);
			}
		}
		snapshot.hiZPyramidSize = visibility.hiZValidationState.pyramidSize;
		snapshot.hiZRenderSize = visibility.hiZValidationState.renderSize;
		snapshot.hiZCameraSignature = visibility.hiZValidationState.cameraSignature;
		snapshot.hiZDepthSceneSignature = visibility.hiZValidationState.depthSceneSignature;
		snapshot.hiZWarmupFrames = visibility.hiZValidationState.warmupFrames;
		snapshot.hiZActive = visibility.hiZValidationState.state == VirtualGeometryHiZState::Active;
		snapshot.hiZHistoryValid = snapshot.hiZActive && visibility.hiZValidationState.warmupFrames > 0u;
		snapshot.hiZInvalidReason = snapshot.hiZActive ? std::string() : visibility.hiZValidationState.reason;
		snapshot.hiZValidation.ownerViewType = visibility.viewType;
		snapshot.hiZValidation.sameViewHistory = snapshot.hiZActive;
		snapshot.hiZValidation.sameTargetHistory = snapshot.hiZActive
			&& visibility.hiZValidationState.pyramidSize == visibility.hiZValidationState.renderSize;
		snapshot.hiZValidation.projectionMatches = snapshot.hiZActive && visibility.hiZValidationState.cameraSignature != 0u;
		snapshot.hiZValidation.failOpen = visibility.hiZValidationState.failOpen
			|| visibility.hiZValidationState.state != VirtualGeometryHiZState::Active;
		snapshot.hiZValidation.inactiveReason = snapshot.hiZInvalidReason;
		if (snapshot.hiZValidation.failOpen && !snapshot.hiZInvalidReason.empty())
		{
			++snapshot.hiZInactiveFrames;
			snapshot.warnings.push_back("Virtual geometry Hi-Z is " + std::string(ToString(snapshot.hiZState)) + ": " + snapshot.hiZInvalidReason);
			const bool pageSourceDiagnosticsPresent =
				snapshot.residentPagePayloadCount > 0u
				|| snapshot.emittedPageSourceCount > 0u
				|| snapshot.pageRequestCount > 0u
				|| snapshot.pageSources.missingSources > 0u;
			if (pageSourceDiagnosticsPresent)
			{
				// Keep the combined warning textual so existing tools can show
				// it without a schema migration, while still exposing the typed
				// counters above for automation and tests.
				snapshot.warnings.push_back("Virtual geometry page-source diagnostics observed while Hi-Z failed open: " + snapshot.hiZInvalidReason);
			}
		}
		if (visibility.source == VirtualGeometryCounterSource::DelayedGPUReadback
			|| visibility.source == VirtualGeometryCounterSource::Unavailable)
		{
			const std::string reason = visibility.counters.unavailableReason.empty()
				? std::string("no reason was recorded")
				: visibility.counters.unavailableReason;
			snapshot.warnings.push_back("Virtual geometry counters are not authoritative GPU readback for this frame: " + reason);
		}
		if (visibility.source == VirtualGeometryCounterSource::Unsupported)
		{
			const std::string reason = visibility.counters.unavailableReason.empty()
				? std::string("counter readback unsupported")
				: visibility.counters.unavailableReason;
			snapshot.warnings.push_back("Virtual geometry counters are unsupported: " + reason);
		}
		const std::size_t debugSampleCount = std::min<std::size_t>(
			visibility.occlusionDebugSamples.size(),
			kVirtualGeometryOcclusionDebugSampleCapacity);
		snapshot.occlusionDebugSamples.insert(
			snapshot.occlusionDebugSamples.end(),
			visibility.occlusionDebugSamples.begin(),
			visibility.occlusionDebugSamples.begin() + static_cast<std::ptrdiff_t>(debugSampleCount));
		for (const VirtualGeometrySelectedClusterRecord& cluster : visibility.selectedClusters)
		{
			if (snapshot.selectedLodHistogram.size() <= cluster.lodIndex)
			{
				snapshot.selectedLodHistogram.resize(static_cast<std::size_t>(cluster.lodIndex) + 1u, 0u);
			}
			++snapshot.selectedLodHistogram[cluster.lodIndex];
		}
		snapshot.shadowViewCount = static_cast<std::uint32_t>(visibility.shadowVisibilitySelections.size());
		snapshot.shadowVisibilitySelections = visibility.shadowVisibilitySelections;
		if (snapshot.shadowVisibilitySelections.empty() && visibility.viewType == RenderViewType::Shadow)
		{
			ShadowVisibilitySelection shadowSelection{};
			shadowSelection.shadowViewId = visibility.outputId;
			shadowSelection.frameIndex = visibility.frameId;
			shadowSelection.candidatePackets = static_cast<std::uint32_t>(visibility.clusterPackets.empty() ? visibility.drawRanges.size() : visibility.clusterPackets.size());
			shadowSelection.selectedPackets = shadowSelection.candidatePackets;
			for (const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw : visibility.drawRanges)
			{
				if (!draw.range.virtualGeometryActive)
				{
					continue;
				}
				shadowSelection.selectedMeshlets += draw.range.meshletCount;
				shadowSelection.dispatchedMeshlets += draw.range.meshletCount;
				if (draw.range.failOpen)
				{
					shadowSelection.fallbackReasons.push_back(draw.range.fallbackReason.empty()
						? "shadow virtual geometry failed open"
						: draw.range.fallbackReason);
				}
			}
			snapshot.shadowVisibilitySelections.push_back(std::move(shadowSelection));
			snapshot.shadowViewCount = static_cast<std::uint32_t>(snapshot.shadowVisibilitySelections.size());
		}
		snapshot.virtualGeometryPerformanceGates = visibility.performanceGates;
		std::vector<PerformanceGateResult> generatedGates = BuildVirtualGeometryPerformanceGates(snapshot);
		snapshot.virtualGeometryPerformanceGates.insert(
			snapshot.virtualGeometryPerformanceGates.end(),
			std::make_move_iterator(generatedGates.begin()),
			std::make_move_iterator(generatedGates.end()));
		if (!snapshot.hiZBaselineMatched)
		{
			snapshot.warnings.push_back("Virtual geometry Hi-Z regression baseline did not match the accepted working behavior.");
		}
		return snapshot;
	}

	VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryHiZDiagnostics(const VirtualGeometryHiZDiagnosticsInput& input)
	{
		VirtualGeometryViewDiagnosticsSnapshot snapshot;
		const bool hiZInactive = !input.hiZActive;
		snapshot.viewType = input.viewType;
		snapshot.viewId = 0u;
		snapshot.frameId = input.frameId;
		snapshot.active = input.qualityRequested;
		snapshot.hiZActive = input.hiZActive;
		snapshot.hiZHistoryValid = input.historyValid;
		snapshot.hiZInvalidReason = hiZInactive ? input.invalidReason : std::string();
		snapshot.counterSource = VirtualGeometryCounterSource::Prepared;
		snapshot.hiZState = hiZInactive ? VirtualGeometryHiZState::FailOpen : VirtualGeometryHiZState::Active;
		snapshot.hiZValidation.ownerViewType = input.ownerViewType;
		snapshot.hiZValidation.sameViewHistory = input.sameViewHistory;
		snapshot.hiZValidation.sameTargetHistory = input.sameTargetHistory;
		snapshot.hiZValidation.projectionMatches = input.projectionMatches;
		snapshot.hiZValidation.failOpen = hiZInactive;
		snapshot.hiZValidation.inactiveReason = hiZInactive ? input.invalidReason : std::string();
		snapshot.culledByFrustum = input.culledByFrustum;
		snapshot.culledByCone = input.culledByCone;
		snapshot.culledByHiZ = input.culledByHiZ;
		snapshot.culledMeshlets = input.culledByFrustum + input.culledByCone + input.culledByHiZ;
		snapshot.hiZFalseOcclusionRiskWarnings = input.falseOcclusionRiskWarnings;

		if (hiZInactive)
		{
			++snapshot.hiZInactiveFrames;
			snapshot.warnings.push_back("Virtual geometry Hi-Z occlusion is inactive: " + (input.invalidReason.empty() ? std::string("unknown") : input.invalidReason));
		}
		if (input.falseOcclusionRiskWarnings > 0u)
		{
			snapshot.warnings.push_back("Virtual geometry Hi-Z validation reported diagnostic false-occlusion risk metadata.");
		}
		return snapshot;
	}

	void AddVirtualGeometryFailOpenDiagnostic(RenderDiagnosticsSummary& summary, const TerraEngine::UUID& meshId, const VirtualGeometryDrawRange& range)
	{
		if (!range.failOpen)
		{
			return;
		}

		const std::string reason = range.fallbackReason.empty()
			? "no reason was recorded"
			: range.fallbackReason;
		summary.backendFallbacks.push_back(
			"Virtual geometry fail-open for mesh "
			+ TerraEngine::ToHexString(meshId)
			+ ": "
			+ reason
			+ ". The renderer kept the highest-detail visible fallback range.");
	}

	VirtualGeometrySchedulerDiagnostics BuildVirtualGeometrySchedulerDiagnostics(
		std::string owningSystem,
		std::string schedulerPhase,
		std::vector<std::string> readResources,
		std::vector<std::string> writeResources,
		std::string workerSafePreparation,
		std::string protectedBoundary,
		std::string visibilityBoundary)
	{
		// Keep scheduler metadata as plain strings so editor diagnostics,
		// automation responses, and tests can report the same worker/protected
		// boundary contract without depending on scheduler internals.
		VirtualGeometrySchedulerDiagnostics diagnostics;
		diagnostics.owningSystem = std::move(owningSystem);
		diagnostics.schedulerPhase = std::move(schedulerPhase);
		diagnostics.readResources = std::move(readResources);
		diagnostics.writeResources = std::move(writeResources);
		diagnostics.workerSafePreparation = std::move(workerSafePreparation);
		diagnostics.protectedBoundary = std::move(protectedBoundary);
		diagnostics.visibilityBoundary = std::move(visibilityBoundary);
		return diagnostics;
	}

	void ApplyVirtualGeometryDiagnostics(
		RenderDiagnosticsSummary& summary,
		std::vector<VirtualGeometryViewDiagnosticsSnapshot> viewSnapshots,
		std::vector<VirtualGeometrySchedulerDiagnostics> schedulerSnapshots)
	{
		// Diagnostics are copied into the published summary only when callers ask
		// for them. Normal frames can keep virtual geometry counters closed until
		// an editor window, test, or automation request enables collection.
		if (!summary.detailed && viewSnapshots.empty() && schedulerSnapshots.empty())
		{
			return;
		}

		summary.virtualGeometryViews = std::move(viewSnapshots);
		summary.virtualGeometrySchedulers = std::move(schedulerSnapshots);
		summary.virtualGeometryBudgetContributors.clear();
		for (const VirtualGeometryViewDiagnosticsSnapshot& snapshot : summary.virtualGeometryViews)
		{
			summary.virtualGeometryBudgetContributors.insert(
				summary.virtualGeometryBudgetContributors.end(),
				snapshot.budgetContributors.begin(),
				snapshot.budgetContributors.end());
			if (!snapshot.active)
			{
				summary.resourceWarnings.push_back(
					"Virtual geometry inactive for view "
					+ std::to_string(static_cast<int>(snapshot.viewType))
					+ ": "
					+ ToString(snapshot.inactiveReason));
			}
			for (const PerformanceGateResult& gate : snapshot.virtualGeometryPerformanceGates)
			{
				if (gate.status == VirtualGeometryPerformanceGateStatus::Fail)
				{
					summary.resourceWarnings.push_back(
						"Virtual geometry performance gate failed: "
						+ gate.fixtureId
						+ " view "
						+ ToString(gate.viewType)
						+ "#"
						+ std::to_string(gate.viewId)
						+ " source "
						+ ToString(gate.source)
						+ " "
						+ gate.metricName
						+ " expected "
						+ std::to_string(gate.threshold)
						+ " actual "
						+ std::to_string(gate.actual)
						+ ". "
						+ gate.diagnostic);
				}
			}
		}
	}

	VirtualGeometryGpuTimingSnapshot BuildVirtualGeometryGpuTimingSnapshot(
		const VirtualGeometryMeasurementRecord& measurement,
		const VirtualGeometryFrameViewIdentity& publicationIdentity)
	{
		VirtualGeometryGpuTimingSnapshot snapshot{};
		snapshot.stage = measurement.stage;
		snapshot.identity = measurement.identity;
		snapshot.queryIdentity = measurement.queryIdentity;
		snapshot.executionEvidenceId = measurement.executionEvidenceId;
		snapshot.reason = measurement.rejectionReason;
		if(publicationIdentity.frameId >= measurement.identity.frameId) {
			snapshot.latencyFrames = static_cast<std::uint32_t>(
				publicationIdentity.frameId - measurement.identity.frameId);
		}

		// A query result belongs to exactly one immutable renderer generation. Do
		// not re-label an older result as current merely because its slot completed.
		if(measurement.identity != publicationIdentity) {
			snapshot.availability = VirtualGeometryMeasurementAvailability::Invalid;
			snapshot.reason = "timing identity does not match the published frame, view, and renderer generation";
			return snapshot;
		}

		snapshot.availability = measurement.availability;
		if(measurement.availability != VirtualGeometryMeasurementAvailability::Measured) {
			return snapshot;
		}
		if(!measurement.HasAuthoritativeDuration()) {
			snapshot.availability = VirtualGeometryMeasurementAvailability::Invalid;
			snapshot.reason = "measured timing lacks a completed query or execution evidence ID";
			return snapshot;
		}

		// Zero remains a valid measured duration here because the query and command
		// evidence have already proven that the backend completed the named pass.
		snapshot.durationNanoseconds = measurement.durationNanoseconds;
		return snapshot;
	}

	void ApplyVirtualGeometryGpuTimingDiagnostics(
		RenderDiagnosticsSummary& summary,
		std::vector<VirtualGeometryMeasurementRecord> measurements,
		const VirtualGeometryFrameViewIdentity& publicationIdentity)
	{
		VirtualGeometryCapabilityDiagnosticsSnapshot diagnostics =
			summary.virtualGeometryCapabilityDiagnostics;
		diagnostics.publicationIdentity = publicationIdentity;
		diagnostics.measurements = std::move(measurements);
		diagnostics.timingSnapshots.clear();
		diagnostics.timingSnapshots.reserve(diagnostics.measurements.size());
		for(VirtualGeometryMeasurementRecord& measurement : diagnostics.measurements) {
			VirtualGeometryGpuTimingSnapshot snapshot =
				BuildVirtualGeometryGpuTimingSnapshot(measurement, publicationIdentity);
			// Preserve the rejected record for automation, but strip any stale number
			// before publication so a raw delayed value cannot pass a timing gate.
			if(snapshot.availability != VirtualGeometryMeasurementAvailability::Measured) {
				measurement.availability = snapshot.availability;
				measurement.durationNanoseconds.reset();
				measurement.rawTicks.reset();
				measurement.rejectionReason = snapshot.reason;
			}
			diagnostics.timingSnapshots.push_back(std::move(snapshot));
		}
		summary.virtualGeometryCapabilityDiagnostics = std::move(diagnostics);
	}

	void ApplyVirtualGeometryCapabilityDiagnostics(
		RenderDiagnosticsSummary& summary,
		VirtualGeometryCapabilityDiagnosticsSnapshot diagnostics)
	{
		if(diagnostics.schemaVersion.empty()) {
			diagnostics.schemaVersion = "1";
		}
		for(VirtualGeometryCapabilityEvidence& capability : diagnostics.capabilities) {
			if(capability.executed
				&& capability.execution.identity != diagnostics.publicationIdentity) {
				// This is a rejected publication, not a fallback execution. Preserve the
				// support fact but atomically withdraw stale execution/validation claims.
				capability.executed = false;
				capability.validated = false;
				capability.validationArtifactIds.clear();
				capability.fallbackReason = "execution evidence belongs to a stale published generation";
			}
		}

		summary.virtualGeometryCapabilityDiagnostics = std::move(diagnostics);
		ApplyVirtualGeometryGpuTimingDiagnostics(
			summary,
			summary.virtualGeometryCapabilityDiagnostics.measurements,
			summary.virtualGeometryCapabilityDiagnostics.publicationIdentity);
	}

	RenderCaptureSchedulerDiagnostics BuildRenderCaptureSchedulerDiagnostics(
		std::string owningSystem,
		std::string schedulerPhase,
		std::string sourceKind,
		bool backendReadback,
		bool deterministicFallback)
	{
		RenderCaptureSchedulerDiagnostics diagnostics;
		diagnostics.owningSystem = std::move(owningSystem);
		diagnostics.schedulerPhase = std::move(schedulerPhase);
		diagnostics.sourceKind = std::move(sourceKind);
		diagnostics.readResources = {
			"CaptureQueue",
			"GameViewCameraData",
			"ResolvedCameraDescriptor",
			"RenderSettings",
			"RenderOutputState",
			"ResourceReadinessSnapshots"
		};
		diagnostics.writeResources = { "CaptureResultQueue", "CaptureDiagnostics" };
		// Capture requests can validate JSON, resolve camera descriptors, and
		// prepare immutable matrices before the protected GPU handoff. Actual
		// readback remains coordinator-owned because color targets and staging
		// resources are tied to live backend frame ownership.
		diagnostics.workerSafePreparation = "Validate capture request, resolve camera descriptors, clamp target size, and prepare immutable view/projection data.";
		diagnostics.protectedBoundary = "LiveGpuSubmission";
		diagnostics.visibilityBoundary = "Capture response becomes visible only after the protected readback or deterministic fallback completes.";
		diagnostics.backendReadback = backendReadback;
		diagnostics.deterministicFallback = deterministicFallback;
		return diagnostics;
	}

	std::optional<TerraEngine::Profiling::PresentationWaitKind> ClassifyUnavoidablePresentationWait(BackendSubmitStage stage)
	{
		// Only synchronization that is inherent to acquiring, completing, and
		// presenting a frame belongs in the unavoidable stream. Forced idle and
		// swapchain recovery remain ordinary stall diagnostics because callers can
		// often remove or prevent those waits.
		switch (stage)
		{
		case BackendSubmitStage::ImageAcquire:
			return TerraEngine::Profiling::PresentationWaitKind::SwapchainAcquire;
		case BackendSubmitStage::QueueSubmit:
			return TerraEngine::Profiling::PresentationWaitKind::RenderQueueBackpressure;
		case BackendSubmitStage::FenceWait:
			return TerraEngine::Profiling::PresentationWaitKind::GpuCompletion;
		case BackendSubmitStage::Present:
			return TerraEngine::Profiling::PresentationWaitKind::UiComposition;
		default:
			return std::nullopt;
		}
	}

	std::vector<TerraEngine::Profiling::PresentationWaitRecord> BuildPresentationWaitRecords(const BackendFrameBreakdown& breakdown)
	{
		std::vector<TerraEngine::Profiling::PresentationWaitRecord> records;
		records.reserve(breakdown.stageTimings.size());
		for (const BackendStageTiming& timing : breakdown.stageTimings)
		{
			const auto waitKind = ClassifyUnavoidablePresentationWait(timing.stage);
			if (!waitKind.has_value())
			{
				continue;
			}

			TerraEngine::Profiling::PresentationWaitRecord record;
			record.kind = *waitKind;
			record.duration.availability = TerraEngine::Profiling::TelemetryAvailability::Available;
			// Backend timing is currently measured in milliseconds. Convert once at
			// the diagnostics boundary so profiler streams retain integer nanoseconds.
			record.duration.nanoseconds = static_cast<std::uint64_t>(std::llround(
				static_cast<double>(std::max(0.0f, timing.milliseconds)) * 1'000'000.0));
			record.detail = timing.label.empty() ? ToString(timing.stage) : timing.label;
			records.push_back(std::move(record));
		}
		return records;
	}

	void ApplyBackendBreakdownToRenderDiagnostics(RenderDiagnosticsSummary& summary, const BackendFrameBreakdown& breakdown)
	{
		summary.backendFrameBreakdown = breakdown;
		summary.visibleFrameMilliseconds = SumVisibleBackendFrameCost(summary.backendFrameBreakdown);
		summary.queuedBackgroundMilliseconds = SumQueuedBackendFrameCost(summary.backendFrameBreakdown);
		summary.backendStalls = CollectBackendStallDiagnostics(summary.backendFrameBreakdown, 0.25f);
		summary.viewSubmitWork = summary.backendFrameBreakdown.viewSubmitWork;
		summary.presentationWaits = BuildPresentationWaitRecords(summary.backendFrameBreakdown);
		summary.recentRendererPhases = BuildRecentRendererPhaseRing(summary.backendFrameBreakdown);
		if (summary.detailed && !summary.backendFrameBreakdown.stageTimings.empty())
		{
			const float coverage = CalculateBackendStageAttributionPercent(summary.backendFrameBreakdown);
			if (coverage > 0.0f)
			{
				RenderPerformanceGateResult gate;
				gate.workloadName = "backend-stage-attribution";
				gate.baselineValue = 95.0f;
				gate.latestValue = coverage;
				gate.thresholdPercent = 0.0f;
				gate.passed = coverage >= 95.0f;
				summary.performanceGates.push_back(gate);
			}
		}
		for (const BackendStallDiagnostic& stall : summary.backendStalls)
		{
			if (stall.severity != BackendStallSeverity::Info)
			{
				summary.backendFallbacks.push_back(
					std::string("Backend stall ")
					+ ToString(stall.waitType)
					+ " in "
					+ ToString(stall.owningStage)
					+ ": "
					+ stall.recommendedAction);
			}
		}
		for (const ViewSubmitWorkItem& item : summary.viewSubmitWork)
		{
			if (item.reuseDecision != ViewSubmitReuseDecision::Unknown && item.reuseDecision != ViewSubmitReuseDecision::Reused)
			{
				summary.backendFallbacks.push_back(
					std::string("View submit ")
					+ ToString(item.reuseDecision)
					+ ": "
					+ item.reuseReason);
			}
		}
	}

	void ApplyRendererRecoveryDiagnostics(RenderDiagnosticsSummary& summary, RendererRecoverySnapshot snapshot)
	{
		const bool recoveryActive = IsRendererRecoveryActive(snapshot.state);
		// Keep both the compact snapshot and expanded diagnostic form together.
		// Tooling can show the compact fields in tables and still drill into
		// incident/session details when a renderer failure is under inspection.
		summary.rendererRecovery = snapshot;
		summary.rendererRecoveryDiagnostics = BuildRecoveryDiagnosticSnapshot(
			summary.rendererRecovery,
			summary.recentRendererPhases);
		if (!recoveryActive)
		{
			return;
		}

		summary.degradedWork.push_back(
			std::string("Renderer recovery state: ")
			+ ToString(summary.rendererRecovery.state)
			+ " (incident "
			+ summary.rendererRecovery.incidentId
			+ ").");
		if (!summary.rendererRecovery.firstFailureResult.empty())
		{
			std::string firstFailure =
				std::string("First renderer failure: ")
				+ ToString(summary.rendererRecovery.firstFailureStage)
				+ " returned "
				+ summary.rendererRecovery.firstFailureResult
				+ ".";
			if (!summary.rendererRecovery.hardwareName.empty())
			{
				firstFailure += " GPU=" + summary.rendererRecovery.hardwareName;
			}
			if (!summary.rendererRecovery.driverVersion.empty())
			{
				firstFailure += " driver=" + summary.rendererRecovery.driverVersion;
			}
			if (!summary.rendererRecovery.apiVersion.empty())
			{
				firstFailure += " api=" + summary.rendererRecovery.apiVersion;
			}
			summary.backendFallbacks.push_back(std::move(firstFailure));
		}
		if (summary.rendererRecovery.duplicateFailureCount > 0u)
		{
			summary.backendFallbacks.push_back(
				"Suppressed "
				+ std::to_string(summary.rendererRecovery.duplicateFailureCount)
				+ " duplicate renderer failures after the first root cause.");
		}
		if (!summary.rendererRecovery.sessionId.empty()
			|| summary.rendererRecovery.finalOutcome != RendererRecoveryOutcome::None
			|| !summary.rendererRecovery.fatalDialogReportId.empty())
		{
			std::string recoveryResult =
				std::string("Renderer recovery outcome=")
				+ ToString(summary.rendererRecovery.finalOutcome)
				+ " action="
				+ ToString(summary.rendererRecovery.selectedAction)
				+ " session="
				+ summary.rendererRecovery.sessionId;
			if (!summary.rendererRecovery.fatalDialogReportId.empty())
			{
				recoveryResult += " fatalReport=" + summary.rendererRecovery.fatalDialogReportId;
			}
			if (!summary.rendererRecovery.crashDumpPath.empty())
			{
				recoveryResult += " crashDump=" + summary.rendererRecovery.crashDumpPath;
			}
			if (!summary.rendererRecovery.crashDumpStatus.empty())
			{
				recoveryResult += " crashDumpStatus=" + summary.rendererRecovery.crashDumpStatus;
			}
			summary.backendFallbacks.push_back(std::move(recoveryResult));
		}
	}

	bool RenderDiagnosticsRequested()
	{
		return g_renderDiagnosticsRequested.load(std::memory_order_relaxed);
	}

	void SetRenderDiagnosticsRequested(bool requested)
	{
		g_renderDiagnosticsRequested.store(requested, std::memory_order_relaxed);
		if (!requested)
		{
			ClearPublishedRenderDiagnostics();
		}
	}

	void ClearPublishedRenderDiagnostics()
	{
		std::lock_guard<std::mutex> lock(g_renderDiagnosticsMutex);
		g_latestRenderDiagnostics = {};
	}

	void PublishRenderDiagnostics(RenderDiagnosticsSummary summary)
	{
		if (!RenderDiagnosticsRequested())
		{
			return;
		}
		std::lock_guard<std::mutex> lock(g_renderDiagnosticsMutex);
		g_latestRenderDiagnostics = std::move(summary);
	}

	RenderDiagnosticsSummary LatestRenderDiagnostics()
	{
		std::lock_guard<std::mutex> lock(g_renderDiagnosticsMutex);
		return g_latestRenderDiagnostics;
	}

	std::vector<VirtualGeometryViewDiagnosticsSnapshot> LatestVirtualGeometryDiagnosticsSnapshots()
	{
		std::lock_guard<std::mutex> lock(g_renderDiagnosticsMutex);
		return g_latestRenderDiagnostics.virtualGeometryViews;
	}

	void PublishBackendResourceResidencySnapshot(BackendResourceResidencySnapshot snapshot)
	{
		std::lock_guard<std::mutex> lock(g_backendResourceResidencyMutex);
		g_latestBackendResourceResidency = std::move(snapshot);
	}

	BackendResourceResidencySnapshot LatestBackendResourceResidencySnapshot()
	{
		std::lock_guard<std::mutex> lock(g_backendResourceResidencyMutex);
		return g_latestBackendResourceResidency;
	}
}
