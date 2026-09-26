// LocomotionNavigationCompatibility.h - copied capsule/profile safety contract.

#pragma once

#include "LocomotionTypes.h"
#include "Navigation/NavigationSettings.h"
#include "Physics/PhysicsTypes.h"

#include <cstdint>

namespace TerraEngine::Locomotion {
	struct TERRAENGINE_API NavigationCompatibilityInput {
		bool controllerEnabled = true;
		bool physicsEnabled = true;
		bool ownerColliderSetEnabled = false;
		Physics::PhysicsShapeKind shape = Physics::PhysicsShapeKind::Capsule;
		Physics::PhysicsVector3 worldScale{1.0F, 1.0F, 1.0F};
		Physics::PhysicsVector3 upDirection{0.0F, 1.0F, 0.0F};
		Physics::PhysicsVector3 localCenter{0.0F, 1.0F, 0.0F};
		double radiusMetres = 0.5;
		double totalHeightMetres = 2.0;
		double contactPaddingMetres = 0.02;
		double stepHeightMetres = 0.4;
		double maxSlopeDegrees = 50.0;
		Navigation::NavigationProfile profile = Navigation::NavigationProfile::Defaults();
	};

	struct TERRAENGINE_API NavigationCompatibilityResult {
		bool compatible = false;
		LocomotionReason reason = LocomotionReason::UnsupportedController;
		NavigationCompatibilityField field = NavigationCompatibilityField::None;
		double controllerValue = 0.0;
		double profileValue = 0.0;
	};

	// Pure validation on copied values. Physics currently creates virtual
	// characters with authored (unscaled) geometry, so non-unit effective world
	// scale fails closed rather than claiming an unsupported conversion.
	[[nodiscard]] TERRAENGINE_API NavigationCompatibilityResult CheckNavigationCompatibility(
		const NavigationCompatibilityInput& input) noexcept;
}
