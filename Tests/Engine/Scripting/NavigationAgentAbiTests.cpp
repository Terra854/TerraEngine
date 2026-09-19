// NavigationAgentAbiTests.cpp - frozen native agent ABI v1 contract.
//
// The agent bridge is additive: these checks intentionally keep the existing
// direct-navigation ABI v2 assertions in their own test file.

#include "../TestHarness.h"

#include "Scripting/ScriptGlue.h"
#include "Navigation/Agents/NavigationAgentTypes.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace TerraEngine::Tests {
	void RegisterNavigationAgentAbiTests(TestRegistry& registry) {
		registry.Add("Navigation.AgentAbi.V1RecordsRetainPublishedStridesAndAlignment", [](TestContext& context) {
			context.ExpectEqual(1U, Native_NavigationAgent_GetAbiVersion(),
				"The optional agent bridge must advertise its independent ABI v1.");
			context.ExpectEqual(static_cast<std::size_t>(16U), sizeof(NativeNavigationAgentResolveRequest), "Resolve request stride is frozen.");
			context.ExpectEqual(static_cast<std::size_t>(80U), sizeof(NativeNavigationAgentIdentitySnapshot), "Identity snapshot stride is frozen.");
			context.ExpectEqual(static_cast<std::size_t>(176U), sizeof(NativeNavigationAgentCommand), "Command stride is frozen.");
			context.ExpectEqual(static_cast<std::size_t>(104U), sizeof(NativeNavigationAgentCommandReceipt), "Command receipt stride is frozen.");
			context.ExpectEqual(static_cast<std::size_t>(72U), sizeof(NativeNavigationAgentStatusQuery), "Status query stride is frozen.");
			context.ExpectEqual(static_cast<std::size_t>(48U), sizeof(NativeNavigationAgentStatusSnapshot), "Status snapshot stride is frozen.");
			context.ExpectEqual(static_cast<std::size_t>(72U), sizeof(NativeNavigationAgentGuidanceQuery), "Guidance query stride is frozen.");
			context.ExpectEqual(static_cast<std::size_t>(304U), sizeof(NativeNavigationAgentGuidanceSnapshot), "Guidance snapshot stride is frozen.");
			context.ExpectEqual(static_cast<std::size_t>(80U), sizeof(NativeNavigationAgentReceiptQuery), "Receipt query stride is frozen.");
			context.Expect(alignof(NativeNavigationAgentCommand) == 8U
				&& offsetof(NativeNavigationAgentResolveRequest, header) == 0U
				&& offsetof(NativeNavigationAgentCommand, header) == 0U
				&& offsetof(NativeNavigationAgentGuidanceSnapshot, header) == 0U,
				"Agent records must use the stable eight-byte native alignment and leading ABI header.");
			context.Expect(offsetof(NativeNavigationAgentCommand, location) == 80U
				&& offsetof(NativeNavigationAgentCommand, correlationId) == 168U
				&& offsetof(NativeNavigationAgentGuidanceSnapshot, nextWaypoint) == 104U
				&& offsetof(NativeNavigationAgentGuidanceSnapshot, destinationObject) == 280U,
				"Native member boundaries must agree with the independently executed CLR offset probe.");
			context.Expect(static_cast<std::uint32_t>(NativeNavigationAgentCommandKind::SetLocationDestination) == 0U
				&& static_cast<std::uint32_t>(NativeNavigationAgentCommandKind::SetObjectDestination) == 1U
				&& static_cast<std::uint32_t>(NativeNavigationAgentCommandKind::Stop) == 2U
				&& static_cast<std::uint32_t>(Navigation::NavigationAgentState::Following) == 2U
				&& static_cast<std::uint32_t>(Navigation::NavigationAgentState::Stopped) == 4U
				&& static_cast<std::uint32_t>(Navigation::NavigationAgentReceiptState::Rejected) == 2U,
				"ABI v1 enum values must retain their fixed managed/native meanings.");
		});

		registry.Add("Navigation.AgentAbi.InvalidHeadersAndRecordsHaveNoPartialEffect", [](TestContext& context) {
			NativeNavigationAgentResolveRequest resolve{};
			NativeNavigationAgentIdentitySnapshot identity{};
			resolve.header = {1U + 1U, sizeof(resolve)};
			identity.header = {1U, sizeof(identity)};
			const NativeNavigationStatus status = Native_NavigationAgent_ResolveIdentity(&resolve, &identity);
			context.Expect(status == NativeNavigationStatus::VersionMismatch
				&& identity.header.abiVersion == 1U && identity.header.structSize == sizeof(identity),
				"Unsupported agent ABI versions must reject before returning or mutating an identity.");

			NativeNavigationAgentCommand command{};
			NativeNavigationAgentCommandReceipt receipt{};
			command.header = {1U, sizeof(command) - 1U};
			receipt.header = {1U, sizeof(receipt)};
			const NativeNavigationStatus submit = Native_NavigationAgent_SubmitCommand(&command, &receipt);
			context.Expect(submit == NativeNavigationStatus::InvalidArgument
				|| submit == NativeNavigationStatus::VersionMismatch,
				"Undersized copied commands must be rejected without admitting runtime intent.");

			resolve.header = {1U, sizeof(resolve)};
			resolve.reserved = 1U;
			identity = {};
			identity.header = {1U, sizeof(identity)};
			context.Expect(Native_NavigationAgent_ResolveIdentity(&resolve, &identity)
				== NativeNavigationStatus::InvalidArgument,
				"Reserved resolve fields must be rejected rather than interpreted as future flags.");
			command.header = {1U, sizeof(command)};
			command.kind = static_cast<NativeNavigationAgentCommandKind>(99U);
			receipt = {};
			receipt.header = {1U, sizeof(receipt)};
			context.Expect(Native_NavigationAgent_SubmitCommand(&command, &receipt)
				== NativeNavigationStatus::InvalidArgument,
				"Unknown command kinds must fail closed at the native ABI boundary.");
			command.kind = NativeNavigationAgentCommandKind::SetLocationDestination;
			command.location.offsetX = std::numeric_limits<double>::quiet_NaN();
			receipt = {};
			receipt.header = {1U, sizeof(receipt)};
			context.Expect(Native_NavigationAgent_SubmitCommand(&command, &receipt)
				== NativeNavigationStatus::InvalidArgument,
				"Non-finite native destination coordinates must be rejected without queueing intent.");
		});

		registry.Add("Navigation.AgentAbi.DirectNavigationV2RemainsIndependent", [](TestContext& context) {
			context.ExpectEqual(2U, Native_Navigation_GetAbiVersion(),
				"Adding the optional agent bridge must not revise direct navigation ABI v2.");
		});
	}
}
