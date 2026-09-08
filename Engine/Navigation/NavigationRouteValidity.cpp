// NavigationRouteValidity.cpp - bounded local dependency checks.
#include "Navigation/NavigationRouteValidity.h"

#include <algorithm>

namespace TerraEngine::Navigation
{
	bool NavigationBounds::Intersects(const NavigationBounds& other) const noexcept
	{
		return minimum.x <= other.maximum.x && maximum.x >= other.minimum.x
			&& minimum.y <= other.maximum.y && maximum.y >= other.minimum.y
			&& minimum.z <= other.maximum.z && maximum.z >= other.minimum.z;
	}

	bool NavigationBounds::IsEmpty() const noexcept
	{
		return minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z;
	}

	NavigationRouteValidity::RetainedRoute NavigationRouteValidity::RetainRoute(
		RouteDependency dependency, std::uint64_t profileRevision) const noexcept
	{
		dependency.profileRevision = profileRevision;
		return {dependency, dependency.sourceRevision};
	}

	NavigationRouteValidityObservation NavigationRouteValidity::ObservePendingRequest(
		const RouteDependency&, const RevisionChange& change) const noexcept
	{
		if(!change.physicalReady || !change.navigationReady)
			return {NavigationRouteValidityState::PendingPreparation, NavigationRouteValidityReason::NotReady, change.sourceRevision};
		return {NavigationRouteValidityState::Valid, NavigationRouteValidityReason::None, change.sourceRevision};
	}

	NavigationRouteValidityObservation NavigationRouteValidity::ObserveRoute(
		const RetainedRoute& route, const RevisionChange& change, std::uint64_t currentProfileRevision) const noexcept
	{
		if(currentProfileRevision != 0U && route.dependency.profileRevision != 0U
			&& currentProfileRevision != route.dependency.profileRevision)
			return {NavigationRouteValidityState::StaleProfile, NavigationRouteValidityReason::ProfileRevision, change.sourceRevision};

		const bool oldAffected = !change.oldAffectedBounds.IsEmpty()
			&& route.dependency.sweptBounds.Intersects(change.oldAffectedBounds);
		const bool newAffected = !change.newAffectedBounds.IsEmpty()
			&& route.dependency.sweptBounds.Intersects(change.newAffectedBounds);
		if(!oldAffected && !newAffected) return {NavigationRouteValidityState::Valid, NavigationRouteValidityReason::None, change.sourceRevision};

		if(change.operation == NavigationGeometryChangeOperation::Deleted
			|| change.operation == NavigationGeometryChangeOperation::Disabled
			|| !change.physicalReady)
			return {NavigationRouteValidityState::Invalidated, NavigationRouteValidityReason::LostSupport, change.sourceRevision};
		if(newAffected || oldAffected)
			return {NavigationRouteValidityState::Invalidated, NavigationRouteValidityReason::Overlap, change.sourceRevision};
		return {NavigationRouteValidityState::Valid, NavigationRouteValidityReason::None, change.sourceRevision};
	}

	bool NavigationRetainedRouteIndex::Retain(NavigationRetainedRouteDependency dependency)
	{
		if(!dependency.route.IsValid() || dependency.intentGeneration == 0U
			|| dependency.profileRevision == 0U || dependency.sources.empty()) return false;
		for(const NavigationRouteSourceDependency& source : dependency.sources) {
			if(source.source.value == TerraEngine::EMPTY_UUID || source.sourceRevision == 0U
				|| source.sweptBounds.IsEmpty()) return false;
		}
		std::sort(dependency.sources.begin(), dependency.sources.end(),
			[](const NavigationRouteSourceDependency& left, const NavigationRouteSourceDependency& right) {
				return left.source.value < right.source.value;
			});
		const auto duplicate = std::adjacent_find(dependency.sources.begin(), dependency.sources.end(),
			[](const NavigationRouteSourceDependency& left, const NavigationRouteSourceDependency& right) {
				return left.source == right.source;
			});
		if(duplicate != dependency.sources.end()) return false;

		const auto existing = std::find_if(entries_.begin(), entries_.end(), [&dependency](const Entry& entry) {
			return entry.dependency.route == dependency.route;
		});
		if(existing == entries_.end()) entries_.push_back({std::move(dependency), {}});
		else *existing = {std::move(dependency), {}};
		return true;
	}

	bool NavigationRetainedRouteIndex::Forget(NavigationRouteHandle route) noexcept
	{
		const auto removed = std::erase_if(entries_, [route](const Entry& entry) {
			return entry.dependency.route == route;
		});
		return removed != 0U;
	}

	std::vector<NavigationRouteReplanObservation> NavigationRetainedRouteIndex::ObserveSourceChange(
		const NavigationRouteSourceChange& change)
	{
		std::vector<NavigationRouteReplanObservation> observations;
		if(change.source.value == TerraEngine::EMPTY_UUID || change.sourceRevision == 0U) return observations;
		for(Entry& entry : entries_) {
			if(entry.invalidation.replanRequired) continue;
			const auto dependency = std::find_if(entry.dependency.sources.begin(), entry.dependency.sources.end(),
				[&change](const NavigationRouteSourceDependency& current) { return current.source == change.source; });
			if(dependency == entry.dependency.sources.end()) continue;
			const bool oldAffected = !change.oldAffectedBounds.IsEmpty()
				&& dependency->sweptBounds.Intersects(change.oldAffectedBounds);
			const bool newAffected = !change.newAffectedBounds.IsEmpty()
				&& dependency->sweptBounds.Intersects(change.newAffectedBounds);
			if(!oldAffected && !newAffected) continue;

			const NavigationReason reason = change.removesSupport || !change.physicalReady
				? NavigationReason::MissingSupport : NavigationReason::DirtyCoverage;
			entry.invalidation = {entry.dependency.route, entry.dependency.route.owner, reason,
				NavigationOutcome::Stale, change.sourceRevision, true};
			observations.push_back(entry.invalidation);
		}
		return observations;
	}

	NavigationRouteReplanObservation NavigationRetainedRouteIndex::ValidateForConsumption(
		NavigationRouteHandle route, const NavigationBounds& currentPoseBounds,
		NavigationGeneration currentProfileRevision) const noexcept
	{
		const auto found = std::find_if(entries_.begin(), entries_.end(), [route](const Entry& entry) {
			return entry.dependency.route == route;
		});
		if(found == entries_.end()) {
			return {route, route.owner, NavigationReason::SceneReplaced, NavigationOutcome::Stale, 0U, true};
		}
		if(found->invalidation.replanRequired) return found->invalidation;
		if(currentProfileRevision != 0U && currentProfileRevision != found->dependency.profileRevision) {
			return {route, route.owner, NavigationReason::DirtyCoverage, NavigationOutcome::Stale,
				currentProfileRevision, true};
		}
		const bool attached = std::any_of(found->dependency.sources.begin(), found->dependency.sources.end(),
			[&currentPoseBounds](const NavigationRouteSourceDependency& source) {
				return source.sweptBounds.Intersects(currentPoseBounds);
			});
		if(!attached) {
			return {route, route.owner, NavigationReason::MissingSupport, NavigationOutcome::Stale, 0U, true};
		}
		return {route, route.owner, NavigationReason::None, NavigationOutcome::Complete, 0U, false};
	}
}
