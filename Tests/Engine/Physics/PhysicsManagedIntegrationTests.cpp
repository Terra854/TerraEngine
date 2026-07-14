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
			context.Expect(physics.find("Native_Physics_QueryImmediate") != std::string::npos && physics.find("Native_Physics_ReadQuery") != std::string::npos && physics.find("BodyStates") != std::string::npos,
				"Managed immediate and scheduled queries plus state reads must copy immutable native snapshots rather than retaining provider storage.");
			context.Expect(physics.find("CorrelationId = info.CorrelationId") != std::string::npos,
				"Managed query results must preserve the numeric correlation returned by both immediate and scheduled ABI reads.");
			context.Expect(events.find("DispatchProtected") != std::string::npos && events.find("catch (Exception") != std::string::npos,
				"Managed event dispatch must be protected-boundary work and isolate individual callback failures.");
		});

		registry.Add("Engine.PhysicsManagedIntegration.WrappersReloadAndCallbacksStayIsolated", [](TestContext& context) {
			const std::string rigidbody = ReadSource("Engine/Managed/Source/Physics/Rigidbody.cs");
			const std::string events = ReadSource("Engine/Managed/Source/Physics/PhysicsEvents.cs");
			const std::string lifecycle = ReadSource("Engine/Managed/Source/InternalCalls.cs");
			context.Expect(rigidbody.find("Physics.BindComponent") != std::string::npos && rigidbody.find("Identity = Identity") != std::string::npos,
				"Managed rigidbody calls must bind a copied generation-bearing identity instead of retaining a provider pointer.");
			context.Expect(events.find("GetInvocationList") != std::string::npos && events.find("catch (Exception exception)") != std::string::npos,
				"Protected event delivery must snapshot subscribers and isolate each throwing callback.");
			context.Expect(lifecycle.find("PhysicsEvents.ResetManagedSubscriptions") != std::string::npos,
				"Gameplay reload and stop must invalidate managed physics subscriptions before their runtime context is released.");
		});
	}
}
