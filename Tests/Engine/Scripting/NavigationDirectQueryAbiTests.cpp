// NavigationDirectQueryAbiTests.cpp - regression coverage for frozen direct-route ABI v2.
//
// Agent navigation adds a separate interface. These tests keep the original
// object-route packets callable without attaching an agent or changing their
// fixed binary contract.

#include "../TestHarness.h"

#include "Scripting/ScriptGlue.h"

#include <cstddef>

namespace TerraEngine::Tests {
	void RegisterNavigationDirectQueryAbiTests(TestRegistry& registry) {
		registry.Add("Navigation.DirectQueryAbi.V2RecordsRejectMismatchedHeadersWithoutMutation", [](TestContext& context) {
			NativeNavigationObjectToLocationRequest request{};
			request.header = {kNativeNavigationAbiVersion + 1U, sizeof(request)};
			NativeNavigationSubmitReceipt receipt{};
			receipt.header = {kNativeNavigationAbiVersion, sizeof(receipt)};

			const NativeNavigationStatus status = Native_Navigation_RequestObjectToLocation(&request, &receipt);
			context.Expect(status == NativeNavigationStatus::VersionMismatch
				&& receipt.status == NativeNavigationStatus::VersionMismatch,
				"A malformed direct-query record must fail at the ABI boundary before it can create or replace a route request.");
			context.Expect(receipt.request.owner.world.worldId == 0U
				&& receipt.request.owner.world.worldGeneration == 0U
				&& receipt.request.owner.ownerId == 0U
				&& receipt.request.slotGeneration == 0U
				&& receipt.correlationId == 0U,
				"Rejected direct-query packets must not return a live request handle or mutate a caller intent.");
		});

		registry.Add("Navigation.DirectQueryAbi.V2DirectPacketsRemainIndependentOfAgentIdentity", [](TestContext& context) {
			context.ExpectEqual(kNativeNavigationAbiVersion, Native_Navigation_GetAbiVersion(),
				"Adding optional agents must not revise the existing direct navigation ABI version.");
			context.ExpectEqual(static_cast<std::size_t>(416U), sizeof(NativeNavigationObjectToLocationRequest),
				"Object-to-location requests must retain the published v2 copied stride.");
			context.ExpectEqual(static_cast<std::size_t>(360U), sizeof(NativeNavigationObjectToObjectRequest),
				"Object-to-object requests must retain the published v2 copied stride.");
			context.Expect(offsetof(NativeNavigationObjectToLocationRequest, sourceObjectEntity)
				< offsetof(NativeNavigationObjectToLocationRequest, destination)
				&& offsetof(NativeNavigationObjectToObjectRequest, sourceObjectEntity)
				< offsetof(NativeNavigationObjectToObjectRequest, targetObjectEntity),
				"Direct request packets must continue to identify ordinary source and target objects without an agent handle field.");
		});
	}
}
