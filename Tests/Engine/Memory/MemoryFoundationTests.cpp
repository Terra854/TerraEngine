// MemoryFoundationTests.cpp - Correctness, pressure, and isolation contracts.

#include "TestHarness.h"

#include "Core/Memory/FixedBlockPool.h"
#include "Core/Memory/MemoryStrategy.h"
#include "Core/Memory/ReusableArenaResource.h"
#include "Core/Memory/TrackingMemoryResource.h"
#include "Runtime/CorrectnessTestSupport.h"
#include "ThreadPool/ThreadPool.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <memory_resource>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		// Each fixture owns its counters, so later worker jobs can exercise separate
		// pools without making either the pool or its value type shared mutable state.
		struct PoolOwnershipValue {
			std::size_t* destructions;
			std::uint64_t value;
			std::uint64_t padding = 0;
			PoolOwnershipValue(std::size_t& counter, std::uint64_t supplied)
				: destructions(&counter), value(supplied) {}
			~PoolOwnershipValue() { ++*destructions; }
		};

		struct alignas(128) AlignedPoolValue {
			std::size_t* destructions;
			std::uint64_t value;
			AlignedPoolValue(std::size_t& counter, std::uint64_t supplied)
				: destructions(&counter), value(supplied) {}
			~AlignedPoolValue() { ++*destructions; }
		};

		struct PoolJobObservation {
			std::thread::id thread;
			std::size_t destructions;
			std::size_t rejections;
		};

		PoolJobObservation ExerciseIndependentPool()
		{
			// The pool, values, counters and assertions all belong to this one job.
			// Parallel coverage does not grant concurrent access to a shared pool.
			TestContext context;
			std::size_t destructions = 0U;
			std::size_t foreignDestructions = 0U;
			std::size_t rejections = 0U;
			PoolOwnershipValue foreign(foreignDestructions, 991U);
			Memory::FixedBlockPool<PoolOwnershipValue, 2U> pool;
			for (std::size_t cycle = 0U; cycle < 256U; ++cycle) {
				auto* first = pool.Create(destructions, cycle);
				auto* neighbor = pool.Create(destructions, cycle + 1U);
				context.Expect(first && neighbor, "each isolated cycle must retain both fixed slots");
				auto* interior = reinterpret_cast<PoolOwnershipValue*>(&first->value);
				for (auto* candidate : { static_cast<PoolOwnershipValue*>(nullptr), &foreign, interior }) {
					context.Expect(!pool.Destroy(candidate) && destructions == cycle * 3U
						&& foreignDestructions == 0U && pool.Size() == 2U && pool.HighWater() == 2U
						&& first->value == cycle && neighbor->value == cycle + 1U,
						"invalid destruction must preserve this job's complete live state");
					++rejections;
				}
				context.Expect(pool.Destroy(first) && destructions == cycle * 3U + 1U,
					"each cycle must destroy its first owned lifetime once");
				for (unsigned repeat = 0U; repeat < 2U; ++repeat) {
					context.Expect(!pool.Destroy(first) && destructions == cycle * 3U + 1U
						&& pool.Size() == 1U && pool.HighWater() == 2U && neighbor->value == cycle + 1U,
						"unoccupied rejection must leave the neighbor and reusable slot intact");
					++rejections;
				}
				auto* replacement = pool.Create(destructions, cycle + 2U);
				context.Expect(replacement == first && replacement->value == cycle + 2U
					&& pool.Create(destructions, 0U) == nullptr,
					"reuse must return exactly the freed slot without increasing capacity");
				pool.Clear();
				pool.Clear();
				context.Expect(destructions == (cycle + 1U) * 3U && pool.Size() == 0U
					&& pool.HighWater() == 2U && foreignDestructions == 0U,
					"clear must settle each successful lifetime exactly once before the next cycle");
			}
			return { std::this_thread::get_id(), destructions, rejections };
		}

		// Lifetime counters belong to completed objects. The red-state guard below
		// prevents cleanup from touching a slot whose construction did not complete.
		struct ThrowingPoolValue {
			inline static int constructions = 0;
			inline static int destructions = 0;
			int value = 0;
			ThrowingPoolValue(bool fail, int supplied = 0, const std::function<void()>& before = {}) : value(supplied) {
				if (before) before();
				if (fail) throw std::runtime_error("fixed-pool constructor checkpoint");
				++constructions;
			}
			~ThrowingPoolValue() { ++destructions; }
			static void ResetCounters() { constructions = 0; destructions = 0; }
		};

		template<std::size_t Capacity>
		bool RejectPoolConstruction(Memory::FixedBlockPool<ThrowingPoolValue, Capacity>& pool) {
			try { (void)pool.Create(true); }
			catch (const std::runtime_error&) { return true; }
			return false;
		}

		template<std::size_t Capacity>
		void RequireSafeFailedReservation(TestContext& context,
			std::unique_ptr<Memory::FixedBlockPool<ThrowingPoolValue, Capacity>>& pool,
			std::size_t expectedSize, std::size_t expectedHighWater) {
			const auto size = pool->Size();
			const auto highWater = pool->HighWater();
			if (size != expectedSize || highWater != expectedHighWater) {
				// The old implementation advertises an unconstructed slot as live.
				// Its destructor would invoke undefined behavior. Abandon this bounded
				// invalid red-only fixture and fail immediately; the test process owns
				// its reclamation. A successful test always retains normal RAII cleanup.
				(void)pool.release();
				std::cout << "red-only invalid pool abandoned before unsafe cleanup: size="
					<< size << " highWater=" << highWater << '\n';
				context.Expect(false, "failed construction changed successful size/high-water accounting");
			}
		}

		void ObservePool(const char* caseId, std::size_t size, std::size_t highWater) {
			// Emit measured facts only after assertions and normal owned cleanup.
			std::cout << "RMS_OBSERVATION {\"caseId\":\"" << caseId
				<< "\",\"size\":" << size << ",\"highWater\":" << highWater
				<< ",\"successfulConstructions\":" << ThrowingPoolValue::constructions
				<< ",\"destructions\":" << ThrowingPoolValue::destructions << "}\n";
		}
	}

	void RegisterMemoryFoundationTests(TestRegistry& registry)
	{
		using namespace TerraEngine::Memory;

		registry.Add("Engine.Memory.StrategiesDefaultAndSelectIndependently", [](TestContext& context) {
			const MemoryStrategyConfiguration standard = MemoryStrategyConfiguration::Parse({});
			const MemoryStrategyConfiguration selected = MemoryStrategyConfiguration::Parse(
				"animation=reusable-arena,resource=reusable-arena,unknown=reusable-arena");
			const MemoryStrategyConfiguration removedResourceSelector =
				MemoryStrategyConfiguration::Parse("resource=reusable-arena");
			context.Expect(standard.For(MemoryDomain::AnimationBinding) == MemoryStrategy::Standard,
				"an absent experiment policy must preserve the STL-backed standard route");
			context.Expect(selected.For(MemoryDomain::AnimationBinding) == MemoryStrategy::ReusableArena
					&& selected.For(MemoryDomain::PhysicsPreparation) == MemoryStrategy::Standard,
				"domain selection must not enable unrelated candidates");
			context.Expect(removedResourceSelector.For(MemoryDomain::AnimationBinding) == MemoryStrategy::Standard
					&& removedResourceSelector.For(MemoryDomain::PhysicsPreparation) == MemoryStrategy::Standard,
				"the promoted resource path must no longer expose its removed opt-in selector");
		});

		registry.Add("Engine.Memory.ReusableArenaAlignsResetsAndReportsPressure", [](TestContext& context) {
			ReusableArenaResource arena(256U);
			std::pmr::vector<std::uint64_t> values(&arena);
			values.resize(16U, 42U);
			const auto first = arena.Statistics();
			context.Expect(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(std::uint64_t) == 0U
					&& first.reuseAllocationCount > 0U && first.fallbackAllocationCount == 0U,
				"in-capacity allocations must be aligned and served by fixed backing");
			values.clear();
			values.shrink_to_fit();
			arena.Reset();
			context.Expect(arena.Statistics().usedBytes == 0U && arena.Statistics().resetCount == 1U
					&& arena.Statistics().highWaterBytes >= 128U,
				"reset must preserve high-water evidence while making backing reusable");
			TrackingMemoryResource upstream;
			{
				ReusableArenaResource fallback(0U, &upstream);
				void* first = fallback.allocate(512U, 64U);
				(void)fallback.allocate(1024U, 256U);
				const auto allocated = fallback.Statistics();
				context.Expect(allocated.fallbackAllocationCount == upstream.Statistics().allocationCount
					&& allocated.fallbackDeallocationCount == upstream.Statistics().deallocationCount
					&& allocated.fallbackOutstandingCount == 2U
					&& allocated.fallbackOutstandingBytes == upstream.Statistics().bytesInUse,
					"committed fallback diagnostics must match actual upstream ownership");
				fallback.deallocate(first, 512U, 64U);
				context.Expect(fallback.Statistics().fallbackOutstandingCount == 2U
					&& upstream.Statistics().deallocationCount == 0U,
					"individual deallocation must not fabricate reset-scoped reclamation");
				fallback.Reset();
				fallback.Reset();
				const auto reclaimed = fallback.Statistics();
				context.Expect(reclaimed.fallbackAllocationCount == 2U && reclaimed.fallbackDeallocationCount == 2U
					&& reclaimed.fallbackDeallocationCount == upstream.Statistics().deallocationCount
					&& reclaimed.fallbackOutstandingCount == 0U && reclaimed.fallbackOutstandingBytes == 0U
					&& reclaimed.resetCount == 2U && upstream.Statistics().bytesInUse == 0U,
					"repeated resets must preserve actual deallocation totals instead of counting reset calls");
				(void)fallback.allocate(256U, 64U);
				context.Expect(fallback.Statistics().fallbackAllocationCount == 3U
					&& fallback.Statistics().fallbackDeallocationCount == 2U
					&& fallback.Statistics().fallbackOutstandingCount == 1U
					&& fallback.Statistics().fallbackOutstandingBytes == 256U,
					"healthy reuse must extend cumulative counts and expose only new outstanding bytes");
			}
			context.Expect(upstream.Statistics().allocationCount == 3U && upstream.Statistics().deallocationCount == 3U
				&& upstream.Statistics().bytesInUse == 0U, "destruction must reclaim the final actual fallback allocation");
		});

		registry.Add("Engine.Memory.TrackingResourceBalancesOwnedBytes", [](TestContext& context) {
			TrackingMemoryResource tracked;
			{
				std::pmr::vector<std::uint32_t> values(&tracked);
				values.resize(64U, 7U);
				context.Expect(tracked.Statistics().allocationCount > 0U
						&& tracked.Statistics().bytesInUse >= 64U * sizeof(std::uint32_t),
					"tracked allocations must expose attributable counts and bytes");
			}
			context.Expect(tracked.Statistics().bytesInUse == 0U
					&& tracked.Statistics().deallocationCount == tracked.Statistics().allocationCount,
				"container destruction must balance every tracked allocation");
		});

		registry.Add("Engine.Memory.FixedPoolRejectsPressureAndReusesBlocks", [](TestContext& context) {
			FixedBlockPool<std::uint64_t, 2U> pool;
			std::uint64_t* first = pool.Create(11U);
			std::uint64_t* second = pool.Create(12U);
			context.Expect(first != nullptr && second != nullptr && pool.Create(13U) == nullptr,
				"a full pool must reject pressure instead of growing");
			const bool destroyed = pool.Destroy(first);
			std::uint64_t* reused = pool.Create(14U);
			context.Expect(destroyed && reused == first && *reused == 14U
					&& pool.Size() == 2U && pool.HighWater() == 2U,
				"destroyed blocks must be reused without changing the fixed bound");
		});

		registry.Add("Engine.Memory.FixedPoolRejectsForeignLiveStorageWithoutMutation", [](TestContext& context) {
			std::size_t destructions = 0;
			FixedBlockPool<PoolOwnershipValue, 3U> pool;
			auto* first = pool.Create(destructions, 11U);
			auto* second = pool.Create(destructions, 22U);
			PoolOwnershipValue stack(destructions, 33U);
			auto heap = std::make_unique<PoolOwnershipValue>(destructions, 44U);
			FixedBlockPool<PoolOwnershipValue, 1U> other;
			auto* otherValue = other.Create(destructions, 55U);
			context.Expect(first && second && otherValue, "fixture allocations must succeed");
			// Foreign storage remains alive throughout every call. These cases expose
			// the contract, but runtime success alone cannot prove the old unrelated
			// pointer subtraction was defined; source review also verifies its removal.
			for (auto* candidate : { static_cast<PoolOwnershipValue*>(nullptr), &stack, heap.get(), otherValue }) {
				context.Expect(!pool.Destroy(candidate) && destructions == 0U
					&& pool.Size() == 2U && pool.HighWater() == 2U
					&& first->value == 11U && second->value == 22U
					&& stack.value == 33U && heap->value == 44U && otherValue->value == 55U,
					"foreign rejection must preserve every live value, destructor count and pool observation");
			}
			auto* third = pool.Create(destructions, 66U);
			context.Expect(third && third != first && third != second && pool.Create(destructions, 77U) == nullptr,
				"rejected foreign values must not consume or duplicate the remaining free slot");
		});

		registry.Add("Engine.Memory.FixedPoolRejectsInteriorAndUnoccupiedSlots", [](TestContext& context) {
			std::size_t destructions = 0;
			FixedBlockPool<PoolOwnershipValue, 2U> pool;
			auto* first = pool.Create(destructions, 71U);
			auto* neighbor = pool.Create(destructions, 72U);
			context.Expect(first && neighbor, "fixture allocations must succeed");
			// Convert an aligned member address inside live storage only for equality
			// comparison. It is never dereferenced as a PoolOwnershipValue object.
			auto* interior = reinterpret_cast<PoolOwnershipValue*>(&first->value);
			context.Expect(!pool.Destroy(interior) && destructions == 0U && pool.Size() == 2U
				&& pool.HighWater() == 2U && first->value == 71U && neighbor->value == 72U,
				"an interior address must not authorize destruction or alter its neighbor");
			context.Expect(pool.Destroy(first) && destructions == 1U, "a live owned object must be destroyed once");
			context.Expect(!pool.Destroy(first) && !pool.Destroy(first) && destructions == 1U
				&& pool.Size() == 1U && pool.HighWater() == 2U && neighbor->value == 72U,
				"an unoccupied slot must reject repeated destruction before reuse");
			auto* reused = pool.Create(destructions, 73U);
			context.Expect(reused == first && reused->value == 73U && pool.Size() == 2U
				&& pool.Create(destructions, 74U) == nullptr,
				"rejection must leave precisely one reusable slot without duplicating its free-list entry");
		});

		registry.Add("Engine.Memory.FixedPoolHandlesZeroCapacityAndOverAlignedValues", [](TestContext& context) {
			std::size_t destructions = 0;
			AlignedPoolValue foreign(destructions, 9U);
			FixedBlockPool<AlignedPoolValue, 0U> empty;
			context.Expect(empty.Create(destructions, 1U) == nullptr && !empty.Destroy(nullptr)
				&& !empty.Destroy(&foreign) && empty.Size() == 0U && empty.HighWater() == 0U,
				"zero capacity must reject creation and destruction without accessing nonexistent slots");
			FixedBlockPool<AlignedPoolValue, 2U> pool;
			auto* first = pool.Create(destructions, 81U);
			auto* second = pool.Create(destructions, 82U);
			context.Expect(first && second && reinterpret_cast<std::uintptr_t>(first) % alignof(AlignedPoolValue) == 0U
				&& reinterpret_cast<std::uintptr_t>(second) % alignof(AlignedPoolValue) == 0U,
				"every slot must honor the value type's extended alignment");
			context.Expect(!pool.Destroy(&foreign) && destructions == 0U && pool.Destroy(first)
				&& destructions == 1U && second->value == 82U && pool.HighWater() == 2U,
				"aligned foreign rejection and owned destruction must preserve live neighbors and high-water");
			pool.Clear();
			pool.Clear();
			context.Expect(destructions == 2U && pool.Size() == 0U && pool.HighWater() == 2U,
				"repeated clear must destroy only the two owned values and retain pressure history");
		});

		registry.Add("Engine.Memory.FixedPoolIndependentJobsPreserveReuseAtActualWorkerCounts", [](TestContext& context) {
			for (const std::size_t requestedWorkers : { 1U, 2U, 8U }) {
				// Owned capacity leaves the engine's frame executor unchanged. The gate
				// cancels before executor destruction on every assertion/exception exit.
				auto executor = ThreadPool::CreateOwned(requestedWorkers);
				Correctness::ControlledGate gate;
				std::vector<std::shared_ptr<Task<PoolJobObservation>>> tasks;
				std::vector<std::future<PoolJobObservation>> results;
				context.Expect(executor->getThreadCount() == requestedWorkers,
					"actual owned worker capacity must match the requested 1/2/8 lane");
				for (std::size_t job = 0U; job < requestedWorkers; ++job) {
					auto task = executor->createTask([wait = gate.MakeWaiter()] {
						if (wait() != Correctness::GateResult::Released)
							throw std::runtime_error("isolated pool job did not receive its start release");
						return ExerciseIndependentPool();
					});
					results.push_back(task->getFuture());
					tasks.push_back(task);
					executor->enqueue(task);
				}
				// Each job holds a worker at this gate. N arrivals therefore prove N
				// distinct workers actually ran, rather than merely requesting capacity.
				context.Expect(gate.WaitForArrivals(requestedWorkers), "every configured worker must enter a real pool job");
				gate.Release();
				std::set<std::thread::id> observedThreads;
				std::size_t destructions = 0U;
				std::size_t rejections = 0U;
				for (auto& result : results) {
					context.Expect(result.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
						"each finite isolated pool job must settle before evidence is collected");
					const auto observation = result.get();
					observedThreads.insert(observation.thread);
					destructions += observation.destructions;
					rejections += observation.rejections;
				}
				executor->shutdown();
				context.Expect(observedThreads.size() == requestedWorkers
					&& destructions == requestedWorkers * 256U * 3U
					&& rejections == requestedWorkers * 256U * 5U && executor->getThreadCount() == 0U,
					"all actual workers must finish independent reuse/rejection cycles before physical drain");
				std::cout << "POOL_OWNERSHIP_OBSERVATION {\"actualWorkers\":" << observedThreads.size()
					<< ",\"cyclesPerJob\":256,\"destructions\":" << destructions
					<< ",\"rejections\":" << rejections << "}\n";
			}
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.FixedPool.ConstructorFailureReturnsSlot", [](TestContext& context) {
			ThrowingPoolValue::ResetCounters();
			auto pool = std::make_unique<FixedBlockPool<ThrowingPoolValue, 1U>>();
			context.Expect(RejectPoolConstruction(*pool), "constructor exception must reach the caller");
			RequireSafeFailedReservation(context, pool, 0U, 0U);
			context.Expect(pool->Size() == 0U && pool->HighWater() == 0U,
				"failed construction must not commit occupancy or successful high-water");
			auto* retried = pool->Create(false, 17);
			context.Expect(retried != nullptr && retried->value == 17 && pool->Size() == 1U,
				"the failed reservation must remain available for a successful retry");
			const auto size = pool->Size();
			const auto highWater = pool->HighWater();
			pool.reset();
			context.Expect(ThrowingPoolValue::constructions == 1 && ThrowingPoolValue::destructions == 1,
				"successful retry must be destroyed once by ordinary pool ownership");
			ObservePool("Engine.RuntimeMemorySafety.Allocators.FixedPool.ConstructorFailureReturnsSlot", size, highWater);
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.FixedPool.NeighborAndHighWaterRemainCommitted", [](TestContext& context) {
			ThrowingPoolValue::ResetCounters();
			auto pool = std::make_unique<FixedBlockPool<ThrowingPoolValue, 3U>>();
			auto* neighbor = pool->Create(false, 42);
			context.Expect(RejectPoolConstruction(*pool), "the neighboring reservation must throw");
			RequireSafeFailedReservation(context, pool, 1U, 1U);
			context.Expect(pool->Size() == 1U && pool->HighWater() == 1U && neighbor->value == 42,
				"failed construction must preserve the live neighbor and successful accounting");
			auto* second = pool->Create(false, 43);
			auto* third = pool->Create(false, 44);
			context.Expect(second != nullptr && third != nullptr && pool->Create(false) == nullptr,
				"failure must not consume one of the remaining fixed slots");
			const auto size = pool->Size();
			const auto highWater = pool->HighWater();
			pool.reset();
			context.Expect(ThrowingPoolValue::constructions == 3 && ThrowingPoolValue::destructions == 3,
				"all successful neighbors must be destroyed once");
			// A nested successful construction changes freeHead while the outer slot
			// is reserved. Rollback must join the current list, never restore a stale
			// head that would allocate the nested object's live slot a second time.
			auto nestedPool = std::make_unique<FixedBlockPool<ThrowingPoolValue, 3U>>();
			auto* stable = nestedPool->Create(false, 51);
			ThrowingPoolValue* nested = nullptr;
			bool rejected = false;
			try {
				(void)nestedPool->Create(true, 52, [&] { nested = nestedPool->Create(false, 53); });
			} catch (const std::runtime_error&) { rejected = true; }
			RequireSafeFailedReservation(context, nestedPool, 2U, 2U);
			context.Expect(rejected && stable->value == 51 && nested != nullptr && nested->value == 53,
				"outer failure must preserve both previously live and nested successful objects");
			auto* retried = nestedPool->Create(false, 54);
			context.Expect(retried != nullptr && retried != nested && retried != stable
				&& nestedPool->Create(false) == nullptr && nested->value == 53,
				"rollback must return only the failed reservation to the current free list");
			nestedPool.reset();
			context.Expect(ThrowingPoolValue::constructions == 6 && ThrowingPoolValue::destructions == 6,
				"nested and ordinary successful constructions must each be destroyed once");
			ObservePool("Engine.RuntimeMemorySafety.Allocators.FixedPool.NeighborAndHighWaterRemainCommitted", size, highWater);
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.FixedPool.ClearDestroysSuccessfulObjectsOnly", [](TestContext& context) {
			ThrowingPoolValue::ResetCounters();
			bool rejected = false;
			std::size_t size = 0U;
			std::size_t highWater = 0U;
			{
				auto pool = std::make_unique<FixedBlockPool<ThrowingPoolValue, 3U>>();
				(void)pool->Create(false, 1);
				rejected = RejectPoolConstruction(*pool);
				RequireSafeFailedReservation(context, pool, 1U, 1U);
				pool->Clear();
				pool->Clear();
				size = pool->Size();
				highWater = pool->HighWater();
			}
			context.Expect(rejected && ThrowingPoolValue::constructions == 1
				&& ThrowingPoolValue::destructions == 1,
				"clear and destruction must destroy each successful object once and no failed object");
			ObservePool("Engine.RuntimeMemorySafety.Allocators.FixedPool.ClearDestroysSuccessfulObjectsOnly", size, highWater);
		});

		registry.Add("Engine.RuntimeMemorySafety.Allocators.FixedPool.RepeatedFailurePreservesCapacity", [](TestContext& context) {
			ThrowingPoolValue::ResetCounters();
			auto pool = std::make_unique<FixedBlockPool<ThrowingPoolValue, 1U>>();
			for (int attempt = 0; attempt < 32; ++attempt) {
				context.Expect(RejectPoolConstruction(*pool), "every retry must reach the throwing constructor");
				RequireSafeFailedReservation(context, pool, 0U, 0U);
				context.Expect(pool->Size() == 0U && pool->HighWater() == 0U,
					"repeated failure must preserve empty successful accounting");
			}
			auto* value = pool->Create(false, 99);
			context.Expect(value != nullptr && value->value == 99 && pool->Destroy(value)
				&& ThrowingPoolValue::constructions == ThrowingPoolValue::destructions,
				"the pool must remain usable after repeated constructor rejection");
			const auto size = pool->Size();
			const auto highWater = pool->HighWater();
			pool.reset();
			context.Expect(ThrowingPoolValue::destructions == 1, "destroyed retry must not be destroyed again at pool teardown");
			ObservePool("Engine.RuntimeMemorySafety.Allocators.FixedPool.RepeatedFailurePreservesCapacity", size, highWater);
		});
	}

} // namespace TerraEngine::Tests
