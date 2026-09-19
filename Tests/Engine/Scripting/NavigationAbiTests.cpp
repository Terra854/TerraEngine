// NavigationAbiTests.cpp - fixed-width direct-query and agent navigation ABI checks.

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

	}
}
