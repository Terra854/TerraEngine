// DynamicNavigationGeometryTests.cpp - general dynamic geometry acceptance matrix.

#include "TestHarness.h"

#include "Navigation/Dynamic/NavigationDirtyIndex.h"

#include <array>
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
	}
}
