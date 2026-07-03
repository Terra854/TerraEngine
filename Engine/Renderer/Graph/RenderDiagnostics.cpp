#include "RenderDiagnostics.h"
#include "RenderFrameSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>

namespace TerraEngine::Renderer {

	namespace {
		std::atomic<bool> g_renderDiagnosticsRequested = false;
		std::mutex g_renderDiagnosticsMutex;
		RenderDiagnosticsSummary g_latestRenderDiagnostics;
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
					++snapshot.residentPages;
					++contributor.residentPages;
					contributor.residentBytes += page.byteSize;
					if (page.fallbackPage || page.pinnedFallback || page.state == GeometryPageState::Pinned)
					{
						++snapshot.pinnedFallbackPages;
						++snapshot.pageResidency.pinnedFallbackPages;
						++contributor.pinnedFallbackPages;
						snapshot.pageResidency.pinnedFallbackBytes += page.byteSize;
						contributor.pinnedFallbackBytes += page.byteSize;
					}
					if (!page.fallbackPage && !page.pinnedFallback && page.state != GeometryPageState::Pinned)
					{
						contributor.reclaimableBytes += page.byteSize;
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
		snapshot.clusterSelection.selectedClusters = static_cast<std::uint32_t>(visibility.selectedClusters.size());
		snapshot.clusterSelection.fallbackClusters = visibility.counters.fallbackSubstitutions;
		for (const ClusterWorkBatch& batch : visibility.clusterBatches)
		{
			++snapshot.submittedBatchCount;
			snapshot.submittedMeshlets += batch.submittedMeshlets;
			if (batch.fallbackMode == VirtualGeometryBatchFallbackMode::LegacyClusterDraw)
			{
				++snapshot.legacyFallbackBatchCount;
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
		snapshot.gpuPacketRecordCount = static_cast<std::uint32_t>(visibility.gpuPacketRecords.size());
		snapshot.gpuBatchDescriptorCount = static_cast<std::uint32_t>(visibility.gpuBatchDescriptors.size());
		snapshot.gpuPacketFrameSetCount = static_cast<std::uint32_t>(visibility.gpuPacketFrameSets.size());
		snapshot.gpuCompactionDispatchCount = static_cast<std::uint32_t>(visibility.gpuCompactionDispatches.size());
		snapshot.generatedSubmissionCount = static_cast<std::uint32_t>(visibility.generatedSubmissions.size());
		snapshot.gpuCounterReadbackCount = static_cast<std::uint32_t>(visibility.gpuCounterReadbacks.size());
		snapshot.gpuFallbackRecordCount = static_cast<std::uint32_t>(visibility.gpuFallbackRecords.size());
		for (const GpuVirtualGeometryPacketFrameSet& frameSet : visibility.gpuPacketFrameSets)
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
		for (const GeneratedVirtualGeometrySubmissionRecord& submission : visibility.generatedSubmissions)
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
		for (const VirtualGeometryCounterReadbackRecord& readback : visibility.gpuCounterReadbacks)
		{
			snapshot.gpuCounterReadbackLatencyFrames = std::max(snapshot.gpuCounterReadbackLatencyFrames, readback.latencyFrames);
			if (!readback.unavailableReason.empty() && snapshot.counterUnavailableReason.empty())
			{
				snapshot.counterUnavailableReason = readback.unavailableReason;
			}
		}
		for (const VirtualGeometryGpuFallbackRecord& fallback : visibility.gpuFallbackRecords)
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
		for (const PageResidencyRecord& page : visibility.pageResidency)
		{
			switch (page.state)
			{
			case GeometryPageState::Resident:
				++snapshot.residentPages;
				break;
			case GeometryPageState::Pinned:
				++snapshot.residentPages;
				++snapshot.pinnedFallbackPages;
				++snapshot.pageResidency.pinnedFallbackPages;
				snapshot.pageResidency.pinnedFallbackBytes += page.byteCost;
				break;
			case GeometryPageState::Requested:
			case GeometryPageState::Loading:
				++snapshot.requestedPages;
				snapshot.pageResidency.requestedBytes += page.byteCost;
				break;
			case GeometryPageState::PendingIO:
				++snapshot.requestedPages;
				++snapshot.pageResidency.pendingIoPages;
				snapshot.pageResidency.pendingIoBytes += page.byteCost;
				break;
			case GeometryPageState::PendingUpload:
				++snapshot.requestedPages;
				++snapshot.pageResidency.pendingUploadPages;
				snapshot.pageResidency.pendingUploadBytes += page.byteCost;
				break;
			case GeometryPageState::Failed:
				++snapshot.failedPages;
				snapshot.pageResidency.failedBytes += page.byteCost;
				if (!page.failureReason.empty())
				{
					++snapshot.pageResidency.failedPageReasons;
				}
				break;
			case GeometryPageState::Evicting:
				++snapshot.evictingPages;
				++snapshot.pageResidency.evictingPages;
				snapshot.pageResidency.evictingBytes += page.byteCost;
				break;
			case GeometryPageState::Evicted:
				++snapshot.evictedPages;
				snapshot.pageResidency.evictedBytes += page.byteCost;
				break;
			case GeometryPageState::NotResident:
				++snapshot.missingPages;
				break;
			}
		}
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

	void ApplyBackendBreakdownToRenderDiagnostics(RenderDiagnosticsSummary& summary, const BackendFrameBreakdown& breakdown)
	{
		summary.backendFrameBreakdown = breakdown;
		summary.visibleFrameMilliseconds = SumVisibleBackendFrameCost(summary.backendFrameBreakdown);
		summary.queuedBackgroundMilliseconds = SumQueuedBackendFrameCost(summary.backendFrameBreakdown);
		summary.backendStalls = CollectBackendStallDiagnostics(summary.backendFrameBreakdown, 0.25f);
		summary.viewSubmitWork = summary.backendFrameBreakdown.viewSubmitWork;
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

	RenderDiagnosticsSummary BuildRenderDiagnosticsSummary(
		const RenderFrameSnapshot& snapshot,
		const RenderWorkPacketPlan& packetPlan,
		const RenderSubmissionPlan& submissionPlan,
		const BackendCapabilityProfile& capabilities)
	{
		RenderDiagnosticsSummary summary;
		summary.frameId = snapshot.frameId;
		summary.workerPacketCount = packetPlan.workerPacketCount;
		summary.serialPacketCount = packetPlan.serialPacketCount;
		summary.skippedPacketCount = packetPlan.skippedPacketCount;
		summary.serializedReasons = packetPlan.serializedReasons;
		summary.queueReadiness = submissionPlan.queueReadiness;
		summary.degradedWork = submissionPlan.degradedWork;
		summary.runtimeLayoutRenderRecordCount = snapshot.renderables.size() + snapshot.lights.size() + snapshot.viewRequests.size();
		summary.valid = submissionPlan.valid;
		summary.detailed = RenderDiagnosticsRequested();

		if (!capabilities.supportsParallelRecording)
		{
			summary.backendFallbacks.push_back("Backend profile requires serial render recording.");
		}
		for (const RenderGraphValidationDiagnostic& diagnostic : submissionPlan.validationDiagnostics)
		{
			summary.resourceWarnings.push_back(diagnostic.code + ": " + diagnostic.message);
			if (diagnostic.code.find("stale") != std::string::npos)
			{
				summary.backendFallbacks.push_back("Rejected stale render snapshot/submission work before protected GPU submit: " + diagnostic.recommendedAction);
			}
		}

		if (summary.detailed)
		{
			ApplyBackendBreakdownToRenderDiagnostics(summary, BuildUnavailableBackendFrameBreakdown(snapshot));
			summary.renderWorkerGroups.push_back(BuildRenderWorkerTaskGroup(snapshot, capabilities, "Render.Preparation", EstimateSnapshotWorkload(snapshot)));
			if (capabilities.supportsPacketLocalDiagnostics)
			{
				summary.backendFallbacks.push_back("Packet-local render diagnostics are available for worker merge.");
			}
			summary.topPassCosts.push_back({ "snapshot", static_cast<float>(snapshot.renderables.size()) * 0.001f });
			summary.topPassCosts.push_back({ "packet-plan", static_cast<float>(packetPlan.packets.size()) * 0.001f });
			summary.topPassCosts.push_back({ "submission-validation", submissionPlan.valid ? 0.001f : 0.01f });
			summary.budgetDiagnostics.push_back(BuildRenderBudgetDiagnostic({ RenderWorkBudgetCategory::DrawPreparation, capabilities.workerSchedulingThreshold, EstimateSnapshotWorkload(snapshot), RenderBudgetOverflowPolicy::Serialize, "draw-preparation" }));
			summary.performanceGates.push_back(EvaluateRenderPerformanceGate("small-scene-overhead", 1.0f, snapshot.renderables.size() <= 10u ? 1.0f : 0.95f, 5.0f));
			std::sort(summary.topPassCosts.begin(), summary.topPassCosts.end(), [](const RenderPassTiming& left, const RenderPassTiming& right) {
				return left.milliseconds > right.milliseconds;
			});
		}

		return summary;
	}

}
