// RecastDetourGroundBackendTests.cpp - real provider pipeline smoke coverage.

#include "../TestHarness.h"

#include "Navigation/Backend/DetourGroundShard.h"
#include "Navigation/Backend/RecastGroundBuilder.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

namespace TerraEngine::Tests {
	void RegisterRecastDetourGroundBackendTests(TestRegistry& registry) {
		registry.Add("Navigation.Backend.RecastBytesLoadIntoPrivateDetourShard", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			// Clockwise winding viewed from above produces an upward Recast normal.
			document.vertices = {{1.0F, 0.0F, 1.0F}, {8.0F, 0.0F, 8.0F},
				{8.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 8.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			const auto snapshot = NavigationGeometrySnapshot::Create(descriptor, document);
			context.Expect(snapshot.accepted && snapshot.snapshot != nullptr,
				"The backend smoke fixture must first create immutable source geometry.");

			RecastGroundBuildInput buildInput{};
			buildInput.profile = NavigationProfile::Defaults();
			buildInput.sources = {snapshot.snapshot};
			buildInput.maximumTiles = 4U;
			const RecastGroundBuildResult built = BuildRecastGroundTiles(buildInput);
			context.Expect(built.Succeeded() && built.tiles.size() == 1U
				&& !built.tiles.front().empty && !built.tiles.front().pristineBytes.empty(),
				"A walkable source tile must contain real dtCreateNavMeshData bytes.");

			DetourGroundShardBuildInput shardInput{};
			shardInput.shard = {0, 0, 0, MakeGroundProfileBuildKey(buildInput.profile)};
			shardInput.expectedGeneration = 1U;
			shardInput.tiles = built.tiles;
			const DetourGroundShardBuildResult assembled = AssembleDetourGroundShard(shardInput);
			context.Expect(assembled.accepted && assembled.generation
				&& assembled.generation->providerState && assembled.generation->reference.generation == 2U,
				"The private Detour navmesh must accept copied Recast tile bytes atomically.");
		});
	}
}
