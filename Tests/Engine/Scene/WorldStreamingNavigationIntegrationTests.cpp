// WorldStreamingNavigationIntegrationTests.cpp - protected residency/navigation ordering.

#include "../TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Resource/ResourceManager.h"
#include "Scene/WorldStreamingSystem.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Scene;

	namespace
	{
		WorldRegionDirectoryEntry MakeEntry(WorldRegionKey key, WorldRegionState state)
		{
			WorldRegionDirectoryEntry entry{};
			entry.key = key;
			entry.artifactPath = "Regions/test.teregion";
			entry.contentHash = std::string(64U, 'a');
			entry.revision = 1U;
			entry.state = state;
			return entry;
		}
	}

	void RegisterWorldStreamingNavigationIntegrationTests(TestRegistry& registry)
	{
		registry.Add("Navigation.StreamingIntegration.PhasePrecedesNavigationAndDeclaresBoundaries", [](TestContext& context) {
			const auto phases = TerraEngine::Scheduling::DefaultSchedulerPhases();
			const auto streaming = std::find_if(phases.begin(), phases.end(), [](const auto& phase) {
				return phase.name == "Streaming";
			});
			const auto navigation = std::find_if(phases.begin(), phases.end(), [](const auto& phase) {
				return phase.name == "Navigation";
			});
			context.Expect(streaming != phases.end() && navigation != phases.end()
				&& streaming->order == 32 && streaming->order < navigation->order,
				"Streaming must prepare and publish region residency before Navigation consumes it.");

			const auto declaration = TerraEngine::Scene::WorldStreamingSystem::Instance().SchedulerDeclaration();
			context.Expect(declaration.phase == "Streaming" && !declaration.mainThread,
				"World streaming must expose worker-safe preparation in the Streaming phase.");
		});

		registry.Add("Navigation.StreamingIntegration.InterestAndLeaseWorkRemainBounded", [](TestContext& context) {
			WorldRegionDirectory directory;
			for(std::int64_t x = 0; x < 4; ++x)
				context.Expect(directory.Upsert(MakeEntry({x, 0, 0}, WorldRegionState::Unloaded)),
					"Fixture regions must enter the checked directory.");
			ObserverInterest observer{};
			observer.observerId = "navigation-test";
			observer.radiusMetres = 40'000.0;
			observer.priority = ObserverInterestPriority::Critical;
			const auto plan = BuildDeterministicRegionInterestPlan({observer}, directory, {}, {2U, 1U}, 1U);
			context.Expect(plan.requested.size() == 2U,
				"Region interest admission must never exceed its resident-region budget.");

			const WorldRegionKey leased{0, 0, 0};
			context.Expect(directory.AcquireLease(leased, {"route-1", 1U, 4U, 0U}),
				"A retained route must acquire a generation-qualified expiring lease.");
			context.Expect(directory.ExpireLeases(3U, 0U, 1U) == 0U
				&& directory.ExpireLeases(4U, 0U, 1U) == 1U,
				"Frame lease expiry must be deterministic and bounded per flush.");
		});

		registry.Add("Navigation.StreamingIntegration.InvalidatesBeforeRetiringPublishedRegion", [](TestContext& context) {
			auto& streaming = TerraEngine::Scene::WorldStreamingSystem::Instance();
			streaming.Shutdown();
			streaming.Initialize();
			streaming.SetBudget({1U, 1U});
			const WorldRegionKey key{7, 0, 9};
			context.Expect(streaming.Directory().Upsert(MakeEntry(key, WorldRegionState::Prepared)),
				"Prepared region metadata must be accepted before publication.");
			auto snapshot = std::make_shared<WorldRegionSnapshot>();
			snapshot->key = key;
			snapshot->directoryRevision = 1U;
			snapshot->contentHash = std::string(64U, 'a');
			streaming.SubmitPreparedRegion({key, 1U, 1U, false, {}, snapshot});
			context.Expect(streaming.PublishAtFrameEnd(ResourceManager::Instance(), 1U),
				"A complete prepared region must publish atomically at frame end.");
			const auto retainedOldRoot = streaming.CurrentResidency().residentRegions.at(key);

			streaming.Update(0.0F);
			context.Expect(streaming.PublishAtFrameEnd(ResourceManager::Instance(), 2U),
				"A no-longer-requested region must publish its retirement boundary.");
			const auto retired = streaming.CurrentResidency();
			context.Expect(!retired.residentRegions.contains(key)
				&& std::find(retired.invalidatedRegions.begin(), retired.invalidatedRegions.end(), key)
					!= retired.invalidatedRegions.end(),
				"The new residency root must expose invalidation before deferred reclamation.");
			context.Expect(retainedOldRoot && retainedOldRoot->key == key,
				"Readers holding the old immutable root must remain safe after retirement.");
			streaming.Shutdown();
		});
	}
}
