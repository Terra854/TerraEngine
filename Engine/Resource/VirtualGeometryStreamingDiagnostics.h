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
		std::uint64_t lastPreparedFrame = 0u;
		std::uint64_t lastPublicationFrame = 0u;
		std::string lastRejectionReason;
		std::vector<VirtualGeometryStreamingDiagnosticEvent> recentEvents;
	};

	TERRAENGINE_API void RecordVirtualGeometryStreamingEvent(
		VirtualGeometryStreamingDiagnostics &diagnostics,
		VirtualGeometryStreamingDiagnosticEvent event,
		std::size_t maximumEvents = 64u);
} // namespace TerraEngine::Resource
