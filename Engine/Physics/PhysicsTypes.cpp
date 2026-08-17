// PhysicsTypes.cpp - readable diagnostics for provider-neutral physics values.

#include "PhysicsTypes.h"

#include <cmath>

namespace TerraEngine::Physics {
	bool PhysicsVector3::IsFinite() const noexcept {
		return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
	}

	bool PhysicsQuaternion::IsFinite() const noexcept {
		return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(w);
	}

	bool PhysicsPose::IsFinite() const noexcept {
		return position.IsFinite() && rotation.IsFinite();
	}

	bool PhysicsFrameDescriptor::IsValid() const noexcept {
		return frame.IsValid() && world.IsValid()
			&& std::isfinite(maximumLocalMagnitudeMetres)
			&& maximumLocalMagnitudeMetres > 0.0
			&& TerraEngine::Scene::NormalizeWorldPosition(canonicalOrigin).Succeeded();
	}

	bool PhysicsFrameLocalPose::IsFinite() const noexcept {
		return frame.IsValid() && pose.IsFinite();
	}

	PhysicsFramePoseConversion LocalizePhysicsPose(
		const TerraEngine::Scene::WorldPose& canonicalPose,
		const PhysicsFrameDescriptor& frame) noexcept {
		PhysicsFramePoseConversion result;
		if(!frame.IsValid()) {
			result.error = PhysicsFrameConversionError::InvalidFrame;
			return result;
		}
		if(!std::isfinite(canonicalPose.position.offsetMetres.x)
			|| !std::isfinite(canonicalPose.position.offsetMetres.y)
			|| !std::isfinite(canonicalPose.position.offsetMetres.z)
			|| !std::isfinite(canonicalPose.rotation.x) || !std::isfinite(canonicalPose.rotation.y)
			|| !std::isfinite(canonicalPose.rotation.z) || !std::isfinite(canonicalPose.rotation.w)) {
			result.error = PhysicsFrameConversionError::NonFinitePose;
			return result;
		}
		const TerraEngine::Scene::WorldLocalizationResult localized =
			TerraEngine::Scene::LocalizeWorldPosition(
				canonicalPose.position, frame.canonicalOrigin, frame.maximumLocalMagnitudeMetres);
		if(!localized.Succeeded()) {
			result.error = localized.error == TerraEngine::Scene::WorldCoordinateError::UnsafeLocalRange
				? PhysicsFrameConversionError::OutsideLocalBounds
				: PhysicsFrameConversionError::CoordinateOverflow;
			return result;
		}
		result.localPose.frame = frame.frame;
		result.localPose.pose.position = {static_cast<float>(localized.offsetMetres.x),
			static_cast<float>(localized.offsetMetres.y), static_cast<float>(localized.offsetMetres.z)};
		result.localPose.pose.rotation = {canonicalPose.rotation.x, canonicalPose.rotation.y,
			canonicalPose.rotation.z, canonicalPose.rotation.w};
		return result;
	}

	PhysicsCanonicalPoseConversion ReconstructCanonicalPhysicsPose(
		const PhysicsFrameLocalPose& localPose,
		const PhysicsFrameDescriptor& frame) noexcept {
		PhysicsCanonicalPoseConversion result;
		if(!frame.IsValid()) {
			result.error = PhysicsFrameConversionError::InvalidFrame;
			return result;
		}
		if(localPose.frame != frame.frame) {
			result.error = PhysicsFrameConversionError::StaleFrame;
			return result;
		}
		if(!localPose.pose.IsFinite()) {
			result.error = PhysicsFrameConversionError::NonFinitePose;
			return result;
		}
		const double x = static_cast<double>(localPose.pose.position.x);
		const double y = static_cast<double>(localPose.pose.position.y);
		const double z = static_cast<double>(localPose.pose.position.z);
		if(std::sqrt(x * x + y * y + z * z) > frame.maximumLocalMagnitudeMetres) {
			result.error = PhysicsFrameConversionError::OutsideLocalBounds;
			return result;
		}
		TerraEngine::Scene::WorldDelta delta;
		delta.offsetMetres = {x, y, z};
		const TerraEngine::Scene::WorldCoordinateResult position =
			TerraEngine::Scene::ApplyWorldDelta(frame.canonicalOrigin, delta);
		if(!position.Succeeded()) {
			result.error = PhysicsFrameConversionError::CoordinateOverflow;
			return result;
		}
		result.canonicalPose.position = position.position;
		result.canonicalPose.rotation = {localPose.pose.rotation.w, localPose.pose.rotation.x,
			localPose.pose.rotation.y, localPose.pose.rotation.z};
		return result;
	}

