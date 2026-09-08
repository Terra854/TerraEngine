// Test-first contracts for bounded concurrent dynamic navigation preparation.
//
// The runtime must provide copied worker packets and an explicit completion queue.
// These tests forbid the tempting but unsafe alternatives: live scene access,
// unbounded work admission, late cancellation success, and mutable buffer reuse.

#include "../TestHarness.h"

#include "Navigation/Dynamic/NavigationPreparation.h"
#include "Navigation/Dynamic/NavigationPublication.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Navigation;

		NavigationPreparationLimits MakeLimits()
		{
			NavigationPreparationLimits limits{};
			limits.maximumConcurrentBuilds = 2U;
			limits.maximumConcurrentSearches = 2U;
			limits.maximumCompletionQueueEntries = 2U;
			limits.maximumPreparationBytes = 512U;
			return limits;
		}

		NavigationDynamicWorkRequest MakeBuild(NavigationGeneration revision, std::size_t bytes = 64U)
		{
			NavigationDynamicWorkRequest request{};
			request.kind = NavigationDynamicWorkKind::Build;
			request.source = NavigationUuid::FromString("bc7e3d11-e834-464c-85d7-55222c5a8e6e");
			request.revision = revision;
			request.estimatedBytes = bytes;
			request.copiedInput = std::make_shared<const std::vector<std::byte>>(bytes,
				static_cast<std::byte>(static_cast<unsigned char>(revision)));
			return request;
		}

		NavigationDynamicWorkRequest MakeSearch(NavigationGeneration revision, std::size_t bytes = 64U)
		{
			NavigationDynamicWorkRequest request = MakeBuild(revision, bytes);
			request.kind = NavigationDynamicWorkKind::Search;
			return request;
		}
	}

	void RegisterNavigationConcurrencyTests(TestRegistry& registry)
	{
		registry.Add("Navigation.DynamicConcurrency.RunsIndependentBuildAndSearchPacketsConcurrently", [](TestContext& context) {
			NavigationPreparationCoordinator coordinator(MakeLimits());
			const NavigationDynamicWorkHandle build = coordinator.Submit(MakeBuild(1U));
			const NavigationDynamicWorkHandle search = coordinator.Submit(MakeSearch(1U));
			context.Expect(build.IsValid() && search.IsValid(), "Bounded admission must accept one independent build and search packet.");
			const NavigationPreparationTickResult tick = coordinator.RunWorkerQuantumForTesting(2U);
			context.Expect(tick.completedBuildCount == 1U && tick.completedSearchCount == 1U,
				"Shared-pool preparation must let independent build and search work complete in the same bounded quantum.");
			context.Expect(coordinator.WorkerPacketsHaveNoLiveSceneReferences(),
				"Concurrent navigation work must retain copied immutable packets rather than scene/component pointers.");
		});

		registry.Add("Navigation.DynamicConcurrency.CoalescesOverloadButNeverDropsRemovalTombstone", [](TestContext& context) {
			NavigationPreparationLimits limits = MakeLimits();
			limits.maximumCompletionQueueEntries = 1U;
			NavigationPreparationCoordinator coordinator(limits);
			const NavigationDynamicWorkHandle first = coordinator.Submit(MakeBuild(3U));
			const NavigationDynamicWorkHandle newest = coordinator.Submit(MakeBuild(4U));
			const NavigationDynamicWorkHandle removal = coordinator.SubmitRemovalTombstone(
				NavigationUuid::FromString("bc7e3d11-e834-464c-85d7-55222c5a8e6e"), 5U);
			context.Expect(first.IsValid() && newest.IsValid() && removal.IsValid(), "The fixture must admit typed coalescible build and removal work.");
			coordinator.RunWorkerQuantumForTesting(3U);
			const NavigationPreparationQueueSnapshot snapshot = coordinator.CompletionSnapshot();
			context.Expect(snapshot.newestRevisionFor(first.source) == 4U,
				"Overload may coalesce superseded build completions only to the newest source revision.");
			context.Expect(snapshot.ContainsRemovalTombstone(first.source, 5U),
				"Queue pressure must never drop a removal tombstone, even when ordinary completions coalesce.");
			context.Expect(snapshot.droppedOrCoalescedCount > 0U,
				"Bounded overload must report explicit coalescing instead of silently discarding work.");
		});

		registry.Add("Navigation.DynamicConcurrency.CancellationCannotPublishLateSuccess", [](TestContext& context) {
			NavigationPreparationCoordinator coordinator(MakeLimits());
			const NavigationDynamicWorkHandle handle = coordinator.Submit(MakeSearch(7U));
			context.Expect(coordinator.Cancel(handle), "Cancelling an admitted request must be idempotently observable before worker completion.");
			coordinator.RunWorkerQuantumForTesting(1U);
			const NavigationPreparationCompletion completion = coordinator.ReadCompletion(handle);
			context.Expect(completion.state == NavigationPreparationState::Cancelled,
				"A cancelled search must complete as Cancelled, never as a delayed successful result.");
			context.Expect(!coordinator.TryTakePublishableCompletion(handle).has_value(),
				"Cancelled worker work must not enter the protected publication candidate queue.");
		});

		registry.Add("Navigation.DynamicConcurrency.RejectsMemoryPressureBeforeDispatchAndKeepsSafeWork", [](TestContext& context) {
			NavigationPreparationLimits limits = MakeLimits();
			limits.maximumPreparationBytes = 96U;
			NavigationPreparationCoordinator coordinator(limits);
			const NavigationDynamicWorkHandle safe = coordinator.Submit(MakeSearch(11U, 64U));
			const NavigationDynamicWorkHandle rejected = coordinator.Submit(MakeBuild(12U, 64U));
			context.Expect(safe.IsValid(), "A first in-budget work item must remain available under later pressure.");
			context.Expect(!rejected.IsValid(), "A work item exceeding the reserved preparation budget must reject before dispatch.");
			context.Expect(coordinator.PreparationBytesReserved() == 64U,
				"Rejected memory admission must leave the safe packet reservation unchanged.");
			coordinator.RunWorkerQuantumForTesting(1U);
			context.Expect(coordinator.ReadCompletion(safe).state == NavigationPreparationState::Ready,
				"Memory pressure on one update must not prevent an unrelated admitted search from completing.");
		});

		registry.Add("Navigation.DynamicConcurrency.SharedBuffersAreCopiedAndChargedOnce", [](TestContext& context) {
			NavigationPreparationCoordinator coordinator(MakeLimits());
			auto mutableBytes = std::make_shared<std::vector<std::byte>>(64U, std::byte{0x22});
			NavigationDynamicWorkRequest first = MakeBuild(31U);
			first.copiedInput = mutableBytes;
			NavigationDynamicWorkRequest second = MakeSearch(31U);
			second.copiedInput = mutableBytes;
			const NavigationDynamicWorkHandle firstHandle = coordinator.Submit(first);
			const NavigationDynamicWorkHandle secondHandle = coordinator.Submit(second);
			(*mutableBytes)[0] = std::byte{0x7f};
			coordinator.RunWorkerQuantumForTesting(2U);
			const NavigationPreparationCompletion firstCompletion = coordinator.ReadCompletion(firstHandle);
			const NavigationPreparationCompletion secondCompletion = coordinator.ReadCompletion(secondHandle);
			context.Expect(firstCompletion.inputFingerprint == secondCompletion.inputFingerprint,
				"Packets sharing one immutable source allocation must retain one stable copied input identity.");
			context.Expect(firstCompletion.firstInputByte == std::byte{0x22} && secondCompletion.firstInputByte == std::byte{0x22},
				"Worker packets must not observe caller mutation after submission; shared input is copied or frozen before dispatch.");
			context.Expect(coordinator.SharedInputBytesCharged() == 64U,
				"One shared immutable buffer must be charged once, not once per build/search consumer.");
		});
	}
}
