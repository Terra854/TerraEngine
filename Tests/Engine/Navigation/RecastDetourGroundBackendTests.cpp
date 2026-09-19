// RecastDetourGroundBackendTests.cpp - real provider pipeline smoke coverage.

#include "../TestHarness.h"

#include "Navigation/Backend/DetourGroundShard.h"
#include "Navigation/Backend/RecastGroundBuilder.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <algorithm>
#include <limits>

namespace TerraEngine::Tests {
	void RegisterRecastDetourGroundBackendTests(TestRegistry& registry) {
		registry.Add("Navigation.Backend.RequestedTileSubsetPreservesFullBuildBytes", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument ground{};
			ground.kind = NavigationGeometryPayloadKind::TriangleMesh;
			ground.vertices = {{1.0F, 0.0F, 1.0F}, {63.0F, 0.0F, 31.0F},
				{63.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 31.0F}};
			ground.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = descriptor.canonicalPlacementRevision = descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto terrain = NavigationGeometrySnapshot::Create(descriptor, ground);
			NavigationGeometryDocument obstacle{};
			obstacle.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			NavigationGeometryPrimitive box{};
			box.kind = NavigationGeometryPrimitiveKind::Box;
			box.center = {32.5F, 1.0F, 16.0F};
			box.halfExtents = {1.0F, 1.0F, 4.0F};
			obstacle.primitives.push_back(box);
			descriptor.sourceKind = NavigationSourceKind::Collider;
			descriptor.sourceRevision = 2U;
			const auto collider = NavigationGeometrySnapshot::Create(descriptor, obstacle);
			NavigationGeometryDocument boundaryObstacle{};
			boundaryObstacle.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			// This transformed box begins just beyond tile 0's raster halo. Keeping its
			// canonical-frame offset nonzero exercises the same float conversion used by
			// shard-local provider input, rather than relying on an integer tile edge.
			boundaryObstacle.primitives.push_back({NavigationGeometryPrimitiveKind::Box,
				{33.900001F, 1.0F, 16.0F}, {1.0F, 1.0F, 1.0F}, 0.0F, 0.0F});
			NavigationGeometrySnapshotDescriptor boundaryDescriptor = descriptor;
			boundaryDescriptor.sourceRevision = 3U;
			boundaryDescriptor.canonicalFrameOrigin.offsetMetres.x = 0.000001;
			const auto boundaryCollider = NavigationGeometrySnapshot::Create(boundaryDescriptor, boundaryObstacle);
			RecastGroundBuildInput input{};
			input.profile = NavigationProfile::Defaults();
			input.sources = {terrain.snapshot, collider.snapshot, boundaryCollider.snapshot};
			const auto full = BuildRecastGroundTiles(input);
			input.maximumTiles = 1U;
			const auto boundedFull = BuildRecastGroundTiles(input);
			input.requestedTiles = {{0, 0, 0, MakeGroundProfileBuildKey(input.profile)}};
			const auto selected = BuildRecastGroundTiles(input);
			// The blocker straddles the selected tile boundary. Exact byte equality
			// proves the selective build retained its halo influence and provider input.
			const auto matching = std::find_if(full.tiles.begin(), full.tiles.end(),
				[&input](const GroundTileBlob& tile) { return tile.key == input.requestedTiles.front(); });
			context.Expect(terrain.accepted && collider.accepted && boundaryCollider.accepted
				&& full.Succeeded() && full.tiles.size() > 1U
				&& boundedFull.status == RecastGroundBuildStatus::BudgetExceeded
				&& selected.Succeeded() && selected.tiles.size() == 1U
				&& matching != full.tiles.end()
				&& selected.tiles.front().pristineBytes == matching->pristineBytes,
				"A one-tile request must avoid unrelated rasterization while preserving full-build border geometry exactly.");
			input.requestedTiles.front().profileBuildKey ^= 1U;
			const auto wrongProfile = BuildRecastGroundTiles(input);
			context.Expect(!wrongProfile.Succeeded() && wrongProfile.reason == "RequestedTileProfileMismatch",
				"A worker's tile selection must never substitute another traversal profile.");
			input.requestedTiles.clear();
			input.maximumTiles = 4U;
			for(const double resolution : {0.001, 1.0e-50}) {
				input.profile.horizontalResolutionMetres = resolution;
				const auto unsupportedGrid = BuildRecastGroundTiles(input);
				context.Expect(input.profile.Validate().accepted
					&& unsupportedGrid.status == RecastGroundBuildStatus::RecastConfigurationFailed
					&& unsupportedGrid.tiles.empty(),
					"A mathematically valid but unsupported provider resolution must reject before unsafe float/int narrowing or allocation.");
			}
		});

