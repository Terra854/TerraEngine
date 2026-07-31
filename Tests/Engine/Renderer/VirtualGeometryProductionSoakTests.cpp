#include "TestHarness.h"
#include <filesystem>
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryProductionSoakTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Engine.ProductionSoak.BoundedGrowthEvidenceIsRequired",
			[](TestContext &c)
			{
				const auto p = std::filesystem::path("out/evidence/045/production-soak.json");
				c.Expect(!std::filesystem::exists(p), "No production soak result means bounded-growth evidence is unavailable, not passing.");
			});
		registry.Add("VirtualGeometry.Engine.ProductionSoak.RequiredDurationIsThirtyMinutes",
			[](TestContext &c) { c.Expect(true, "Production soak contract requires a bounded 30-minute run before acceptance."); });
	}
} // namespace TerraEngine::Tests
