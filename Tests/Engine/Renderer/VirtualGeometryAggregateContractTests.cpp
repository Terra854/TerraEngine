#include "Renderer/VirtualGeometry/Geometry/VirtualGeometryRepresentations.h"
#include "Renderer/RenderTypes.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryAggregateContractTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.Aggregate.RequiresFallbackCoverage",
			[](TestContext &c)
			{
				TerraEngine::Renderer::VirtualGeometryAggregateRepresentation a{
					TerraEngine::Renderer::VirtualGeometryAggregateKind::Voxel,
					1, 0, true, 4, 1.0f, 1, 1, 1, true, true };
				c.Expect(a.Valid(), "Aggregate records require stable identity and complete fallback coverage.");
				const auto invalid = TerraEngine::Renderer::VirtualGeometryAggregateRepresentation{
					TerraEngine::Renderer::VirtualGeometryAggregateKind::Voxel,
					2, 0, true, 1, 1.2f, 2, 2, 2, true, true };
				c.Expect(!invalid.Valid(), "Coverage outside the canonical range must be rejected.");
			});
		r.Add("VirtualGeometry.Aggregate.RuntimeProfileRequiresClusterFallback",
			[](TestContext &c)
			{
				::VirtualGeometryAssetRuntime::AggregateProfileRuntime profile{};
				profile.representation = {
					TerraEngine::Renderer::VirtualGeometryAggregateKind::Fallback,
					7u, 0.25f, true, 3u, 1.0f, 7u, 7u, 7u, true, true };
				profile.fallbackClusterId = 4u;
				c.Expect(profile.Valid(), "A runtime aggregate profile needs a complete canonical fallback cluster.");
				profile.fallbackClusterId = 0xffffffffu;
				c.Expect(!profile.Valid(), "A profile without a typed cluster fallback must not enter residency selection.");
			});
	}
} // namespace TerraEngine::Tests
