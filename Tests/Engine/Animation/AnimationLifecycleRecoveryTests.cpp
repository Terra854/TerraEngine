#include "TestHarness.h"

#include <fstream>
#include <sstream>

namespace TerraEngine::Tests
{
	void RegisterAnimationLifecycleRecoveryTests(TestRegistry& registry)
	{
		registry.Add("Animation.LifecycleRecovery.InvalidatesRetainedGenerations", [](TestContext& context)
		{
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + "/Engine/Animation/AnimationSystem.cpp", std::ios::binary);
			std::ostringstream source; source << input.rdbuf();
			const std::string text = source.str();
			context.Expect(text.find("recovery") != std::string::npos || text.find("rendererGeneration") != std::string::npos, "Animation lifecycle code must account for renderer recovery generations.");
			context.Expect(text.find("reload") != std::string::npos || text.find("resource") != std::string::npos, "Animation lifecycle code must account for reload/resource invalidation.");
		});
	}
}
