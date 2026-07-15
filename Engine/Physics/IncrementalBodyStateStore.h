// IncrementalBodyStateStore.h - immutable paged complete physics state.
//
// Provider workers prepare engine-owned updates. Apply validates and seals only
// touched pages, then publishes a complete snapshot and its changed packet as one
// same-tick value. Readers never observe mutable page storage.

#pragma once

#include "Core/Export.h"
#include "PhysicsAuthoringSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace TerraEngine::Physics {
	struct IncrementalBodyStateUpdate {
		PhysicsBodyState state;
		PhysicsBodyHandle removedHandle;
		PhysicsBodyStateFieldMask fields = PhysicsBodyStateFieldMask::None;
		PhysicsBodyStateChangeMask change = PhysicsBodyStateChangeMask::None;
		bool removed = false;
	};

	struct IncrementalBodyStateUpdateBatch {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
		std::uint64_t providerOrderGeneration = 0U;
		std::size_t expectedCompleteBodyCount = 0U;
		bool fullState = false;
		std::vector<IncrementalBodyStateUpdate> updates;
	};

	struct IncrementalBodyStatePage {
		std::uint32_t pageIndex = 0U;
		std::uint64_t generation = 0U;
		std::size_t liveBodyCount = 0U;
		std::vector<std::optional<PhysicsBodyState>> slots;
	};

	struct IncrementalBodyStateSnapshot {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
		std::uint64_t storeGeneration = 0U;
		std::uint64_t providerOrderGeneration = 0U;
		PhysicsTick lastFullStateVerificationTick = 0U;
		std::size_t pageSize = 0U;
		std::size_t bodyCount = 0U;
		std::size_t retainedBytes = 0U;
		std::vector<std::shared_ptr<const IncrementalBodyStatePage>> pages;

		// Find validates the complete generation-bearing handle, so reuse of one
		// compact slot cannot make a stale reader observe a replacement body.
		const PhysicsBodyState* Find(PhysicsBodyHandle handle) const noexcept;
		std::vector<PhysicsBodyState> Flatten() const;
	};

	struct ChangedBodyStateRecord {
		PhysicsBodyHandle handle;
		std::optional<PhysicsBodyState> state;
		PhysicsBodyStateFieldMask fields = PhysicsBodyStateFieldMask::None;
		PhysicsBodyStateChangeMask change = PhysicsBodyStateChangeMask::None;
		bool removed = false;
	};

	struct ChangedBodyStatePacket {
		PhysicsWorldHandle world;
		PhysicsTick tick = 0U;
		std::uint64_t storeGeneration = 0U;
		std::shared_ptr<const std::vector<ChangedBodyStateRecord>> changes;
	};

	struct IncrementalBodyStatePublication {
		std::shared_ptr<const IncrementalBodyStateSnapshot> complete;
		std::shared_ptr<const ChangedBodyStatePacket> changed;
		std::size_t pagesReused = 0U;
		std::size_t pagesCopied = 0U;
	};

	struct IncrementalBodyStateStoreConfiguration {
		std::size_t pageSize = 256U;
		std::size_t maximumSlots = 1U << 20U;
		std::size_t maximumRetainedBytes = 64U * 1024U * 1024U;
	};

	struct IncrementalBodyStateStoreStatistics {
		std::uint64_t publicationCount = 0U;
		std::uint64_t rejectedBatchCount = 0U;
		std::uint64_t pagesReused = 0U;
		std::uint64_t pagesCopied = 0U;
		std::size_t retainedBytes = 0U;
	};

	class TERRAENGINE_API IncrementalBodyStateStore final {
	public:
		explicit IncrementalBodyStateStore(IncrementalBodyStateStoreConfiguration configuration = {});

		IncrementalBodyStateStore(const IncrementalBodyStateStore&) = delete;
		IncrementalBodyStateStore& operator=(const IncrementalBodyStateStore&) = delete;

		std::optional<IncrementalBodyStatePublication> Apply(const IncrementalBodyStateUpdateBatch& batch);
		std::shared_ptr<const IncrementalBodyStateSnapshot> AcquireComplete() const;
		std::shared_ptr<const ChangedBodyStatePacket> AcquireChanged() const;
		IncrementalBodyStateStoreStatistics Statistics() const;
		void Reset(PhysicsWorldHandle world = {});

	private:
		IncrementalBodyStateStoreConfiguration configuration;
		mutable std::mutex mutex;
		std::uint64_t generation = 0U;
		PhysicsWorldHandle expectedWorld;
		std::shared_ptr<const IncrementalBodyStateSnapshot> latestComplete;
		std::shared_ptr<const ChangedBodyStatePacket> latestChanged;
		IncrementalBodyStateStoreStatistics statistics;
	};
}
