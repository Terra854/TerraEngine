// RecastDetourGroundBackendTests.cpp - real provider pipeline smoke coverage.

#include "../TestHarness.h"

#include "Navigation/Backend/DetourGroundShard.h"
#include "Navigation/Backend/RecastGroundBuilder.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <algorithm>

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

		registry.Add("Navigation.Backend.AutomaticColliderBlocksGroundRoute", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument ground{};
			ground.kind = NavigationGeometryPayloadKind::TriangleMesh;
			ground.vertices = {{1.0F, 0.0F, 1.0F}, {31.0F, 0.0F, 31.0F},
				{31.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 31.0F}};
			ground.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor groundDescriptor{};
			groundDescriptor.sourceRevision = 1U;
			groundDescriptor.canonicalPlacementRevision = 1U;
			groundDescriptor.physicalReadyRevision = 1U;
			groundDescriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto groundSnapshot = NavigationGeometrySnapshot::Create(groundDescriptor, ground);

			NavigationGeometryDocument obstacle{};
			obstacle.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			NavigationGeometryPrimitive box{};
			box.kind = NavigationGeometryPrimitiveKind::Box;
			box.center = {16.0F, 1.0F, 16.0F};
			box.halfExtents = {2.0F, 1.0F, 5.0F};
			obstacle.primitives.push_back(box);
			NavigationGeometrySnapshotDescriptor obstacleDescriptor{};
			obstacleDescriptor.sourceRevision = 2U;
			obstacleDescriptor.canonicalPlacementRevision = 1U;
			obstacleDescriptor.physicalReadyRevision = 1U;
			obstacleDescriptor.sourceKind = NavigationSourceKind::Collider;
			const auto obstacleSnapshot = NavigationGeometrySnapshot::Create(obstacleDescriptor, obstacle);

			RecastGroundBuildInput buildInput{};
			buildInput.profile = NavigationProfile::Defaults();
			buildInput.sources = {groundSnapshot.snapshot, obstacleSnapshot.snapshot};
			buildInput.maximumTiles = 4U;
			const RecastGroundBuildResult built = BuildRecastGroundTiles(buildInput);
			DetourGroundShardBuildInput shardInput{};
			shardInput.shard = {0, 0, 0, MakeGroundProfileBuildKey(buildInput.profile)};
			shardInput.expectedGeneration = 1U;
			shardInput.tiles = built.tiles;
			const DetourGroundShardBuildResult shard = AssembleDetourGroundShard(shardInput);
			const DetourGroundRouteResult route = shard.generation ? QueryDetourGroundShard(*shard.generation,
				{{4.0F, 0.0F, 16.0F}, {28.0F, 0.0F, 16.0F}, 2.0F, 4.0F, 64U}) : DetourGroundRouteResult{};
			const bool leavesObstacleFootprint = std::any_of(route.points.begin(), route.points.end(),
				[](const NavigationGeometryVector3& point) { return point.z < 11.0F || point.z > 21.0F; });

			context.Expect(groundSnapshot.accepted && obstacleSnapshot.accepted && built.Succeeded()
				&& shard.accepted && route.outcome == NavigationOutcome::Complete && leavesObstacleFootprint,
				"A world-space automatic collider must remove its footprint from Recast coverage so a Detour route detours around it.");
		});
	}
}
