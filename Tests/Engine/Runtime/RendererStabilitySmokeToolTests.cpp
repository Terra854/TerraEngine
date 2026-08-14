#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::Tests
{
	void RegisterRendererStabilitySmokeToolTests(TestRegistry& registry)
	{
		registry.Add("RendererStability.SmokeTool.PacedOptInContract", [](TestContext& context) {
			std::ifstream stream(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tests/Tools/RendererStability/RunLiveSmoke.ps1");
			std::ostringstream source;
			source << stream.rdbuf();
			context.Expect(source.str().find("-AllowLiveGpuStress") != std::string::npos, "Live GPU stress must be explicitly opted in.");
			context.Expect(source.str().find("Start-Sleep") != std::string::npos, "Resize operations must be paced.");
			context.Expect(source.str().find("driver") != std::string::npos, "The harness must capture driver-event evidence.");
		});
	}
}
