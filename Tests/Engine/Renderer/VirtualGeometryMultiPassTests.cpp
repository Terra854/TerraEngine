#include "Renderer/VirtualGeometry/Selection/VirtualGeometryViewPolicy.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryMultiPassTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.MultiPass.CompatibleViewsShareGeneration",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryViewPolicyState o{ VirtualGeometryViewKind::Camera, 1, 7, 2 }, v{ VirtualGeometryViewKind::Reflection, 2, 7, 3 };
				c.Expect(EvaluateVirtualGeometryViewPolicy(v, o).sharesGeometry,
					"Shadow/capture/reflection/editor views share only compatible geometry generations.");
			});
	}
} // namespace TerraEngine::Tests
