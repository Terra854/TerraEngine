// MemoryFoundationTests.cpp - Correctness, pressure, and isolation contracts.

#include "TestHarness.h"

#include "Core/Memory/FixedBlockPool.h"
#include "Core/Memory/MemoryStrategy.h"
#include "Core/Memory/ReusableArenaResource.h"
#include "Core/Memory/TrackingMemoryResource.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
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
