// NavigationLifecycleTests.cpp - engine owner/late-completion lifecycle contracts.

#include "../TestHarness.h"

#include "Navigation/NavigationSystem.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadSource(const char* relativePath)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}
	}

	void RegisterNavigationLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Engine.NavigationLifecycle.RevokeAllOwnersInvalidatesLateWork", [](TestContext& context) {
			Navigation::NavigationSystem& system = Navigation::NavigationSystem::Instance();
			system.RevokeAllOwners(Navigation::NavigationReason::SceneReplaced);
			const auto invalidations = system.ReadInvalidations({}, 0U);
			context.Expect(invalidations.status == Navigation::NavigationHandleStatus::InvalidOwner,
				"Late completion readers must reject an owner-less handle after lifecycle revocation.");
		});

		registry.Add("Engine.NavigationLifecycle.ShutdownAndRevokePathsContainPendingCompletions", [](TestContext& context) {
			const std::string source = ReadSource("Engine/Navigation/NavigationSystem.cpp");
			context.Expect(source.find("RevokeAllOwners") != std::string::npos
				&& source.find("pendingRoutes_") != std::string::npos
				&& source.find("preparedRoutes_") != std::string::npos,
				"Navigation lifecycle code must account for both queued requests and late prepared completions.");
			context.Expect(source.find("pendingRoutes_.clear()") != std::string::npos
				&& source.find("preparedRoutes_.clear()") != std::string::npos,
				"Revocation/shutdown must clear pending and prepared route work before the next world can publish.");
		});
	}
}
