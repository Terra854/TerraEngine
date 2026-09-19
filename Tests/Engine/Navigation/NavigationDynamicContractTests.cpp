// Test-first dynamic-world contract matrix for navigation consumers.
//
// Dynamic changes are value packets. The tests cover source revisions and
// affected bounds so direct queries and agents see coherent invalidation.
#include "../TestHarness.h"

#include "Navigation/Dynamic/NavigationDirtyIndex.h"
#include "Navigation/Dynamic/NavigationPreparation.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;

	namespace
	{
		NavigationGeometryBounds Bounds(float minimum, float maximum)
		{
			return { { minimum, 0.0F, -1.0F }, { maximum, 2.0F, 1.0F } };
		}

		NavigationDynamicSourceChange Change(NavigationDynamicChangeKind kind,
			NavigationGeneration revision, float oldMinimum, float newMinimum)
		{
			NavigationDynamicSourceChange value{};
			value.source = NavigationUuid::FromString("7a34f2a7-0f5e-42ac-9e01-000000000001");
			value.sourceRevision = revision;
			value.physicalReadyRevision = revision;
			value.kind = kind;
			value.previousBounds = Bounds(oldMinimum, oldMinimum + 2.0F);
			value.currentBounds = kind == NavigationDynamicChangeKind::Removed
				? NavigationGeometryBounds{} : Bounds(newMinimum, newMinimum + 2.0F);
			return value;
		}
	}

	void RegisterNavigationDynamicContractTests(TestRegistry& registry)
	{
		registry.Add("Navigation.DynamicContract.CoversCreationActivationTransformRemovalReplacementAndReshape", [](TestContext& context) {
			NavigationDirtyIndex dirty({ .clearanceExpansionMetres = 0.5F, .buildHaloMetres = 0.25F });
			constexpr std::array<NavigationDynamicChangeKind, 6U> kinds{
				NavigationDynamicChangeKind::Created, NavigationDynamicChangeKind::Enabled,
				NavigationDynamicChangeKind::Moved, NavigationDynamicChangeKind::Removed,
				NavigationDynamicChangeKind::Replaced, NavigationDynamicChangeKind::Reshaped };
			for(std::size_t index = 0U; index < kinds.size(); ++index) {
				context.Expect(dirty.CommitSourceChange(Change(kinds[index], index + 1U,
					static_cast<float>(index), static_cast<float>(index + 1U))).accepted,
					"Every supported automatic geometry mutation must enter the typed dirty index.");
			}
			context.Expect(dirty.PendingPreparationCount() == kinds.size(),
				"Each committed source revision must remain observable until publication acknowledges it.");
		});

		registry.Add("Navigation.DynamicContract.RemovalInvalidatesBeforeReplacementIsReady", [](TestContext& context) {
			NavigationDirtyIndex dirty({});
			const NavigationDynamicSourceChange removed = Change(NavigationDynamicChangeKind::Removed, 19U, -2.0F, 0.0F);
			context.Expect(dirty.CommitSourceChange(removed).accepted,
				"A removed source must produce a committed tombstone packet.");
			const NavigationSourceTombstoneSnapshot snapshot = dirty.ObserveAtProtectedBoundary();
			context.Expect(snapshot.Contains(removed.source, removed.sourceRevision),
				"Removed geometry must be unsafe at the next protected observation boundary.");
			context.Expect(dirty.PendingPreparationCount() == 1U,
				"Removal preparation may remain pending while the tombstone is already visible.");
		});

		registry.Add("Navigation.DynamicContract.UnrelatedSourcesRemainUsableAfterLocalPublication", [](TestContext& context) {
			NavigationDirtyIndex dirty({});
			const NavigationUuid first = NavigationUuid::FromString("7a34f2a7-0f5e-42ac-9e01-000000000002");
			const NavigationUuid second = NavigationUuid::FromString("7a34f2a7-0f5e-42ac-9e01-000000000003");
			NavigationDynamicSourceChange firstChange = Change(NavigationDynamicChangeKind::Moved, 1U, -4.0F, -2.0F);
			firstChange.source = first;
			NavigationDynamicSourceChange secondChange = Change(NavigationDynamicChangeKind::Moved, 1U, 12.0F, 14.0F);
			secondChange.source = second;
			context.Expect(dirty.CommitSourceChange(firstChange).accepted && dirty.CommitSourceChange(secondChange).accepted,
				"Independent geometry changes must be admitted as separate source records.");
			context.Expect(dirty.AcknowledgePublishedThrough(1U),
				"A publication acknowledging the local revision must drain the complete copied batch.");
			context.Expect(dirty.PendingPreparationCount() == 0U && dirty.LatestSourceRevision(first) == 1U
				&& dirty.LatestSourceRevision(second) == 1U,
				"Acknowledgement must preserve unrelated source revision evidence while clearing pending work.");
		});

		registry.Add("Navigation.DynamicContract.RealWorkersProduceEquivalentPreparationResults", [](TestContext& context) {
			// Run the same immutable packet corpus through real shared-pool workers.
			// Each task owns a coordinator because the coordinator's protected state is
			// intentionally not a worker-shared mutable object.
			ThreadPool& pool = ThreadPool::instance();
			const std::size_t previousWorkerCount = pool.getThreadCount();
			pool.shutdown();
			struct ThreadPoolRestore final {
				ThreadPool& pool;
				std::size_t workerCount;
				~ThreadPoolRestore() {
					pool.shutdown();
					if(workerCount != 0U) pool.initialize(workerCount);
				}
			} restore{pool, previousWorkerCount};
			const auto runCorpus = [](std::size_t workerCount) {
				ThreadPool& workerPool = ThreadPool::instance();
				workerPool.initialize(workerCount);
				if(workerPool.getThreadCount() != workerCount) {
					workerPool.shutdown();
					return std::uint64_t{0U};
				}
				std::vector<std::future<std::uint64_t>> futures;
				futures.reserve(32U);
				for(std::uint32_t packetIndex = 0U; packetIndex < 32U; ++packetIndex) {
					auto task = workerPool.createTask([packetIndex]() -> std::uint64_t {
						NavigationPreparationCoordinator coordinator({
							.maximumConcurrentBuilds = 1U,
							.maximumConcurrentSearches = 1U,
							.maximumCompletionQueueEntries = 2U,
							.maximumPreparationBytes = 256U });
						auto bytes = std::make_shared<const std::vector<std::byte>>(16U,
							static_cast<std::byte>((packetIndex * 17U + 3U) & 0xffU));
						NavigationDynamicWorkRequest request{};
						request.kind = NavigationDynamicWorkKind::Build;
						request.source = NavigationUuid::FromString("f7e0a977-56f8-46a8-bca8-000000000001");
						request.revision = static_cast<NavigationGeneration>(packetIndex + 1U);
						request.estimatedBytes = bytes->size();
						request.copiedInput = bytes;
						const NavigationDynamicWorkHandle handle = coordinator.Submit(request);
						if(!handle.IsValid()) return 0U;
						(void)coordinator.RunWorkerQuantumForTesting(1U);
						const NavigationPreparationCompletion completion = coordinator.ReadCompletion(handle);
						return completion.state == NavigationPreparationState::Ready
							? completion.inputFingerprint ^ (static_cast<std::uint64_t>(std::to_integer<unsigned char>(completion.firstInputByte)) << 32U)
							: 0U;
					});
					futures.push_back(task->getFuture());
					workerPool.enqueue(task, 1U);
				}
				std::uint64_t aggregate = 14695981039346656037ULL;
				bool everyCompletionReady = true;
				for(std::future<std::uint64_t>& future : futures) {
					const std::uint64_t result = future.get();
					everyCompletionReady = everyCompletionReady && result != 0U;
					aggregate ^= result;
					aggregate *= 1099511628211ULL;
				}
				workerPool.shutdown();
				return everyCompletionReady ? aggregate : 0U;
			};

			const std::uint64_t oneWorker = runCorpus(1U);
			const std::uint64_t fourWorkers = runCorpus(4U);
			const std::uint64_t eightWorkers = runCorpus(8U);
			context.Expect(oneWorker != 0U && oneWorker == fourWorkers && oneWorker == eightWorkers,
				"The same immutable dynamic preparation corpus must produce one deterministic result across 1, 4, and 8 real workers.");
		});
	}
}
