// Test-first contracts for dynamic ground-route validity.
//
// These tests intentionally name the provider-neutral validity boundary from
// A route may remain immutable, but its usability must be re-observed
// against committed geometry, physical readiness, and navigation readiness
// before gameplay consumes movement.
#include "../TestHarness.h"

#include "Navigation/NavigationRouteValidity.h"

#include <cstdint>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;

	namespace
	{
		NavigationRouteValidity::RevisionChange Change(
			NavigationGeometryChangeOperation operation,
			std::uint64_t revision,
			NavigationBounds oldBounds,
			NavigationBounds newBounds,
			bool physicalReady,
			bool navigationReady)
		{
			NavigationRouteValidity::RevisionChange change{};
			change.operation = operation;
			change.sourceRevision = revision;
			change.oldAffectedBounds = oldBounds;
			change.newAffectedBounds = newBounds;
			change.physicalReady = physicalReady;
			change.navigationReady = navigationReady;
			return change;
		}

		NavigationRouteValidity::RouteDependency Dependency()
		{
			NavigationRouteValidity::RouteDependency dependency{};
			dependency.sourceId = 41U;
			dependency.sourceRevision = 7U;
			dependency.sweptBounds = {{-2.0F, -1.0F, -2.0F}, {8.0F, 3.0F, 2.0F}};
			return dependency;
		}
	}

	void RegisterNavigationRouteValidityTests(TestRegistry& registry)
	{
		registry.Add("Navigation.RouteValidity.PendingRequestsWaitForBothReadinessGenerations", [](TestContext& context) {
			NavigationRouteValidity validity;
			const auto pending = validity.ObservePendingRequest(
				Dependency(), Change(NavigationGeometryChangeOperation::Created, 8U,
					{{-1.0F, -1.0F, -1.0F}, {4.0F, 2.0F, 1.0F}},
					{{-1.0F, -1.0F, -1.0F}, {4.0F, 2.0F, 1.0F}}, false, false));
			context.Expect(pending.state == NavigationRouteValidityState::PendingPreparation,
				"A new source must remain pending until physical and navigation readiness are both published.");
			const auto physicalOnly = validity.ObservePendingRequest(
				Dependency(), Change(NavigationGeometryChangeOperation::Created, 8U,
					{{-1.0F, -1.0F, -1.0F}, {4.0F, 2.0F, 1.0F}},
					{{-1.0F, -1.0F, -1.0F}, {4.0F, 2.0F, 1.0F}}, true, false));
			context.Expect(physicalOnly.state == NavigationRouteValidityState::PendingPreparation,
				"Physical readiness alone must not publish a queryable navigation addition.");
			const auto ready = validity.ObservePendingRequest(
				Dependency(), Change(NavigationGeometryChangeOperation::Created, 8U,
					{{-1.0F, -1.0F, -1.0F}, {4.0F, 2.0F, 1.0F}},
					{{-1.0F, -1.0F, -1.0F}, {4.0F, 2.0F, 1.0F}}, true, true));
			context.Expect(ready.state == NavigationRouteValidityState::Valid,
				"A source becomes usable only after both readiness generations agree.");
		});

		registry.Add("Navigation.RouteValidity.RemovalAndOverlapInvalidateBeforeMovement", [](TestContext& context) {
			NavigationRouteValidity validity;
			const auto route = validity.RetainRoute(Dependency(), 12U);
			const auto removed = validity.ObserveRoute(route, Change(NavigationGeometryChangeOperation::Deleted, 13U,
				{{-2.0F, -1.0F, -2.0F}, {8.0F, 3.0F, 2.0F}},
				{{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}}, true, false));
			context.Expect(removed.state == NavigationRouteValidityState::Invalidated
				&& removed.reason == NavigationRouteValidityReason::LostSupport,
				"Removed support must invalidate a retained route before gameplay movement consumes it.");
			const auto blocked = validity.ObserveRoute(route, Change(NavigationGeometryChangeOperation::Reshaped, 14U,
				{{-2.0F, -1.0F, -2.0F}, {8.0F, 3.0F, 2.0F}},
				{{-2.0F, -1.0F, -2.0F}, {8.0F, 3.0F, 2.0F}}, true, true));
			context.Expect(blocked.state == NavigationRouteValidityState::Invalidated
				&& blocked.reason == NavigationRouteValidityReason::Overlap,
				"A newly overlapping obstruction must invalidate before route-derived movement.");
		});

		registry.Add("Navigation.RouteValidity.UnrelatedChangesPreserveRetainedRoute", [](TestContext& context) {
			NavigationRouteValidity validity;
			const auto route = validity.RetainRoute(Dependency(), 21U);
			const auto observed = validity.ObserveRoute(route, Change(NavigationGeometryChangeOperation::Moved, 22U,
				{{100.0F, 0.0F, 100.0F}, {104.0F, 2.0F, 104.0F}},
				{{108.0F, 0.0F, 100.0F}, {112.0F, 2.0F, 104.0F}}, true, true));
			context.Expect(observed.state == NavigationRouteValidityState::Valid,
				"Changes outside the retained swept corridor must not invalidate its immutable route.");
		});

		registry.Add("Navigation.RouteValidity.TerrainLossMakesRetainedRoutesUnavailableBeforeStale", [](TestContext& context) {
			const NavigationObservationEvidence captured{7U, 3U, 4U, 19U, 31U, 23U};
			const NavigationObservationEvidence terrainLost{8U, 3U, 4U, 0U, 32U, 24U};
			const NavigationRouteObservationValidation observed =
				ValidateNavigationRouteObservation(captured, terrainLost);
			context.Expect(!observed.usable && observed.outcome == NavigationOutcome::UnavailableCoverage
				&& observed.reason == NavigationReason::MissingTerrain,
				"Terrain disappearance must invalidate retained routes as unavailable before a mixed or stale route can be consumed.");
		});

		registry.Add("Navigation.RouteValidity.RejectsAnyChangedPublishedWorldEvidence", [](TestContext& context) {
			const NavigationObservationEvidence captured{7U, 3U, 4U, 19U, 31U, 23U};
			const NavigationObservationEvidence changedStreaming{8U, 3U, 4U, 19U, 32U, 24U};
			const NavigationRouteObservationValidation observed =
				ValidateNavigationRouteObservation(captured, changedStreaming);
			context.Expect(!observed.usable && observed.outcome == NavigationOutcome::Stale
				&& observed.reason == NavigationReason::DirtyCoverage,
				"Streaming, source, terrain, resource, or profile publication must reject a route prepared against the older complete tuple.");
		});
	}
}
