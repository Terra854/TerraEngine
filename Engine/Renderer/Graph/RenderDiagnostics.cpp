#include "RenderDiagnostics.h"

#include <algorithm>
#include <cmath>
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

	VirtualGeometryViewDiagnosticsSnapshot BuildInactiveVirtualGeometryDiagnostics(RenderViewType viewType, std::uint64_t frameId, VirtualGeometryInactiveReason reason)
	{
		VirtualGeometryViewDiagnosticsSnapshot snapshot;
		snapshot.viewType = viewType;
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
		snapshot.frameId = frameId;
		snapshot.active = true;
		snapshot.effectiveLodErrorScale = quality.lodErrorScale;
		snapshot.configuredMaxLodDrop = quality.maxLodDrop;
		snapshot.geometryBudgetBytes = quality.geometryBudgetBytes;
		snapshot.cookOutput = BuildVirtualGeometryCookOutputDiagnostics(assets);

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
					}
					if (!page.fallbackPage && !page.pinnedFallback && page.state != GeometryPageState::Pinned)
					{
						contributor.reclaimableBytes += page.byteSize;
					}
					break;
				case GeometryPageState::Requested:
				case GeometryPageState::Loading:
					++snapshot.requestedPages;
					break;
				case GeometryPageState::PendingIO:
					++snapshot.requestedPages;
					++snapshot.pageResidency.pendingIoPages;
					break;
				case GeometryPageState::PendingUpload:
					++snapshot.requestedPages;
					++snapshot.pageResidency.pendingUploadPages;
					break;
				case GeometryPageState::Evicting:
					++snapshot.evictingPages;
					++snapshot.pageResidency.evictingPages;
					++snapshot.missingPages;
					++contributor.missingPages;
					break;
				case GeometryPageState::NotResident:
					++snapshot.missingPages;
					++contributor.missingPages;
					break;
				case GeometryPageState::Failed:
					++snapshot.failedPages;
					++snapshot.missingPages;
					if (!page.failureReason.empty())
					{
						++snapshot.pageResidency.failedPageReasons;
					}
					++contributor.missingPages;
					break;
				case GeometryPageState::Evicted:
					++snapshot.evictedPages;
					++snapshot.missingPages;
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

	VirtualGeometryViewDiagnosticsSnapshot BuildVirtualGeometryHiZDiagnostics(const VirtualGeometryHiZDiagnosticsInput& input)
	{
		VirtualGeometryViewDiagnosticsSnapshot snapshot;
		const bool hiZInactive = !input.hiZActive;
		snapshot.viewType = input.viewType;
		snapshot.frameId = input.frameId;
		snapshot.active = input.qualityRequested;
		snapshot.hiZActive = input.hiZActive;
		snapshot.hiZHistoryValid = input.historyValid;
		snapshot.hiZInvalidReason = hiZInactive ? input.invalidReason : std::string();
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
