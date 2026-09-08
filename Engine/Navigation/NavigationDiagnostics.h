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