		registry.Add("Navigation.Backend.RequestedNegativeAndVerticalTilesPreserveHaloBytes", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument terrainDocument{};
			terrainDocument.kind = NavigationGeometryPayloadKind::TriangleMesh;
			terrainDocument.vertices = {
				{-63.0F, 1.0F, -31.0F}, {63.0F, 1.0F, 31.0F}, {63.0F, 1.0F, -31.0F}, {-63.0F, 1.0F, 31.0F},
				{-63.0F, 129.0F, -31.0F}, {63.0F, 129.0F, 31.0F}, {63.0F, 129.0F, -31.0F}, {-63.0F, 129.0F, 31.0F}};
			terrainDocument.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}, {4U, 5U, 6U}, {4U, 7U, 5U}};
			NavigationGeometrySnapshotDescriptor terrainDescriptor{};
			terrainDescriptor.sourceRevision = terrainDescriptor.canonicalPlacementRevision
				= terrainDescriptor.physicalReadyRevision = 1U;
			terrainDescriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto terrain = NavigationGeometrySnapshot::Create(terrainDescriptor, terrainDocument);

			NavigationGeometryDocument blockerDocument{};
			blockerDocument.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			blockerDocument.primitives.push_back({NavigationGeometryPrimitiveKind::Box,
				{-32.25F, 2.0F, 0.0F}, {2.0F, 1.0F, 3.0F}, 0.0F, 0.0F});
			NavigationGeometrySnapshotDescriptor blockerDescriptor = terrainDescriptor;
			blockerDescriptor.sourceRevision = 2U;
			blockerDescriptor.sourceKind = NavigationSourceKind::Collider;
			const auto blocker = NavigationGeometrySnapshot::Create(blockerDescriptor, blockerDocument);

			RecastGroundBuildInput input{};
			input.profile = NavigationProfile::Defaults();
			input.sources = {terrain.snapshot, blocker.snapshot};
			input.maximumTiles = 32U;
			const RecastGroundBuildResult full = BuildRecastGroundTiles(input);
			const std::uint64_t profileKey = MakeGroundProfileBuildKey(input.profile);
			const auto negativeTarget = std::find_if(full.tiles.begin(), full.tiles.end(),
				[profileKey](const GroundTileBlob& tile) {
					return tile.key.profileBuildKey == profileKey && tile.key.x < 0
						&& tile.key.verticalBand == 0;
				});
			const auto upperTarget = std::find_if(full.tiles.begin(), full.tiles.end(),
				[profileKey](const GroundTileBlob& tile) {
					return tile.key.profileBuildKey == profileKey && tile.key.x < 0
						&& tile.key.verticalBand > 0;
				});
			bool subsetBytesMatch = full.Succeeded() && negativeTarget != full.tiles.end()
				&& upperTarget != full.tiles.end();
			if(subsetBytesMatch) {
				for(const GroundTileBlob* target : {&*negativeTarget, &*upperTarget}) {
					RecastGroundBuildInput subset = input;
					subset.requestedTiles = {target->key};
					const RecastGroundBuildResult selected = BuildRecastGroundTiles(subset);
					subsetBytesMatch = subsetBytesMatch && selected.Succeeded() && selected.tiles.size() == 1U
						&& selected.tiles.front().key == target->key
						&& selected.tiles.front().pristineBytes == target->pristineBytes;
				}
			}
			context.Expect(full.Succeeded() && negativeTarget != full.tiles.end()
				&& upperTarget != full.tiles.end() && subsetBytesMatch,
				"Requested negative and upper vertical tiles must reproduce full-build bytes while retaining nearby collider and triangle halo influence.");
		});

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

		registry.Add("Navigation.Backend.RequestedUnlocalizedTilePublishesAuthoritativeEmptyCoverage", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			// The source bounds span a 2x2 tile rectangle, but the two quads occupy
			// opposite corners. Tile (0,1) is therefore a valid metadata key with no
			// localized triangles; it must be represented as empty coverage, not omitted.
			document.vertices = {
				{1.0F, 1.0F, 1.0F}, {7.0F, 1.0F, 7.0F}, {7.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 7.0F},
				{33.0F, 1.0F, 33.0F}, {39.0F, 1.0F, 39.0F}, {39.0F, 1.0F, 33.0F}, {33.0F, 1.0F, 39.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}, {4U, 5U, 6U}, {4U, 7U, 5U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = descriptor.canonicalPlacementRevision
				= descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto snapshot = NavigationGeometrySnapshot::Create(descriptor, document);
			RecastGroundBuildInput input{};
			input.profile = NavigationProfile::Defaults();
			input.sources = {snapshot.snapshot};
			input.maximumTiles = 8U;
			const RecastGroundBuildResult full = BuildRecastGroundTiles(input);
			const GroundTileKey emptyKey{0, 1, 0, MakeGroundProfileBuildKey(input.profile)};
			const auto fullEmpty = std::find_if(full.tiles.begin(), full.tiles.end(),
				[&emptyKey](const GroundTileBlob& tile) { return tile.key == emptyKey; });
			input.requestedTiles = {emptyKey};
			const RecastGroundBuildResult selected = BuildRecastGroundTiles(input);
			context.Expect(snapshot.accepted && full.Succeeded() && fullEmpty != full.tiles.end()
				&& fullEmpty->empty && fullEmpty->pristineBytes.empty()
				&& fullEmpty->supportComplete && fullEmpty->clearanceComplete
				&& selected.Succeeded() && selected.tiles.size() == 1U
				&& selected.tiles.front().key == emptyKey && selected.tiles.front().empty
				&& selected.tiles.front().pristineBytes.empty()
				&& selected.tiles.front().sourceFingerprints == fullEmpty->sourceFingerprints,
				"A requested tile with valid source-derived coverage but no localized geometry must publish the same authoritative empty tile as the full build.");
		});

		registry.Add("Navigation.Backend.CoverageEvidenceMatchesTerrainColliderBuildAndSubset", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument terrainDocument{};
			terrainDocument.kind = NavigationGeometryPayloadKind::TriangleMesh;
			terrainDocument.vertices = {{1.0F, 0.0F, 1.0F}, {63.0F, 0.0F, 31.0F},
				{63.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 31.0F}};
			terrainDocument.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor terrainDescriptor{};
			terrainDescriptor.sourceRevision = terrainDescriptor.canonicalPlacementRevision
				= terrainDescriptor.physicalReadyRevision = 1U;
			terrainDescriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto terrain = NavigationGeometrySnapshot::Create(terrainDescriptor, terrainDocument);

			NavigationGeometryDocument colliderDocument{};
			colliderDocument.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			colliderDocument.primitives.push_back({NavigationGeometryPrimitiveKind::Box,
				{32.0F, 1.0F, 16.0F}, {1.0F, 1.0F, 3.0F}, 0.0F, 0.0F});
			NavigationGeometrySnapshotDescriptor colliderDescriptor = terrainDescriptor;
			colliderDescriptor.sourceKind = NavigationSourceKind::Collider;
			colliderDescriptor.sourceRevision = 2U;
			const auto collider = NavigationGeometrySnapshot::Create(colliderDescriptor, colliderDocument);

			RecastGroundBuildInput input{};
			input.profile = NavigationProfile::Defaults();
			input.sources = {terrain.snapshot, collider.snapshot};
			const GroundCoverageEvidence evidence = CaptureGroundCoverageEvidence(input);
			const RecastGroundBuildResult full = BuildRecastGroundTiles(input);
			const bool fullKeysAgree = full.Succeeded() && evidence.accepted
				&& evidence.tileKeys.size() == full.tiles.size()
				&& std::equal(evidence.tileKeys.begin(), evidence.tileKeys.end(), full.tiles.begin(),
					[](const GroundTileKey& key, const GroundTileBlob& tile) { return key == tile.key; });
			const bool fullProvenanceAgrees = fullKeysAgree && std::all_of(full.tiles.begin(), full.tiles.end(),
				[&evidence](const GroundTileBlob& tile) {
					return tile.sourceFingerprints == evidence.sourceFingerprints
						&& !tile.sourceFingerprints.empty();
				});

			input.requestedTiles = {full.tiles.empty() ? GroundTileKey{} : full.tiles.back().key};
			const RecastGroundBuildResult selected = BuildRecastGroundTiles(input);
			const bool subsetAgrees = selected.Succeeded() && selected.tiles.size() == 1U
				&& selected.tiles.front().key == input.requestedTiles.front()
				&& selected.tiles.front().sourceFingerprints == evidence.sourceFingerprints;

			NavigationGeometryDocument unsupportedDocument{};
			unsupportedDocument.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
			unsupportedDocument.primitives.push_back({NavigationGeometryPrimitiveKind::Sphere,
				{12.0F, 1.0F, 12.0F}, {1.0F, 1.0F, 1.0F}, 1.0F, 0.0F});
			NavigationGeometrySnapshotDescriptor unsupportedDescriptor = colliderDescriptor;
			unsupportedDescriptor.sourceRevision = 3U;
			const auto unsupported = NavigationGeometrySnapshot::Create(unsupportedDescriptor, unsupportedDocument);
			RecastGroundBuildInput unsupportedInput = input;
			unsupportedInput.requestedTiles.clear();
			unsupportedInput.sources = {terrain.snapshot, unsupported.snapshot};
			const GroundCoverageEvidence unsupportedEvidence = CaptureGroundCoverageEvidence(unsupportedInput);
			const RecastGroundBuildResult unsupportedBuild = BuildRecastGroundTiles(unsupportedInput);

			context.Expect(fullKeysAgree && fullProvenanceAgrees && subsetAgrees
				&& unsupported.accepted && !unsupportedEvidence.accepted
				&& unsupportedEvidence.reason == "UnsupportedAnalyticPrimitiveForRecast"
				&& !unsupportedBuild.Succeeded()
				&& unsupportedBuild.reason == "UnsupportedAnalyticPrimitiveForRecast",
				"Coverage evidence must use the same canonical tile keys and source fingerprints as the real terrain/collider build and requested subset, while unsupported analytic primitives must reject both evidence and provider work.");
		});

		registry.Add("Navigation.Backend.NonzeroCanonicalCellsBuildReachableShardLocalRoutes", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			for(const std::int64_t cellX : {17LL, -17LL}) {
				NavigationGeometryDocument document{};
				document.kind = NavigationGeometryPayloadKind::TriangleMesh;
				document.vertices = {{1.0F, 0.0F, 1.0F}, {8.0F, 0.0F, 8.0F},
					{8.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 8.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
				NavigationGeometrySnapshotDescriptor descriptor{};
				descriptor.sourceRevision = descriptor.canonicalPlacementRevision = descriptor.physicalReadyRevision = 1U;
				descriptor.sourceKind = NavigationSourceKind::Terrain;
				descriptor.canonicalFrameOrigin.cell.x = cellX;
				const auto snapshot = NavigationGeometrySnapshot::Create(descriptor, document);
				RecastGroundBuildInput input{}; input.profile = NavigationProfile::Defaults(); input.sources = {snapshot.snapshot};
				const RecastGroundBuildResult built = BuildRecastGroundTiles(input);
				const std::int64_t shardX = cellX * 256LL;
				DetourGroundShardBuildInput shardInput{};
				shardInput.shard = {shardX, 0LL, 0, MakeGroundProfileBuildKey(input.profile)};
				shardInput.expectedGeneration = 1U; shardInput.tiles = built.tiles;
				const auto shard = AssembleDetourGroundShard(shardInput);
				const auto route = shard.generation ? QueryDetourGroundShard(*shard.generation,
					{{2.0F, 0.0F, 2.0F}, {7.0F, 0.0F, 7.0F}, 2.0F, 4.0F, 32U}) : DetourGroundRouteResult{};
				context.Expect(snapshot.accepted && built.Succeeded() && !built.tiles.empty()
					&& built.tiles.front().key.x == cellX * 2048LL && shard.accepted
					&& route.outcome == NavigationOutcome::Complete,
					"Positive and negative canonical cells must retain exact tile identity while Detour routes in a bounded shard-local frame.");
			}
		});

		registry.Add("Navigation.Backend.RejectsExtremeCanonicalFrameBeforeTileNarrowing", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
			document.triangles = {{0U, 1U, 2U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = descriptor.canonicalPlacementRevision = descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			descriptor.canonicalFrameOrigin.cell.x = std::numeric_limits<std::int64_t>::max();
			const auto snapshot = NavigationGeometrySnapshot::Create(descriptor, document);
			RecastGroundBuildInput input{}; input.profile = NavigationProfile::Defaults(); input.sources = {snapshot.snapshot};
			const auto built = BuildRecastGroundTiles(input);
			context.Expect(snapshot.accepted && !built.Succeeded()
				&& built.reason == "SourceCanonicalTileAddressOverflow",
				"An unrepresentable canonical cell must reject before tile arithmetic can overflow or provider allocation begins.");
		});

		registry.Add("Navigation.Backend.DetourRejectsTruncatedExactRouteOutput", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument ground{};
			ground.kind = NavigationGeometryPayloadKind::TriangleMesh;
			ground.vertices = {{1.0F, 0.0F, 1.0F}, {8.0F, 0.0F, 8.0F},
				{8.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 8.0F}};
			ground.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto snapshot = NavigationGeometrySnapshot::Create(descriptor, ground);

			RecastGroundBuildInput buildInput{};
			buildInput.profile = NavigationProfile::Defaults();
			buildInput.sources = {snapshot.snapshot};
			buildInput.maximumTiles = 4U;
			const RecastGroundBuildResult built = BuildRecastGroundTiles(buildInput);
			DetourGroundShardBuildInput shardInput{};
			shardInput.shard = {0, 0, 0, MakeGroundProfileBuildKey(buildInput.profile)};
			shardInput.expectedGeneration = 1U;
			shardInput.tiles = built.tiles;
			const DetourGroundShardBuildResult shard = AssembleDetourGroundShard(shardInput);
			// One point holds the start but cannot hold the mandatory distinct end. The
			// real provider returns DT_BUFFER_TOO_SMALL, which must not become Complete.
			const DetourGroundRouteResult route = shard.generation ? QueryDetourGroundShard(*shard.generation,
				{{2.0F, 0.0F, 2.0F}, {7.0F, 0.0F, 7.0F}, 2.0F, 4.0F, 1U}) : DetourGroundRouteResult{};

			context.Expect(snapshot.accepted && built.Succeeded() && shard.accepted
				&& route.outcome == NavigationOutcome::BudgetExhausted
				&& route.reason == NavigationReason::OutputLimit && route.points.empty(),
				"A Detour straight-path output buffer limit must reject the exact route rather than publish a truncated success.");
		});

		registry.Add("Navigation.Backend.NearestReachablePreservesPartialCorridorAndBudgets", [](TestContext& context) {
			using namespace TerraEngine::Navigation;
			NavigationGeometryDocument ground{};
			ground.kind = NavigationGeometryPayloadKind::TriangleMesh;
			// Both islands are in the same 256 m provider shard, but no triangle
			// connects them. This forces Detour to return its best reachable corridor
			// rather than exercising the cross-shard portal classifier.
			ground.vertices = {
				{1.0F, 0.0F, 1.0F}, {8.0F, 0.0F, 8.0F}, {8.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 8.0F},
				{20.0F, 0.0F, 1.0F}, {27.0F, 0.0F, 8.0F}, {27.0F, 0.0F, 1.0F}, {20.0F, 0.0F, 8.0F}};
			ground.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}, {4U, 5U, 6U}, {4U, 7U, 5U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = descriptor.canonicalPlacementRevision = descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto snapshot = NavigationGeometrySnapshot::Create(descriptor, ground);
			RecastGroundBuildInput buildInput{};
			buildInput.profile = NavigationProfile::Defaults();
			buildInput.sources = {snapshot.snapshot};
			buildInput.maximumTiles = 4U;
			const RecastGroundBuildResult built = BuildRecastGroundTiles(buildInput);
			DetourGroundShardBuildInput shardInput{};
			shardInput.shard = {0, 0, 0, MakeGroundProfileBuildKey(buildInput.profile)};
			shardInput.expectedGeneration = 1U;
			shardInput.tiles = built.tiles;
			const DetourGroundShardBuildResult shard = AssembleDetourGroundShard(shardInput);

			DetourGroundRouteQuery exact{};
			exact.start = {4.0F, 0.0F, 4.0F};
			exact.destination = {24.0F, 0.0F, 4.0F};
			exact.horizontalSearchRadius = 2.0F;
			exact.verticalSearchRadius = 4.0F;
			exact.maximumPoints = 64U;
			exact.maximumSearchNodes = 128U;
			const DetourGroundRouteResult exactRoute = shard.generation
				? QueryDetourGroundShard(*shard.generation, exact) : DetourGroundRouteResult{};

			DetourGroundRouteQuery nearestReachable = exact;
			nearestReachable.allowPartial = true;
			const DetourGroundRouteResult partialRoute = shard.generation
				? QueryDetourGroundShard(*shard.generation, nearestReachable) : DetourGroundRouteResult{};

			DetourGroundRouteQuery tinyBudget = nearestReachable;
			tinyBudget.maximumPoints = 1U;
			const DetourGroundRouteResult tinyBudgetRoute = shard.generation
				? QueryDetourGroundShard(*shard.generation, tinyBudget) : DetourGroundRouteResult{};
			const NavigationGeometryVector3 requested = exact.destination;
			const NavigationGeometryVector3 lastPartial = partialRoute.points.empty()
				? NavigationGeometryVector3{} : partialRoute.points.back();

			context.Expect(snapshot.accepted && built.Succeeded() && shard.accepted
				&& exactRoute.outcome == NavigationOutcome::Unreachable
				&& exactRoute.reason == NavigationReason::MissingSupport
				&& partialRoute.outcome == NavigationOutcome::Partial
				&& partialRoute.reason == NavigationReason::DomainLimit
				&& !partialRoute.points.empty()
				&& lastPartial != requested && lastPartial.x < 12.0F
				&& tinyBudgetRoute.outcome == NavigationOutcome::BudgetExhausted
				&& tinyBudgetRoute.reason == NavigationReason::OutputLimit,
				"NearestReachable must expose only the real same-shard reachable corridor, while exact and tiny-budget requests remain fail-closed.");
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
			// The public one-point limit bounds returned guidance only. The provider
			// must still search safely with its independent bounded node pool, then
			// reject the truncated straight path as an output-budget result.
			const DetourGroundRouteResult outputLimitedRoute = shard.generation ? QueryDetourGroundShard(*shard.generation,
				{{4.0F, 0.0F, 16.0F}, {28.0F, 0.0F, 16.0F}, 2.0F, 4.0F, 1U}) : DetourGroundRouteResult{};
			const bool leavesObstacleFootprint = std::any_of(route.points.begin(), route.points.end(),
				[](const NavigationGeometryVector3& point) { return point.z < 11.0F || point.z > 21.0F; });

			context.Expect(groundSnapshot.accepted && obstacleSnapshot.accepted && built.Succeeded()
				&& shard.accepted && route.outcome == NavigationOutcome::Complete && leavesObstacleFootprint,
				"A world-space automatic collider must remove its footprint from Recast coverage so a Detour route detours around it.");
			context.Expect(outputLimitedRoute.outcome == NavigationOutcome::BudgetExhausted
				&& outputLimitedRoute.reason == NavigationReason::OutputLimit && outputLimitedRoute.points.empty(),
				"A Detour output-point limit must reject the exact route rather than restrict its internal search pool.");
		});
	}
}
