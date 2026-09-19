// NavigationDiagnostics.cpp -- Bounded copied diagnostic publication.

#include "Navigation/NavigationDiagnostics.h"

#include <utility>

namespace TerraEngine::Navigation
{
	namespace
	{
		[[nodiscard]] std::size_t RecordBytes(const NavigationDiagnosticRecord& record) noexcept
		{
			return sizeof(NavigationDiagnosticRecord) + record.code.size() + record.message.size();
		}
	}

	NavigationDiagnostics::NavigationDiagnostics(std::size_t maximumSnapshotBytes, std::size_t maximumSnapshotRecords)
		: maximumBytes(maximumSnapshotBytes), maximumRecords(maximumSnapshotRecords) {}

	void NavigationDiagnostics::Append(NavigationDiagnosticRecord record)
	{
		const std::size_t bytes = RecordBytes(record);
		std::scoped_lock lock(mutex);
		if(bytes > maximumBytes || maximumRecords == 0U) {
			++droppedRecords;
			droppedBytes += bytes;
			++revision;
			return;
		}
		while(!records.empty() && (records.size() >= maximumRecords || retainedBytes > maximumBytes - bytes)) {
			droppedBytes += RecordBytes(records.front());
			retainedBytes -= RecordBytes(records.front());
			records.pop_front();
			++droppedRecords;
		}
		retainedBytes += bytes;
		records.push_back(std::move(record));
		++revision;
	}

	void NavigationDiagnostics::PublishDynamic(NavigationDynamicDiagnostics next)
	{
		std::scoped_lock lock(mutex);
		dynamic = next;
		++revision;
	}

	std::shared_ptr<const NavigationDiagnosticSnapshot> NavigationDiagnostics::Snapshot() const
	{
		std::scoped_lock lock(mutex);
		auto snapshot = std::make_shared<NavigationDiagnosticSnapshot>();
		snapshot->revision = revision;
		snapshot->droppedRecordCount = droppedRecords;
		snapshot->droppedByteCount = droppedBytes;
		snapshot->retainedByteCount = retainedBytes;
		snapshot->dynamicSourceLagCount = dynamic.dynamicSourceLagCount;
		snapshot->dirtyCoverageCount = dynamic.dirtyCoverageCount;
		snapshot->queueDepth = dynamic.queueDepth;
		snapshot->rejectedUpdateCount = dynamic.rejectedUpdateCount;
		snapshot->latencyNanoseconds = dynamic.latencyNanoseconds;
		snapshot->retiredGenerationCount = dynamic.retiredGenerationCount;
		snapshot->staleDropCount = dynamic.staleDropCount;
		snapshot->preparedRouteRootMismatchCount = dynamic.preparedRouteRootMismatchCount;
		snapshot->preparedRouteTransitionRebasedCount = dynamic.preparedRouteTransitionRebasedCount;
		snapshot->preparedRouteTransitionIntersectedCount = dynamic.preparedRouteTransitionIntersectedCount;
		snapshot->preparedRouteTransitionUnknownCount = dynamic.preparedRouteTransitionUnknownCount;
		snapshot->preparedRouteTransitionInvalidInputCount = dynamic.preparedRouteTransitionInvalidInputCount;
		snapshot->preparedRouteTransitionTerrainMismatchCount = dynamic.preparedRouteTransitionTerrainMismatchCount;
		snapshot->preparedRouteTransitionMissingLinkCount = dynamic.preparedRouteTransitionMissingLinkCount;
		snapshot->preparedRouteTransitionInvalidLinkCount = dynamic.preparedRouteTransitionInvalidLinkCount;
		snapshot->preparedRouteTransitionSourceChainMismatchCount = dynamic.preparedRouteTransitionSourceChainMismatchCount;
		snapshot->preparedRouteTransitionFinalSourceMismatchCount = dynamic.preparedRouteTransitionFinalSourceMismatchCount;
		snapshot->asyncBuildCurrentCount = dynamic.asyncBuildCurrentCount;
		snapshot->asyncBuildRetiredCount = dynamic.asyncBuildRetiredCount;
		snapshot->asyncBuildChargedBytes = dynamic.asyncBuildChargedBytes;
		snapshot->asyncBuildPeakChargedBytes = dynamic.asyncBuildPeakChargedBytes;
		snapshot->asyncBuildAdmissionRejectedCount = dynamic.asyncBuildAdmissionRejectedCount;
		snapshot->asyncBuildCompletionCount = dynamic.asyncBuildCompletionCount;
		snapshot->asyncBuildStaleDiscardCount = dynamic.asyncBuildStaleDiscardCount;
		snapshot->asyncBuildGroundCompletionCount = dynamic.asyncBuildGroundCompletionCount;
		snapshot->asyncBuildDynamicCompletionCount = dynamic.asyncBuildDynamicCompletionCount;
		snapshot->asyncBuildTotalWorkerNanoseconds = dynamic.asyncBuildTotalWorkerNanoseconds;
		snapshot->asyncBuildMaxWorkerNanoseconds = dynamic.asyncBuildMaxWorkerNanoseconds;
		snapshot->asyncBuildTotalQueueWaitNanoseconds = dynamic.asyncBuildTotalQueueWaitNanoseconds;
		snapshot->asyncBuildMaxQueueWaitNanoseconds = dynamic.asyncBuildMaxQueueWaitNanoseconds;
		snapshot->asyncBuildTotalPublicationWaitNanoseconds = dynamic.asyncBuildTotalPublicationWaitNanoseconds;
		snapshot->asyncBuildMaxPublicationWaitNanoseconds = dynamic.asyncBuildMaxPublicationWaitNanoseconds;
		snapshot->dynamicTileReuseAttemptCount = dynamic.dynamicTileReuseAttemptCount;
		snapshot->dynamicTileReuseTileCount = dynamic.dynamicTileReuseTileCount;
		snapshot->dynamicTileRebuildTileCount = dynamic.dynamicTileRebuildTileCount;
		snapshot->dynamicTileReuseFallbackCount = dynamic.dynamicTileReuseFallbackCount;
		snapshot->routeAdmissionToCaptureCount = dynamic.routeAdmissionToCaptureCount;
		snapshot->routeAdmissionToCaptureNanoseconds = dynamic.routeAdmissionToCaptureNanoseconds;
		snapshot->routeAdmissionToCaptureMaxNanoseconds = dynamic.routeAdmissionToCaptureMaxNanoseconds;
		snapshot->routeWorkerSearchCount = dynamic.routeWorkerSearchCount;
		snapshot->routeWorkerSearchNanoseconds = dynamic.routeWorkerSearchNanoseconds;
		snapshot->routeWorkerSearchMaxNanoseconds = dynamic.routeWorkerSearchMaxNanoseconds;
		snapshot->routeWorkerToPublicationCount = dynamic.routeWorkerToPublicationCount;
		snapshot->routeWorkerToPublicationNanoseconds = dynamic.routeWorkerToPublicationNanoseconds;
		snapshot->routeWorkerToPublicationMaxNanoseconds = dynamic.routeWorkerToPublicationMaxNanoseconds;
		snapshot->routeDeferredCoverageWaitCount = dynamic.routeDeferredCoverageWaitCount;
		snapshot->routeDeferredCoverageWaitNanoseconds = dynamic.routeDeferredCoverageWaitNanoseconds;
		snapshot->routeDeferredCoverageWaitMaxNanoseconds = dynamic.routeDeferredCoverageWaitMaxNanoseconds;
		snapshot->records.assign(records.begin(), records.end());
		return snapshot;
	}
}
