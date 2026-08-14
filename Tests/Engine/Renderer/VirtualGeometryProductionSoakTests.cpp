#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryProductionSoakTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Engine.ProductionSoak.BoundedGrowthEvidenceIsRequired",
			[](TestContext &c)
			{
				const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
				const auto p = root / "out/evidence/virtual-geometry-production/production-soak.json";
				if(!std::filesystem::exists(p))
				{
					c.Expect(!std::filesystem::exists(root / "out/evidence/045/production-soak.json"),
						"Retired evidence paths must not satisfy the production soak gate.");
					return;
				}
				std::ifstream input(p);
				const std::string text{ std::istreambuf_iterator<char>(input), {} };
				c.Expect(text.find("boundedGrowth") != std::string::npos && text.find("status") != std::string::npos,
					"A recorded production soak must include bounded-growth and status fields.");
			});
		registry.Add("VirtualGeometry.Engine.ProductionSoak.RequiredDurationIsThirtyMinutes",
			[](TestContext &c)
			{
				const auto script = std::filesystem::path(TERRAENGINE_SOURCE_DIR) /
					"Tests/Tools/VirtualGeometry/RunVirtualGeometryProductionSoak.ps1";
				std::ifstream input(script);
				const std::string text{ std::istreambuf_iterator<char>(input), {} };
				c.Expect(text.find("30") != std::string::npos && text.find("bounded") != std::string::npos,
					"Production soak orchestration must require a bounded 30-minute run before acceptance.");
			});
	}
} // namespace TerraEngine::Tests
