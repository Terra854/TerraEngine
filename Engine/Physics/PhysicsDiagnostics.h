#pragma once

#include "PhysicsTypes.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Physics {
	struct PhysicsDebugLine { PhysicsVector3 start, end; std::uint32_t color = 0xFFFFFFFFU; };
	struct PhysicsDebugTriangle { PhysicsVector3 a, b, c; std::uint32_t color = 0xFFFFFFFFU; };
	struct PhysicsDebugContact { PhysicsVector3 position, normal; float impulse = 0.0F; };
	struct PhysicsDebugConstraint { PhysicsVector3 firstAnchor, secondAnchor; bool broken = false; };
	struct PhysicsDebugCharacter { PhysicsVector3 position, up; float radius = 0.0F; float height = 0.0F; };
	struct PhysicsDebugQuery { PhysicsVector3 origin, direction; float distance = 0.0F; };

	// A selection is deliberately value-only: editor and renderer jobs can choose
	// what to display without retaining a PhysicsWorld or a provider object.
	struct PhysicsDebugDisplaySelection {
		bool showLines = true;
		bool showTriangles = true;
		bool showContacts = true;
		bool showConstraints = true;
		bool showCharacterSupport = true;
		bool showRecentQueries = true;
		std::size_t maximumPackets = 8U;
	};

	// Lifetime metadata lets renderers discard old debug packets without owning
	// or touching the simulation world.
	struct PhysicsDebugPacket {
		PhysicsWorldHandle world;
		PhysicsTick producedTick = 0;
		PhysicsTick expiryTick = 0;
		std::shared_ptr<const std::vector<PhysicsDebugLine>> lines;
		std::shared_ptr<const std::vector<PhysicsDebugTriangle>> triangles;
		std::shared_ptr<const std::vector<PhysicsDebugContact>> contacts;
		std::shared_ptr<const std::vector<PhysicsDebugConstraint>> constraints;
		std::shared_ptr<const std::vector<PhysicsDebugCharacter>> characters;
		std::shared_ptr<const std::vector<PhysicsDebugQuery>> queries;

		// Validity is evaluated only from published scalar metadata, keeping paused
		// Scene View redraws independent from a live simulation/backend call.
		[[nodiscard]] bool IsValidFor(PhysicsWorldHandle activeWorld, PhysicsTick currentTick) const noexcept;
	};

	// This bounded store is the publication boundary between physics extraction
	// and diagnostics consumers. It retains only immutable shared allocations.
	class PhysicsDebugPacketHistory final {
	public:
		explicit PhysicsDebugPacketHistory(std::size_t capacity = 32U);
		void Publish(std::shared_ptr<const PhysicsDebugPacket> packet);
		[[nodiscard]] std::vector<std::shared_ptr<const PhysicsDebugPacket>> Select(
			PhysicsWorldHandle activeWorld,
			PhysicsTick currentTick,
			const PhysicsDebugDisplaySelection& selection = {}) const;
		[[nodiscard]] std::size_t Size() const;
		void Clear();

	private:
		std::size_t capacity;
		mutable std::mutex mutex;
		std::vector<std::shared_ptr<const PhysicsDebugPacket>> packets;
	};

	// The global history is intentionally opt-in data plumbing. It does no work
	// until PhysicsSystem publishes a packet at its protected physics boundary.
	[[nodiscard]] PhysicsDebugPacketHistory& PublishedPhysicsDebugPackets();

	struct PhysicsDiagnosticsCounters {
		std::uint64_t bodyCount = 0, shapeCount = 0, constraintCount = 0, characterCount = 0, contactCount = 0;
		std::uint64_t commandCount = 0, commandCancelledCount = 0;
		std::uint64_t queryCount = 0, queryHitCount = 0, queryTruncatedCount = 0;
		std::uint64_t eventCount = 0, eventFilteredCount = 0, eventDroppedCount = 0;
		std::uint64_t rejectedCount = 0, staleCount = 0;
	};
	struct PhysicsDiagnosticsTimings {
		double prepareMs = 0.0, commitMs = 0.0, simulateMs = 0.0, queryMs = 0.0;
		double extractionMs = 0.0, eventPublishMs = 0.0, stateValidationMs = 0.0;
		double snapshotConstructionMs = 0.0, transactionConstructionMs = 0.0, publishMs = 0.0;
	};

	// Stage availability is explicit so an unsupported provider counter cannot be
	// mistaken for a measured zero by the profiler or installed evidence tools.
	enum class PhysicsMetricAvailability : std::uint8_t {
		Available,
		Unavailable,
		NotApplicable
	};

	enum class PhysicsMetricUnavailableReason : std::uint8_t {
		None,
		ProviderDoesNotReportBytes,
		StageDidNotRun,
		CounterNotSupported
	};

	enum class PhysicsStageKind : std::uint8_t {
		AuthoringPreparation,
		ProviderCommit,
		Simulation,
		Extraction,
		EventPublication,
		StateValidation,
		SnapshotConstruction,
		TransactionConstruction,
		Count
	};

	struct PhysicsStageMeasurement {
		PhysicsStageKind stage = PhysicsStageKind::AuthoringPreparation;
		PhysicsMetricAvailability availability = PhysicsMetricAvailability::NotApplicable;
		PhysicsMetricUnavailableReason unavailableReason = PhysicsMetricUnavailableReason::StageDidNotRun;
		std::uint64_t durationNanoseconds = 0U;
		std::uint64_t workItems = 0U;
		std::uint64_t allocationCount = 0U;
		std::uint64_t allocationBytes = 0U;
		bool allocationCountAvailable = false;
		bool allocationBytesAvailable = false;
	};

	struct PhysicsSimulationQualityDiagnostics {
		std::uint32_t settingsSchemaVersion = 0U;
		std::uint64_t settingsGeneration = 0U;
		PhysicsGeneration worldGeneration = 0U;
		std::uint32_t velocityIterations = 0U;
		std::uint32_t positionIterations = 0U;
		bool applied = false;
	};

	// This is a copied provider-neutral view of the shared-pool bridge. Mutable
	// provider jobs, barriers, and thread objects never cross this boundary.
	struct PhysicsJobTelemetry {
		std::uint64_t submitted = 0U, completed = 0U, rejected = 0U;
		std::uint64_t batchCount = 0U, lockAcquisitionCount = 0U, pumpSubmissionCount = 0U;
		std::uint64_t barrierWaitCount = 0U, barrierWaitMicroseconds = 0U;
		std::uint64_t maximumBarrierWaitMicroseconds = 0U, jobExecutionMicroseconds = 0U;
		std::uint32_t queued = 0U, activePumps = 0U, maximumActivePumps = 0U;
		std::uint32_t workerLimit = 0U, observedWorkerThreads = 0U;
		bool idle = true;
	};
	struct PhysicsDiagnosticsMemory {
		// Jolt's free callback does not report allocation sizes, so live byte totals
		// are intentionally not fabricated. Exact allocation counts accompany the
		// engine-owned snapshot byte estimate and its observed high-water mark.
		std::uint64_t providerBytes = 0, snapshotBytes = 0, highWaterBytes = 0;
		std::uint64_t outstandingProviderAllocations = 0, highWaterProviderAllocations = 0;
	};
	struct PhysicsDiagnosticsCache {
		std::uint64_t sharedShapeCount = 0, assetCount = 0, payloadBytes = 0;
		std::uint64_t hitCount = 0, missCount = 0, publicationCount = 0, retirementCount = 0;
	};
	struct PhysicsDiagnosticsQueues { std::uint64_t commandDepth = 0, queryDepth = 0, eventDepth = 0; };
	struct PhysicsSchedulerDeclaration {
		std::string workload, phase, reads, writes, boundary;
		bool workerSafe = false;
		// Activation and dependency facts mirror the stable scheduler descriptor
		// without retaining its callbacks or mutable frame inputs.
		bool active = true;
		std::string activationReason;
		std::string dependencies;
		std::uint64_t estimatedItems = 0U;
	};

	// Incremental-pipeline diagnostics describe work volume rather than only the
	// final body count. All fields are copied values so editor and automation
	// readers never retain an authoring snapshot, provider range, or transaction.
	struct PhysicsAuthoringDiagnostics {
		std::uint64_t authoringGeneration = 0U, snapshotGeneration = 0U, assetJournalGeneration = 0U;
		std::uint64_t sceneGeneration = 0U, scenePublicationRevision = 0U;
		std::uint64_t changedObjects = 0U, added = 0U, removed = 0U, transforms = 0U;
		std::uint64_t bodyComponents = 0U, colliderComponents = 0U, hierarchyDependencies = 0U;
		std::uint64_t colliderAssets = 0U, materialAssets = 0U, settings = 0U, lifecycle = 0U;
		std::uint64_t assetObservations = 0U, constraintClosureCount = 0U;
		std::uint64_t characterClosureCount = 0U, removalClosureCount = 0U;
		// Mirrors PhysicsAuthoringFallbackReason without retaining authoring state.
		std::uint8_t fallbackReason = 0U;
		bool snapshotReused = false;
		bool fullAssetRefresh = false;
	};

	struct PhysicsPreparationDiagnostics {
		std::string executionPath = "fast";
		std::uint64_t estimatedWorkItems = 0U, rangeCount = 0U;
		std::uint64_t creates = 0U, removals = 0U, updates = 0U, dependencyRebuilds = 0U;
		std::uint64_t scratchActiveItems = 0U, scratchHighWatermark = 0U;
		std::uint64_t scratchAllocations = 0U, scratchReuses = 0U;
		bool rejected = false;
		std::string failure;
	};

	struct PhysicsExtractionDiagnostics {
		std::uint64_t providerOrderGeneration = 0U;
		std::uint64_t totalBodies = 0U, activeBodies = 0U, examinedBodies = 0U;
		std::uint64_t extractedBodies = 0U, convertedBodies = 0U, changedBodies = 0U, removedBodies = 0U;
		std::uint64_t rangeCount = 0U;
		std::uint32_t fieldMask = 0U;
		bool fullState = false, parallelReads = false, serialFallback = false, eventDrain = false;
	};

	struct PhysicsStateStoreDiagnostics {
		std::uint64_t storeGeneration = 0U, providerOrderGeneration = 0U;
		std::uint64_t publicationCount = 0U, rejectedBatchCount = 0U;
		std::uint64_t pageCount = 0U, pagesReused = 0U, pagesCopied = 0U;
		std::uint64_t bodyCount = 0U, retainedBytes = 0U;
	};

	struct PhysicsPublicationDiagnostics {
		std::uint64_t preparedPackets = 0U, acceptedPackets = 0U, rejectedPackets = 0U;
		std::uint64_t acknowledgedEntries = 0U, emptyPacketsSkipped = 0U;
		std::uint64_t currentEntries = 0U, currentChunks = 0U, pendingGroups = 0U;
		std::string targetBoundary = "frame_end";
		std::string lastFailure;
	};

	struct PhysicsDiagnosticsRate {
		std::uint32_t aggregateIntervalFrames = 1U;
		PhysicsTick lastAggregateTick = 0U;
		bool aggregateRefreshed = true;
		// Health and failure fields are always current even when aggregate work
		// volume is intentionally held at the configured publication cadence.
		bool currentHealthPublished = true;
	};

	struct PhysicsDiagnosticsSnapshot {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0;
		PhysicsWorldState state = PhysicsWorldState::Absent;
		PhysicsDiagnosticsCounters counters;
		PhysicsDiagnosticsTimings timings;
		PhysicsDiagnosticsMemory memory;
		PhysicsDiagnosticsCache cache;
		PhysicsDiagnosticsQueues queues;
		std::array<PhysicsStageMeasurement, static_cast<std::size_t>(PhysicsStageKind::Count)> stages{};
		PhysicsSimulationQualityDiagnostics quality;
		PhysicsJobTelemetry jobs;
		PhysicsAuthoringDiagnostics authoring;
		PhysicsPreparationDiagnostics preparation;
		PhysicsExtractionDiagnostics extraction;
		PhysicsStateStoreDiagnostics stateStore;
		PhysicsPublicationDiagnostics publication;
		PhysicsDiagnosticsRate rate;
		std::shared_ptr<const std::vector<PhysicsSchedulerDeclaration>> scheduler;
	};

	// Aggregate counters may be rate limited, but a failure always forces a new
	// aggregate so a slow diagnostics cadence can never hide current health.
	[[nodiscard]] bool ShouldRefreshPhysicsAggregateDiagnostics(
		PhysicsTick tick,
		std::uint32_t intervalFrames,
		PhysicsTick lastAggregateTick,
		bool currentFailure) noexcept;

	// Diagnostics consumers obtain a copied shared snapshot, never a world or
	// mutable counter reference. Publishing is performed by PhysicsSystem only.
	void PublishPhysicsDiagnosticsSnapshot(std::shared_ptr<const PhysicsDiagnosticsSnapshot> snapshot);
	[[nodiscard]] std::shared_ptr<const PhysicsDiagnosticsSnapshot> AcquirePhysicsDiagnosticsSnapshot();
}
