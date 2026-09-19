// NavigationDiagnostics.h -- Bounded immutable navigation diagnostic history.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace TerraEngine::Navigation
{
	enum class NavigationDiagnosticSeverity : std::uint8_t { Information, Warning, Error };
	enum class NavigationMeasurementReason : std::uint8_t
	{
		Measured,
		MissingMeasurement,
		MeasurementUnavailable,
		MeasurementDropped
	};

	struct NavigationDiagnosticRecord
	{
		NavigationDiagnosticSeverity severity = NavigationDiagnosticSeverity::Information;
		NavigationMeasurementReason measurementReason = NavigationMeasurementReason::MissingMeasurement;
		std::uint64_t publishedGeneration = 0U;
		std::uint64_t measuredNanoseconds = 0U;
		std::string code;
		std::string message;
	};

	struct NavigationDiagnosticSnapshot
	{
		std::uint64_t revision = 0U;
		std::size_t droppedRecordCount = 0U;
		std::size_t droppedByteCount = 0U;
		std::size_t retainedByteCount = 0U;
		std::vector<NavigationDiagnosticRecord> records;
		std::uint64_t dynamicSourceLagCount = 0U;
		std::uint64_t dirtyCoverageCount = 0U;
		std::uint64_t queueDepth = 0U;
		std::uint64_t rejectedUpdateCount = 0U;
		std::uint64_t latencyNanoseconds = 0U;
		std::uint64_t retiredGenerationCount = 0U;
		std::uint64_t staleDropCount = 0U;
		// Route workers can finish after a local dynamic publication. Expose only
		// aggregate outcomes so tooling can prove fail-closed rebasing behavior.
		std::uint64_t preparedRouteRootMismatchCount = 0U;
		std::uint64_t preparedRouteTransitionRebasedCount = 0U;
		std::uint64_t preparedRouteTransitionIntersectedCount = 0U;
		std::uint64_t preparedRouteTransitionUnknownCount = 0U;
		// Keep the fail-closed Unknown total actionable without retaining every
		// route packet. These mutually exclusive counters name the first failed
		// certificate check at protected publication.
		std::uint64_t preparedRouteTransitionInvalidInputCount = 0U;
		std::uint64_t preparedRouteTransitionTerrainMismatchCount = 0U;
		std::uint64_t preparedRouteTransitionMissingLinkCount = 0U;
		std::uint64_t preparedRouteTransitionInvalidLinkCount = 0U;
		std::uint64_t preparedRouteTransitionSourceChainMismatchCount = 0U;
		std::uint64_t preparedRouteTransitionFinalSourceMismatchCount = 0U;
		// Expensive Recast/Detour builds run from copied packets outside the frame
		// join. These values prove that retirement keeps their admission charge until
		// a worker actually completes, rather than freeing capacity at revoke time.
		std::uint64_t asyncBuildCurrentCount = 0U;
		std::uint64_t asyncBuildRetiredCount = 0U;
		std::uint64_t asyncBuildChargedBytes = 0U;
		std::uint64_t asyncBuildPeakChargedBytes = 0U;
		std::uint64_t asyncBuildAdmissionRejectedCount = 0U;
		std::uint64_t asyncBuildCompletionCount = 0U;
		std::uint64_t asyncBuildStaleDiscardCount = 0U;
		// Bounded lifetime totals distinguish provider execution from dispatch and
		// protected completion-observation delays without retaining task traces.
		std::uint64_t asyncBuildGroundCompletionCount = 0U;
		std::uint64_t asyncBuildDynamicCompletionCount = 0U;
		std::uint64_t asyncBuildTotalWorkerNanoseconds = 0U;
		std::uint64_t asyncBuildMaxWorkerNanoseconds = 0U;
		std::uint64_t asyncBuildTotalQueueWaitNanoseconds = 0U;
		std::uint64_t asyncBuildMaxQueueWaitNanoseconds = 0U;
		std::uint64_t asyncBuildTotalPublicationWaitNanoseconds = 0U;
		std::uint64_t asyncBuildMaxPublicationWaitNanoseconds = 0U;
		// Dynamic providers still assemble a full Detour shard and portal snapshot.
		// These scalar totals prove how many Recast tiles were safely retained or
		// rebuilt without retaining a diagnostic history of tile payloads.
		std::uint64_t dynamicTileReuseAttemptCount = 0U;
		std::uint64_t dynamicTileReuseTileCount = 0U;
		std::uint64_t dynamicTileRebuildTileCount = 0U;
		std::uint64_t dynamicTileReuseFallbackCount = 0U;
		// Route timing aggregates are protected-boundary observations, not a
		// per-request trace. Admission/capture counts packets captured at least once;
		// worker timing counts searches that actually executed; publication timing
		// counts packets that produced a live route-store handle at protected publication;
		// deferred timing
		// counts deferred requests promoted into the ready queue.
		std::uint64_t routeAdmissionToCaptureCount = 0U;
		std::uint64_t routeAdmissionToCaptureNanoseconds = 0U;
		std::uint64_t routeAdmissionToCaptureMaxNanoseconds = 0U;
		std::uint64_t routeWorkerSearchCount = 0U;
		std::uint64_t routeWorkerSearchNanoseconds = 0U;
		std::uint64_t routeWorkerSearchMaxNanoseconds = 0U;
		std::uint64_t routeWorkerToPublicationCount = 0U;
		std::uint64_t routeWorkerToPublicationNanoseconds = 0U;
		std::uint64_t routeWorkerToPublicationMaxNanoseconds = 0U;
		std::uint64_t routeDeferredCoverageWaitCount = 0U;
		std::uint64_t routeDeferredCoverageWaitNanoseconds = 0U;
		std::uint64_t routeDeferredCoverageWaitMaxNanoseconds = 0U;
	};

	struct NavigationDynamicDiagnostics
	{
		std::uint64_t dynamicSourceLagCount = 0U;
		std::uint64_t dirtyCoverageCount = 0U;
		std::uint64_t queueDepth = 0U;
		std::uint64_t rejectedUpdateCount = 0U;
		std::uint64_t latencyNanoseconds = 0U;
		std::uint64_t retiredGenerationCount = 0U;
		std::uint64_t staleDropCount = 0U;
		std::uint64_t preparedRouteRootMismatchCount = 0U;
		std::uint64_t preparedRouteTransitionRebasedCount = 0U;
		std::uint64_t preparedRouteTransitionIntersectedCount = 0U;
		std::uint64_t preparedRouteTransitionUnknownCount = 0U;
		std::uint64_t preparedRouteTransitionInvalidInputCount = 0U;
		std::uint64_t preparedRouteTransitionTerrainMismatchCount = 0U;
		std::uint64_t preparedRouteTransitionMissingLinkCount = 0U;
		std::uint64_t preparedRouteTransitionInvalidLinkCount = 0U;
		std::uint64_t preparedRouteTransitionSourceChainMismatchCount = 0U;
		std::uint64_t preparedRouteTransitionFinalSourceMismatchCount = 0U;
		std::uint64_t asyncBuildCurrentCount = 0U;
		std::uint64_t asyncBuildRetiredCount = 0U;
		std::uint64_t asyncBuildChargedBytes = 0U;
		std::uint64_t asyncBuildPeakChargedBytes = 0U;
		std::uint64_t asyncBuildAdmissionRejectedCount = 0U;
		std::uint64_t asyncBuildCompletionCount = 0U;
		std::uint64_t asyncBuildStaleDiscardCount = 0U;
		std::uint64_t asyncBuildGroundCompletionCount = 0U;
		std::uint64_t asyncBuildDynamicCompletionCount = 0U;
		std::uint64_t asyncBuildTotalWorkerNanoseconds = 0U;
		std::uint64_t asyncBuildMaxWorkerNanoseconds = 0U;
		std::uint64_t asyncBuildTotalQueueWaitNanoseconds = 0U;
		std::uint64_t asyncBuildMaxQueueWaitNanoseconds = 0U;
		std::uint64_t asyncBuildTotalPublicationWaitNanoseconds = 0U;
		std::uint64_t asyncBuildMaxPublicationWaitNanoseconds = 0U;
		std::uint64_t dynamicTileReuseAttemptCount = 0U;
		std::uint64_t dynamicTileReuseTileCount = 0U;
		std::uint64_t dynamicTileRebuildTileCount = 0U;
		std::uint64_t dynamicTileReuseFallbackCount = 0U;
		std::uint64_t routeAdmissionToCaptureCount = 0U;
		std::uint64_t routeAdmissionToCaptureNanoseconds = 0U;
		std::uint64_t routeAdmissionToCaptureMaxNanoseconds = 0U;
		std::uint64_t routeWorkerSearchCount = 0U;
		std::uint64_t routeWorkerSearchNanoseconds = 0U;
		std::uint64_t routeWorkerSearchMaxNanoseconds = 0U;
		std::uint64_t routeWorkerToPublicationCount = 0U;
		std::uint64_t routeWorkerToPublicationNanoseconds = 0U;
		std::uint64_t routeWorkerToPublicationMaxNanoseconds = 0U;
		std::uint64_t routeDeferredCoverageWaitCount = 0U;
		std::uint64_t routeDeferredCoverageWaitNanoseconds = 0U;
		std::uint64_t routeDeferredCoverageWaitMaxNanoseconds = 0U;
	};

	// The ring publishes copied snapshots. Readers therefore never observe a
	// partially appended record or retain an internal container across a commit.
	class TERRAENGINE_API NavigationDiagnostics final
	{
	public:
		explicit NavigationDiagnostics(std::size_t maximumBytes = 4U * 1024U * 1024U,
			std::size_t maximumRecords = 4096U);
		void Append(NavigationDiagnosticRecord record);
		void PublishDynamic(NavigationDynamicDiagnostics dynamic);
		[[nodiscard]] std::shared_ptr<const NavigationDiagnosticSnapshot> Snapshot() const;

	private:
		std::size_t maximumBytes;
		std::size_t maximumRecords;
		mutable std::mutex mutex;
		std::uint64_t revision = 0U;
		std::size_t retainedBytes = 0U;
		std::size_t droppedRecords = 0U;
		std::size_t droppedBytes = 0U;
		std::deque<NavigationDiagnosticRecord> records;
		NavigationDynamicDiagnostics dynamic;
	};
}
