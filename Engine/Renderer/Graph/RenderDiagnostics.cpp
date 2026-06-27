#include "RenderDiagnostics.h"

#include <algorithm>

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
