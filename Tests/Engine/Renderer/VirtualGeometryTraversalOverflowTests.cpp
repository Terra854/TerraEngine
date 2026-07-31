#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryTraversalOverflowTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.Traversal.FailOpenOverflow",
			[](TestContext &c)
			{
				TerraEngine::Renderer::VirtualGeometrySelectionResult s{ { 1, 2, 3 }, {}, false };
				auto p = TerraEngine::Renderer::BuildVirtualGeometryGpuPlan(s, 1);
				c.Expect(p.overflow && p.failOpen && p.packets.size() == 1,
					"Bounded plans flag overflow and retain conservative fallback coverage.");
				c.Expect(p.IsComplete() && p.traversal.requestCapacity == 1u
					&& p.compaction.outputCapacity == 1u && p.generatedWork.commandCapacity == 1u,
					"Traversal, compaction, and generated work share one immutable bounded capacity contract.");
			});
	}
} // namespace TerraEngine::Tests
