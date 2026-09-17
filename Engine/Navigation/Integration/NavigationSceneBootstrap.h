// NavigationSceneBootstrap.h - copied scene-to-navigation capture boundary.
//
// This helper deliberately accepts value packets instead of ObjectManager,
// ResourceManager, renderer, or physics pointers. The caller captures those
// published values at its existing protected boundary, after which worker work
// can safely retain the returned immutable snapshots.

#pragma once

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/Geometry/TerrainNavigationSource.h"
#include "Navigation/Integration/NavigationProfileResolver.h"
#include "Navigation/NavigationWorld.h"

#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation {
	// The protected capture boundary has already applied physical eligibility
	// before constructing this packet. Bootstrap only consumes immutable values;
	// it never reads scene component markers or makes a renderer-visibility call.
	struct NavigationBootstrapSourceInput {
		NavigationEligibleSourceCandidate candidate{};
		std::optional<TerrainNavigationPublishedSamples> terrain;
	};

	struct NavigationSceneBootstrapInput {
		NavigationProfile defaultProfile = NavigationProfile::Defaults();
		std::vector<NavigationProfileRegistration> registeredProfiles;
		std::vector<NavigationBootstrapSourceInput> sources;
	};

	// One build input exists per resolved profile. It can be passed directly to
	// a scheduler-owned build queue without touching authored component storage.
	struct NavigationBootstrapBuildInput {
		NavigationProfile profile{};
		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
		std::vector<NavigationSourceIndexRecord> sourceIndex;
		std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex;
		// The terrain physical publication is copied with the index so a prepared
		// available world can be rejected if terrain changed before commit.
		NavigationGeneration terrainPublicationGeneration = 0U;
	};

	struct NavigationSceneBootstrapCapture {
		bool accepted = false;
		// A terrain-free capture is a known unavailable capability, not a mesh-only
		// fallback. Publication owners use this signal to replace stale coverage.
		bool terrainAvailable = false;
		std::string reason;
		std::vector<NavigationBootstrapBuildInput> builds;
		std::shared_ptr<const NavigationEligibleSourceIndex> eligibleSourceIndex;
	};

	// Builds one detached, deterministic source index from automatic terrain and
	// physical geometry capture. A missing terrain reports unavailable capability;
	// it never falls back to a terrain-free mesh/collider navigation world.
	[[nodiscard]] TERRAENGINE_API NavigationSceneBootstrapCapture CaptureNavigationSceneBootstrap(
		const NavigationSceneBootstrapInput& input);
}
