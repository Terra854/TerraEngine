// SpatialDiagnostics.h — Spatial System Diagnostic Types
//
// Defines the structures used to capture and report performance metrics for the spatial partitioning system.
// It tracks statistics such as object counts, query times, and structure health for both editor and runtime analysis.
// These diagnostics help developers tune spatial parameters and identify performance bottlenecks in large scenes.

#pragma once

#include "Scene/WorldCoordinate.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Scene
{
	// Describes how much spatial-index storage changed during the latest
	// update. Tooling can use this to confirm stable scenes are reusing the
	// existing tree instead of rebuilding it every frame.
	enum class SpatialRefreshMode : std::uint8_t
	{
		Uninitialized,
		Unchanged,
		Incremental,
		Full
	};

	// Transition outcomes remain scalar inspection data. Editor diagnostics can
	// explain why a movement was rejected without ever turning its exact pose into
	// a display float that another subsystem might accidentally reuse as authority.
	enum class SpatialTransitionOutcome : std::uint8_t
	{
		Queued,
		Prepared,
		Committed,
		Rejected,
		Conflicted,
		RolledBack
	};

	// Persistence failures have a stable category so tools can distinguish an
	// unsupported historic file from a damaged current-generation artifact.
	enum class SpatialPersistenceOutcome : std::uint8_t
	{
		None,
		RejectedLegacyFormat,
		RejectedMalformedCoordinate,
		RejectedHashLineage,
		RejectedGenerationChange,
		Published
	};

	struct SpatialDiagnosticRecord
	{
		std::string correlationId;
		std::string producer;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		SpatialTransitionOutcome outcome = SpatialTransitionOutcome::Queued;
		SpatialPersistenceOutcome persistenceOutcome = SpatialPersistenceOutcome::None;
		std::string reason;
		std::uint64_t observedTransformRevision = 0U;
		std::uint64_t currentTransformRevision = 0U;
		std::uint64_t observedSpatialGeneration = 0U;
		std::uint64_t publishedSpatialGeneration = 0U;
		std::uint64_t preparationTimeNs = 0U;
		std::uint64_t commitTimeNs = 0U;
		double maximumLocalizedMagnitudeMetres = 0.0;
	};

	// PublishedSpatialObject is immutable value state. It carries the canonical
	// pose and its exact revision, never a global matrix or absolute float vector.
	struct PublishedSpatialObject
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		WorldPose pose{};
		std::uint64_t transformRevision = 0U;
		std::uint64_t coordinateFrameRevision = 0U;
	};

	struct PublishedSpatialSnapshot
	{
		std::uint64_t sceneGeneration = 0U;
		std::uint64_t hierarchyRevision = 0U;
		std::uint64_t coordinateFrameRevision = 0U;
		std::uint64_t spatialGeneration = 0U;
		std::uint64_t residencyGeneration = 0U;
		std::string publicationBoundary = "Frame End";
		std::vector<PublishedSpatialObject> objects;
		std::vector<SpatialDiagnosticRecord> transitionDiagnostics;
	};

	// struct SpatialDiagnosticsSnapshot groups related state and operations so callers have one documented contract.
	struct SpatialDiagnosticsSnapshot
	{
		std::uint32_t indexedObjectCount = 0;
		std::uint32_t staticObjectCount = 0;
		std::uint32_t dynamicObjectCount = 0;
		std::uint32_t dirtyObjectCount = 0;
		std::uint32_t transformDirtyRootCount = 0;
		std::uint32_t transformExpandedObjectCount = 0;
		std::uint32_t transformProcessedObjectCount = 0;
		std::uint32_t transformSkippedCleanObjectCount = 0;
		std::uint32_t transformHierarchyLevelCount = 0;
		std::uint32_t boundsRecomputedObjectCount = 0;
		std::uint32_t invalidTransformObjectCount = 0;
		std::uint32_t invalidBoundsObjectCount = 0;
		// These refresh counters describe the index update step, not the query
		// that runs afterward for a particular camera.
		std::uint32_t addedObjectCount = 0;
		std::uint32_t updatedObjectCount = 0;
		std::uint32_t removedObjectCount = 0;
		std::uint32_t unchangedObjectCount = 0;
		std::uint32_t fullRefreshObjectCount = 0;
		std::uint32_t incrementalRefreshCount = 0;
		std::uint32_t fullRefreshCount = 0;
		std::uint32_t revisionMismatchFallbackCount = 0;
		std::uint32_t visibleCandidateCount = 0;
		std::uint32_t rejectedObjectCount = 0;
		std::uint32_t fallbackObjectCount = 0;
		std::uint32_t largeObjectCount = 0;
		std::uint32_t maxNodeDepth = 0;
		SpatialRefreshMode refreshMode = SpatialRefreshMode::Uninitialized;
		std::uint64_t publishedSpatialGeneration = 0U;
		std::uint64_t publishedSnapshotCount = 0U;
		std::uint32_t committedTransitionCount = 0U;
		std::uint32_t rejectedTransitionCount = 0U;
		std::uint32_t rollbackCount = 0U;
		std::string lastPublicationBoundary = "Frame End";
		double lastQueryDurationMs = 0.0;
		double lastUpdateDurationMs = 0.0;
	};
}
