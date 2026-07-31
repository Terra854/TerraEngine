#include "Renderer/VirtualGeometry/Selection/VirtualGeometrySelection.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryTraversalOracleTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.TraversalOracle.RequestsResidentAncestor",
			[](TestContext &c)
			{
				auto x = TerraEngine::Renderer::SelectVirtualGeometryOracle(
					{ { 2, 1, false, 2 }, { 1, 0, true, 0 } }, 1, 4);
				c.Expect(x.selected[0] == 1 && x.requests[0] == 2, "Missing detail requests and selects parent fallback.");
			});

		r.Add("VirtualGeometry.TraversalOracle.WalksResidentAncestorChainDeterministically",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				const std::vector<VirtualGeometryOracleNode> nodes{
					{ 3u, 2u, false, 4.0f },
					{ 1u, 0u, true, 1.0f },
					{ 2u, 1u, false, 3.0f } };
				const VirtualGeometrySelectionResult result =
					SelectVirtualGeometryOracle(nodes, 2.0f, { 4u, true });
				c.Expect(result.selected.size() == 1u && result.selected[0] == 1u,
					"The processor oracle must walk through non-resident parents to the nearest resident ancestor.");
				c.Expect(result.requests.size() == 2u && result.requests[0] == 2u
					&& result.requests[1] == 3u && !result.failOpen,
					"Oracle requests are sorted and deduplicated independently from the selected conservative cut.");
			});
	}
} // namespace TerraEngine::Tests
