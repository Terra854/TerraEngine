// NavigationRuntimeSceneCapture.h - protected live-scene navigation capture.
//
// This is the only navigation integration boundary allowed to inspect the live
// ObjectManager and ResourceManager. Call it at a protected flush/publication
// edge, then pass only its returned immutable result to navigation workers.

#pragma once

#include "Navigation/Integration/NavigationSceneBootstrap.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class ObjectManager;
class ResourceManager;

namespace TerraEngine::Navigation {
	class NavigationSystem;
	struct NavigationRuntimeSceneCaptureSettings {
		NavigationProfile defaultProfile = NavigationProfile::Defaults();
		std::vector<NavigationProfileRegistration> registeredProfiles;
		// Typed producers publish these complete immutable values at their own
		// protected boundary. Runtime capture copies them alongside scene geometry
		// and never accepts an untyped renderer or script pointer.
		std::vector<NavigationEligibleSourceCandidate> proceduralSources;
	};

	struct NavigationRuntimeSceneCapture {
		bool accepted = false;
		std::uint64_t sourceCaptureRevision = 0U;
		std::string reason;
		NavigationSceneBootstrapCapture bootstrap{};
	};

	struct NavigationRuntimeSceneSubmission {
		bool accepted = false;
		std::size_t queuedProfileBuilds = 0U;
		std::string reason;
	};

	// Captures active terrain, collider, mesh-companion, and typed procedural
	// physical values. It never queues worker work itself, so a caller can
	// revalidate the captured component revision before admitting the build.
	[[nodiscard]] TERRAENGINE_API NavigationRuntimeSceneCapture CaptureNavigationRuntimeScene(
		ObjectManager& objects, ResourceManager& resources,
		const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& terrainCollision,
		const NavigationRuntimeSceneCaptureSettings& settings = {});
	// Invoke only after the caller has compared sourceCaptureRevision with its last
	// admitted capture revision. The NavigationSystem copies every snapshot again
	// before returning, so no live ECS/resource state reaches worker execution.
	[[nodiscard]] TERRAENGINE_API NavigationRuntimeSceneSubmission SubmitNavigationRuntimeSceneCapture(
		NavigationSystem& navigation, const NavigationRuntimeSceneCapture& capture);
}
