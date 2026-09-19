// NavigationRouteValidity.h - provider-neutral dynamic route observation.
#pragma once

#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationTypes.h"
#include "Navigation/NavigationSettings.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Navigation
{
	// The route store retains this value beside every completed object request.
	// It is intentionally independent of provider geometry so gameplay can
	// reject an obsolete route before using one of its copied waypoints.
	struct NavigationRouteObservationValidation
	{
		bool usable = false;
		NavigationOutcome outcome = NavigationOutcome::Stale;
		NavigationReason reason = NavigationReason::DirtyCoverage;
	};

	[[nodiscard]] TERRAENGINE_API NavigationRouteObservationValidation
		ValidateNavigationRouteObservation(const NavigationObservationEvidence& captured,
			const NavigationObservationEvidence& current) noexcept;

	// Dynamic publication certificates use this narrow geometric test for both
	// retained and prepared routes. It localizes each copied waypoint in the
	// source frame and tests its travelled segments against one profile-expanded
	// affected bound. Unknown coordinates or malformed inputs return true so a
	// caller can fail closed rather than retain potentially unsafe guidance.
	[[nodiscard]] TERRAENGINE_API bool RouteSegmentsIntersectLocalizedBounds(
		std::span<const NavigationLocation> points, const NavigationProfile& profile,
		const Scene::WorldPosition& origin, const NavigationGeometryBounds& affected) noexcept;

	struct NavigationBounds
	{
		NavigationGeometryVector3 minimum{};
		NavigationGeometryVector3 maximum{};
		[[nodiscard]] bool Intersects(const NavigationBounds& other) const noexcept;
		[[nodiscard]] bool IsEmpty() const noexcept;
	};

	enum class NavigationGeometryChangeOperation : std::uint8_t
	{
		Created,
		Enabled,
		Disabled,
		Moved,
		Rotated,
		Scaled,
		Deleted,
		Replaced,
		Reshaped
	};

	enum class NavigationRouteValidityState : std::uint8_t
	{
		Valid,
		PendingPreparation,
		Invalidated,
		StaleProfile
	};

	enum class NavigationRouteValidityReason : std::uint8_t
	{
		None,
		LostSupport,
		Overlap,
		ProfileRevision,
		SourceRevision,
		NotReady
	};

	struct NavigationRouteValidityObservation
	{
		NavigationRouteValidityState state = NavigationRouteValidityState::Invalidated;
		NavigationRouteValidityReason reason = NavigationRouteValidityReason::SourceRevision;
		std::uint64_t observedRevision = 0U;
	};

	// This class retains only copied route dependency certificates. It never
	// points at scene objects or provider data, so observing validity is safe at
	// gameplay/physics boundaries while workers continue using old generations.
	class NavigationRouteValidity final
	{
	public:
		struct RevisionChange
		{
			NavigationGeometryChangeOperation operation = NavigationGeometryChangeOperation::Reshaped;
			std::uint64_t sourceRevision = 0U;
			NavigationBounds oldAffectedBounds{};
			NavigationBounds newAffectedBounds{};
			bool physicalReady = false;
			bool navigationReady = false;
		};

		struct RouteDependency
		{
			std::uint64_t sourceId = 0U;
			std::uint64_t sourceRevision = 0U;
			std::uint64_t profileRevision = 0U;
			NavigationBounds sweptBounds{};
		};

		struct RetainedRoute
		{
			RouteDependency dependency{};
			std::uint64_t retainedRevision = 0U;
		};

		[[nodiscard]] RetainedRoute RetainRoute(RouteDependency dependency, std::uint64_t profileRevision) const noexcept;
		[[nodiscard]] NavigationRouteValidityObservation ObserveRoute(
			const RetainedRoute& route, const RevisionChange& change,
			std::uint64_t currentProfileRevision = 0U) const noexcept;
		[[nodiscard]] NavigationRouteValidityObservation ObservePendingRequest(
			const RouteDependency& dependency, const RevisionChange& change) const noexcept;
	};

	// A route can depend on several terrain, mesh, and collider snapshots. This
	// registry owns copied dependency certificates and returns replan-required
	// observations; it intentionally never submits a replacement route itself.
	struct NavigationRouteSourceDependency
	{
		NavigationUuid source{};
		NavigationGeneration sourceRevision = 0U;
		NavigationBounds sweptBounds{};
	};

	struct NavigationRetainedRouteDependency
	{
		NavigationRouteHandle route{};
		NavigationGeneration intentGeneration = 0U;
		NavigationGeneration profileRevision = 0U;
		std::vector<NavigationRouteSourceDependency> sources;
	};

	struct NavigationRouteSourceChange
	{
		NavigationUuid source{};
		NavigationGeneration sourceRevision = 0U;
		NavigationBounds oldAffectedBounds{};
		NavigationBounds newAffectedBounds{};
		bool removesSupport = false;
		bool physicalReady = false;
		bool navigationReady = false;
	};

	struct NavigationRouteReplanObservation
	{
		NavigationRouteHandle route{};
		NavigationOwnerHandle owner{};
		NavigationReason reason = NavigationReason::None;
		NavigationOutcome outcome = NavigationOutcome::Stale;
		NavigationGeneration sourceRevision = 0U;
		bool replanRequired = false;
	};

	class NavigationRetainedRouteIndex final
	{
	public:
		// Retention replaces a prior certificate for the same generation-qualified
		// route. Invalid certificates are rejected before they enter observations.
		[[nodiscard]] bool Retain(NavigationRetainedRouteDependency dependency);
		[[nodiscard]] bool Forget(NavigationRouteHandle route) noexcept;
		[[nodiscard]] std::vector<NavigationRouteReplanObservation> ObserveSourceChange(
			const NavigationRouteSourceChange& change);
		[[nodiscard]] NavigationRouteReplanObservation ValidateForConsumption(
			NavigationRouteHandle route, const NavigationBounds& currentPoseBounds,
			NavigationGeneration currentProfileRevision = 0U) const noexcept;

	private:
		struct Entry
		{
			NavigationRetainedRouteDependency dependency{};
			NavigationRouteReplanObservation invalidation{};
		};
		std::vector<Entry> entries_;
	};
}
