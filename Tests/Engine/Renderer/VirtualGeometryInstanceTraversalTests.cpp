#include "Renderer/VirtualGeometry/Selection/VirtualGeometryWorldHierarchy.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryInstanceTraversalTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.InstanceTraversal.Snapshot",
			[](TestContext &c)
			{
				TerraEngine::Renderer::VirtualGeometryWorldHierarchySnapshot s{ 1, TerraEngine::Renderer::VirtualGeometryViewPolicy::Shadow, { { 2, 3, 4 } } };
				c.Expect(s.instances[0].generation == 4, "Snapshots retain immutable instance generations.");
			});
	}
} // namespace TerraEngine::Tests
