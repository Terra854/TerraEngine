// NavigationManagedTests.cpp - C# object-route ABI surface checks.

#include "../TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests {
	void RegisterNavigationManagedTests(TestRegistry& registry) {
		registry.Add("Engine.NavigationManaged.ObjectRouteFacadeKeepsCopiedResultLifecycle", [](TestContext& context) {
			const auto read = [](const char* relativePath) {
				std::ifstream file(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			};
			const std::string types = read("Engine/Managed/Source/Navigation/NavigationTypes.cs");
			const std::string navigation = read("Engine/Managed/Source/Navigation/Navigation.cs");
			const std::string imports = read("Engine/Managed/Source/InternalCalls.cs");
			context.Expect(types.find("MissingTerrain = 1") != std::string::npos
				&& types.find("NativeNavigationObjectToLocationRequest") != std::string::npos
				&& types.find("NativeNavigationObjectToObjectRequest") != std::string::npos,
				"Managed packets must mirror fixed-width v2 object requests and specific native failure reasons.");
			context.Expect(navigation.find("AbiVersionConstant = 2") != std::string::npos
				&& navigation.find("ReadRequest") != std::string::npos
				&& navigation.find("ValidateRoute") != std::string::npos
				&& navigation.find("CancelRoute") != std::string::npos
				&& navigation.find("ReleaseResult") != std::string::npos,
				"Managed gameplay must retain explicit request read, validate, cancel, and release operations.");
			context.Expect(imports.find("Native_Navigation_RequestObjectToLocation") != std::string::npos
				&& imports.find("Native_Navigation_RequestObjectToObject") != std::string::npos,
				"Managed imports must retain both direct object route request shapes.");
		});

		registry.Add("Engine.NavigationManaged.RouteFollowerUsesPhysicsAndBoundedReplans", [](TestContext& context) {
			const auto read = [](const char* relativePath) {
				std::ifstream file(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			};
			const std::string follower = read("Engine/Managed/Source/Navigation/NavigationRouteFollower.cs");
			context.Expect(follower.find("SetDesiredVelocity") != std::string::npos
				&& follower.find("_transform.Position") != std::string::npos
				&& follower.find("ValidateRoute") != std::string::npos
				&& follower.find("HasDisabledOwnerColliderSet") != std::string::npos
				&& follower.find("LastMovementDiagnostic") != std::string::npos,
				"The managed follower must read published progress and command physics velocity instead of assigning a navigation transform.");
			context.Expect(follower.find("WaitingToReplan") != std::string::npos
				&& follower.find("_replanRemainingSeconds") != std::string::npos
				&& follower.find("ScheduleReplan") != std::string::npos
				&& follower.find("ReleaseCurrentRequest") != std::string::npos
				&& follower.find("Dispose() => Stop()") != std::string::npos
				&& follower.find("_progressSinceCheckMetres") != std::string::npos,
				"Blocked, stale, and unavailable routes must release their old request and retry at a bounded interval.");
			context.Expect(follower.find("Teleport(") == std::string::npos
				&& follower.find(".Position =") == std::string::npos
				&& follower.find("Physics.CharacterStates") == std::string::npos,
				"The helper must not bypass collision through teleports or transform writes, or allocate a character-state array each step.");
		});
	}
}
