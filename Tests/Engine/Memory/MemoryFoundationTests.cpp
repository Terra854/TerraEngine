// MemoryFoundationTests.cpp - Correctness, pressure, and isolation contracts.

#include "TestHarness.h"

#include "Core/Memory/FixedBlockPool.h"
#include "Core/Memory/MemoryStrategy.h"
#include "Core/Memory/ReusableArenaResource.h"
#include "Core/Memory/TrackingMemoryResource.h"

#include <cstdint>
#include <memory_resource>
#include <vector>

namespace TerraEngine::Tests {

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
	}

} // namespace TerraEngine::Tests
