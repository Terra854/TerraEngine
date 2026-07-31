#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryVisibility.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryVisibilityLayoutTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.VisibilityLayout.SelectsPackedWideSeparate",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				c.Expect(SelectVirtualGeometryVisibilityLayout({ 16, 12, 4 }) == VirtualGeometryVisibilityLayout::Packed,
					"Small identity ranges use packed layout.");
				c.Expect(SelectVirtualGeometryVisibilityLayout({ 24, 20, 8 }) == VirtualGeometryVisibilityLayout::Wide, "Larger ranges use wide layout.");
				c.Expect(SelectVirtualGeometryVisibilityLayout({ 33, 25, 8 }) == VirtualGeometryVisibilityLayout::Separate,
					"Unrepresentable ranges use separate buffers.");
			});
		r.Add("VirtualGeometry.Visibility.PacksStableRouteIdentity",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				const VirtualGeometryVisibilityRequirements requirements{ 16u, 12u, 4u };
				const VirtualGeometryVisibilityIdentity input{ 42u, 7u, 3u, 0u };
				const auto packed = PackVirtualGeometryVisibilityIdentity(input, requirements);
				const auto output = UnpackVirtualGeometryVisibilityIdentity(packed, requirements);
				c.Expect(packed.representable && output.primitiveId == input.primitiveId
					&& output.instanceId == input.instanceId && output.materialId == input.materialId,
					"Packed visibility preserves the stable identity used by every raster route.");
			});
	}
} // namespace TerraEngine::Tests
