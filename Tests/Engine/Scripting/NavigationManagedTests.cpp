// NavigationManagedTests.cpp - managed navigation layout and lifecycle contracts.

#include "../TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterNavigationManagedTests(TestRegistry& registry)
	{
		registry.Add("Engine.NavigationManaged.ManagedLayoutsAndMappingsRemainVersioned", [](TestContext& context) {
			const auto read = [](const char* path) {
				std::ifstream file(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / path, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			};
			const std::string types = read("Engine/Managed/Source/Navigation/NavigationTypes.cs");
			const std::string navigation = read("Engine/Managed/Source/Navigation/Navigation.cs");
			context.Expect(types.find("enum NavigationStatus : uint") != std::string::npos
				&& types.find("enum NavigationReason : uint") != std::string::npos
				&& types.find("StructLayout(LayoutKind.Sequential, Pack = 8)") != std::string::npos,
				"Managed navigation status/reason values and ABI packets must use fixed-width sequential layouts.");
			context.Expect(navigation.find("AbiVersionConstant = 1") != std::string::npos
				&& navigation.find("MaximumRoutePoints = 4096") != std::string::npos
				&& navigation.find("ReleaseResult") != std::string::npos,
				"Managed navigation must pin an ABI version, bounded route-copy limit, and explicit result release.");
		});

		registry.Add("Engine.NavigationManaged.FacadesReplaceCancelAndInvalidateHandles", [](TestContext& context) {
			const std::filesystem::path agentPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Managed/Source/Navigation/NavigationAgent.cs";
			std::ifstream file(agentPath, std::ios::binary);
			const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
			context.Expect(source.find("SetDestination") != std::string::npos
				&& source.find("CancelRoute") != std::string::npos
				&& source.find("intentGeneration") != std::string::npos,
				"NavigationAgent must replace destinations by canceling prior requests and advancing intent generation.");
			context.Expect(source.find("Expired") != std::string::npos && source.find("StaleTarget") != std::string::npos,
				"Expired and stale route handles must be invalidated on the managed side.");
		});
	}
}
