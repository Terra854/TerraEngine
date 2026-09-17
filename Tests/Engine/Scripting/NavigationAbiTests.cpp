// NavigationAbiTests.cpp - fixed-width component-free navigation ABI checks.

#include "../TestHarness.h"

#include "Scripting/ScriptGlue.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests {
	void RegisterNavigationAbiTests(TestRegistry& registry) {
		registry.Add("Navigation.ManagedAbi.ObjectRoutePacketsAreVersionedAndFixedWidth", [](TestContext& context) {
			context.ExpectEqual(kNativeNavigationAbiVersion, Native_Navigation_GetAbiVersion(),
				"The identity-free navigation profile bridge must retain ABI v2.");
			context.ExpectEqual(static_cast<std::size_t>(416U), sizeof(NativeNavigationObjectToLocationRequest),
				"Object-to-location requests must retain a fixed copied stride.");
			context.ExpectEqual(static_cast<std::size_t>(360U), sizeof(NativeNavigationObjectToObjectRequest),
				"Object-to-object requests must retain a fixed copied stride.");
			context.Expect(offsetof(NativeNavigationObjectToLocationRequest, header) == 0U
				&& offsetof(NativeNavigationObjectToObjectRequest, header) == 0U
				&& offsetof(NativeNavigationSubmitReceipt, header) == 0U,
				"Every object-route packet must begin with a versioned ABI header.");
		});

		registry.Add("Navigation.ManagedAbi.AgentAndCommandExportsArePhysicallyAbsent", [](TestContext& context) {
			const auto read = [](const char* relativePath) {
				std::ifstream file(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			};
			const std::string header = read("Engine/Scripting/ScriptGlue.h");
			const std::string glue = read("Engine/Scripting/ScriptGlue.cpp");
			context.Expect(header.find("Native_Navigation_RequestObjectToLocation") != std::string::npos
				&& header.find("Native_Navigation_RequestObjectToObject") != std::string::npos,
				"The native ABI must expose both object request shapes.");
			context.Expect(header.find("NativeNavigationAgentHandle") == std::string::npos
				&& header.find("NativeNavigationCommandHandle") == std::string::npos
				&& glue.find("Native_Navigation_SetDestination") == std::string::npos,
				"The ABI cutover must remove old agent and command symbols instead of keeping deprecated stubs.");
		});
	}
}
