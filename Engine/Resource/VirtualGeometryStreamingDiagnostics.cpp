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
} // namespace TerraEngine::Resource
