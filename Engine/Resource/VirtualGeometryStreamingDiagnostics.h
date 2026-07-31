// VirtualGeometryStreamingDiagnostics.h - compact observability for page streaming.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Resource
{
	struct VirtualGeometryStreamingDiagnosticEvent
	{
		std::uint64_t frameId = 0u;
		std::uint32_t pageId = 0xffffffffu;
		std::string stage;
		std::string detail;
		bool failure = false;
	};

	struct VirtualGeometryStreamingDiagnostics
	{
		std::uint64_t submittedRequests = 0u;
		std::uint64_t expandedDependencyRequests = 0u;
		std::uint64_t preparedDecodeIntents = 0u;
		std::uint64_t preparedUploadIntents = 0u;
		std::uint64_t completedReads = 0u;
		std::uint64_t cancelledReads = 0u;
		std::uint64_t submittedPublicationCandidates = 0u;
		std::uint64_t acceptedPublicationBatches = 0u;
		std::uint64_t acceptedPublicationPages = 0u;
		std::uint64_t rejectedPublications = 0u;
		std::uint64_t visibleResidentPages = 0u;
		// Direct meshlet routes intentionally bypass virtual-geometry feedback.
		// Count suppression separately so an observed zero cannot be mistaken for
		// an unavailable or silently dropped streaming request.
		std::uint64_t suppressedDirectRouteFeedback = 0u;
		std::uint64_t suppressedDirectRouteDecodeIntents = 0u;
		std::uint64_t suppressedDirectRouteResidencyPublications = 0u;
		std::uint64_t lastPreparedFrame = 0u;
		std::uint64_t lastPublicationFrame = 0u;
		std::string lastRejectionReason;
		std::vector<VirtualGeometryStreamingDiagnosticEvent> recentEvents;
	};

	TERRAENGINE_API void RecordVirtualGeometryStreamingEvent(
		VirtualGeometryStreamingDiagnostics &diagnostics,
		VirtualGeometryStreamingDiagnosticEvent event,
		std::size_t maximumEvents = 64u);
	TERRAENGINE_API void RecordVirtualGeometryDirectRouteSuppression(
		VirtualGeometryStreamingDiagnostics& diagnostics, std::uint64_t frameId,
		std::uint32_t feedbackCount, std::uint32_t decodeIntentCount,
		std::uint32_t residencyPublicationCount);
} // namespace TerraEngine::Resource
