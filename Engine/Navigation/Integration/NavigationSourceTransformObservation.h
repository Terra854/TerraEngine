// NavigationSourceTransformObservation.h - protected source placement observation.
#pragma once

#include "Core/Object/ObjectManager.h"
#include "Navigation/NavigationWorld.h"

#include <optional>
#include <span>

namespace TerraEngine::Navigation {
	// A protected-boundary diagnostic copy of the first source whose retained
	// placement evidence no longer agrees with ObjectManager. This intentionally
	// carries values, rather than an Object or Transform pointer, so callers can
	// log an actionable mismatch without extending live ECS access past the check.
	struct NavigationSourceTransformMismatch {
		UUID sourceId = EMPTY_UUID;
		UUID objectId = EMPTY_UUID;
		NavigationSourceKind sourceKind = NavigationSourceKind::Auto;
		NavigationGeneration retainedLifetimeGeneration = 0U;
		NavigationGeneration liveLifetimeGeneration = 0U;
		NavigationGeneration retainedTransformRevision = 0U;
		NavigationGeneration liveTransformRevision = 0U;
	};

	// Call only at a protected scene boundary. Compare retained source identities
	// with live canonical revisions; workers never inspect ObjectManager. Ordinary
	// agent/target movement is intentionally ignored unless that object also owns
	// published navigation geometry, avoiding a rebuild for every gameplay step.
	[[nodiscard]] inline std::optional<NavigationSourceTransformMismatch> FindNavigationSourceTransformMismatch(
		ObjectManager& objects, std::span<const NavigationSourceIndexRecord> sources) {
		for(const NavigationSourceIndexRecord& source : sources) {
			if(!source.enabled || source.identity.objectId == EMPTY_UUID) continue;
			const auto object = objects.getObject(source.identity.objectId);
			const NavigationGeneration liveLifetime = object
				? objects.getObjectLifetimeGeneration(*object) : 0U;
			const NavigationGeneration liveTransform = object
				? objects.GetObjectTransformRevision(*object) : 0U;
			if(!object || liveLifetime != source.identity.objectLifetimeGeneration
				|| liveTransform != source.revisionEvidence.transformRevision) {
				return NavigationSourceTransformMismatch{
					source.sourceId,
					source.identity.objectId,
					source.identity.kind,
					source.identity.objectLifetimeGeneration,
					liveLifetime,
					source.revisionEvidence.transformRevision,
					liveTransform};
			}
		}
		return std::nullopt;
	}

	// Keep the compact predicate for ordinary callers; diagnostics can opt into
	// the value-returning helper above without duplicating ECS observation logic.
	[[nodiscard]] inline bool NavigationSourceTransformsChanged(
		ObjectManager& objects, std::span<const NavigationSourceIndexRecord> sources) {
		return FindNavigationSourceTransformMismatch(objects, sources).has_value();
	}
}
