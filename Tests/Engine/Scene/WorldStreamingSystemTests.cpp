// WorldStreamingSystemTests.cpp - worker preparation and frame-end residency.

#include "../TestHarness.h"

#include "Resource/ResourceManager.h"
#include "Scene/WorldStreamingSystem.h"

namespace TerraEngine::Tests
{
	void RegisterWorldStreamingSystemTests(TestRegistry& registry)
	{
		registry.Add("WorldStreamingSystem.ArbitratesObserversDeterministically", [](TestContext& context) {
			TerraEngine::Scene::WorldRegionDirectory directory;
			for (std::int64_t x : { 0LL, 1LL })
			{
				TerraEngine::Scene::WorldRegionDirectoryEntry entry{}; entry.key.x = x; entry.artifactPath = "Regions/region.teregion"; entry.contentHash.assign(64, 'b'); entry.revision = 1;
				context.Expect(directory.Upsert(entry), "The fixture region must enter the sparse directory.");
			}
			TerraEngine::Scene::ObserverInterest render{}; render.observerId = "view"; render.priority = TerraEngine::Scene::ObserverInterestPriority::Normal;
			TerraEngine::Scene::ObserverInterest pin = render; pin.observerId = "selection"; pin.priority = TerraEngine::Scene::ObserverInterestPriority::Critical; pin.position.offsetMetres.x = 8'192.0;
			const TerraEngine::Scene::RegionInterestPlan plan = TerraEngine::Scene::BuildDeterministicRegionInterestPlan({ render, pin }, directory, {}, { 1, 1 }, 4);
			context.ExpectEqual(static_cast<std::size_t>(1), plan.requested.size(), "The resident budget should keep only the highest-priority requested region.");
			context.Expect(plan.requested.front().x == 1, "Critical selection interest should deterministically win arbitration.");
		});

		registry.Add("WorldStreamingSystem.PublishesPreparedRegionsOnlyAtFrameEnd", [](TestContext& context) {
			auto& streaming = TerraEngine::Scene::WorldStreamingSystem::Instance();
			streaming.Initialize();
			TerraEngine::Scene::WorldRegionDirectoryEntry entry{}; entry.artifactPath = "Regions/ready.teregion"; entry.contentHash.assign(64, 'c'); entry.revision = 3;
			context.Expect(streaming.Directory().Upsert(entry), "The fixture directory entry should be valid.");
			context.Expect(streaming.Directory().Transition(entry.key, TerraEngine::Scene::WorldRegionState::Requested), "Fixture should request the region.");
			context.Expect(streaming.Directory().Transition(entry.key, TerraEngine::Scene::WorldRegionState::Loading), "Fixture should load the region.");
			context.Expect(streaming.Directory().Transition(entry.key, TerraEngine::Scene::WorldRegionState::Prepared), "Fixture should stage a prepared candidate.");
			auto snapshot = std::make_shared<TerraEngine::Scene::WorldRegionSnapshot>(); snapshot->key = entry.key; snapshot->directoryRevision = entry.revision; snapshot->contentHash = entry.contentHash;
			streaming.SubmitPreparedRegion({ entry.key, entry.revision, 1, false, {}, snapshot });
			context.Expect(streaming.CurrentResidency().residentRegions.empty(), "Submitting worker output must not expose residency before the protected boundary.");
			context.Expect(streaming.PublishAtFrameEnd(ResourceManager::Instance(), 1), "A complete prepared snapshot should publish atomically at Frame End.");
			context.Expect(streaming.CurrentResidency().residentRegions.contains(entry.key), "Published residency should expose the complete region after the boundary.");
			streaming.Shutdown();
		});
	}
}
