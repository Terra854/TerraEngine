#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryVisibility.h"
#include "Renderer/RenderSystem.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryVisibilitySemanticsTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.Visibility.DepthTieIsStable",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				c.Expect(WinsVirtualGeometryDepthTie({ .5f, 2 }, { .5f, 3 }), "Equal-depth MSAA/alpha samples select the lower stable primitive.");
				c.Expect(!WinsVirtualGeometryDepthTie({ .6f, 1 }, { .5f, 9 }), "Clipped/viewport candidates behind current depth cannot overwrite.");
			});

		r.Add("VirtualGeometry.Visibility.EditorPickingUsesStableCompactedIdentity",
			[](TestContext &c)
			{
				const TerraEngine::UUID wideObject = TerraEngine::GenerateUUID();
				const TerraEngine::UUID tightObject = TerraEngine::GenerateUUID();
				const TerraEngine::UUID mesh = TerraEngine::GenerateUUID();
				std::vector<GpuVirtualGeometryPacketRecord> records(2u);
				records[0].objectId = wideObject;
				records[0].meshId = mesh;
				records[0].frameId = 9u;
				records[0].meshletCount = 2u;
				records[0].projectedBounds = glm::vec4(0.1f, 0.1f, 0.9f, 0.9f);
				records[0].debugIdentity = 20u;
				records[1].objectId = tightObject;
				records[1].meshId = mesh;
				records[1].frameId = 9u;
				records[1].meshletOffset = 12u;
				records[1].meshletCount = 1u;
				records[1].projectedBounds = glm::vec4(0.4f, 0.4f, 0.6f, 0.6f);
				records[1].debugIdentity = 10u;

				const std::optional<VirtualGeometryVisibilityPick> pick =
					RenderSystem::PickVirtualGeometryVisibilityRecord(
						records,
						glm::vec2(0.5f, 0.5f));
				c.Expect(
					pick.has_value() && pick->objectId == tightObject,
					"Scene picking should select the tightest compacted visibility record under the cursor.");
				c.Expect(
					pick.has_value() && pick->meshletId == 12u
						&& pick->debugIdentity == 10u,
					"Picking should preserve stable meshlet/debug identity across raster routes.");
			});
	}
} // namespace TerraEngine::Tests
