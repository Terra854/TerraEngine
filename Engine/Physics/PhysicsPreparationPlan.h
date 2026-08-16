// PhysicsPreparationPlan.h - deterministic worker-ready authoring operations.
//
// Plans contain only engine-owned values. They may be partitioned on the shared
// ThreadPool, while provider commits remain in the later protected phase.

#pragma once

#include "Core/Export.h"
#include "PhysicsAuthoringGeneration.h"
#include "PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ThreadPool;

namespace TerraEngine::Physics {
	enum class PhysicsPreparationExecutionPath : std::uint8_t {
		Fast,
		Inline,
		WorkerRanges,
		Rejected
	};

	enum class PhysicsPreparationWorkKind : std::uint8_t {
		CreateBody,
		RemoveBody,
		UpdateBody,
		RebuildDependency,
		Constraint,
		Character,
		Command,
		Query
	};

	struct PhysicsPreparationOperation {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		PhysicsGeneration objectGeneration = 0U;
		PhysicsDescriptorIndex descriptorIndex = kInvalidPhysicsDescriptorIndex;
		PhysicsAuthoringChangeReason reasons = PhysicsAuthoringChangeReason::None;
	};

	struct PhysicsPreparationWorkItem {
		PhysicsPreparationWorkKind kind = PhysicsPreparationWorkKind::CreateBody;
		std::size_t stableOrdinal = 0U;
		PhysicsPreparationOperation operation;
		PhysicsCommandId commandId = 0U;
		PhysicsQueryHandle query;
	};

	struct PhysicsPreparationRange {
		PhysicsPreparationWorkKind kind = PhysicsPreparationWorkKind::CreateBody;
		std::size_t begin = 0U;
		std::size_t count = 0U;
		std::size_t stableRangeOrdinal = 0U;
	};

	struct PhysicsPreparationPlanOptions {
		std::size_t parallelThreshold = 512U;
		std::size_t rangeSize = 256U;
		std::size_t availableWorkerCount = 1U;
		std::vector<PhysicsPreparationOperation> constraintOperations;
		std::vector<PhysicsPreparationOperation> characterOperations;
		std::vector<PhysicsCommandId> commands;
		std::vector<PhysicsQueryHandle> queries;
	};

	struct PhysicsPreparationPlanData {
		PhysicsWorldHandle world;
		// A plan belongs to one frame/island. Scheduler code partitions plans by
		// this immutable key before workers prepare provider-independent records.
		PhysicsFrameHandle frame;
		PhysicsGeneration authoringGeneration = 0U;
		PhysicsTick tick = 0U;
		PhysicsPreparationExecutionPath executionPath = PhysicsPreparationExecutionPath::Fast;
		std::size_t estimatedWorkItems = 0U;
		std::vector<PhysicsPreparationOperation> creates;
		std::vector<PhysicsPreparationOperation> removals;
		std::vector<PhysicsPreparationOperation> updates;
		std::vector<PhysicsPreparationOperation> dependencyRebuilds;
		std::vector<PhysicsPreparationOperation> constraints;
		std::vector<PhysicsPreparationOperation> characters;
		std::vector<PhysicsCommandId> commands;
		std::vector<PhysicsQueryHandle> queries;
		std::vector<PhysicsPreparationWorkItem> workItems;
		std::vector<PhysicsPreparationRange> ranges;
		std::string rejectionReason;
	};

	class TERRAENGINE_API PhysicsPreparationPlan final {
	public:
		static std::optional<PhysicsPreparationPlan> Build(
			const PhysicsAuthoringGeneration& generation,
			PhysicsTick tick,
			PhysicsPreparationPlanOptions options = {});
		static std::optional<PhysicsPreparationPlan> Create(
			PhysicsPreparationPlanData data);

		const PhysicsPreparationPlanData& Data() const noexcept { return *data; }
		bool IsValid() const noexcept { return data != nullptr; }
		bool Empty() const noexcept { return data == nullptr || data->workItems.empty(); }

	private:
		explicit PhysicsPreparationPlan(std::shared_ptr<const PhysicsPreparationPlanData> value)
			: data(std::move(value)) {}

		std::shared_ptr<const PhysicsPreparationPlanData> data;
	};

	class PhysicsPreparationScratch;
	TERRAENGINE_API bool ExecutePhysicsPreparationPlan(
		const PhysicsPreparationPlan& plan,
		ThreadPool& threadPool,
		PhysicsPreparationScratch& scratch,
		const std::function<bool(const PhysicsPreparationWorkItem&)>& prepare);

	struct PhysicsPreparationScratchSlot {
		PhysicsGeneration generation = 0U;
		PhysicsPreparationWorkItem item;
		bool attempted = false;
		bool accepted = false;
	};

	// Scratch storage is deliberately value-only and capacity-bounded. Starting a
	// generation clears all previously active slots before any worker sees them.
	class TERRAENGINE_API PhysicsPreparationScratch final {
	public:
		explicit PhysicsPreparationScratch(std::size_t maximumItems = 65536U);
		PhysicsPreparationScratch(const PhysicsPreparationScratch&) = delete;
		PhysicsPreparationScratch& operator=(const PhysicsPreparationScratch&) = delete;
		PhysicsPreparationScratch(PhysicsPreparationScratch&&) = delete;
		PhysicsPreparationScratch& operator=(PhysicsPreparationScratch&&) = delete;

		bool BeginGeneration(PhysicsGeneration generation, std::size_t itemCount);
		std::size_t MaximumItems() const noexcept { return maximumItems; }
		std::size_t ActiveItems() const noexcept { return activeItems; }
		std::size_t HighWatermark() const noexcept { return highWatermark; }
		std::size_t AllocationCount() const noexcept { return allocationCount; }
		std::size_t ReuseCount() const noexcept { return reuseCount; }
		const std::vector<PhysicsPreparationScratchSlot>& Slots() const noexcept { return slots; }

	private:
		friend TERRAENGINE_API bool ExecutePhysicsPreparationPlan(
			const PhysicsPreparationPlan&,
			ThreadPool&,
			PhysicsPreparationScratch&,
			const std::function<bool(const PhysicsPreparationWorkItem&)>&);

		const std::size_t maximumItems;
		std::size_t activeItems = 0U;
		std::size_t highWatermark = 0U;
		std::size_t allocationCount = 0U;
		std::size_t reuseCount = 0U;
		std::vector<PhysicsPreparationScratchSlot> slots;
		// Futures are move-only, but retaining their vector capacity avoids a second
		// per-generation allocation alongside the result slots.
		std::vector<std::future<void>> futures;
	};

	// The callback declared above must use only the supplied immutable work item
	// and write to caller-owned range-disjoint output. Results are recorded by
	// stable ordinal, so completion order never changes protected commit order.
}
