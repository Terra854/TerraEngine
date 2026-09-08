// NavigationSceneBootstrap.h - copied scene-to-navigation capture boundary.
//
// This helper deliberately accepts value packets instead of ObjectManager,
// ResourceManager, renderer, or physics pointers. The caller captures those
// published values at its existing protected boundary, after which worker work
// can safely retain the returned immutable snapshots.

#pragma once

#include "Components/Navigation/NavigationSourceComponent.h"
#include "Navigation/Geometry/MeshColliderNavigationSource.h"
#include "Navigation/Geometry/TerrainNavigationSource.h"
#include "Navigation/Integration/NavigationProfileResolver.h"
#include "Navigation/NavigationWorld.h"

#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation {
	struct NavigationBootstrapSourceInput {
		::NavigationSourceComponent component{};
		std::optional<TerrainNavigationPublishedSamples> terrain;
		std::optional<MeshColliderNavigationSourceInput> meshOrCollider;
		// ResourceManager returns a shared immutable companion after validating its
		// content. Reusing it avoids decoding or borrowing renderer/physics data
		// while this protected capture is preparing navigation work.
		std::shared_ptr<const NavigationGeometrySnapshot> immutableGeometry;
	};

	struct NavigationBootstrapAgentInput {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		::NavigationAgentComponent component{};
	};

	struct NavigationSceneBootstrapInput {
		NavigationProfile defaultProfile = NavigationProfile::Defaults();
		std::vector<NavigationProfileRegistration> registeredProfiles;
		std::vector<NavigationBootstrapSourceInput> sources;
		std::vector<NavigationBootstrapAgentInput> agents;
	};

	struct NavigationBootstrapAgentResolution {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		NavigationProfileResolution resolution{};
	};

	// One build input exists per resolved profile. It can be passed directly to
	// a scheduler-owned build queue without touching authored component storage.
	struct NavigationBootstrapBuildInput {
		NavigationProfile profile{};
		std::vector<std::shared_ptr<const NavigationGeometrySnapshot>> sources;
		std::vector<NavigationSourceIndexRecord> sourceIndex;
	};

	struct NavigationSceneBootstrapCapture {
		bool accepted = false;
		std::string reason;
		std::vector<NavigationBootstrapBuildInput> builds;
		std::vector<NavigationBootstrapAgentResolution> agents;
	};

	// Captures all eligible terrain and mesh/collider data atomically. A malformed
	// source rejects the complete geometry capture; an invalid agent profile is
	// returned as an explicit per-agent rejection and cannot enter a build.
	[[nodiscard]] TERRAENGINE_API NavigationSceneBootstrapCapture CaptureNavigationSceneBootstrap(
		const NavigationSceneBootstrapInput& input);
}
