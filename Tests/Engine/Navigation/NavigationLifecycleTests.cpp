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

		registry.Add("Engine.NavigationLifecycle.ObjectRequestsHaveNoAgentOrCommandSynchronization", [](TestContext& context) {
			const std::string system = ReadSource("Engine/Navigation/NavigationSystem.cpp");
			const std::string stores = ReadSource("Engine/Navigation/NavigationStores.h");
			context.Expect(system.find("SynchronizeSceneObjectAgents") == std::string::npos
				&& system.find("QueueSetDestination") == std::string::npos,
				"Component-free navigation must admit object routes directly instead of synchronizing agent destination commands.");
			context.Expect(stores.find("NavigationAgentStore") == std::string::npos
				&& stores.find("NavigationCommandStore") == std::string::npos,
				"Bounded stores must retain only request and route records after agent and command state removal.");
		});

		registry.Add("Engine.NavigationLifecycle.PlayModeTerrainRepublishInvalidatesBootstrap", [](TestContext& context) {
			const std::string header = ReadSource("Engine/Core/MainEngine.h");
			const std::string source = ReadSource("Engine/Core/MainEngine.cpp");
			context.Expect(header.find("lastNavigationBootstrapTerrainCollisionStatus") != std::string::npos
				&& header.find("lastNavigationBootstrapTerrainRecipeGeneration") != std::string::npos
				&& header.find("lastNavigationBootstrapPhysicsWorldGeneration") != std::string::npos,
				"The bootstrap cache must retain collision availability and physics-world ownership beside authored terrain generation.");
			context.Expect(source.find("terrainCollisionStatus != lastNavigationBootstrapTerrainCollisionStatus") != std::string::npos
				&& source.find("terrainRecipeGeneration != lastNavigationBootstrapTerrainRecipeGeneration") != std::string::npos
				&& source.find("physicsWorldGeneration != lastNavigationBootstrapPhysicsWorldGeneration") != std::string::npos,
				"A Pending-to-Current terrain republish or fresh physics world must reattempt automatic navigation capture after Play Mode restart.");
		});
	}
}
