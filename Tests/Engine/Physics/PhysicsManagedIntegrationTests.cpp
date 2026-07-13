// PhysicsManagedIntegrationTests.cpp - managed facade source/contract checks.

#include "../TestHarness.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		std::string ReadSource(const char* relativePath) {
			// Source-contract checks must not depend on CTest's working directory.
			// CMake supplies the checkout root, so the same test works from IDEs,
			// build trees, and the repository root.
			std::ifstream source{std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath};
			return {std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
		}
	}

	void RegisterPhysicsManagedIntegrationTests(TestRegistry& registry) {
		registry.Add("Engine.PhysicsManagedIntegration.RigidbodyUsesDeferredAccelerationForCustomGravity", [](TestContext& context) {
			const std::string source = ReadSource("Engine/Managed/Source/Physics/Rigidbody.cs");
			context.Expect(source.find("UseGravity") != std::string::npos && source.find("ForceMode.Acceleration") != std::string::npos,
				"Managed Rigidbody must expose authored UseGravity and the mass-independent Acceleration force mode for custom gravity.");
		});

		registry.Add("Engine.PhysicsManagedIntegration.QueriesEventsAndSnapshotsRemainCopied", [](TestContext& context) {
			const std::string physics = ReadSource("Engine/Managed/Source/Physics/Physics.cs");
			const std::string events = ReadSource("Engine/Managed/Source/Physics/PhysicsEvents.cs");
			context.Expect(physics.find("Native_Physics_ReadQuery") != std::string::npos && physics.find("BodyStates") != std::string::npos,
				"Managed queries and state reads must copy immutable native snapshots rather than retaining provider storage.");
			context.Expect(events.find("DispatchProtected") != std::string::npos && events.find("catch (Exception") != std::string::npos,
				"Managed event dispatch must be protected-boundary work and isolate individual callback failures.");
		});
	}
}
