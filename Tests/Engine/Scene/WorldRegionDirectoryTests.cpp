// WorldRegionDirectoryTests.cpp - exact sparse region directory coverage.

#include "../TestHarness.h"

#include "Scene/WorldRegion.h"

namespace TerraEngine::Tests
{
	void RegisterWorldRegionDirectoryTests(TestRegistry& registry)
	{
		registry.Add("WorldRegionDirectory.DerivesExactNegativeAndExtremeRegionKeys", [](TestContext& context) {
			TerraEngine::Scene::WorldPosition position{};
			position.cell.x = 15'258'789'062LL;
			position.offsetMetres.x = -32'768.0;
			TerraEngine::Scene::WorldRegionKey key{};
			context.Expect(TerraEngine::Scene::WorldPositionToRegionKey(position, key) == TerraEngine::Scene::WorldCoordinateError::None, "Extreme canonical positions should derive a checked exact sparse key.");
			// The negative half-edge belongs to the preceding region under signed
			// floor division, so this exact cell product carries a -4 local region.
			context.ExpectEqual(122'070'312'492LL, key.x, "Canonical negative-edge offsets must derive the preceding exact Version 1 region key.");
			position.cell.x = -1;
			position.offsetMetres.x = -32'768.0;
			// Cell -1 contributes regions [-8, -1]; its negative half-edge is the
			// preceding local region, so the exact key is -12 rather than -8.
			context.Expect(TerraEngine::Scene::WorldPositionToRegionKey(position, key) == TerraEngine::Scene::WorldCoordinateError::None && key.x == -12, "Negative canonical offsets must use floor-based sparse region conversion.");
		});

		registry.Add("WorldRegionDirectory.ValidatesStableMetadataAndLifecycle", [](TestContext& context) {
			TerraEngine::Scene::WorldRegionDirectory directory;
			TerraEngine::Scene::WorldRegionDirectoryEntry entry{};
			entry.key = { 2, -3, 4 };
			entry.artifactPath = "Regions/2_-3_4.teregion";
			entry.contentHash.assign(64, 'a');
			entry.revision = 1;
			context.Expect(directory.Upsert(entry), "A strict relative region artifact with a full content hash should enter the sparse directory.");
			TerraEngine::UUID objectId{}; objectId[0] = 7;
			context.Expect(directory.SetHomeRegion(objectId, entry.key), "Object ownership should persist as a stable UUID-to-home-region mapping.");
			context.Expect(directory.Transition(entry.key, TerraEngine::Scene::WorldRegionState::Requested), "Unloaded regions should enter the requested state.");
			context.Expect(directory.Transition(entry.key, TerraEngine::Scene::WorldRegionState::Loading), "Requested regions should enter loading.");
			context.Expect(directory.Transition(entry.key, TerraEngine::Scene::WorldRegionState::Prepared), "Loading regions should publish only prepared worker candidates.");
			// Prepared work has not crossed the protected residency boundary. A
			// canceled or superseded prepare task may discard it directly instead of
			// publishing a transient resident region only to retire it immediately.
			context.Expect(directory.Transition(entry.key, TerraEngine::Scene::WorldRegionState::Unloaded), "Prepared-but-unpublished data may return to unloaded on cancellation without exposing partial residency.");
		});
	}
}
