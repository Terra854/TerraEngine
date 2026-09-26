// LocomotionNavigationCompatibility.cpp - conservative copied ground capsule checks.

#include "LocomotionNavigationCompatibility.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Locomotion {
	namespace {
		// A distance, angle and unit axis have different dimensions even though
		// all three contracts currently allow the same numerical float roundoff.
		constexpr double kLinearToleranceMetres = 0.001;
		constexpr double kSlopeToleranceDegrees = 0.001;
		constexpr double kUnitVectorTolerance = 0.001;
		constexpr double kScaleTolerance = 0.001;

		NavigationCompatibilityResult Mismatch(NavigationCompatibilityField field,
			double controllerValue = 0.0, double profileValue = 0.0,
			LocomotionReason reason = LocomotionReason::UnsupportedController) noexcept {
			return {false, reason, field, controllerValue, profileValue};
		}
	}

	NavigationCompatibilityResult CheckNavigationCompatibility(
		const NavigationCompatibilityInput& input) noexcept {
		if(!input.controllerEnabled) return Mismatch(NavigationCompatibilityField::ControllerEnabled);
		if(!input.physicsEnabled) return Mismatch(NavigationCompatibilityField::PhysicsEnabled);
		if(!input.profile.Validate().accepted)
			return Mismatch(NavigationCompatibilityField::InvalidProfile, 0.0, 0.0,
				LocomotionReason::InvalidArgument);
		if(input.shape != Physics::PhysicsShapeKind::Capsule)
			return Mismatch(NavigationCompatibilityField::Shape,
				static_cast<double>(static_cast<unsigned>(input.shape)),
				static_cast<double>(static_cast<unsigned>(Physics::PhysicsShapeKind::Capsule)));
		// The current character provider creates the shape from unscaled authored
		// dimensions. A different effective scale cannot be safely compared to a
		// route capsule until a supported provider conversion exists.
		const auto& scale = input.worldScale;
		if(!scale.IsFinite() || std::abs(scale.x - 1.0F) > kScaleTolerance
			|| std::abs(scale.y - 1.0F) > kScaleTolerance
			|| std::abs(scale.z - 1.0F) > kScaleTolerance)
			return Mismatch(NavigationCompatibilityField::WorldScale,
				std::max({std::abs(static_cast<double>(scale.x) - 1.0),
					std::abs(static_cast<double>(scale.y) - 1.0),
					std::abs(static_cast<double>(scale.z) - 1.0)}), 0.0);
		const auto& up = input.upDirection;
		if(!up.IsFinite() || std::abs(up.x) > kUnitVectorTolerance
			|| std::abs(up.y - 1.0F) > kUnitVectorTolerance
			|| std::abs(up.z) > kUnitVectorTolerance)
			return Mismatch(NavigationCompatibilityField::UpDirection, up.y, 1.0);
		if(!std::isfinite(input.radiusMetres) || input.radiusMetres <= 0.0)
			return Mismatch(NavigationCompatibilityField::Radius,
				input.radiusMetres, input.profile.radiusMetres);
		if(!std::isfinite(input.totalHeightMetres)
			|| input.totalHeightMetres < 2.0 * input.radiusMetres)
			return Mismatch(NavigationCompatibilityField::Height,
				input.totalHeightMetres, input.profile.heightMetres);
		const auto& center = input.localCenter;
		if(!center.IsFinite() || std::abs(center.x) > kLinearToleranceMetres
			|| std::abs(static_cast<double>(center.y) - input.totalHeightMetres * 0.5) > kLinearToleranceMetres
			|| std::abs(center.z) > kLinearToleranceMetres)
			return Mismatch(NavigationCompatibilityField::LocalCenter,
				center.y, input.totalHeightMetres * 0.5);
		if(input.ownerColliderSetEnabled)
			return Mismatch(NavigationCompatibilityField::OwnerColliderSet);
		if(!std::isfinite(input.contactPaddingMetres) || input.contactPaddingMetres < 0.0)
			return Mismatch(NavigationCompatibilityField::ClearanceMargin,
				input.contactPaddingMetres, input.profile.clearanceMarginMetres);
		if(!std::isfinite(input.stepHeightMetres) || input.stepHeightMetres < 0.0)
			return Mismatch(NavigationCompatibilityField::StepHeight,
				input.stepHeightMetres, input.profile.stepHeightMetres);
		if(!std::isfinite(input.maxSlopeDegrees) || input.maxSlopeDegrees <= 0.0)
			return Mismatch(NavigationCompatibilityField::SlopeAngle,
				input.maxSlopeDegrees, input.profile.maxSlopeDegrees);
		if(input.profile.radiusMetres + kLinearToleranceMetres < input.radiusMetres)
			return Mismatch(NavigationCompatibilityField::Radius,
				input.radiusMetres, input.profile.radiusMetres);
		if(input.profile.heightMetres + kLinearToleranceMetres < input.totalHeightMetres)
			return Mismatch(NavigationCompatibilityField::Height,
				input.totalHeightMetres, input.profile.heightMetres);
		if(input.profile.clearanceMarginMetres + kLinearToleranceMetres < input.contactPaddingMetres)
			return Mismatch(NavigationCompatibilityField::ClearanceMargin,
				input.contactPaddingMetres, input.profile.clearanceMarginMetres);
		if(input.profile.stepHeightMetres > input.stepHeightMetres + kLinearToleranceMetres)
			return Mismatch(NavigationCompatibilityField::StepHeight,
				input.stepHeightMetres, input.profile.stepHeightMetres);
		if(input.profile.maxSlopeDegrees > input.maxSlopeDegrees + kSlopeToleranceDegrees)
			return Mismatch(NavigationCompatibilityField::SlopeAngle,
				input.maxSlopeDegrees, input.profile.maxSlopeDegrees);
		return {true, LocomotionReason::None, NavigationCompatibilityField::None, 0.0, 0.0};
	}
}
