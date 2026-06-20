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

	struct RenderDiagnosticsSummary
	{
		std::uint64_t frameId = 0u;
		std::size_t workerPacketCount = 0u;
		std::size_t serialPacketCount = 0u;
		std::size_t skippedPacketCount = 0u;
		std::size_t failedPacketCount = 0u;
		std::vector<RenderPassTiming> topPassCosts;
		BackendFrameBreakdown backendFrameBreakdown;
		std::vector<RenderWorkerTaskGroup> renderWorkerGroups;
		std::vector<std::string> serializedReasons;
		std::vector<std::string> resourceWarnings;
		std::vector<std::string> backendFallbacks;
		bool valid = true;
		bool detailed = false;
	};

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
