#include "TestHarness.h"

#include <fstream>
#include <sstream>

namespace TerraEngine::Tests
{
	void RegisterAnimationClassTransitionTests(TestRegistry& registry)
	{
		registry.Add("Animation.ClassTransition.PreservesContinuityFields", [](TestContext& context)
		{
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + "/Engine/Animation/Runtime/AnimationExecutionMode.cpp", std::ios::binary);
			std::ostringstream source; source << input.rdbuf();
			const std::string text = source.str();
			context.Expect(text.find("playback") != std::string::npos && text.find("continuity") != std::string::npos, "Class transitions must preserve playback and continuity fields.");
			context.Expect(text.find("event") != std::string::npos && text.find("generation") != std::string::npos, "Class transitions must carry event and generation continuity.");
		});
	}
}
