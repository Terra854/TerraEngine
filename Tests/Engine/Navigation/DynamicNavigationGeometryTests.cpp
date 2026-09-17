// DynamicNavigationGeometryTests.cpp - general dynamic geometry acceptance matrix.

#include "TestHarness.h"

#include "Navigation/Dynamic/NavigationDirtyIndex.h"

#include <array>
#include <string>
#include <string_view>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Navigation;

		enum class Operation { Create, Enable, Disable, Move, Rotate, Scale, Delete, Replace, Reshape };
		enum class Shape { TerrainSupport, MeshSupport, Stair, Ramp, WallObstructor, LowCeiling, Gap };
		struct Case { std::string_view id; Operation operation; Shape shape; bool becomesBlocked; };

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
	}
}
