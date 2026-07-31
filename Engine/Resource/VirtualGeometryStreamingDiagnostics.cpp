// VirtualGeometryStreamingDiagnostics.cpp - bounded streaming diagnostic history.

#include "VirtualGeometryStreamingDiagnostics.h"

#include <cstddef>
#include <utility>

namespace TerraEngine::Resource
{
	void RecordVirtualGeometryStreamingEvent(
		VirtualGeometryStreamingDiagnostics &diagnostics, VirtualGeometryStreamingDiagnosticEvent event, std::size_t maximumEvents)
	{
		// Keeping a bounded value-only history makes diagnostics safe to copy across
		// scheduler boundaries without retaining live resource objects.
		if (maximumEvents == 0u)
		{
			return;
		}
		diagnostics.recentEvents.push_back(std::move(event));
		if (diagnostics.recentEvents.size() > maximumEvents)
		{
			diagnostics.recentEvents.erase(diagnostics.recentEvents.begin(),
				diagnostics.recentEvents.begin() + static_cast<std::ptrdiff_t>(diagnostics.recentEvents.size() - maximumEvents));
		}
	}

	void RecordVirtualGeometryDirectRouteSuppression(
		VirtualGeometryStreamingDiagnostics& diagnostics, std::uint64_t frameId,
		std::uint32_t feedbackCount, std::uint32_t decodeIntentCount,
		std::uint32_t residencyPublicationCount)
	{
		// Direct routes do not fail streaming; they intentionally avoid producing
		// VG work. Keeping these counters separate preserves that distinction for
		// editor, evidence, and automation consumers.
		diagnostics.suppressedDirectRouteFeedback += feedbackCount;
		diagnostics.suppressedDirectRouteDecodeIntents += decodeIntentCount;
		diagnostics.suppressedDirectRouteResidencyPublications += residencyPublicationCount;
		RecordVirtualGeometryStreamingEvent(diagnostics, {
			frameId,
			0xffffffffu,
			"direct-route-suppressed",
			"direct meshlet route suppressed virtual-geometry feedback, decode, and residency publication work",
			false
		});
	}
} // namespace TerraEngine::Resource
