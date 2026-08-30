// BoundedRingBufferTests.cpp - Fixed-capacity ordering and pressure regressions.

#include "TestHarness.h"

#include "Utilities/Containers/BoundedRingBuffer.h"

#include <cstdint>

namespace TerraEngine::Tests {

	void RegisterBoundedRingBufferTests(TestRegistry& registry)
	{
		using TerraEngine::Utilities::BoundedRingBuffer;
		using TerraEngine::Utilities::BoundedRingBufferConfigureStatus;
		using TerraEngine::Utilities::BoundedRingBufferPushStatus;

		registry.Add("Engine.Utilities.BoundedRingRejectsUnconfiguredAndZeroCapacityPushes",
			[](TestContext& context) {
				BoundedRingBuffer<std::uint64_t> ring;
				context.Expect(
					ring.Push(1U) == BoundedRingBufferPushStatus::Unconfigured,
					"A caller must configure fixed storage before publishing entries.");
				context.Expect(
					ring.Configure(0U, 8U) == BoundedRingBufferConfigureStatus::Configured
						&& ring.Push(2U) == BoundedRingBufferPushStatus::NoCapacity,
					"An explicitly empty ring should remain bounded and reject storage.");
			});

		registry.Add("Engine.Utilities.BoundedRingOverwritesOldestInFifoOrder",
			[](TestContext& context) {
				BoundedRingBuffer<std::uint64_t> ring;
				static_cast<void>(ring.Configure(3U, 8U));
				static_cast<void>(ring.Push(10U));
				static_cast<void>(ring.Push(11U));
				static_cast<void>(ring.Push(12U));
				const BoundedRingBufferPushStatus pressure = ring.Push(13U);

				const auto& statistics = ring.Statistics();
				context.Expect(pressure == BoundedRingBufferPushStatus::ReplacedOldest
						&& ring.Oldest() && *ring.Oldest() == 11U
						&& ring.Newest() && *ring.Newest() == 13U
						&& ring.At(1U) && *ring.At(1U) == 12U,
					"A full ring must replace exactly its oldest entry without changing FIFO order.");
				context.Expect(statistics.size == 3U
						&& statistics.capacity == 3U
						&& statistics.highWaterEntries == 3U
						&& statistics.pushes == 4U
						&& statistics.overwrites == 1U,
					"Pressure statistics should expose occupancy, high water, pushes, and overwrites.");
			});

		registry.Add("Engine.Utilities.BoundedRingReconfigurationIsAtomic",
			[](TestContext& context) {
				BoundedRingBuffer<std::uint64_t> ring;
				static_cast<void>(ring.Configure(2U, 8U));
				static_cast<void>(ring.Push(20U));
				static_cast<void>(ring.Push(21U));

				const BoundedRingBufferConfigureStatus rejected = ring.Configure(9U, 8U);
				context.Expect(rejected == BoundedRingBufferConfigureStatus::CapacityExceeded
						&& ring.Statistics().size == 2U
						&& ring.Oldest() && *ring.Oldest() == 20U
						&& ring.Newest() && *ring.Newest() == 21U,
					"A rejected capacity change must leave the prior ring completely visible.");

				ring.Clear();
				context.Expect(ring.Statistics().size == 0U
						&& ring.Statistics().highWaterEntries == 2U
						&& ring.Statistics().pushes == 2U
						&& ring.Oldest() == nullptr,
					"Clear should remove entries while retaining bounded pressure evidence.");
			});
	}

} // namespace TerraEngine::Tests