	std::string_view ToString(PhysicsFrameConversionError error) noexcept {
		switch(error) {
		case PhysicsFrameConversionError::None: return "None";
		case PhysicsFrameConversionError::InvalidFrame: return "InvalidFrame";
		case PhysicsFrameConversionError::StaleFrame: return "StaleFrame";
		case PhysicsFrameConversionError::NonFinitePose: return "NonFinitePose";
		case PhysicsFrameConversionError::OutsideLocalBounds: return "OutsideLocalBounds";
		case PhysicsFrameConversionError::CoordinateOverflow: return "CoordinateOverflow";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsHandleStatus status) {
		switch(status) {
		case PhysicsHandleStatus::Invalid: return "Invalid";
		case PhysicsHandleStatus::Alive: return "Alive";
		case PhysicsHandleStatus::Destroyed: return "Destroyed";
		case PhysicsHandleStatus::StaleSlot: return "StaleSlot";
		case PhysicsHandleStatus::StaleWorld: return "StaleWorld";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsResultStatus status) {
		switch(status) {
		case PhysicsResultStatus::Success: return "Success";
		case PhysicsResultStatus::Accepted: return "Accepted";
		case PhysicsResultStatus::Rejected: return "Rejected";
		case PhysicsResultStatus::Cancelled: return "Cancelled";
		case PhysicsResultStatus::StaleTarget: return "StaleTarget";
		case PhysicsResultStatus::Expired: return "Expired";
		case PhysicsResultStatus::LimitExceeded: return "LimitExceeded";
		case PhysicsResultStatus::Conflict: return "Conflict";
		case PhysicsResultStatus::ProviderFailure: return "ProviderFailure";
		case PhysicsResultStatus::InternalFailure: return "InternalFailure";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsMotionType type) {
		switch(type) {
		case PhysicsMotionType::Static: return "Static";
		case PhysicsMotionType::Dynamic: return "Dynamic";
		case PhysicsMotionType::Kinematic: return "Kinematic";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsMotionQuality quality) {
		switch(quality) {
		case PhysicsMotionQuality::Discrete: return "Discrete";
		case PhysicsMotionQuality::LinearCast: return "LinearCast";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsForceMode mode) {
		switch(mode) {
		case PhysicsForceMode::Force: return "Force";
		case PhysicsForceMode::Acceleration: return "Acceleration";
		case PhysicsForceMode::Impulse: return "Impulse";
		case PhysicsForceMode::VelocityChange: return "VelocityChange";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsWorldState state) {
		switch(state) {
		case PhysicsWorldState::Absent: return "Absent";
		case PhysicsWorldState::Creating: return "Creating";
		case PhysicsWorldState::Active: return "Active";
		case PhysicsWorldState::Paused: return "Paused";
		case PhysicsWorldState::Stopping: return "Stopping";
		case PhysicsWorldState::Draining: return "Draining";
		case PhysicsWorldState::Failed: return "Failed";
		case PhysicsWorldState::Destroyed: return "Destroyed";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsQueryKind kind) {
		switch(kind) {
		case PhysicsQueryKind::RayCast: return "RayCast";
		case PhysicsQueryKind::ShapeCast: return "ShapeCast";
		case PhysicsQueryKind::Overlap: return "Overlap";
		case PhysicsQueryKind::Point: return "Point";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsShapeKind kind) {
		switch(kind) {
		case PhysicsShapeKind::Box: return "Box";
		case PhysicsShapeKind::Sphere: return "Sphere";
		case PhysicsShapeKind::Capsule: return "Capsule";
		case PhysicsShapeKind::Cylinder: return "Cylinder";
		case PhysicsShapeKind::TaperedCapsule: return "TaperedCapsule";
		case PhysicsShapeKind::ConvexHull: return "ConvexHull";
		case PhysicsShapeKind::Mesh: return "Mesh";
		case PhysicsShapeKind::HeightField: return "HeightField";
		case PhysicsShapeKind::Compound: return "Compound";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsConstraintKind kind) {
		switch(kind) {
		case PhysicsConstraintKind::Fixed: return "Fixed";
		case PhysicsConstraintKind::Point: return "Point";
		case PhysicsConstraintKind::Distance: return "Distance";
		case PhysicsConstraintKind::Hinge: return "Hinge";
		case PhysicsConstraintKind::Slider: return "Slider";
		case PhysicsConstraintKind::Cone: return "Cone";
		case PhysicsConstraintKind::SwingTwist: return "SwingTwist";
		case PhysicsConstraintKind::SixDegreesOfFreedom: return "SixDegreesOfFreedom";
		case PhysicsConstraintKind::Gear: return "Gear";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsCombineMode mode) {
		switch(mode) {
		case PhysicsCombineMode::Average: return "Average";
		case PhysicsCombineMode::Minimum: return "Minimum";
		case PhysicsCombineMode::Multiply: return "Multiply";
		case PhysicsCombineMode::Maximum: return "Maximum";
		}
		return "Unknown";
	}
}
