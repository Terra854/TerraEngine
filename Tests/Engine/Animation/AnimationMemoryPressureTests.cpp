// AnimationMemoryPressureTests.cpp - Bounded animation memory pressure checks.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationMemory.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationMemoryConfiguration MakeConfiguration(
			std::size_t perClassBytes,
			std::size_t totalBytes,
			std::size_t reservations = 64U)
		{
			AnimationMemoryConfiguration configuration;
			for(AnimationMemoryClassBudget& budget : configuration.classes) {
				budget.maximumBytes = perClassBytes;
			}
			configuration.maximumTotalBytes = totalBytes;
			configuration.maximumReservations = reservations;
			return configuration;
		}

		AnimationMemoryReservationRequest MakeRequest(
			AnimationMemoryClass memoryClass,
			std::size_t bytes,
			AnimationMemoryRetention retention = AnimationMemoryRetention::Required,
			std::uint32_t retentionPriority = 0U,
			AnimationGeneration ownerGeneration = 1U)
		{
			AnimationMemoryReservationRequest request;
			request.memoryClass = memoryClass;
			request.bytes = bytes;
			request.ownerGeneration = ownerGeneration;
			request.retention = retention;
			request.retentionPriority = retentionPriority;
			return request;
		}

		std::size_t MemoryClassIndex(AnimationMemoryClass memoryClass)
		{
			return static_cast<std::size_t>(memoryClass);
		}
	}

	void RegisterAnimationMemoryPressureTests(TestRegistry& registry)
	{
		registry.Add("Animation.Memory.EveryStorageClassIsBoundedAndAccounted", [](TestContext& context) {
			AnimationMemoryController controller;
			context.Expect(controller.Configure(MakeConfiguration(32U, 224U, 16U))
				== AnimationMemoryStatus::Reserved,
				"A checked memory configuration should allocate its bounded reservation table once.");

			constexpr std::array classes{
				AnimationMemoryClass::FrameArena,
				AnimationMemoryClass::PosePool,
				AnimationMemoryClass::ScratchPool,
				AnimationMemoryClass::EventRing,
				AnimationMemoryClass::HistoryRing,
				AnimationMemoryClass::DiagnosticRing,
				AnimationMemoryClass::Residency};
			std::array<AnimationMemoryLease, classes.size()> leases;
			bool allReserved = true;
			for(std::size_t index = 0U; index < classes.size(); ++index) {
				allReserved = allReserved
					&& controller.TryReserve(MakeRequest(classes[index], 16U), leases[index])
						== AnimationMemoryStatus::Reserved;
			}
			context.Expect(allReserved,
				"Pose, scratch, event, history, diagnostic, residency, and frame storage should share bounded accounting.");

			const AnimationMemorySnapshot beforeRejection = controller.Snapshot();
			AnimationMemoryLease rejected;
			context.Expect(controller.TryReserve(
				MakeRequest(AnimationMemoryClass::PosePool, 17U), rejected)
					== AnimationMemoryStatus::BudgetExceeded
				&& !rejected.IsActive(),
				"A per-class overflow must fail before charging a partial pose allocation.");
			const AnimationMemorySnapshot afterRejection = controller.Snapshot();
			context.Expect(afterRejection.totalBytesInUse == beforeRejection.totalBytesInUse
				&& afterRejection.activeReservations == beforeRejection.activeReservations
				&& afterRejection.classes[MemoryClassIndex(AnimationMemoryClass::PosePool)].rejectedRequests == 1U,
				"Rejected bytes must change diagnostics only, never live allocation accounting.");
		});

		registry.Add("Animation.Memory.PressureEvictsOnlySufficientOptionalStateAtomically", [](TestContext& context) {
			AnimationMemoryController controller;
			context.Expect(controller.Configure(MakeConfiguration(128U, 128U, 16U))
				== AnimationMemoryStatus::Reserved,
				"The pressure fixture needs one small shared budget.");

			AnimationMemoryLease diagnostics;
			AnimationMemoryLease history;
			AnimationMemoryLease events;
			AnimationMemoryLease pose;
			static_cast<void>(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::DiagnosticRing,
				32U,
				AnimationMemoryRetention::Evictable,
				0U), diagnostics));
			static_cast<void>(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::HistoryRing,
				32U,
				AnimationMemoryRetention::Evictable,
				1U), history));
			static_cast<void>(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::EventRing,
				32U,
				AnimationMemoryRetention::Evictable,
				2U), events));
			static_cast<void>(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::PosePool,
				32U), pose));

			AnimationMemoryLease scratch;
			context.Expect(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::ScratchPool,
				64U), scratch) == AnimationMemoryStatus::Reserved
				&& !diagnostics.IsActive()
				&& !history.IsActive()
				&& events.IsActive()
				&& pose.IsActive()
				&& scratch.IsActive(),
				"Required scratch should evict the least-important optional records while preserving required pose data.");

			// Only one optional 32-byte event reservation remains. A required 64-byte
			// residency request cannot fit, so its failed plan must not evict that event.
			const AnimationMemorySnapshot beforeFailedPlan = controller.Snapshot();
			AnimationMemoryLease residency;
			context.Expect(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::Residency,
				64U), residency) == AnimationMemoryStatus::BudgetExceeded
				&& events.IsActive()
				&& !residency.IsActive(),
				"An insufficient eviction plan must leave every previously live reservation unchanged.");
			const AnimationMemorySnapshot afterFailedPlan = controller.Snapshot();
			context.Expect(afterFailedPlan.totalBytesInUse == beforeFailedPlan.totalBytesInUse
				&& afterFailedPlan.activeReservations == beforeFailedPlan.activeReservations,
				"Rejected pressure work must not expose partial eviction or byte accounting.");
		});

		registry.Add("Animation.Memory.RequiredHistoryAndHighPriorityCachesRemainResident", [](TestContext& context) {
			AnimationMemoryController controller;
			context.Expect(controller.Configure(MakeConfiguration(64U, 96U, 8U))
				== AnimationMemoryStatus::Reserved,
				"The retention fixture should configure its bounded table.");

			AnimationMemoryLease requiredHistory;
			AnimationMemoryLease importantResidency;
			static_cast<void>(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::HistoryRing,
				48U,
				AnimationMemoryRetention::Required), requiredHistory));
			static_cast<void>(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::Residency,
				48U,
				AnimationMemoryRetention::Evictable,
				10U), importantResidency));

			AnimationMemoryLease lowPriorityDiagnostic;
			context.Expect(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::DiagnosticRing,
				48U,
				AnimationMemoryRetention::Evictable,
				0U), lowPriorityDiagnostic) == AnimationMemoryStatus::BudgetExceeded
				&& requiredHistory.IsActive()
				&& importantResidency.IsActive(),
				"Optional low-priority detail must not evict required rollback history or a higher-priority cache.");
		});

		registry.Add("Animation.Memory.FrameArenaAlignsReusesAndFailsWithoutAdvancing", [](TestContext& context) {
			AnimationFrameArena arena;
			context.Expect(arena.Configure(64U) == AnimationMemoryStatus::Reserved
				&& arena.BeginGeneration(1U) == AnimationMemoryStatus::Reserved,
				"A frame arena should allocate its backing store before worker use.");
			const AnimationArenaAllocation first = arena.Allocate(3U, 8U);
			const AnimationArenaAllocation second = arena.Allocate(5U, 16U);
			context.Expect(first && second
				&& first.offset == 0U
				&& second.offset == 16U
				&& arena.BytesInUse() == 21U,
				"Arena suballocations must honor power-of-two alignment without growing storage.");

			const std::size_t beforeFailure = arena.BytesInUse();
			arena.FailNextAllocationForTesting();
			const AnimationArenaAllocation injected = arena.Allocate(8U);
			const AnimationArenaAllocation oversized = arena.Allocate(128U);
			context.Expect(injected.status == AnimationMemoryStatus::AllocationFailure
				&& oversized.status == AnimationMemoryStatus::BudgetExceeded
				&& arena.BytesInUse() == beforeFailure,
				"Injected and capacity failures must leave the arena cursor unchanged.");

			context.Expect(arena.BeginGeneration(2U) == AnimationMemoryStatus::Reserved
				&& arena.BytesInUse() == 0U
				&& arena.HighWaterBytes() == 21U,
				"A new frame generation reuses bytes while retaining diagnostic high-water evidence.");
		});

		registry.Add("Animation.Memory.EventHistoryAndDiagnosticRingsStayAtFixedCapacity", [](TestContext& context) {
			AnimationBoundedMemoryRing<std::uint64_t> events;
			AnimationBoundedMemoryRing<std::uint64_t> history;
			AnimationBoundedMemoryRing<std::uint64_t> diagnostics;
			context.Expect(events.Configure(2U, 8U) == AnimationMemoryStatus::Reserved
				&& history.Configure(3U, 8U) == AnimationMemoryStatus::Reserved
				&& diagnostics.Configure(1U, 8U) == AnimationMemoryStatus::Reserved,
				"Each optional stream should configure an explicit fixed entry capacity.");

			static_cast<void>(events.Push(10U));
			static_cast<void>(events.Push(11U));
			static_cast<void>(events.Push(12U));
			static_cast<void>(history.Push(20U));
			static_cast<void>(history.Push(21U));
			static_cast<void>(history.Push(22U));
			static_cast<void>(history.Push(23U));
			static_cast<void>(diagnostics.Push(30U));
			static_cast<void>(diagnostics.Push(31U));
			context.Expect(events.Counters().size == 2U
				&& events.Counters().evictions == 1U
				&& events.Oldest() && *events.Oldest() == 11U
				&& history.Counters().size == 3U
				&& history.Counters().evictions == 1U
				&& history.Oldest() && *history.Oldest() == 21U
				&& diagnostics.Counters().size == 1U
				&& diagnostics.Newest() && *diagnostics.Newest() == 31U,
				"Full rings must evict oldest optional detail instead of increasing capacity.");

			context.Expect(events.Configure(9U, 8U) == AnimationMemoryStatus::InvalidArgument
				&& events.Counters().size == 2U
				&& events.Oldest() && *events.Oldest() == 11U,
				"A rejected ring reconfiguration must preserve the complete prior contents.");
		});

		registry.Add("Animation.Memory.InjectedFailureAndShutdownPreserveAccounting", [](TestContext& context) {
			AnimationMemoryController controller;
			context.Expect(controller.Configure(MakeConfiguration(64U, 128U, 8U))
				== AnimationMemoryStatus::Reserved,
				"The fail-safe fixture should configure successfully.");
			AnimationMemoryLease pose;
			static_cast<void>(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::PosePool,
				32U), pose));
			controller.FailNextReservationForTesting();
			AnimationMemoryLease scratch;
			context.Expect(controller.TryReserve(MakeRequest(
				AnimationMemoryClass::ScratchPool,
				32U), scratch) == AnimationMemoryStatus::AllocationFailure
				&& pose.IsActive()
				&& controller.Snapshot().totalBytesInUse == 32U,
				"A deterministic allocation failure must preserve prior pose accounting exactly.");

			controller.Shutdown();
			AnimationMemoryLease afterShutdown;
			context.Expect(!pose.IsActive()
				&& controller.Snapshot().totalBytesInUse == 0U
				&& controller.TryReserve(MakeRequest(
					AnimationMemoryClass::PosePool,
					16U), afterShutdown) == AnimationMemoryStatus::Shutdown,
				"Shutdown must invalidate all leases and reject new work without a dangling reservation.");
		});

		registry.Add("Animation.Memory.ConcurrentReservationsNeverExceedConfiguredBudget", [](TestContext& context) {
			AnimationMemoryController controller;
			context.Expect(controller.Configure(MakeConfiguration(128U, 128U, 32U))
				== AnimationMemoryStatus::Reserved,
				"Concurrent workers need one shared bounded controller.");

			std::atomic_bool begin = false;
			std::atomic_bool unexpectedStatus = false;
			std::vector<std::thread> workers;
			for(std::size_t workerIndex = 0U; workerIndex < 8U; ++workerIndex) {
				workers.emplace_back([&controller, &begin, &unexpectedStatus, workerIndex]() {
					while(!begin.load(std::memory_order_acquire)) {
						std::this_thread::yield();
					}
					for(std::size_t iteration = 0U; iteration < 500U; ++iteration) {
						AnimationMemoryLease lease;
						const AnimationMemoryStatus status = controller.TryReserve(MakeRequest(
							AnimationMemoryClass::ScratchPool,
							16U,
							AnimationMemoryRetention::Required,
							0U,
							static_cast<AnimationGeneration>(workerIndex + 1U)), lease);
						if(status != AnimationMemoryStatus::Reserved
							&& status != AnimationMemoryStatus::BudgetExceeded
							&& status != AnimationMemoryStatus::ReservationTableFull) {
							unexpectedStatus.store(true, std::memory_order_release);
						}
						if(lease.IsActive()) {
							std::this_thread::yield();
						}
					}
				});
			}
			begin.store(true, std::memory_order_release);
			for(std::thread& worker : workers) {
				worker.join();
			}

			const AnimationMemorySnapshot snapshot = controller.Snapshot();
			context.Expect(!unexpectedStatus.load(std::memory_order_acquire)
				&& snapshot.totalBytesInUse == 0U
				&& snapshot.activeReservations == 0U
				&& snapshot.totalHighWaterBytes <= 128U
				&& snapshot.classes[MemoryClassIndex(AnimationMemoryClass::ScratchPool)].highWaterBytes <= 128U,
				"Worker contention may reject work but must never overcommit or leak the configured budget.");
		});
	}
}
