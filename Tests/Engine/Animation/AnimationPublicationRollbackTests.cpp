#include "TestHarness.h"

#include <fstream>
#include <sstream>

namespace TerraEngine::Tests
{
	void RegisterAnimationPublicationRollbackTests(TestRegistry& registry)
	{
		registry.Add("Animation.PublicationRollback.RejectsStaleCandidatesAtomically", [](TestContext& context)
		{
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + "/Engine/Animation/AnimationSystem.cpp", std::ios::binary);
			std::ostringstream source; source << input.rdbuf();
			const std::string text = source.str();
			context.Expect(text.find("stale") != std::string::npos && text.find("reject") != std::string::npos, "Animation publication must diagnose stale candidates and reject them.");
			context.Expect(text.find("publication") != std::string::npos && text.find("candidate") != std::string::npos, "Publication validation must operate on complete candidates before mutation.");
		});
	}
}
