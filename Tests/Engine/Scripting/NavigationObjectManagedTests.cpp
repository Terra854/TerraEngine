// NavigationObjectManagedTests.cpp - component-free managed route surface.

#include "../TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests {
	void RegisterNavigationObjectManagedTests(TestRegistry& registry) {
		registry.Add("Navigation.ObjectManaged.UsesObjectsAndNeverNavigationAgents", [](TestContext& context) {
			const auto read = [](const char* relativePath) {
				std::ifstream file(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			};
			const std::string facade = read("Engine/Managed/Source/Navigation/Navigation.cs");
			const std::string glue = read("Engine/Scripting/ScriptGlue.cpp");
			context.Expect(facade.find("RequestRoute(Object source, NavigationLocation") != std::string::npos
				&& facade.find("RequestRoute(Object source, Object target") != std::string::npos,
				"Managed navigation must request location and object-target routes directly from ordinary Objects.");
			context.Expect(facade.find("never moves") != std::string::npos
				&& glue.find("RequestObjectToLocation") != std::string::npos,
				"The managed ABI must copy a route request without assigning movement authority to navigation.");
			context.Expect(glue.find("ResolveObjectAgent") == std::string::npos
				&& glue.find("QueueSetDestination") == std::string::npos,
				"The object route bridge must not retain any agent or destination-command fallback.");
		});
	}
}
