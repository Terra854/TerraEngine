// PhysicsManagedAbiTests.cpp - C ABI contract tests for managed physics.

#include "../TestHarness.h"

#include "Scripting/ScriptGlue.h"
#include "Scripting/ExportVTable.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

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

		registry.Add("Engine.PhysicsManagedAbi.RejectedQueryInitializesCopiedReceipt", [](TestContext& context) {
			NativePhysicsQueryRequest request{};
			request.header = {kNativePhysicsAbiVersion, sizeof(request)};
			request.world = {999U, 1U, 0U};
			request.kind = NativePhysicsQueryKind::RayCast;
			request.hitMode = 0U;
			request.correlationId = 77U;
			NativePhysicsQueryReceipt receipt{};
			receipt.header = {kNativePhysicsAbiVersion, sizeof(receipt)};
			const NativePhysicsStatus status = Native_Physics_SubmitQuery(&request, &receipt);
			context.Expect(status == NativePhysicsStatus::StaleTarget && receipt.status == NativePhysicsStatus::StaleTarget,
				"A stale query must return a typed copied receipt rather than leaving managed output uninitialized.");
			context.Expect(receipt.header.abiVersion == kNativePhysicsAbiVersion && receipt.header.structSize == sizeof(receipt)
				&& receipt.correlationId == request.correlationId,
				"Rejected query receipts must preserve ABI metadata and caller correlation without retaining native state.");
		});

		registry.Add("Engine.PhysicsManagedAbi.NativeAotVtableKeepsPhysicsTailAppendOnly", [](TestContext& context) {
			context.Expect(kManagedVTablePhysicsVersion == 2U && kManagedVTableV1Size == 112U
				&& offsetof(ManagedVTable, tableSize) == kManagedVTableV1Size
				&& sizeof(ManagedVTable) == kManagedVTableV2Size,
				"The NativeAOT physics hooks must remain an append-only v2 tail after the v1 table.");

			std::ifstream managedMirror{std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Managed/Source/Export/ExportVTable.cs"};
			const std::string source{std::istreambuf_iterator<char>(managedMirror), std::istreambuf_iterator<char>()};
			context.Expect(source.find("Size = 136") != std::string::npos
				&& source.find("FieldOffset(120)") != std::string::npos
				&& source.find("GetPhysicsAbiVersion") != std::string::npos
				&& source.find("DispatchPhysicsEventBatch") != std::string::npos,
				"The managed NativeAOT mirror must expose the native physics ABI version and protected event-dispatch hooks at matching offsets.");
		});
	}
}
