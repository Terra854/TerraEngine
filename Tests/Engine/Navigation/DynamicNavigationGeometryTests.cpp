// DynamicNavigationGeometryTests.cpp - general dynamic geometry acceptance matrix.

#include "TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Backend/RecastGroundBuilder.h"
#include "Navigation/Dynamic/NavigationDirtyIndex.h"
#include "Navigation/Dynamic/NavigationRebuildPipeline.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Scene/SceneMutationQueue.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Navigation;

		enum class Operation { Create, Enable, Disable, Move, Rotate, Scale, Delete, Replace, Reshape };
		enum class Shape { TerrainSupport, MeshSupport, Stair, Ramp, WallObstructor, LowCeiling, Gap };
		struct Case { std::string_view id; Operation operation; Shape shape; bool becomesBlocked; };

		[[nodiscard]] NavigationLocation AutomaticLocation(
			NavigationWorldHandle world, double x, double z = 0.0) {
			NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, 0.0, z};
			return location;
		}

		[[nodiscard]] NavigationEligibleSourceCandidate AutomaticSource(
			NavigationSourceKind kind, NavigationGeneration revision, double originX,
			std::shared_ptr<const NavigationGeometrySnapshot>& geometry,
			const NavigationEligibleSourceIdentity* retainedIdentity = nullptr) {
			NavigationEligibleSourceCandidate candidate{};
			if(retainedIdentity != nullptr) candidate.identity = *retainedIdentity;
			else {
				candidate.identity.sourceId = GenerateUUID();
				candidate.identity.objectId = GenerateUUID();
				candidate.identity.objectLifetimeGeneration = 1U;
				candidate.identity.assetId = GenerateUUID();
				candidate.identity.kind = kind;
			}
			candidate.revisions = {1U, revision, 1U, 1U, 1U};
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			if(kind == NavigationSourceKind::Terrain) {
				document.vertices = {{-32.0F, 0.0F, -32.0F}, {32.0F, 0.0F, 32.0F},
					{32.0F, 0.0F, -32.0F}, {-32.0F, 0.0F, 32.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			} else {
				// The small vertical mesh gives the diff a finite local swept bound
				// without taking ownership of any live collider or scene object.
				document.vertices = {{-1.0F, 0.0F, -1.0F}, {1.0F, 2.0F, -1.0F},
					{1.0F, 0.0F, 1.0F}, {-1.0F, 2.0F, 1.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 2U, 3U}};
			}
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceUuid.value = candidate.identity.sourceId;
			descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(
				candidate.identity, candidate.revisions);
			descriptor.canonicalPlacementRevision = revision;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = kind;
			descriptor.canonicalFrameOrigin.offsetMetres.x = originX;
			geometry = NavigationGeometrySnapshot::Create(descriptor, std::move(document)).snapshot;
			candidate.geometry = geometry;
			candidate.activeInHierarchy = true;
			candidate.physicalBackingCurrent = true;
			return candidate;
		}

		[[nodiscard]] NavigationSystem::InitialGroundBuildInput AutomaticBuild(
			NavigationProfile profile,
			const NavigationEligibleSourceCandidate& terrain,
			const NavigationEligibleSourceCandidate& changed) {
			const std::array<NavigationEligibleSourceCandidate, 2U> candidates{{terrain, changed}};
			NavigationEligibleSourceIndexBuildResult indexed = BuildNavigationEligibleSourceIndex(candidates);
			NavigationSystem::InitialGroundBuildInput build{};
			build.profile = std::move(profile);
			build.sources = {terrain.geometry, changed.geometry};
			build.sourceIndex = {{terrain.identity.sourceId, terrain.geometry->Descriptor().sourceRevision,
				terrain.revisions.geometryRevision, true, terrain.identity, terrain.revisions},
				{changed.identity.sourceId, changed.geometry->Descriptor().sourceRevision,
				changed.revisions.geometryRevision, true, changed.identity, changed.revisions}};
			build.eligibleSourceIndex = indexed.accepted
				? std::make_shared<const NavigationEligibleSourceIndex>(std::move(indexed.index)) : nullptr;
			build.terrainPublicationGeneration = 1U;
			return build;
		}

		[[nodiscard]] NavigationRequestHandle SubmitAutomaticRoute(
			NavigationSystem& system, NavigationWorldHandle world, std::uint64_t owner, double startX, double destinationX) {
			Ground::GroundRouteQuery::Request request{};
			request.owner = {world, NavigationOwnerId{owner}, 1U};
			request.start = AutomaticLocation(world, startX);
			request.destination = AutomaticLocation(world, destinationX);
			request.profile = NavigationProfile::Defaults();
			// Use the normal public request budget. This regression observes source
			// replacement validity, not a deliberately constrained provider search.
			request.maxExpansions = 32'768U;
			return system.SubmitOneShotRoute(std::move(request));
		}

		// Tests observe only the bounded aggregate publication counters. They never
		// retain provider tile bytes or mutable worker packets merely to prove reuse.
		struct DynamicTileWorkSnapshot {
			std::uint64_t reuseAttempts = 0U;
			std::uint64_t reusedTiles = 0U;
			std::uint64_t rebuiltTiles = 0U;
			std::uint64_t fullShardFallbacks = 0U;
		};

		[[nodiscard]] DynamicTileWorkSnapshot DynamicTileWork(const NavigationSystem& system) {
			const std::shared_ptr<const NavigationDiagnosticSnapshot> diagnostics = system.PublishedDiagnostics();
			if(!diagnostics) return {};
			return {diagnostics->dynamicTileReuseAttemptCount, diagnostics->dynamicTileReuseTileCount,
				diagnostics->dynamicTileRebuildTileCount, diagnostics->dynamicTileReuseFallbackCount};
		}

		using DynamicShardAddress = std::tuple<std::int64_t, std::int64_t, std::int32_t>;

		[[nodiscard]] std::int64_t DynamicShardCoordinate(std::int64_t tile) noexcept {
			// C++ truncates negative division toward zero. The worker uses floor
			// addressing, so the test mirrors that rule before it groups retained packs.
			std::int64_t coordinate = tile / 8LL;
			if(tile % 8LL < 0LL) --coordinate;
			return coordinate;
		}

		[[nodiscard]] std::vector<NavigationDynamicShardReuseBaseline> RetainFullTileBaselines(
			const NavigationProfile& profile,
			const std::vector<std::shared_ptr<const NavigationGeometrySnapshot>>& sources,
			NavigationGeneration rootGeneration, NavigationGeneration terrainPublicationGeneration,
			NavigationGeneration sourceIndexRevision) {
			RecastGroundBuildInput input{};
			input.profile = profile;
			input.sources = sources;
			const RecastGroundBuildResult built = BuildRecastGroundTiles(input);
			if(!built.Succeeded()) return {};

			std::map<DynamicShardAddress, std::vector<GroundTileBlob>> tilesByShard;
			for(const GroundTileBlob& tile : built.tiles) {
				const DynamicShardAddress address{DynamicShardCoordinate(tile.key.x),
					DynamicShardCoordinate(tile.key.z), tile.key.verticalBand};
				tilesByShard[address].push_back(tile);
			}

			std::vector<NavigationDynamicShardReuseBaseline> baselines;
			baselines.reserve(tilesByShard.size());
			const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(profile);
			for(auto& [address, tiles] : tilesByShard) {
				NavigationDynamicShardReuseBaseline baseline{};
				baseline.reference.shard = {std::get<0>(address), std::get<1>(address),
					std::get<2>(address), profileBuildKey};
				baseline.reference.generation = rootGeneration;
				baseline.rootGeneration = rootGeneration;
				baseline.terrainPublicationGeneration = terrainPublicationGeneration;
				baseline.sourceIndexRevision = sourceIndexRevision;
				auto retainedPack = std::make_shared<RetainedGroundTilePack>();
				retainedPack->tiles = std::move(tiles);
				baseline.retainedTiles = std::move(retainedPack);
				baselines.push_back(std::move(baseline));
			}
			return baselines;
		}

		[[nodiscard]] NavigationDynamicRebuildRequest DirectDynamicRequest(
			const NavigationProfile& profile, const NavigationSystem::InitialGroundBuildInput& previous,
			const NavigationSystem::InitialGroundBuildInput& replacement,
			NavigationUuid changedSource, NavigationGeneration changedRevision,
			const NavigationGeometryBounds& previousBounds, const NavigationGeometryBounds& currentBounds,
			NavigationDynamicChangeKind kind = NavigationDynamicChangeKind::Moved) {
			constexpr NavigationGeneration kRootGeneration = 11U;
			constexpr NavigationGeneration kTerrainPublicationGeneration = 13U;
			constexpr NavigationGeneration kSourceIndexRevision = 17U;
			NavigationDynamicSourceChange change{};
			change.source = changedSource;
			change.sourceRevision = changedRevision;
			change.changeSequence = 19U;
			change.physicalReadyRevision = 1U;
			change.kind = kind;
			change.previousBounds = previousBounds;
			change.currentBounds = currentBounds;

			NavigationDirtyIndexSettings dirtySettings{};
			if(const std::optional<GroundTileBuildHalo> halo = ComputeGroundTileBuildHalo(profile)) {
				// The direct packet must use the same conservative halo as production
				// dirty admission. Otherwise a correct reuse plan would fail solely
				// because the fixture omitted coverage that the worker must inspect.
				dirtySettings.buildHaloMetres = static_cast<float>(std::max(
					halo->horizontalMetres, halo->verticalMetres));
			}
			NavigationDirtyIndex dirty(dirtySettings);
			const NavigationDynamicChangeReceipt receipt = dirty.CommitSourceChange(change);
			NavigationDynamicRebuildRequest request{};
			request.change = change;
			request.expectedRootGeneration = kRootGeneration;
			request.expectedTerrainPublicationGeneration = kTerrainPublicationGeneration;
			request.expectedSourceIndexRevision = kSourceIndexRevision;
			request.profile = profile;
			request.tombstones = dirty.ObserveAtProtectedBoundary();
			request.pendingChanges = dirty.ObservePendingChanges();
			request.dirtyCoverage = receipt.accepted
				? std::vector<NavigationDirtyCoverageDependency>(dirty.DirtyCoverage().begin(), dirty.DirtyCoverage().end())
				: std::vector<NavigationDirtyCoverageDependency>{};
			request.sourceIndex = replacement.sourceIndex;
			request.sources = replacement.sources;
			request.reuseBaselines = RetainFullTileBaselines(profile, previous.sources,
				kRootGeneration, kTerrainPublicationGeneration, kSourceIndexRevision);
			return request;
		}

		[[nodiscard]] bool EquivalentCoverage(const NavigationDynamicRebuildPlan& first,
			const NavigationDynamicRebuildPlan& second) {
			if(!first.accepted || !second.accepted || first.candidate.coverage.size() != second.candidate.coverage.size()) return false;
			for(std::size_t index = 0U; index < first.candidate.coverage.size(); ++index) {
				const NavigationCoverageRecord& left = first.candidate.coverage[index];
				const NavigationCoverageRecord& right = second.candidate.coverage[index];
				if(!(left.key == right.key) || left.state != right.state || left.complete != right.complete) return false;
			}
			return true;
		}

		NavigationDynamicChangeKind ToDynamicKind(Operation operation) {
			switch(operation) {
			case Operation::Create: return NavigationDynamicChangeKind::Created;
			case Operation::Enable: return NavigationDynamicChangeKind::Enabled;
			case Operation::Disable: return NavigationDynamicChangeKind::Disabled;
			case Operation::Move: return NavigationDynamicChangeKind::Moved;
			case Operation::Rotate: return NavigationDynamicChangeKind::Rotated;
			case Operation::Scale: return NavigationDynamicChangeKind::Scaled;
			case Operation::Delete: return NavigationDynamicChangeKind::Removed;
			case Operation::Replace: return NavigationDynamicChangeKind::Replaced;
			case Operation::Reshape: return NavigationDynamicChangeKind::Reshaped;
			}
			return NavigationDynamicChangeKind::Reshaped;
		}

		// One case for every supported mutation operation, exercised against both a
		// support and an obstruction. Future publication tests must prove old and
		// new clearance-expanded bounds invalidate before a route is consumed.
		constexpr std::array<Case, 18U> kCases{{
			{"create-support",Operation::Create,Shape::TerrainSupport,false},{"create-wall",Operation::Create,Shape::WallObstructor,true},
			{"enable-support",Operation::Enable,Shape::MeshSupport,false},{"enable-ceiling",Operation::Enable,Shape::LowCeiling,true},
			{"disable-support",Operation::Disable,Shape::Ramp,true},{"disable-wall",Operation::Disable,Shape::WallObstructor,false},
			{"move-support",Operation::Move,Shape::Stair,true},{"move-wall",Operation::Move,Shape::WallObstructor,true},
			{"rotate-support",Operation::Rotate,Shape::Ramp,true},{"rotate-wall",Operation::Rotate,Shape::WallObstructor,true},
			{"scale-support",Operation::Scale,Shape::MeshSupport,true},{"scale-ceiling",Operation::Scale,Shape::LowCeiling,true},
			{"delete-support",Operation::Delete,Shape::TerrainSupport,true},{"delete-wall",Operation::Delete,Shape::WallObstructor,false},
			{"replace-support",Operation::Replace,Shape::Gap,true},{"replace-wall",Operation::Replace,Shape::WallObstructor,true},
			{"reshape-support",Operation::Reshape,Shape::Stair,true},{"reshape-wall",Operation::Reshape,Shape::WallObstructor,true}
		}};
	}
	void RegisterDynamicNavigationGeometryTests(TestRegistry& registry) {
		registry.Add("Navigation.DynamicGeometry.CoversEveryFr014OperationAndSupportObstructorBounds", [](TestContext& context) {
			std::array<bool, 9U> seen{}; std::size_t blocked = 0U, reopened = 0U;
			for(const Case& testCase : kCases) { seen[static_cast<std::size_t>(testCase.operation)] = true; testCase.becomesBlocked ? ++blocked : ++reopened; }
			for(bool operationCovered : seen) context.Expect(operationCovered, "Every supported mutation must invalidate old/new affected bounds.");
			context.Expect(blocked > 0U && reopened > 0U, "The matrix must cover blocked and reopened connectivity/clearance behavior.");
		});
		registry.Add("Navigation.DynamicGeometry.CommitsTenThousandGeneralSourceChanges", [](TestContext& context) {
			NavigationDirtyIndex dirty({
				.clearanceExpansionMetres = 0.55F,
				.buildHaloMetres = 0.25F,
				.maximumPendingChanges = 10000U});
			const NavigationUuid source = NavigationUuid::FromString("ef5115c2-4c64-45a7-8d89-678074ca9fd2");
			std::size_t accepted = 0U;
			NavigationGeneration latestRemovalRevision = 0U;
			for(std::size_t index = 0U; index < 10000U; ++index) {
				const Case& testCase = kCases[index % kCases.size()];
				const float offset = static_cast<float>(index % 8U);
				NavigationDynamicSourceChange change{};
				change.source = source;
				change.sourceRevision = index + 1U;
				change.physicalReadyRevision = index + 1U;
				change.kind = ToDynamicKind(testCase.operation);
				change.previousBounds = {{offset, 0.0F, 0.0F}, {offset + 2.0F, 2.0F, 2.0F}};
				change.currentBounds = {{offset + 1.0F, 0.0F, 0.0F}, {offset + 3.0F, 2.0F, 2.0F}};
				accepted += dirty.CommitSourceChange(change).accepted ? 1U : 0U;
				if(change.kind == NavigationDynamicChangeKind::Removed
					|| change.kind == NavigationDynamicChangeKind::Disabled) latestRemovalRevision = change.sourceRevision;
			}
			context.Expect(accepted == 10000U,
				"All 10,000 committed changes across the supported operation matrix must enter bounded local preparation.");
			context.Expect(dirty.PendingPreparationCount() == 10000U,
				"The qualification workload must not silently coalesce or discard committed source changes before observation.");
			context.Expect(dirty.ObserveAtProtectedBoundary().Contains(source, latestRemovalRevision),
				"The latest removal tombstone must remain observable throughout the 10,000-change workload.");
		});
		registry.Add("Navigation.DynamicGeometry.OrdersDecreasingContentHashesByCommittedSequence", [](TestContext& context) {
			NavigationDirtyIndex dirty({});
			NavigationDynamicSourceChange first{};
			first.source = NavigationUuid::FromString("ef5115c2-4c64-45a7-8d89-678074ca9fd2");
			first.sourceRevision = 900U;
			first.changeSequence = 40U;
			first.physicalReadyRevision = 1U;
			first.kind = NavigationDynamicChangeKind::Moved;
			first.previousBounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
			first.currentBounds = {{1.0F, 0.0F, 0.0F}, {2.0F, 1.0F, 1.0F}};
			NavigationDynamicSourceChange newer = first;
			// Automatic source revisions are content hashes. Deliberately make the
			// later content value smaller to prove publication ordering is separate.
			newer.sourceRevision = 17U;
			newer.changeSequence = 41U;
			context.Expect(dirty.CommitSourceChange(first).accepted && dirty.CommitSourceChange(newer).accepted,
				"A later committed sequence must accept a numerically smaller source-content hash.");
			context.Expect(dirty.LatestChangeSequence() == newer.changeSequence
				&& dirty.LatestChangeSequence(newer.source) == newer.changeSequence,
				"Dirty preparation must order the newest source observation by its protected receipt sequence.");
			context.Expect(dirty.AcknowledgePublishedThrough(newer.changeSequence),
				"Only the newest committed sequence may retire its bounded dirty backlog.");
		});
		registry.Add("Navigation.DynamicGeometry.ProfileAwareAdmissionExpandsAxesButPreservesPendingBounds", [](TestContext& context) {
			NavigationDirtyIndexSettings settings{};
			settings.tileSizeMetres = 32.0F;
			settings.verticalBandMetres = 128.0F;
			NavigationDirtyIndex dirty(settings);
			NavigationDynamicSourceChange change{};
			change.source = NavigationUuid::FromString("b28461d1-f555-47b0-9c15-e0f153d5fc9e");
			change.sourceRevision = 1U;
			change.changeSequence = 1U;
			change.physicalReadyRevision = 1U;
			change.kind = NavigationDynamicChangeKind::Moved;
			change.previousBounds = {{30.0F, 120.0F, 30.0F}, {31.0F, 121.0F, 31.0F}};
			change.currentBounds = change.previousBounds;
			const NavigationDirtyAdmissionExpansion expansion{4.0, 12.0};
			const NavigationDynamicChangeReceipt receipt = dirty.CommitSourceChange(change, expansion);
			const std::vector<NavigationDynamicSourceChange> pending = dirty.ObservePendingChanges();
			const std::vector<NavigationDirtyCoverageDependency>& coverage = dirty.DirtyCoverage();
			const bool coversIndependentAxisEnvelope = coverage.size() == 8U
				&& std::find(coverage.begin(), coverage.end(), NavigationDirtyCoverageDependency{0LL, 0LL, 0}) != coverage.end()
				&& std::find(coverage.begin(), coverage.end(), NavigationDirtyCoverageDependency{1LL, 1LL, 1}) != coverage.end();
			const bool originalPacketPreserved = pending.size() == 1U
				&& pending.front().previousBounds == change.previousBounds
				&& pending.front().currentBounds == change.currentBounds
				&& pending.front().canonicalTileOriginX == change.canonicalTileOriginX
				&& pending.front().canonicalTileOriginZ == change.canonicalTileOriginZ
				&& pending.front().canonicalVerticalBandOrigin == change.canonicalVerticalBandOrigin;
			context.Expect(receipt.accepted && receipt.dirtyCoverageCount == coverage.size()
				&& coversIndependentAxisEnvelope && originalPacketPreserved,
				"A profile-aware admission envelope must expand horizontal tiles and vertical bands independently while retaining the original copied source bounds for later profile-specific preparation.");
		});
		registry.Add("Navigation.DynamicGeometry.ProfileAwareAdmissionRoundsTinyExpansionOutward", [](TestContext& context) {
			NavigationDirtyIndexSettings settings{};
			settings.tileSizeMetres = 32.0F;
			NavigationDirtyIndex dirty(settings);
			NavigationDynamicSourceChange boundary{};
			boundary.source = NavigationUuid::FromString("886dda60-5fcc-4c68-9fdd-6215fcf64b46");
			boundary.sourceRevision = 1U;
			boundary.changeSequence = 1U;
			boundary.physicalReadyRevision = 1U;
			boundary.kind = NavigationDynamicChangeKind::Moved;
			boundary.previousBounds = {{32.0F, 16.0F, 16.0F}, {32.0F, 16.0F, 16.0F}};
			boundary.currentBounds = boundary.previousBounds;
			const NavigationDynamicChangeReceipt receipt = dirty.CommitSourceChange(boundary, {1.0e-7, 0.0});
			const std::vector<NavigationDynamicSourceChange> pending = dirty.ObservePendingChanges();
			const bool containsBothSides = dirty.DirtyCoverage().size() == 2U
				&& std::find(dirty.DirtyCoverage().begin(), dirty.DirtyCoverage().end(),
					NavigationDirtyCoverageDependency{0LL, 0LL, 0}) != dirty.DirtyCoverage().end()
				&& std::find(dirty.DirtyCoverage().begin(), dirty.DirtyCoverage().end(),
					NavigationDirtyCoverageDependency{1LL, 0LL, 0}) != dirty.DirtyCoverage().end();
			context.Expect(receipt.accepted && containsBothSides && pending.size() == 1U
				&& pending.front().previousBounds == boundary.previousBounds
				&& pending.front().currentBounds == boundary.currentBounds,
				"A positive profile halo smaller than one float ULP at a tile boundary must round outward into both affected tiles without mutating the copied pending bounds.");
		});
		registry.Add("Navigation.DynamicGeometry.ProfileAwareAdmissionRejectsInvalidCapAndInt64OverflowAtomically", [](TestContext& context) {
			NavigationDirtyIndexSettings settings{};
			settings.maximumDependenciesPerChange = 8U;
			NavigationDirtyIndex dirty(settings);
			NavigationDynamicSourceChange sentinel{};
			sentinel.source = NavigationUuid::FromString("c9b743df-45d2-4d23-b616-52ce76ea4d61");
			sentinel.sourceRevision = 1U;
			sentinel.changeSequence = 1U;
			sentinel.physicalReadyRevision = 1U;
			sentinel.kind = NavigationDynamicChangeKind::Removed;
			sentinel.previousBounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
			const bool sentinelAccepted = dirty.CommitSourceChange(sentinel).accepted;
			const std::vector<NavigationDynamicSourceChange> pendingBefore = dirty.ObservePendingChanges();
			const std::vector<NavigationDirtyCoverageDependency> coverageBefore = dirty.DirtyCoverage();
			const std::vector<NavigationDirtySeamDependency> seamsBefore = dirty.DirtySeams();
			const NavigationSourceTombstoneSnapshot tombstonesBefore = dirty.ObserveAtProtectedBoundary();
			const NavigationGeneration latestBefore = dirty.LatestChangeSequence();

			auto nextChange = [&sentinel](NavigationGeneration sequence) {
				NavigationDynamicSourceChange value = sentinel;
				value.source = NavigationUuid::FromString("9d717071-58ae-4e85-af42-5ab4fcc6b87e");
				value.sourceRevision = sequence;
				value.changeSequence = sequence;
				value.kind = NavigationDynamicChangeKind::Moved;
				value.previousBounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
				value.currentBounds = value.previousBounds;
				return value;
			};
			const bool rejectedInvalid = !dirty.CommitSourceChange(nextChange(2U),
				{std::numeric_limits<double>::quiet_NaN(), 0.0}).accepted;
			const bool rejectedCapacity = !dirty.CommitSourceChange(nextChange(3U), {96.0, 0.0}).accepted;
			NavigationDynamicSourceChange overflow = nextChange(4U);
			overflow.canonicalTileOriginX = std::numeric_limits<std::int64_t>::max();
			overflow.previousBounds = {{32.0F, 0.0F, 0.0F}, {33.0F, 1.0F, 1.0F}};
			overflow.currentBounds = overflow.previousBounds;
			const bool rejectedOverflow = !dirty.CommitSourceChange(overflow, {0.0, 0.0}).accepted;
			const bool unchanged = dirty.ObservePendingChanges().size() == pendingBefore.size()
				&& dirty.DirtyCoverage() == coverageBefore && dirty.DirtySeams() == seamsBefore
				&& dirty.ObserveAtProtectedBoundary().entries.size() == tombstonesBefore.entries.size()
				&& dirty.ObserveAtProtectedBoundary().Contains(NavigationUuid{sentinel.source.value}, sentinel.sourceRevision)
				&& dirty.LatestChangeSequence() == latestBefore;

			NavigationDirtyIndexSettings numericSettings{};
			numericSettings.tileSizeMetres = 1.0F;
			numericSettings.maximumDependenciesPerChange = 8U;
			NavigationDirtyIndex numericDirty(numericSettings);
			const bool numericSentinelAccepted = numericDirty.CommitSourceChange(sentinel).accepted;
			// The one-metre grid maps the inclusive [0, 1] sentinel to four cells and
			// eight seams. Capture that real baseline instead of coupling this atomicity
			// check to a particular tile granularity.
			const std::vector<NavigationDynamicSourceChange> numericPendingBefore = numericDirty.ObservePendingChanges();
			const std::vector<NavigationDirtyCoverageDependency> numericCoverageBefore = numericDirty.DirtyCoverage();
			const std::vector<NavigationDirtySeamDependency> numericSeamsBefore = numericDirty.DirtySeams();
			const NavigationGeneration numericLatestBefore = numericDirty.LatestChangeSequence();
			NavigationDynamicSourceChange cellOverflow = nextChange(2U);
			const float exclusiveInt64Cell = std::ldexp(1.0F, 63);
			cellOverflow.previousBounds = {{exclusiveInt64Cell, 0.0F, 0.0F},
				{exclusiveInt64Cell, 1.0F, 1.0F}};
			cellOverflow.currentBounds = cellOverflow.previousBounds;
			const bool rejectedCellForBoundary = !numericDirty.CommitSourceChange(cellOverflow, {0.0, 0.0}).accepted;
			NavigationDynamicSourceChange spanning = nextChange(3U);
			spanning.previousBounds = {{-1.0e18F, 0.0F, 0.0F}, {1.0e18F, 1.0F, 1.0F}};
			spanning.currentBounds = spanning.previousBounds;
			const bool rejectedSpanningBounds = !numericDirty.CommitSourceChange(spanning, {0.0, 0.0}).accepted;
			const std::vector<NavigationDynamicSourceChange> numericPendingAfter = numericDirty.ObservePendingChanges();
			// Pending changes are intentionally ordinary value packets rather than an
			// equality-bearing public type. Compare every retained receipt field so this
			// test still proves rejection leaves the complete worker input unchanged.
			const bool numericPendingUnchanged = numericPendingAfter.size() == numericPendingBefore.size()
				&& std::equal(numericPendingAfter.begin(), numericPendingAfter.end(), numericPendingBefore.begin(),
					[](const NavigationDynamicSourceChange& current, const NavigationDynamicSourceChange& previous) {
						return current.source == previous.source
							&& current.sourceRevision == previous.sourceRevision
							&& current.changeSequence == previous.changeSequence
							&& current.physicalReadyRevision == previous.physicalReadyRevision
							&& current.canonicalTileOriginX == previous.canonicalTileOriginX
							&& current.canonicalTileOriginZ == previous.canonicalTileOriginZ
							&& current.canonicalVerticalBandOrigin == previous.canonicalVerticalBandOrigin
							&& current.kind == previous.kind
							&& current.previousBounds == previous.previousBounds
							&& current.currentBounds == previous.currentBounds;
					});
			const bool numericUnchanged = numericPendingUnchanged
				&& numericDirty.DirtyCoverage() == numericCoverageBefore
				&& numericDirty.DirtySeams() == numericSeamsBefore
				&& numericDirty.LatestChangeSequence() == numericLatestBefore;
			context.Expect(sentinelAccepted && rejectedInvalid && rejectedCapacity && rejectedOverflow && unchanged
				&& numericSentinelAccepted && rejectedCellForBoundary && rejectedSpanningBounds && numericUnchanged,
				"Invalid, over-capacity, int64-boundary, or enormous-span profile-aware envelopes must reject atomically without changing pending work, dirty coverage, seams, tombstones, or source ordering.");
		});
		registry.Add("Navigation.DynamicGeometry.DiffsAutomaticSourceGenerationsWithoutSceneMarkers", [](TestContext& context) {
			NavigationDirtyIndex dirty({});
			const NavigationGeometryBounds bounds{{-2.0F, 0.0F, -2.0F}, {2.0F, 2.0F, 2.0F}};
			const std::array<NavigationDynamicChangeKind, 6U> automaticChanges{{
				NavigationDynamicChangeKind::Created,
				NavigationDynamicChangeKind::Enabled,
				NavigationDynamicChangeKind::Moved,
				NavigationDynamicChangeKind::Replaced,
				NavigationDynamicChangeKind::Reshaped,
				NavigationDynamicChangeKind::Removed
			}};
			for(std::size_t index = 0U; index < automaticChanges.size(); ++index) {
				// Each UUID stands for one copied terrain, collider, mesh-companion,
				// procedural, resource-reload, or streamed source. The dirty path sees
				// only stable identities and revisions, never scene marker components.
				NavigationDynamicSourceChange change{};
				change.source = NavigationUuid::FromString("1a6d9e5b-bf61-4734-9954-0fcf0e6a0c0" + std::to_string(index));
				change.sourceRevision = 1U;
				change.physicalReadyRevision = 10U + index;
				change.kind = automaticChanges[index];
				change.previousBounds = bounds;
				change.currentBounds = bounds;
				context.Expect(dirty.CommitSourceChange(change).accepted,
					"Every copied automatic source generation must enter local dirty preparation.");
			}
			context.Expect(dirty.PendingPreparationCount() == automaticChanges.size(),
				"Automatic terrain/resource/streaming changes must not require a navigation source marker.");
		});
		registry.Add("Navigation.DynamicGeometry.PublishedWatermarkDrainsBoundedPreparationBacklog", [](TestContext& context) {
			NavigationDirtyIndex dirty({.maximumPendingChanges = 2U});
			const NavigationUuid source = NavigationUuid::FromString("8a17dc46-bdf4-40d7-b90d-77d9abf57a65");
			auto change = [source](NavigationGeneration revision) {
				NavigationDynamicSourceChange value{};
				value.source = source;
				value.sourceRevision = revision;
				value.physicalReadyRevision = revision;
				value.kind = NavigationDynamicChangeKind::Moved;
				value.previousBounds = {{0.0F, 0.0F, 0.0F}, {2.0F, 2.0F, 2.0F}};
				value.currentBounds = {{1.0F, 0.0F, 0.0F}, {3.0F, 2.0F, 2.0F}};
				return value;
			};
			context.Expect(dirty.CommitSourceChange(change(1U)).accepted
				&& dirty.CommitSourceChange(change(2U)).accepted,
				"The bounded dirty queue must admit changes up to its configured limit.");
			context.Expect(!dirty.CommitSourceChange(change(3U)).accepted,
				"An unacknowledged full queue must fail closed instead of dropping copied work.");
			context.Expect(!dirty.AcknowledgePublishedThrough(1U),
				"A stale publication watermark must not retire newer dirty dependencies.");
			context.Expect(dirty.AcknowledgePublishedThrough(2U)
				&& dirty.PendingPreparationCount() == 0U
				&& dirty.DirtyCoverage().empty() && dirty.DirtySeams().empty(),
				"A coherent latest publication must retire its complete preparation backlog.");
			context.Expect(dirty.LatestSourceRevision(source) == 2U
				&& dirty.CommitSourceChange(change(3U)).accepted,
				"Acknowledgement must retain revision evidence while reopening bounded admission.");
			context.Expect(dirty.AcknowledgePublishedThrough(3U),
				"The reopened queue must acknowledge its next coherent publication.");
			bool remainedBounded = true;
			for(NavigationGeneration revision = 4U; revision <= 20000U; ++revision) {
				remainedBounded = remainedBounded && dirty.CommitSourceChange(change(revision)).accepted
					&& dirty.AcknowledgePublishedThrough(revision);
			}
			context.Expect(remainedBounded && dirty.PendingPreparationCount() == 0U,
				"More than the former queue cap of coherent publications must drain without exhausting admission.");
		});
		registry.Add("Navigation.DynamicGeometry.AutomaticCaptureInvalidatesOnlyChangedSourcePaths", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> firstChangedGeometry;
			const NavigationEligibleSourceCandidate terrain = AutomaticSource(
				NavigationSourceKind::Terrain, 1U, 0.0, terrainGeometry);
			const NavigationEligibleSourceCandidate firstChanged = AutomaticSource(
				NavigationSourceKind::Mesh, 1U, -10.0, firstChangedGeometry);
			const NavigationSystem::InitialGroundBuildInput firstBuild = AutomaticBuild(
				NavigationProfile::Defaults(), terrain, firstChanged);
			const bool firstQueued = terrainGeometry != nullptr && firstChangedGeometry != nullptr
				&& firstBuild.eligibleSourceIndex != nullptr && system.QueueInitialGroundBuilds({firstBuild});
			system.Update(0.0F);
			const std::shared_ptr<const NavigationWorldRoot> firstRoot = system.PublishedWorld();
			const NavigationRequestHandle affected = firstRoot
				? SubmitAutomaticRoute(system, firstRoot->world, 101U, -12.0, -6.0) : NavigationRequestHandle{};
			const NavigationRequestHandle unrelated = firstRoot
				? SubmitAutomaticRoute(system, firstRoot->world, 102U, 8.0, 14.0) : NavigationRequestHandle{};
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) system.Update(0.0F);
			const NavigationRouteHandle affectedRoute = system.PublishedRouteForRequest(affected);
			const NavigationRouteHandle unrelatedRoute = system.PublishedRouteForRequest(unrelated);
			const bool initiallyComplete = affectedRoute.IsValid() && unrelatedRoute.IsValid()
				&& system.ReadRoute(affectedRoute).IsValid() && system.ReadRoute(unrelatedRoute).IsValid();
			const auto initialAffectedRequest = system.ReadRequest(affected);
			const auto initialUnrelatedRequest = system.ReadRequest(unrelated);
			const std::string initialState = "firstQueued=" + std::to_string(firstQueued)
				+ ", terrain=" + std::to_string(terrainGeometry != nullptr)
				+ ", changed=" + std::to_string(firstChangedGeometry != nullptr)
				+ ", index=" + std::to_string(firstBuild.eligibleSourceIndex != nullptr)
				+ ", root=" + std::to_string(firstRoot != nullptr)
				+ ", affected=" + (initialAffectedRequest.record
					? std::string(ToString(initialAffectedRequest.record->outcome)) + "/"
						+ std::string(ToString(initialAffectedRequest.record->reason)) : "missing")
				+ ", unrelated=" + (initialUnrelatedRequest.record
					? std::string(ToString(initialUnrelatedRequest.record->outcome)) + "/"
						+ std::string(ToString(initialUnrelatedRequest.record->reason)) : "missing");

			std::shared_ptr<const NavigationGeometrySnapshot> replacementChangedGeometry;
			const NavigationEligibleSourceCandidate replacementChanged = AutomaticSource(
				NavigationSourceKind::Mesh, 2U, -8.0, replacementChangedGeometry, &firstChanged.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = AutomaticBuild(
				NavigationProfile::Defaults(), terrain, replacementChanged);
			const bool replacementQueued = replacementChangedGeometry != nullptr
				&& replacementBuild.eligibleSourceIndex != nullptr && system.QueueInitialGroundBuilds({replacementBuild});
			const auto affectedBeforePublication = system.ReadRequest(affected);
			const bool affectedRouteUsableBeforePublication = system.ReadRoute(affectedRoute).IsValid();
			const bool unrelatedRouteUsableBeforePublication = system.ReadRoute(unrelatedRoute).IsValid();

			// A malformed replacement batch is rejected before it can alter the staged
			// transition. In particular, it must not make a stale changed-path route
			// usable again while the prior authoritative root remains published.
			NavigationSystem::InitialGroundBuildInput malformed{};
			malformed.profile = NavigationProfile::Defaults();
			const bool rejectedAtomically = !system.QueueInitialGroundBuilds({std::move(malformed)})
				&& !system.ReadRoute(affectedRoute).IsValid();

			system.Update(0.0F);
			const bool affectedRouteUsableAfterPublication = system.ReadRoute(affectedRoute).IsValid();
			const bool unrelatedRouteUsableAfterPublication = system.ReadRoute(unrelatedRoute).IsValid();
			const auto affectedRequest = system.ReadRequest(affected);

			context.Expect(firstQueued && initiallyComplete && replacementQueued,
				"The automatic source replacement regression requires two complete retained routes and a coherent copied replacement index: "
					+ initialState);
			context.Expect(affectedBeforePublication.record.has_value()
				&& affectedBeforePublication.record->outcome == NavigationOutcome::Stale
				&& affectedBeforePublication.record->reason == NavigationReason::DirtyCoverage
				&& !affectedRouteUsableBeforePublication && unrelatedRouteUsableBeforePublication,
				"A changed copied source must retire only the overlapping retained path before replacement coverage is published.");
			context.Expect(rejectedAtomically && !affectedRouteUsableAfterPublication
				&& affectedRequest.record.has_value() && affectedRequest.record->outcome == NavigationOutcome::Stale,
				"A rejected automatic batch must leave the stale changed-path route unusable and must not resurrect old guidance.");
			context.Expect(unrelatedRouteUsableAfterPublication,
				"A retained route outside old and new changed-source bounds must survive the successful automatic root replacement.");
			system.Shutdown();
		});
		registry.Add("Navigation.DynamicGeometry.IncrementalReusePreservesUnchangedRouteOutcome", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			const NavigationEligibleSourceCandidate terrain = AutomaticSource(
				NavigationSourceKind::Terrain, 1U, 0.0, terrainGeometry);
			const NavigationEligibleSourceCandidate changed = AutomaticSource(
				NavigationSourceKind::Mesh, 1U, -10.0, changedGeometry);
			const NavigationSystem::InitialGroundBuildInput initial = AutomaticBuild(
				NavigationProfile::Defaults(), terrain, changed);
			const bool initialized = terrainGeometry != nullptr && changedGeometry != nullptr
				&& initial.eligibleSourceIndex != nullptr && system.QueueInitialGroundBuilds({initial});
			if(initialized) system.Update(0.0F);
			const std::shared_ptr<const NavigationWorldRoot> initialRoot = system.PublishedWorld();
			const NavigationRequestHandle before = initialRoot
				? SubmitAutomaticRoute(system, initialRoot->world, 401U, 8.0, 16.0) : NavigationRequestHandle{};
			for(std::uint32_t frame = 0U; before.IsValid() && frame < 2U; ++frame) system.Update(0.0F);
			const auto beforeRead = system.ReadRequest(before);
			const DynamicTileWorkSnapshot workBefore = DynamicTileWork(system);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = AutomaticSource(
				NavigationSourceKind::Mesh, 2U, -8.0, replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = AutomaticBuild(
				NavigationProfile::Defaults(), terrain, replacement);
			const auto replacementSource = std::find_if(replacementBuild.sourceIndex.begin(), replacementBuild.sourceIndex.end(),
				[&replacement](const NavigationSourceIndexRecord& source) {
					return source.sourceId == replacement.identity.sourceId;
				});
			NavigationSystem::DynamicGroundSourceReplacement staged{};
			if(replacementSource != replacementBuild.sourceIndex.end()) staged.source = *replacementSource;
			staged.snapshot = replacementGeometry;
			staged.committedChangeSequence = 401U;
			const bool stagedAccepted = replacementGeometry != nullptr
				&& replacementSource != replacementBuild.sourceIndex.end()
				&& system.StageDynamicGroundSourceReplacements({std::move(staged)});

			Scene::NavigationSourceMutationReceipt receipt{};
			receipt.accepted = true;
			receipt.transactionStatus = Scene::TransactionStatus::Committed;
			receipt.intent.sourceId = replacement.identity.sourceId;
			receipt.intent.operation = Scene::NavigationSourceMutationOperation::Move;
			receipt.intent.oldBounds = {{{0, 0, 0}, {-12.0, -1.0, -2.0}},
				{{0, 0, 0}, {-8.0, 3.0, 2.0}}, true};
			receipt.intent.newBounds = {{{0, 0, 0}, {-10.0, -1.0, -2.0}},
				{{0, 0, 0}, {-6.0, 3.0, 2.0}}, true};
			receipt.intent.committedSourceRevision = replacementSource != replacementBuild.sourceIndex.end()
				? replacementSource->sourceRevision : 0U;
			receipt.intent.committedChangeSequence = 401U;
			receipt.intent.physicalReadyRevision = 1U;
			receipt.intent.oldEnabled = true;
			receipt.intent.newEnabled = true;
			const bool committed = stagedAccepted && system.ObserveSceneMutationReceipts(
				std::span<const Scene::NavigationSourceMutationReceipt>{&receipt, 1U}) == 1U;
			for(std::uint32_t frame = 0U; committed && frame < 3U; ++frame) system.Update(0.0F);
			system.ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd, 401U);

			const std::shared_ptr<const NavigationWorldRoot> rebuiltRoot = system.PublishedWorld();
			const NavigationRequestHandle after = rebuiltRoot
				? SubmitAutomaticRoute(system, rebuiltRoot->world, 402U, 8.0, 16.0) : NavigationRequestHandle{};
			for(std::uint32_t frame = 0U; after.IsValid() && frame < 2U; ++frame) system.Update(0.0F);
			const auto afterRead = system.ReadRequest(after);
			const DynamicTileWorkSnapshot workAfter = DynamicTileWork(system);
			const bool routeParity = beforeRead.record.has_value() && afterRead.record.has_value()
				&& beforeRead.record->outcome == NavigationOutcome::Complete
				&& afterRead.record->outcome == beforeRead.record->outcome
				&& afterRead.record->reason == beforeRead.record->reason;
			const bool localReuse = workAfter.reuseAttempts > workBefore.reuseAttempts
				&& workAfter.reusedTiles > workBefore.reusedTiles
				&& workAfter.rebuiltTiles > workBefore.rebuiltTiles
				&& workAfter.fullShardFallbacks == workBefore.fullShardFallbacks;
			context.Expect(initialized && initialRoot != nullptr && stagedAccepted && committed
				&& rebuiltRoot != nullptr && routeParity && localReuse,
				"A local automatic-source move must reuse unchanged tiles, rebuild the copied dirty closure, and preserve an unrelated route's complete outcome.");
			system.Shutdown();
		});
		registry.Add("Navigation.DynamicGeometry.IncrementalReuseMatchesFullShardReplacement", [](TestContext& context) {
			const NavigationProfile profile = NavigationProfile::Defaults();
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> priorGeometry;
			const NavigationEligibleSourceCandidate terrain = AutomaticSource(
				NavigationSourceKind::Terrain, 1U, 0.0, terrainGeometry);
			const NavigationEligibleSourceCandidate prior = AutomaticSource(
				NavigationSourceKind::Mesh, 1U, -10.0, priorGeometry);
			const NavigationSystem::InitialGroundBuildInput previous = AutomaticBuild(profile, terrain, prior);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = AutomaticSource(
				NavigationSourceKind::Mesh, 2U, -8.0, replacementGeometry, &prior.identity);
			const NavigationSystem::InitialGroundBuildInput current = AutomaticBuild(profile, terrain, replacement);
			const NavigationGeometryBounds oldBounds{{-11.0F, -1.0F, -2.0F}, {-9.0F, 3.0F, 2.0F}};
			const NavigationGeometryBounds newBounds{{-9.0F, -1.0F, -2.0F}, {-7.0F, 3.0F, 2.0F}};
			NavigationDynamicRebuildRequest reuseRequest = DirectDynamicRequest(profile, previous, current,
				NavigationUuid{replacement.identity.sourceId}, replacementGeometry->Descriptor().sourceRevision,
				oldBounds, newBounds);
			NavigationDynamicRebuildRequest fullRequest = reuseRequest;
			fullRequest.reuseBaselines.clear();

			const NavigationDynamicRebuildPlan reused = PrepareDynamicGroundReplacement(reuseRequest);
			const NavigationDynamicRebuildPlan full = PrepareDynamicGroundReplacement(fullRequest);
			context.Expect(reused.accepted && reused.incrementalReuseAttempted
				&& reused.reusedTileCount > 0U && reused.rebuiltTileCount > 0U
				&& reused.fullShardFallbackCount == 0U && full.accepted
				&& EquivalentCoverage(reused, full),
				"A copied local receipt must retain tiles outside its profile halo while publishing coverage equivalent to the established full replacement.");
		});
		registry.Add("Navigation.DynamicGeometry.RemovalFallsBackWithoutDiscardingAuthoritativeEmptyTiles", [](TestContext& context) {
			const NavigationProfile profile = NavigationProfile::Defaults();
			std::shared_ptr<const NavigationGeometrySnapshot> retainedColliderGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> removedColliderGeometry;
			// Two physical colliders occupy distinct tiles in one shard. Recast must
			// retain their no-support results as authoritative empty coverage rather
			// than treating the unaffected collider tile as absent after a removal.
			const NavigationEligibleSourceCandidate retainedCollider = AutomaticSource(
				NavigationSourceKind::Collider, 1U, -100.0, retainedColliderGeometry);
			const NavigationEligibleSourceCandidate removedCollider = AutomaticSource(
				NavigationSourceKind::Collider, 1U, -10.0, removedColliderGeometry);
			const NavigationSystem::InitialGroundBuildInput previous = AutomaticBuild(
				profile, retainedCollider, removedCollider);
			NavigationSystem::InitialGroundBuildInput removal{};
			removal.profile = profile;
			removal.sources = {retainedColliderGeometry};
			for(const NavigationSourceIndexRecord& source : previous.sourceIndex)
				if(source.sourceId == retainedCollider.identity.sourceId) removal.sourceIndex.push_back(source);
			removal.terrainPublicationGeneration = previous.terrainPublicationGeneration;

			const NavigationGeometryBounds removedBounds{{-11.0F, -1.0F, -2.0F}, {-9.0F, 3.0F, 2.0F}};
			NavigationDynamicRebuildRequest request = DirectDynamicRequest(profile, previous, removal,
				NavigationUuid{removedCollider.identity.sourceId}, removedColliderGeometry->Descriptor().sourceRevision,
				removedBounds, {}, NavigationDynamicChangeKind::Removed);
			const bool baselineContainedAnAuthoritativeEmptyTile = std::any_of(request.reuseBaselines.begin(), request.reuseBaselines.end(),
				[](const NavigationDynamicShardReuseBaseline& baseline) {
					return baseline.retainedTiles != nullptr && std::any_of(baseline.retainedTiles->tiles.begin(),
						baseline.retainedTiles->tiles.end(), [](const GroundTileBlob& tile) { return tile.empty; });
				});
			const NavigationDynamicRebuildPlan plan = PrepareDynamicGroundReplacement(request);
			NavigationDynamicRebuildRequest fullRequest = request;
			fullRequest.reuseBaselines.clear();
			const NavigationDynamicRebuildPlan full = PrepareDynamicGroundReplacement(fullRequest);
			const bool survivorOnlyEmptyPack = !plan.shards.empty()
				&& std::any_of(plan.shards.begin(), plan.shards.end(), [](const auto& shard) {
					return shard != nullptr && shard->retainedTiles != nullptr
						&& std::any_of(shard->retainedTiles->tiles.begin(), shard->retainedTiles->tiles.end(),
							[](const GroundTileBlob& tile) {
								// The surviving collider remains at canonical tile -4. The removed
								// source formerly occupied tile -1, which must not survive the rebuild.
								return tile.empty && tile.key.x == -4LL;
							});
					})
				&& std::all_of(plan.shards.begin(), plan.shards.end(), [](const auto& shard) {
					return shard != nullptr && shard->retainedTiles != nullptr
						&& std::none_of(shard->retainedTiles->tiles.begin(), shard->retainedTiles->tiles.end(),
							[](const GroundTileBlob& tile) { return tile.key.x == -1LL; });
					});
			const bool explicitFallback = plan.accepted && plan.incrementalReuseAttempted
				&& plan.reusedTileCount == 0U && plan.rebuiltTileCount > 0U
				&& plan.fullShardFallbackCount > 0U
				&& plan.incrementalReuseFallbackReason == "IncrementalReuseRecastFailed:NoCanonicalTileGeometry";
			const std::string evidence = "accepted=" + std::to_string(plan.accepted)
				+ ", reused=" + std::to_string(plan.reusedTileCount)
				+ ", rebuilt=" + std::to_string(plan.rebuiltTileCount)
				+ ", fallback=" + std::to_string(plan.fullShardFallbackCount)
				+ ", reason=" + plan.incrementalReuseFallbackReason
				+ ", plan=" + plan.reason;
			context.Expect(baselineContainedAnAuthoritativeEmptyTile && explicitFallback
				&& survivorOnlyEmptyPack && EquivalentCoverage(plan, full),
				"A removal with no current source in its incremental closure must take the explicit complete-shard fallback, retain the surviving authoritative empty tile, and remove the old tile: " + evidence);
		});
		registry.Add("Navigation.DynamicGeometry.RetainedTilePackKeepsItsBudgetLeaseUntilLastReader", [](TestContext& context) {
			const NavigationProfile profile = NavigationProfile::Defaults();
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			const NavigationEligibleSourceCandidate terrain = AutomaticSource(
				NavigationSourceKind::Terrain, 1U, 0.0, terrainGeometry);
			RecastGroundBuildInput recastInput{};
			recastInput.profile = profile;
			recastInput.sources = {terrainGeometry};
			const RecastGroundBuildResult recast = BuildRecastGroundTiles(recastInput);
			const auto nonEmpty = std::find_if(recast.tiles.begin(), recast.tiles.end(),
				[](const GroundTileBlob& tile) { return !tile.empty && !tile.pristineBytes.empty(); });
			if(nonEmpty == recast.tiles.end()) {
				context.Expect(false, "The retained-pack lifetime fixture requires one complete non-empty terrain tile.");
				return;
			}

			const std::size_t conservativeBudget = nonEmpty->pristineBytes.size()
				+ nonEmpty->sourceFingerprints.capacity() * sizeof(std::uint64_t);
			NavigationBudgetLimits limits{};
			limits.publishedNavigationBytes = conservativeBudget + nonEmpty->pristineBytes.size();
			NavigationBudget budget(limits);
			DetourGroundShardBuildInput input{};
			input.shard = {DynamicShardCoordinate(nonEmpty->key.x), DynamicShardCoordinate(nonEmpty->key.z),
				nonEmpty->key.verticalBand, nonEmpty->key.profileBuildKey};
			input.expectedGeneration = 1U;
			input.tiles = {*nonEmpty};
			input.minimumClearanceMetres = static_cast<float>(profile.clearanceMarginMetres);
			input.budget = &budget;
			DetourGroundShardBuildResult assembled = AssembleDetourGroundShard(input);
			std::shared_ptr<const RetainedGroundTilePack> retained = assembled.generation
				? assembled.generation->retainedTiles : nullptr;
			const std::size_t chargedWithShard = budget.UsedBytes(NavigationBudgetCategory::PublishedNavigation);
			const std::size_t retainedCharge = retained != nullptr && retained->budgetReservation != nullptr
				? retained->budgetReservation->Bytes() : 0U;
			const bool chargedWhileOwned = assembled.accepted && retained != nullptr
				&& retainedCharge > 0U && chargedWithShard > retainedCharge;
			assembled.generation.reset();
			const bool chargedWhileRetained = retained != nullptr
				&& budget.UsedBytes(NavigationBudgetCategory::PublishedNavigation) == retainedCharge;
			retained.reset();
			const bool releasedAfterLastReader = budget.UsedBytes(NavigationBudgetCategory::PublishedNavigation) == 0U;

			NavigationBudgetLimits constrainedLimits{};
			// The first reservation fits exactly, but the provider-copy reservation
			// cannot. The failed assembly must destroy that temporary proof pack.
			constrainedLimits.publishedNavigationBytes = retainedCharge;
			NavigationBudget constrainedBudget(constrainedLimits);
			input.budget = &constrainedBudget;
			const DetourGroundShardBuildResult rejected = AssembleDetourGroundShard(input);
			const bool rejectedAtomically = !rejected.accepted && rejected.generation == nullptr
				&& constrainedBudget.UsedBytes(NavigationBudgetCategory::PublishedNavigation) == 0U;
			context.Expect(chargedWhileOwned && chargedWhileRetained && releasedAfterLastReader && rejectedAtomically,
				"A retained Recast proof must keep its independent budget lease after the Detour shard retires, and a later allocation failure must publish nothing or leak a reservation.");
		});
		registry.Add("Navigation.DynamicGeometry.IncrementalReuseFallsBackForStaleTupleAndHaloEscape", [](TestContext& context) {
			const NavigationProfile profile = NavigationProfile::Defaults();
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> priorGeometry;
			const NavigationEligibleSourceCandidate terrain = AutomaticSource(
				NavigationSourceKind::Terrain, 1U, 0.0, terrainGeometry);
			const NavigationEligibleSourceCandidate prior = AutomaticSource(
				NavigationSourceKind::Mesh, 1U, -10.0, priorGeometry);
			const NavigationSystem::InitialGroundBuildInput previous = AutomaticBuild(profile, terrain, prior);
			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = AutomaticSource(
				NavigationSourceKind::Mesh, 2U, -8.0, replacementGeometry, &prior.identity);
			const NavigationSystem::InitialGroundBuildInput current = AutomaticBuild(profile, terrain, replacement);
			const NavigationGeometryBounds oldBounds{{-11.0F, -1.0F, -2.0F}, {-9.0F, 3.0F, 2.0F}};
			const NavigationGeometryBounds newBounds{{-9.0F, -1.0F, -2.0F}, {-7.0F, 3.0F, 2.0F}};
			const NavigationDynamicRebuildRequest valid = DirectDynamicRequest(profile, previous, current,
				NavigationUuid{replacement.identity.sourceId}, replacementGeometry->Descriptor().sourceRevision,
				oldBounds, newBounds);

			NavigationDynamicRebuildRequest staleTuple = valid;
			for(NavigationDynamicShardReuseBaseline& baseline : staleTuple.reuseBaselines) ++baseline.rootGeneration;
			const NavigationDynamicRebuildPlan stalePlan = PrepareDynamicGroundReplacement(staleTuple);

			NavigationDynamicRebuildRequest haloEscape = valid;
			// The receipt spans both Z-side tiles after the Recast halo. Supplying only
			// one selected dirty shard deliberately proves that reuse expands no hidden
			// coverage; the established complete shard replacement is required instead.
			haloEscape.dirtyCoverage = {{-1LL, -1LL, 0}};
			const NavigationDynamicRebuildPlan haloPlan = PrepareDynamicGroundReplacement(haloEscape);
			const bool staleFallback = stalePlan.accepted && stalePlan.incrementalReuseAttempted
				&& stalePlan.reusedTileCount == 0U && stalePlan.fullShardFallbackCount > 0U
				&& stalePlan.incrementalReuseFallbackReason == "IncrementalReuseBaselineUnavailable";
			const bool haloFallback = haloPlan.accepted && haloPlan.incrementalReuseAttempted
				&& haloPlan.reusedTileCount == 0U && haloPlan.fullShardFallbackCount > 0U
				&& haloPlan.incrementalReuseFallbackReason == "IncrementalReuseHaloEscapesDirtyShards";
			context.Expect(staleFallback && haloFallback,
				"A stale retained tuple or profile halo outside the selected dirty closure must reject reuse and rebuild the complete affected shard.");
		});
		registry.Add("Navigation.DynamicGeometry.ProfileAwareSystemAdmissionMarksNeighborShardAndRollsBackPartialReceipt", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();
			NavigationProfile defaultProfile = NavigationProfile::Defaults();
			NavigationProfile largeProfile = NavigationProfile::Defaults();
			largeProfile.radiusMetres = 4.0;
			largeProfile.heightMetres = 8.0;
			largeProfile.horizontalResolutionMetres = 0.25;

			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			// This terrain spans canonical tiles 6..8. The moved mesh remains in tile
			// 7: the generic 0.75m margin cannot reach shard 1, while the active large
			// profile's exact Recast halo must mark its neighboring tile-8 coverage dirty.
			const NavigationEligibleSourceCandidate terrain = AutomaticSource(
				NavigationSourceKind::Terrain, 1U, 224.0, terrainGeometry);
			const NavigationEligibleSourceCandidate changed = AutomaticSource(
				NavigationSourceKind::Mesh, 1U, 252.0, changedGeometry);
			const bool initialQueued = terrainGeometry != nullptr && changedGeometry != nullptr
				&& system.QueueInitialGroundBuilds({AutomaticBuild(defaultProfile, terrain, changed),
					AutomaticBuild(largeProfile, terrain, changed)});
			for(std::uint32_t frame = 0U; initialQueued && frame < 2U; ++frame) system.Update(0.0F);
			const std::shared_ptr<const NavigationWorldRoot> before = system.PublishedWorld();
			const std::uint64_t largeProfileKey = MakeGroundProfileBuildKey(largeProfile);
			const auto neighboringCoverage = [largeProfileKey](const std::shared_ptr<const NavigationWorldRoot>& root,
				NavigationCoverageState expected) {
				return root != nullptr && std::any_of(root->coverage.begin(), root->coverage.end(),
					[largeProfileKey, expected](const NavigationCoverageRecord& coverage) {
						return coverage.key.profileBuildKey == largeProfileKey && coverage.key.shardX == 1LL
							&& coverage.state == expected;
					});
			};

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = AutomaticSource(
				NavigationSourceKind::Mesh, 2U, 250.0, replacementGeometry, &changed.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = AutomaticBuild(
				defaultProfile, terrain, replacement);
			const auto replacementSource = std::find_if(replacementBuild.sourceIndex.begin(), replacementBuild.sourceIndex.end(),
				[&replacement](const NavigationSourceIndexRecord& source) { return source.sourceId == replacement.identity.sourceId; });
			constexpr NavigationGeneration kSequence = 601U;
			NavigationSystem::DynamicGroundSourceReplacement staged{};
			if(replacementSource != replacementBuild.sourceIndex.end()) staged.source = *replacementSource;
			staged.snapshot = replacementGeometry;
			staged.committedChangeSequence = kSequence;
			const bool firstStageAccepted = replacementGeometry != nullptr
				&& replacementSource != replacementBuild.sourceIndex.end()
				&& system.StageDynamicGroundSourceReplacements({std::move(staged)});

			Scene::NavigationSourceMutationReceipt receipt{};
			receipt.accepted = true;
			receipt.transactionStatus = Scene::TransactionStatus::Committed;
			receipt.intent.sourceId = replacement.identity.sourceId;
			receipt.intent.operation = Scene::NavigationSourceMutationOperation::Move;
			receipt.intent.oldBounds = {{{0, 0, 0}, {251.0, -1.0, -2.0}},
				{{0, 0, 0}, {253.0, 3.0, 2.0}}, true};
			receipt.intent.newBounds = {{{0, 0, 0}, {249.0, -1.0, -2.0}},
				{{0, 0, 0}, {251.0, 3.0, 2.0}}, true};
			receipt.intent.committedSourceRevision = replacementSource != replacementBuild.sourceIndex.end()
				? replacementSource->sourceRevision : 0U;
			receipt.intent.committedChangeSequence = kSequence;
			receipt.intent.physicalReadyRevision = 1U;
			receipt.intent.oldEnabled = true;
			receipt.intent.newEnabled = true;
			Scene::NavigationSourceMutationReceipt rejectedSibling = receipt;
			rejectedSibling.accepted = false;
			rejectedSibling.intent.intentId = "profile-aware-partial-receipt";
			const std::array<Scene::NavigationSourceMutationReceipt, 2U> partialBatch{receipt, rejectedSibling};
			const bool partialRejected = firstStageAccepted && system.ObserveSceneMutationReceipts(partialBatch) == 0U
				&& system.PublishedWorld() == before && neighboringCoverage(before, NavigationCoverageState::Ready);

			NavigationSystem::DynamicGroundSourceReplacement restaged{};
			if(replacementSource != replacementBuild.sourceIndex.end()) restaged.source = *replacementSource;
			restaged.snapshot = replacementGeometry;
			restaged.committedChangeSequence = kSequence;
			const bool restageAccepted = partialRejected && system.StageDynamicGroundSourceReplacements({std::move(restaged)});
			const bool receiptAccepted = restageAccepted && system.ObserveSceneMutationReceipts(
				std::span<const Scene::NavigationSourceMutationReceipt>{&receipt, 1U}) == 1U;
			const std::shared_ptr<const NavigationWorldRoot> after = system.PublishedWorld();
			context.Expect(initialQueued && neighboringCoverage(before, NavigationCoverageState::Ready)
				&& partialRejected && restageAccepted && receiptAccepted
				&& neighboringCoverage(after, NavigationCoverageState::Dirty),
				"The maximum active-profile halo must dirty the neighboring shard before recovery, while a two-receipt partial batch leaves the published root and its ready coverage unchanged.");
			system.Shutdown();
		});
		registry.Add("Navigation.DynamicGeometry.AutomaticCaptureWaitsForEveryProfileAndNeverRefreshesOmittedProfile", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			NavigationProfile firstProfile = NavigationProfile::Defaults();
			NavigationProfile secondProfile = NavigationProfile::Defaults();
			secondProfile.radiusMetres = 0.35;
			secondProfile.heightMetres = 1.6;
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> initialChangedGeometry;
			const NavigationEligibleSourceCandidate terrain = AutomaticSource(
				NavigationSourceKind::Terrain, 1U, 0.0, terrainGeometry);
			const NavigationEligibleSourceCandidate initialChanged = AutomaticSource(
				NavigationSourceKind::Mesh, 1U, -10.0, initialChangedGeometry);
			const bool initialQueued = terrainGeometry != nullptr && initialChangedGeometry != nullptr
				&& system.QueueInitialGroundBuilds({AutomaticBuild(firstProfile, terrain, initialChanged),
					AutomaticBuild(secondProfile, terrain, initialChanged)});
			// NavigationSystem prepares one bounded ground build per compatibility tick.
			// Both copied profiles must therefore commit before object routes are admitted.
			for(std::uint32_t frame = 0U; frame < 2U; ++frame) system.Update(0.0F);
			const std::shared_ptr<const NavigationWorldRoot> initialRoot = system.PublishedWorld();
			const Object firstOwner = objects.createObject("Automatic profile one owner");
			const Object secondOwner = objects.createObject("Automatic profile two owner");
			Transform* firstTransform = objects.AddComponent<Transform>(firstOwner);
			Transform* secondTransform = objects.AddComponent<Transform>(secondOwner);
			if(firstTransform != nullptr) firstTransform->worldPose.position.offsetMetres = {8.0, 0.0, 0.0};
			if(secondTransform != nullptr) secondTransform->worldPose.position.offsetMetres = {14.0, 0.0, 0.0};
			const auto firstReceipt = initialRoot ? system.RequestRoute(objects, objects.getUUID(firstOwner),
				AutomaticLocation(initialRoot->world, 14.0), firstProfile) : NavigationSystem::ObjectRouteReceipt{};
			const auto secondReceipt = initialRoot ? system.RequestRoute(objects, objects.getUUID(secondOwner),
				AutomaticLocation(initialRoot->world, 20.0), secondProfile) : NavigationSystem::ObjectRouteReceipt{};
			for(std::uint32_t frame = 0U; frame < 3U; ++frame) system.Update(0.0F);
			const NavigationRouteHandle firstRoute = system.PublishedRouteForRequest(firstReceipt.request);
			const NavigationRouteHandle secondRoute = system.PublishedRouteForRequest(secondReceipt.request);
			const bool initiallyUsable = firstReceipt.Succeeded() && secondReceipt.Succeeded()
				&& system.ValidateRoute(firstRoute).usable && system.ValidateRoute(secondRoute).usable;

			std::shared_ptr<const NavigationGeometrySnapshot> changedGeometry;
			const NavigationEligibleSourceCandidate changed = AutomaticSource(
				NavigationSourceKind::Mesh, 2U, -8.0, changedGeometry, &initialChanged.identity);
			const bool replacementQueued = changedGeometry != nullptr
				&& system.QueueInitialGroundBuilds({AutomaticBuild(firstProfile, terrain, changed),
					AutomaticBuild(secondProfile, terrain, changed)});
			system.Update(0.0F);
			const bool withheldUntilSecondProfile = !system.ValidateRoute(firstRoute).usable
				&& !system.ValidateRoute(secondRoute).usable;
			system.Update(0.0F);
			const bool bothProfilesRefreshed = system.ValidateRoute(firstRoute).usable
				&& system.ValidateRoute(secondRoute).usable;

			std::shared_ptr<const NavigationGeometrySnapshot> omittedProfileChangedGeometry;
			const NavigationEligibleSourceCandidate omittedProfileChanged = AutomaticSource(
				NavigationSourceKind::Mesh, 3U, -6.0, omittedProfileChangedGeometry, &initialChanged.identity);
			const bool omittedProfileQueued = omittedProfileChangedGeometry != nullptr
				&& system.QueueInitialGroundBuilds({AutomaticBuild(firstProfile, terrain, omittedProfileChanged)});
			system.Update(0.0F);
			const bool onlyPublishedProfileRefreshed = system.ValidateRoute(firstRoute).usable
				&& !system.ValidateRoute(secondRoute).usable;

			context.Expect(initialQueued && initiallyUsable && replacementQueued,
				"The multi-profile transition fixture requires two current object routes from one complete automatic source index.");
			context.Expect(withheldUntilSecondProfile && bothProfilesRefreshed,
				"A successful first profile must not refresh any route until every profile from the exact automatic capture has published.");
			context.Expect(omittedProfileQueued && onlyPublishedProfileRefreshed,
				"A later automatic capture must refresh only its published profile and leave an omitted retained profile stale.");
			system.Shutdown();
			objects.clear();
		});
		registry.Add("Navigation.DynamicGeometry.RebuildsEveryProfileBeforeAcknowledgingDirtySource", [](TestContext& context) {
			NavigationSystem& system = NavigationSystem::Instance();
			system.Shutdown();
			system.Initialize();

			NavigationProfile firstProfile = NavigationProfile::Defaults();
			NavigationProfile secondProfile = NavigationProfile::Defaults();
			secondProfile.radiusMetres = 0.35;
			secondProfile.heightMetres = 1.6;
			std::shared_ptr<const NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const NavigationGeometrySnapshot> initialGeometry;
			const NavigationEligibleSourceCandidate terrain = AutomaticSource(
				NavigationSourceKind::Terrain, 1U, 0.0, terrainGeometry);
			const NavigationEligibleSourceCandidate initial = AutomaticSource(
				NavigationSourceKind::Mesh, 1U, -10.0, initialGeometry);
			const bool initialQueued = terrainGeometry != nullptr && initialGeometry != nullptr
				&& system.QueueInitialGroundBuilds({AutomaticBuild(firstProfile, terrain, initial),
					AutomaticBuild(secondProfile, terrain, initial)});
			for(std::uint32_t frame = 0U; frame < 2U; ++frame) system.Update(0.0F);

			std::shared_ptr<const NavigationGeometrySnapshot> replacementGeometry;
			const NavigationEligibleSourceCandidate replacement = AutomaticSource(
				NavigationSourceKind::Mesh, 2U, -8.0, replacementGeometry, &initial.identity);
			const NavigationSystem::InitialGroundBuildInput replacementBuild = AutomaticBuild(
				firstProfile, terrain, replacement);
			constexpr NavigationGeneration kCommittedSequence = 91U;
			// The eligible index is sorted by stable UUID, not by the authoring order
			// supplied to AutomaticBuild. Locate the changed mesh by identity so this
			// transaction fixture remains deterministic after earlier tests consume UUIDs.
			const auto replacementSource = std::find_if(replacementBuild.sourceIndex.begin(),
				replacementBuild.sourceIndex.end(), [&replacement](const NavigationSourceIndexRecord& source) {
				return source.sourceId == replacement.identity.sourceId;
			});
			NavigationSystem::DynamicGroundSourceReplacement staged{};
			if(replacementSource != replacementBuild.sourceIndex.end()) staged.source = *replacementSource;
			staged.snapshot = replacementGeometry;
			staged.committedChangeSequence = kCommittedSequence;
			const bool stageAccepted = replacementGeometry != nullptr
				&& replacementSource != replacementBuild.sourceIndex.end()
				&& system.StageDynamicGroundSourceReplacements({std::move(staged)});

			Scene::NavigationSourceMutationReceipt receipt{};
			receipt.accepted = true;
			receipt.transactionStatus = Scene::TransactionStatus::Committed;
			receipt.intent.sourceId = replacement.identity.sourceId;
			receipt.intent.operation = Scene::NavigationSourceMutationOperation::Move;
			receipt.intent.oldBounds = {{{0, 0, 0}, {-12.0, -1.0, -2.0}},
				{{0, 0, 0}, {-8.0, 3.0, 2.0}}, true};
			receipt.intent.newBounds = {{{0, 0, 0}, {-10.0, -1.0, -2.0}},
				{{0, 0, 0}, {-6.0, 3.0, 2.0}}, true};
			receipt.intent.committedSourceRevision = replacementSource != replacementBuild.sourceIndex.end()
				? replacementSource->sourceRevision : 0U;
			receipt.intent.committedChangeSequence = kCommittedSequence;
			receipt.intent.physicalReadyRevision = 1U;
			receipt.intent.oldEnabled = true;
			receipt.intent.newEnabled = true;
			const auto rootBeforePartialBatch = system.PublishedWorld();
			const std::size_t pendingBeforePartialBatch = system.DynamicDirtyIndexForTesting().PendingPreparationCount();
			Scene::NavigationSourceMutationReceipt rejectedSibling = receipt;
			rejectedSibling.accepted = false;
			rejectedSibling.intent.intentId = "partial-dynamic-receipt-rejected";
			const std::array<Scene::NavigationSourceMutationReceipt, 2U> partialBatch{receipt, rejectedSibling};
			const bool partialBatchRejected = stageAccepted && system.ObserveSceneMutationReceipts(partialBatch) == 0U
				&& system.PublishedWorld() == rootBeforePartialBatch
				&& system.DynamicDirtyIndexForTesting().PendingPreparationCount() == pendingBeforePartialBatch;
			NavigationSystem::DynamicGroundSourceReplacement restaged{};
			if(replacementSource != replacementBuild.sourceIndex.end()) restaged.source = *replacementSource;
			restaged.snapshot = replacementGeometry;
			restaged.committedChangeSequence = kCommittedSequence;
			const bool restageAccepted = partialBatchRejected
				&& system.StageDynamicGroundSourceReplacements({std::move(restaged)});
			const bool receiptAccepted = restageAccepted && system.ObserveSceneMutationReceipts(
				std::span<const Scene::NavigationSourceMutationReceipt>{&receipt, 1U}) == 1U;

			struct ProfileCoverageProgress {
				bool hasReadyCoverage = false;
				bool hasDirtyCoverage = false;
				NavigationGeneration latestReadyGeneration = 0U;
			};
			const auto profileProgress = [&system](const NavigationProfile& profile) {
				ProfileCoverageProgress progress{};
				const auto root = system.PublishedWorld();
				if(!root) return progress;
				const std::uint64_t key = MakeGroundProfileBuildKey(profile);
				for(const NavigationCoverageRecord& coverage : root->coverage) {
					if(coverage.key.profileBuildKey != key) continue;
					if(coverage.state == NavigationCoverageState::Ready && coverage.complete) {
						progress.hasReadyCoverage = true;
						progress.latestReadyGeneration = std::max(progress.latestReadyGeneration,
							coverage.coverageGeneration);
					}
					if(coverage.state == NavigationCoverageState::Dirty || !coverage.complete)
						progress.hasDirtyCoverage = true;
				}
				return progress;
			};
			const ProfileCoverageProgress initialFirst = profileProgress(firstProfile);
			const ProfileCoverageProgress initialSecond = profileProgress(secondProfile);

			system.Update(0.0F);
			const ProfileCoverageProgress afterFirst = profileProgress(firstProfile);
			const ProfileCoverageProgress afterFirstSecond = profileProgress(secondProfile);
			// The first rebuilt profile becomes Ready while the still-dirty peer remains
			// incomplete. This checks the affected shard state directly rather than
			// treating unrelated retained Ready coverage as proof that the peer rebuilt.
			const bool firstOnlyPublished = initialFirst.hasReadyCoverage && initialSecond.hasReadyCoverage
				&& afterFirst.hasReadyCoverage
				&& afterFirst.latestReadyGeneration > initialFirst.latestReadyGeneration
				&& afterFirstSecond.hasDirtyCoverage
				&& system.DynamicDirtyIndexForTesting().PendingPreparationCount() == 1U;
			system.Update(0.0F);
			const ProfileCoverageProgress afterSecond = profileProgress(secondProfile);
			const bool bothProfilesPublished = afterFirst.hasReadyCoverage && afterSecond.hasReadyCoverage
				&& afterSecond.latestReadyGeneration > initialSecond.latestReadyGeneration
				&& !afterSecond.hasDirtyCoverage
				&& system.DynamicDirtyIndexForTesting().PendingPreparationCount() == 0U;

			context.Expect(initialQueued && stageAccepted && partialBatchRejected && restageAccepted && receiptAccepted,
				"The dynamic multi-profile fixture requires one accepted immutable replacement receipt.");
			const std::string profileState = "first=" + std::to_string(afterFirst.hasReadyCoverage)
				+ "/" + std::to_string(afterFirst.hasDirtyCoverage)
				+ "/" + std::to_string(afterFirst.latestReadyGeneration)
				+ ", second-after-first=" + std::to_string(afterFirstSecond.hasReadyCoverage)
				+ "/" + std::to_string(afterFirstSecond.hasDirtyCoverage)
				+ "/" + std::to_string(afterFirstSecond.latestReadyGeneration)
				+ ", second-after-second=" + std::to_string(afterSecond.hasReadyCoverage)
				+ "/" + std::to_string(afterSecond.hasDirtyCoverage)
				+ "/" + std::to_string(afterSecond.latestReadyGeneration)
				+ ", pending=" + std::to_string(system.DynamicDirtyIndexForTesting().PendingPreparationCount());
			const auto diagnostics = system.PublishedDiagnostics();
			std::string dynamicRebuildReasons;
			if(diagnostics) {
				for(const NavigationDiagnosticRecord& record : diagnostics->records) {
					if(record.code != "navigation.dynamic_rebuild_unavailable") continue;
					if(!dynamicRebuildReasons.empty()) dynamicRebuildReasons += " | ";
					dynamicRebuildReasons += record.message;
				}
			}
			const NavigationSettings settings = system.SettingsSnapshot();
			const std::string rebuildEvidence = ", source="
				+ std::to_string(replacementSource != replacementBuild.sourceIndex.end())
				+ "/" + std::to_string(replacementSource != replacementBuild.sourceIndex.end()
					? replacementSource->sourceRevision : 0U)
				+ "/" + std::to_string(replacementGeometry ? replacementGeometry->Descriptor().sourceRevision : 0U)
				+ "/" + std::to_string(receipt.intent.committedSourceRevision)
				+ ", settings=profiles:" + std::to_string(settings.maxActiveProfileBuilds)
				+ ",builds:" + std::to_string(settings.maxConcurrentBuilds)
				+ ",searches:" + std::to_string(settings.maxConcurrentSearches)
				+ ",geometryBytes:" + std::to_string(settings.maxGeometryBytes)
				+ ",scratchBytes:" + std::to_string(settings.maxBuildScratchBytes)
				+ ", dynamicReason=" + (dynamicRebuildReasons.empty() ? "none" : dynamicRebuildReasons);
			context.Expect(firstOnlyPublished && bothProfilesPublished,
				"A dirty source must retain its dependency certificate until every profile rebuild publishes, then acknowledge exactly once: "
					+ profileState + rebuildEvidence);
			system.Shutdown();
		});
	}
}
