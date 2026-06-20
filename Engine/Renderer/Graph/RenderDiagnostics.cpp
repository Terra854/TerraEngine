#include "RenderDiagnostics.h"

#include <algorithm>

namespace TerraEngine::Renderer {

	namespace {
		std::atomic<bool> g_renderDiagnosticsRequested = false;
		std::mutex g_renderDiagnosticsMutex;
		RenderDiagnosticsSummary g_latestRenderDiagnostics;
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
			summary.backendFrameBreakdown = BuildUnavailableBackendFrameBreakdown(snapshot);
			summary.renderWorkerGroups.push_back(BuildRenderWorkerTaskGroup(snapshot, capabilities, "Render.Preparation", EstimateSnapshotWorkload(snapshot)));
			if (capabilities.supportsPacketLocalDiagnostics)
			{
				summary.backendFallbacks.push_back("Packet-local render diagnostics are available for worker merge.");
			}
			summary.topPassCosts.push_back({ "snapshot", static_cast<float>(snapshot.renderables.size()) * 0.001f });
			summary.topPassCosts.push_back({ "packet-plan", static_cast<float>(packetPlan.packets.size()) * 0.001f });
			summary.topPassCosts.push_back({ "submission-validation", submissionPlan.valid ? 0.001f : 0.01f });
			std::sort(summary.topPassCosts.begin(), summary.topPassCosts.end(), [](const RenderPassTiming& left, const RenderPassTiming& right) {
				return left.milliseconds > right.milliseconds;
			});
		}

		return summary;
	}

}
