// Test-first direct-query contracts for agent-independent navigation.
//
// These cases exercise copied endpoint inputs and validation. They deliberately
// do not attach a component or mutate an object, because direct route callers
// must remain usable alongside NavigationAgentComponent callers.
#include "../TestHarness.h"

#include "Navigation/NavigationTypes.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;

	namespace
	{
		NavigationLocation Location(double x, double y, double z)
		{
			NavigationLocation value{};
			value.world = { NavigationWorldId{ 59001U }, 1U };
			value.position.cell = { 0, 0, 0 };
			value.position.offsetMetres = { x, y, z };
			return value;
		}

		NavigationObjectEndpoint Endpoint(std::uint64_t objectId, std::uint64_t revision,
			NavigationLocation location)
		{
			NavigationObjectEndpoint endpoint{};
			const auto parsedId = ParseUUID(objectId == 1U
				? "8a5f72b749bc4b6ab9a8000000000001"
				: "8a5f72b749bc4b6ab9a8000000000002");
			endpoint.object = { parsedId.value_or(EMPTY_UUID), revision };
			endpoint.location = location;
			endpoint.transformRevision = revision;
			endpoint.activeInHierarchy = true;
			return endpoint;
		}
	}

	void RegisterNavigationDirectQueryTests(TestRegistry& registry)
	{
		registry.Add("Navigation.DirectQuery.AcceptsPointToPointWithoutAnAgent", [](TestContext& context) {
			const NavigationLocation start = Location(1.0, 0.0, 1.0);
			const NavigationLocation destination = Location(12.0, 0.0, 1.0);
			context.Expect(ValidateNavigationLocation(start).Succeeded(),
				"A finite point-to-point start must be accepted without an agent component.");
			context.Expect(ValidateNavigationLocation(destination).Succeeded(),
				"A finite point-to-point destination must be accepted without an agent component.");
			context.Expect(start.world == destination.world,
				"A direct query must use one explicit world generation for both endpoints.");
		});

		registry.Add("Navigation.DirectQuery.ValidatesObjectToLocationAndObjectToObjectCopies", [](TestContext& context) {
			const NavigationObjectEndpoint source = Endpoint(1U, 4U, Location(2.0, 0.0, 2.0));
			const NavigationObjectEndpoint target = Endpoint(2U, 9U, Location(8.0, 0.0, 8.0));
			context.Expect(ValidateNavigationObjectEndpoint(source).Succeeded(),
				"An active object endpoint with a transform revision is valid as a direct source.");
			context.Expect(ValidateNavigationObjectEndpoint(target).Succeeded(),
				"An active object endpoint with a transform revision is valid as a direct target.");

			NavigationRequestedDestination locationDestination{};
			locationDestination.kind = NavigationDestinationKind::Location;
			locationDestination.location = target.location;
			context.Expect(ValidateNavigationRequestedDestination(locationDestination).Succeeded(),
				"Object-to-location queries must copy and validate the requested location.");

			NavigationRequestedDestination objectDestination{};
			objectDestination.kind = NavigationDestinationKind::Object;
			objectDestination.object = target.object;
			context.Expect(ValidateNavigationRequestedDestination(objectDestination).Succeeded(),
				"Object-to-object queries must copy the target identity for protected capture.");
		});

		registry.Add("Navigation.DirectQuery.RejectsInvalidAndUnnormalizedEndpoints", [](TestContext& context) {
			NavigationLocation invalid = Location(0.0, 0.0, 0.0);
			invalid.world = {};
			context.Expect(ValidateNavigationLocation(invalid).code == NavigationLocationValidationCode::InvalidWorld,
				"A direct query with no world handle must return an explicit invalid-world outcome.");

			NavigationLocation nonFinite = Location(0.0, 0.0, 0.0);
			nonFinite.position.offsetMetres.x = std::numeric_limits<double>::quiet_NaN();
			context.Expect(ValidateNavigationLocation(nonFinite).code == NavigationLocationValidationCode::NonFinitePosition,
				"A non-finite endpoint must be rejected before provider search.");

			NavigationObjectEndpoint inactive = Endpoint(1U, 1U, Location(0.0, 0.0, 0.0));
			inactive.activeInHierarchy = false;
			context.Expect(ValidateNavigationObjectEndpoint(inactive).code == NavigationEndpointValidationCode::InactiveObject,
				"An inactive object target must produce an explicit outcome rather than a stale route.");
		});

		registry.Add("Navigation.DirectQuery.CapturesTargetPositionAtSubmission", [](TestContext& context) {
			const NavigationObjectEndpoint captured = Endpoint(2U, 17U, Location(20.0, 0.0, 20.0));
			NavigationRequestedDestination request{};
			request.kind = NavigationDestinationKind::Object;
			request.object = captured.object;
			context.Expect(ValidateNavigationRequestedDestination(request).Succeeded(),
				"An object target request must be admitted using copied identity data.");
			context.Expect(captured.transformRevision == 17U && captured.location == Location(20.0, 0.0, 20.0),
				"The protected capture must retain the target position and revision observed at submission.");
		});
	}
}
