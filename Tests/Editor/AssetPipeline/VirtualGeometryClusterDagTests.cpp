#include "TestHarness.h"

#include "VirtualGeometryClusterDagBuilder.h"

#include <algorithm>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryClusterDagTests(TestRegistry &registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometry.Dag.GroupsAdjacentMaterialPeersDeterministically",
			[](TestContext &context)
			{
				using namespace TerraEngine::Editor;
				const std::vector<VirtualGeometryClusterInput> ordered = {
					{ 10u, 2u, { 20u }, 1.0f }, { 20u, 2u, { 10u, 30u }, 2.0f }, { 30u, 3u, { 20u }, 3.0f }
				};
				const std::vector<VirtualGeometryClusterInput> reversed(ordered.rbegin(), ordered.rend());
				const auto first = BuildVirtualGeometryClusterDag(ordered, { 4u });
				const auto second = BuildVirtualGeometryClusterDag(reversed, { 4u });

				context.Expect(ValidateVirtualGeometryDag(first), "Generated adjacency groups must form a valid coverage DAG.");
				context.ExpectEqual(first.size(), second.size(), "Worker completion order must not change hierarchy size.");
				for (std::size_t index = 0u; index < first.size(); ++index)
				{
					context.ExpectEqual(first[index].id, second[index].id, "Worker completion order must not change stable node IDs.");
				}
				context.ExpectEqual(static_cast<std::size_t>(4u), first.size(), "Only the adjacent same-material pair should gain a parent.");
				context.Expect(first[0].fallbackId == first[1].fallbackId, "Adjacent children must share their resident parent fallback.");
				context.Expect(first[2].fallbackId == first[2].id, "A material boundary must remain an independent root.");
			});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.Dag.RejectsIndirectCyclesAndIncompleteFallbacks",
			[](TestContext &context)
			{
				using TerraEngine::Editor::ValidateVirtualGeometryDag;
				using TerraEngine::Editor::VirtualGeometryDagNode;
				std::vector<VirtualGeometryDagNode> cycle = { { 1u, 2u, 2u, { 2u }, 1.0f, true }, { 2u, 1u, 1u, { 1u }, 1.0f, true } };
				context.Expect(!ValidateVirtualGeometryDag(cycle), "Indirect parent cycles must be rejected atomically.");

				std::vector<VirtualGeometryDagNode> missingFallback = { { 1u, 0u, 99u, {}, 1.0f, true } };
				context.Expect(!ValidateVirtualGeometryDag(missingFallback), "Every leaf must name a present resident fallback.");
			});
	}
} // namespace TerraEngine::EditorTests
