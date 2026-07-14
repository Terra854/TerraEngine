#pragma once

#include "PhysicsTypes.h"

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
		double eventPublishMs = 0.0, publishMs = 0.0;
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
	struct PhysicsSchedulerDeclaration { std::string workload, phase, reads, writes, boundary; bool workerSafe = false; };

	struct PhysicsDiagnosticsSnapshot {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0;
		PhysicsWorldState state = PhysicsWorldState::Absent;
		PhysicsDiagnosticsCounters counters;
		PhysicsDiagnosticsTimings timings;
		PhysicsDiagnosticsMemory memory;
		PhysicsDiagnosticsCache cache;
		PhysicsDiagnosticsQueues queues;
		std::shared_ptr<const std::vector<PhysicsSchedulerDeclaration>> scheduler;
	};

	// Diagnostics consumers obtain a copied shared snapshot, never a world or
	// mutable counter reference. Publishing is performed by PhysicsSystem only.
	void PublishPhysicsDiagnosticsSnapshot(std::shared_ptr<const PhysicsDiagnosticsSnapshot> snapshot);
	[[nodiscard]] std::shared_ptr<const PhysicsDiagnosticsSnapshot> AcquirePhysicsDiagnosticsSnapshot();
}
