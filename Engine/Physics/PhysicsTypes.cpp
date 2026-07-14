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
