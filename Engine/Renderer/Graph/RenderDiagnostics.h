// RenderDiagnostics.h - opt-in render threading summaries.

#pragma once

#include "RenderFrameBreakdown.h"
#include "RenderWorkerTaskGroup.h"
#include "RenderSubmissionPlan.h"
#include "Core/Export.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	struct RenderPassTiming
	{
		std::string passId;
		float milliseconds = 0.0f;
	};

	struct RenderBudgetDiagnostic
	{
		RenderWorkBudgetCategory category = RenderWorkBudgetCategory::DrawPreparation;
		std::size_t limit = 0u;
		std::size_t currentUsage = 0u;
		RenderBudgetOverflowPolicy overflowPolicy = RenderBudgetOverflowPolicy::Delay;
		bool overBudget = false;
		std::string label;
	};

	struct RenderPerformanceGateResult
	{
		std::string workloadName;
		float baselineValue = 0.0f;
		float latestValue = 0.0f;
		float thresholdPercent = 0.0f;
		bool passed = true;
	};

	struct RenderDiagnosticsSummary
	{
		std::uint64_t frameId = 0u;
		std::size_t workerPacketCount = 0u;
		std::size_t serialPacketCount = 0u;
		std::size_t skippedPacketCount = 0u;
		std::size_t failedPacketCount = 0u;
		float visibleFrameMilliseconds = 0.0f;
		float queuedBackgroundMilliseconds = 0.0f;
		std::vector<RenderPassTiming> topPassCosts;
		BackendFrameBreakdown backendFrameBreakdown;
		std::vector<RenderWorkerTaskGroup> renderWorkerGroups;
		std::vector<RenderBudgetDiagnostic> budgetDiagnostics;
		std::vector<RenderQueueReadiness> queueReadiness;
		std::vector<RenderPerformanceGateResult> performanceGates;
		// backendStalls contains only opt-in diagnostics so normal frames do
		// not pay for stall classification while the editor window is closed.
		std::vector<BackendStallDiagnostic> backendStalls;
		// viewSubmitWork explains whether scene and game views reused prepared
		// backend work or had to stay separate because state differed.
		std::vector<ViewSubmitWorkItem> viewSubmitWork;
		std::vector<std::string> serializedReasons;
		std::vector<std::string> resourceWarnings;
		std::vector<std::string> backendFallbacks;
		std::vector<std::string> delayedWork;
		std::vector<std::string> cancelledWork;
		std::vector<std::string> degradedWork;
		bool valid = true;
		bool detailed = false;
	};

	TERRAENGINE_API RenderBudgetDiagnostic BuildRenderBudgetDiagnostic(const RenderWorkBudget& budget);
	TERRAENGINE_API RenderPerformanceGateResult EvaluateRenderPerformanceGate(std::string workloadName, float baselineValue, float latestValue, float thresholdPercent);
	TERRAENGINE_API void ApplyBackendBreakdownToRenderDiagnostics(RenderDiagnosticsSummary& summary, const BackendFrameBreakdown& breakdown);
	TERRAENGINE_API bool RenderDiagnosticsRequested();
	TERRAENGINE_API void SetRenderDiagnosticsRequested(bool requested);
	TERRAENGINE_API void ClearPublishedRenderDiagnostics();
	TERRAENGINE_API void PublishRenderDiagnostics(RenderDiagnosticsSummary summary);
	TERRAENGINE_API RenderDiagnosticsSummary LatestRenderDiagnostics();
	TERRAENGINE_API RenderDiagnosticsSummary BuildRenderDiagnosticsSummary(
		const RenderFrameSnapshot& snapshot,
		const RenderWorkPacketPlan& packetPlan,
		const RenderSubmissionPlan& submissionPlan,
		const BackendCapabilityProfile& capabilities);

}
