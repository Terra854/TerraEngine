// TerrainDiagnostics.h -- Immutable status records for terrain consumers.

#pragma once

#include "Terrain/TerrainEditTransaction.h"
#include "Terrain/TerrainLayerStack.h"

#include <vector>
#include <optional>
#include <string>
#include <utility>

namespace TerraEngine::Physics::Terrain { struct TerrainCollisionSnapshot; }

namespace TerraEngine::Terrain
{
	enum class TerrainDerivedStatus : std::uint8_t { Absent, Queued, Preparing, ReadyCandidate, PublishedCurrent, Superseded, RejectedStale, Cancelled, Failed };

	struct TerrainResidencyRecord
	{
		TerrainAreaCoordinate coordinate{};
		std::uint64_t residentBytes = 0u;
		bool pinned = false;
	};
	// Resource pressure is supplied by the residency owner. Keeping used and
	// budget values together prevents diagnostics from guessing a percentage.
	struct TerrainResourcePressureRecord { std::string resource; std::uint64_t usedBytes = 0u; std::uint64_t budgetBytes = 0u; bool evictionRequired = false; };
	struct TerrainSeamRecord { TerrainAreaCoordinate coordinate{}; TerrainAreaCoordinate neighbour{}; bool holeBoundary = false; std::string message; };

	struct TerrainJobRecord
	{
		TerraEngine::UUID jobId = TerraEngine::EMPTY_UUID;
		TerrainDerivedStatus status = TerrainDerivedStatus::Absent;
		std::uint64_t workerNanoseconds = 0u;
	};

	struct TerrainConflictRecord
	{
		TerraEngine::UUID transactionId = TerraEngine::EMPTY_UUID;
		TerrainResultCode code = TerrainResultCode::Ok;
		std::string message;
	};

	// These categories are stable automation payloads, not presentation strings.
	// A consumer can choose its own UI while retaining the exact recovery action.
	enum class TerrainDiagnosticSeverity : std::uint8_t { Information, Warning, Error };
	enum class TerrainDiagnosticImpact : std::uint8_t { None, DelayedDerivedData, LastKnownGoodActive, MutationRejected, Unavailable };
	enum class TerrainDiagnosticAction : std::uint8_t { None, Retry, Rebuild, ReloadSource, ResolveConflict, IncreaseBudget, LocateArea };
	enum class TerrainDiagnosticCategory : std::uint8_t { Job, Conflict, Collision, Derived, Residency, Source, Seam };
	struct TerrainDiagnosticItem
	{
		TerrainDiagnosticCategory category = TerrainDiagnosticCategory::Job;
		TerrainDiagnosticSeverity severity = TerrainDiagnosticSeverity::Information;
		TerrainDiagnosticImpact impact = TerrainDiagnosticImpact::None;
		TerrainDiagnosticAction action = TerrainDiagnosticAction::None;
		std::optional<TerrainAreaCoordinate> location;
		ContentHash sourceHash{};
		Generation sourceGeneration = 0u;
		std::string message;
	};
	struct TerrainStagePercentile { std::string stage; std::uint64_t p50Nanoseconds = 0u; std::uint64_t p95Nanoseconds = 0u; std::size_t sampleCount = 0u; };

	// Source loss changes presentation only: paint remains keyed by stable IDs.
	// Editors expose these records so a fallback material is never mistaken for
	// destructive weight remapping.
	struct TerrainLayerFallbackRecord
	{
		std::uint16_t stableLayerId = 0u;
		TerraEngine::UUID materialAssetId = TerraEngine::EMPTY_UUID;
		TerrainLayerFallbackState state = TerrainLayerFallbackState::Healthy;
		std::string message;
	};

	// Collision products are derived and may lag an authored root. This record
	// makes the active last-known-good identity explicit instead of implying that
	// a pending or failed rebuild is physically current.
	enum class TerrainCollisionStatus : std::uint8_t { Unavailable, Pending, Current, Stale, Failed };
	struct TerrainCollisionRecord
	{
		TerrainAreaCoordinate coordinate{};
		TerrainCollisionStatus status = TerrainCollisionStatus::Unavailable;
		ContentHash sourceRootHash{};
		Generation sourcePublicationGeneration = 0u;
		ContentHash activeSourceRootHash{};
		Generation activeSourcePublicationGeneration = 0u;
		std::uint64_t ageMilliseconds = 0u;
		std::uint64_t buildLatencyMilliseconds = 0u;
		bool lastKnownGoodActive = false;
		std::string message;
	};

	struct TerrainDiagnosticSnapshot
	{
		Generation revision = 0u;
		Generation authoredGeneration = 0u;
		Generation savedGeneration = 0u;
		Generation visibleGeneration = 0u;
		Generation renderGeneration = 0u;
		Generation controlGeneration = 0u;
		Generation collisionGeneration = 0u;
		bool dirty = false;
		std::size_t pendingTransactions = 0u;
		std::size_t historyDepth = 0u;
		std::uint64_t historyBytes = 0u;
		std::vector<TerrainResidencyRecord> residency;
		std::vector<TerrainResourcePressureRecord> resourcePressure;
		std::vector<TerrainSeamRecord> seams;
		std::vector<TerrainJobRecord> jobs;
		std::vector<TerrainConflictRecord> conflicts;
		std::vector<TerrainLayerFallbackRecord> layerFallbacks;
		std::vector<TerrainCollisionRecord> collisions;
		std::vector<TerrainDiagnosticItem> items;
		std::vector<TerrainStagePercentile> stagePercentiles;
	};
	struct TerrainDiagnosticBuildInput
	{
		TerrainDiagnosticSnapshot snapshot;
		// Timings are supplied by a scheduler/profiler. The builder never invents
		// latency for stages that did not report a sample.
		std::vector<std::pair<std::string, std::vector<std::uint64_t>>> stageSamples;
	};

	// Adapts a provider-neutral physics snapshot into the editor/automation value
	// contract. Callers supply timing because clocks belong to the scheduler.
	TERRAENGINE_API TerrainCollisionRecord MakeTerrainCollisionRecord(
		const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& snapshot,
		std::uint64_t ageMilliseconds = 0u, std::uint64_t buildLatencyMilliseconds = 0u);
	TERRAENGINE_API TerrainDiagnosticSnapshot BuildTerrainDiagnosticSnapshot(TerrainDiagnosticBuildInput input);
}
