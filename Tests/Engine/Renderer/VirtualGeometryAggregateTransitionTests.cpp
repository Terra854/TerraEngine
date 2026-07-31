#include "TestHarness.h"
#include "VirtualGeometryAssemblyBuilder.h"
#include "VirtualGeometryFoliageCurveBuilder.h"
#include "VirtualGeometryVoxelBuilder.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryAggregateTransitionTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.Aggregate.AssemblyStableOrder",
			[](TestContext &c)
			{
				 auto a = TerraEngine::Editor::BuildVirtualGeometryAssemblyPrototypes({ 3, 1, 2 });
				c.Expect(a.size() == 3 && a[0].Valid() && a[0].sourceElementCount == 1, "Assembly prototypes are deterministic and covered.");
				const auto transition = TerraEngine::Renderer::BuildVirtualGeometryAggregateTransition(a[0], a[1]);
				c.Expect(transition.fromId == a[0].stableId && transition.toId == a[1].stableId, "Transitions retain stable aggregate identities.");
			});

		r.Add("VirtualGeometry.Aggregate.SpecializedBuildersRemainDistinctAndCovered",
			[](TestContext &c)
			{
				const auto foliage =
					TerraEngine::Editor::BuildVirtualGeometryFoliageCurvePrototype(
						17u, false);
				const auto curve =
					TerraEngine::Editor::BuildVirtualGeometryFoliageCurvePrototype(
						17u, true);
				const auto voxel =
					TerraEngine::Editor::BuildVirtualGeometryVoxelPrototype(
						17u, false);
				const auto impostor =
					TerraEngine::Editor::BuildVirtualGeometryVoxelPrototype(
						17u, true);
				c.Expect(
					foliage.Valid() && curve.Valid()
						&& voxel.Valid() && impostor.Valid(),
					"Every specialized aggregate builder must publish complete fallback coverage.");
				c.Expect(
					foliage.stableId != curve.stableId
						&& voxel.stableId != impostor.stableId,
					"Domain salts must keep specialized representations distinct for one source identity.");
			});
	}
} // namespace TerraEngine::Tests
