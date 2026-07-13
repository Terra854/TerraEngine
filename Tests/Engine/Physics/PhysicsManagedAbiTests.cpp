// PhysicsManagedAbiTests.cpp - C ABI contract tests for managed physics.

#include "../TestHarness.h"

#include "Scripting/ScriptGlue.h"

#include <cstddef>

namespace TerraEngine::Tests {
	void RegisterPhysicsManagedAbiTests(TestRegistry& registry) {
		registry.Add("Engine.PhysicsManagedAbi.FixedWidthHeadersAndV1AreStable", [](TestContext& context) {
			context.ExpectEqual(1U, Native_Physics_GetAbiVersion(), "The managed physics ABI must advertise its explicitly pinned v1 contract.");
			context.Expect(sizeof(NativePhysicsAbiHeader) == 8U && offsetof(NativePhysicsBodyCommandRequest, header) == 0U,
				"Every copied physics request must begin with the fixed-width ABI version/size header.");
			context.Expect(sizeof(NativePhysicsUuid) == 16U && sizeof(NativePhysicsWorldHandle) == 16U,
				"UUID and world identity packets must have stable pointer-free layouts.");
		});

		registry.Add("Engine.PhysicsManagedAbi.UnknownVersionHasNoSideEffects", [](TestContext& context) {
			NativePhysicsBodyCommandRequest request{};
			request.header = {999U, sizeof(request)};
			NativePhysicsCommandReceipt receipt{};
			receipt.header = {kNativePhysicsAbiVersion, sizeof(receipt)};
			context.Expect(Native_Physics_SubmitBodyCommand(&request, &receipt) == NativePhysicsStatus::VersionMismatch,
				"Unknown request versions must be rejected before a body command can enter the deferred queue.");
		});

		registry.Add("Engine.PhysicsManagedAbi.VariableCopiesRequireCallerBuffers", [](TestContext& context) {
			std::uint32_t count = 0U;
			std::uint64_t sourceTick = 0U;
			context.Expect(Native_Physics_CopyBodyStates(nullptr, 0U, &count) != NativePhysicsStatus::InvalidArgument,
				"Snapshot copies must support a zero-capacity size probe before callers allocate their own buffer.");
			context.Expect(Native_Physics_CopyEvents(nullptr, 0U, &count, &sourceTick) != NativePhysicsStatus::InvalidArgument,
				"Event packets must use the same caller-owned two-call copy contract.");
		});
	}
}
