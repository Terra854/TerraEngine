// ConstraintSetComponent.cpp - deterministic constraint record validation.

#include "ConstraintSetComponent.h"

#include <cmath>
#include <unordered_set>

namespace TerraEngine::Physics
{
	namespace
	{
		constexpr float kMaximumConstraintMagnitude = 1000000.0F;

		ConstraintSetValidationResult Reject(ConstraintSetValidationCode code, std::size_t index, std::string message)
		{
			return {false, code, index, std::move(message)};
		}

		bool IsKnown(PhysicsConstraintKind value)
		{
			return value == PhysicsConstraintKind::Fixed || value == PhysicsConstraintKind::Point
				|| value == PhysicsConstraintKind::Distance || value == PhysicsConstraintKind::Hinge
				|| value == PhysicsConstraintKind::Slider || value == PhysicsConstraintKind::Cone
				|| value == PhysicsConstraintKind::SwingTwist || value == PhysicsConstraintKind::SixDegreesOfFreedom
				|| value == PhysicsConstraintKind::Gear;
		}

		bool IsKnown(PhysicsConstraintEndpointKind value)
		{
			return value == PhysicsConstraintEndpointKind::ConnectedObject || value == PhysicsConstraintEndpointKind::WorldAnchor;
		}

		bool IsKnown(PhysicsConstraintMotorMode value)
		{
			return value == PhysicsConstraintMotorMode::Disabled || value == PhysicsConstraintMotorMode::Position
				|| value == PhysicsConstraintMotorMode::Velocity;
		}

		bool IsFiniteBounded(float value)
		{
			return std::isfinite(value) && std::abs(value) <= kMaximumConstraintMagnitude;
		}

		bool IsNormalizable(const PhysicsVector3& axis)
		{
			if(!axis.IsFinite()) return false;
			const float lengthSquared = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
			return std::isfinite(lengthSquared) && lengthSquared > 0.000001F;
		}

		bool IsValidFrame(const PhysicsPose& frame)
		{
			if(!frame.IsFinite()) return false;
			const float lengthSquared = frame.rotation.x * frame.rotation.x + frame.rotation.y * frame.rotation.y
				+ frame.rotation.z * frame.rotation.z + frame.rotation.w * frame.rotation.w;
			return std::isfinite(lengthSquared) && lengthSquared > 0.000001F;
		}

		bool IsValidEndpoint(PhysicsConstraintEndpointKind kind, const TerraEngine::UUID& objectId)
		{
			if(kind == PhysicsConstraintEndpointKind::ConnectedObject)
				return objectId != TerraEngine::EMPTY_UUID;
			return kind == PhysicsConstraintEndpointKind::WorldAnchor && objectId == TerraEngine::EMPTY_UUID;
		}
	}
}

TerraEngine::Physics::ConstraintRecord& ConstraintSetComponent::Add(TerraEngine::Physics::ConstraintRecord record)
{
	// Empty IDs are creation-time placeholders. A fresh UUID is assigned before
	// the record enters authoring storage so ordinary edits retain identity.
	if(record.recordId == TerraEngine::EMPTY_UUID)
		record.recordId = TerraEngine::GenerateUUID();
	constraints.push_back(std::move(record));
	return constraints.back();
}

bool ConstraintSetComponent::Remove(const TerraEngine::UUID& recordId)
{
	for(auto iterator = constraints.begin(); iterator != constraints.end(); ++iterator)
	{
		if(iterator->recordId == recordId)
		{
			constraints.erase(iterator);
			return true;
		}
	}
	return false;
}

TerraEngine::Physics::ConstraintSetValidationResult ConstraintSetComponent::Validate() const
{
	using namespace TerraEngine::Physics;
	if(schemaVersion != kConstraintSetComponentSchemaVersion)
		return Reject(ConstraintSetValidationCode::UnsupportedSchema, static_cast<std::size_t>(-1), "Constraint set schema version requires an explicit migration.");

	std::unordered_set<TerraEngine::UUID> seenIds;
	for(std::size_t index = 0; index < constraints.size(); ++index)
	{
		const ConstraintRecord& record = constraints[index];
		if(record.recordId == TerraEngine::EMPTY_UUID)
			return Reject(ConstraintSetValidationCode::EmptyRecordId, index, "Constraint records require a stable non-empty record ID.");
		if(!seenIds.insert(record.recordId).second)
			return Reject(ConstraintSetValidationCode::DuplicateRecordId, index, "Constraint record IDs must be unique within one constraint set.");
		if(record.recordVersion != kConstraintRecordSchemaVersion)
			return Reject(ConstraintSetValidationCode::UnsupportedRecordSchema, index, "Constraint record schema version requires an explicit migration.");
		if(!IsKnown(record.kind))
			return Reject(ConstraintSetValidationCode::InvalidConstraintKind, index, "Constraint kind is not an engine-owned tagged value.");
		if(!IsKnown(record.firstEndpointKind) || !IsKnown(record.secondEndpointKind)
			|| !IsValidEndpoint(record.firstEndpointKind, record.firstObjectId)
			|| !IsValidEndpoint(record.secondEndpointKind, record.secondObjectId)
			|| (record.firstEndpointKind == PhysicsConstraintEndpointKind::WorldAnchor
				&& record.secondEndpointKind == PhysicsConstraintEndpointKind::WorldAnchor)
			|| (record.firstEndpointKind == PhysicsConstraintEndpointKind::ConnectedObject
				&& record.secondEndpointKind == PhysicsConstraintEndpointKind::ConnectedObject
				&& record.firstObjectId == record.secondObjectId))
			return Reject(ConstraintSetValidationCode::InvalidEndpoint, index, "Constraint endpoints must name distinct objects or exactly one world anchor.");
		if(!IsValidFrame(record.firstFrame) || !IsValidFrame(record.secondFrame))
			return Reject(ConstraintSetValidationCode::InvalidFrame, index, "Constraint frames must contain finite positions and normalizable rotations.");
		if(!IsNormalizable(record.firstAxis) || !IsNormalizable(record.secondAxis))
			return Reject(ConstraintSetValidationCode::InvalidAxis, index, "Constraint axes must contain finite, non-zero vectors.");
		if((record.limits.enabled && (!IsFiniteBounded(record.limits.minimum) || !IsFiniteBounded(record.limits.maximum)
			|| record.limits.minimum > record.limits.maximum)))
			return Reject(ConstraintSetValidationCode::InvalidLimits, index, "Enabled constraint limits must be finite, bounded, and ordered minimum-to-maximum.");
		if(!IsFiniteBounded(record.spring.stiffness) || !IsFiniteBounded(record.spring.damping)
			|| record.spring.stiffness < 0.0F || record.spring.damping < 0.0F)
			return Reject(ConstraintSetValidationCode::InvalidSpring, index, "Constraint spring stiffness and damping must be finite, bounded, and nonnegative.");
		if(!IsKnown(record.motor.mode) || !IsFiniteBounded(record.motor.targetPosition)
			|| !IsFiniteBounded(record.motor.targetVelocity) || !IsFiniteBounded(record.motor.maximumForce)
			|| record.motor.maximumForce < 0.0F)
			return Reject(ConstraintSetValidationCode::InvalidMotor, index, "Constraint motor settings must use a known mode and finite nonnegative maximum force.");
		for(std::size_t axisIndex = 0U; axisIndex < record.axisLimits.size(); ++axisIndex)
		{
			const PhysicsConstraintLimits& limits = record.axisLimits[axisIndex];
			const PhysicsConstraintSpring& spring = record.axisSprings[axisIndex];
			const PhysicsConstraintMotor& motor = record.axisMotors[axisIndex];
			if(limits.enabled && (!IsFiniteBounded(limits.minimum) || !IsFiniteBounded(limits.maximum)
				|| limits.minimum > limits.maximum))
				return Reject(ConstraintSetValidationCode::InvalidLimits, index, "SixDOF axis limits must be finite, bounded, and ordered.");
			if(!IsFiniteBounded(spring.stiffness) || !IsFiniteBounded(spring.damping)
				|| spring.stiffness < 0.0F || spring.damping < 0.0F)
				return Reject(ConstraintSetValidationCode::InvalidSpring, index, "SixDOF axis springs must be finite, bounded, and nonnegative.");
			if(!IsKnown(motor.mode) || !IsFiniteBounded(motor.targetPosition)
				|| !IsFiniteBounded(motor.targetVelocity) || !IsFiniteBounded(motor.maximumForce)
				|| motor.maximumForce < 0.0F)
				return Reject(ConstraintSetValidationCode::InvalidMotor, index, "SixDOF axis motors must be finite and use a known mode.");
		}
		if(record.kind == PhysicsConstraintKind::Gear && (!IsFiniteBounded(record.gearRatio) || record.gearRatio == 0.0F))
			return Reject(ConstraintSetValidationCode::InvalidGearRatio, index, "Constraint gear ratio must be finite, bounded, and non-zero.");
		if(record.kind == PhysicsConstraintKind::Gear && (record.firstGearConstraintId == TerraEngine::EMPTY_UUID
			|| record.secondGearConstraintId == TerraEngine::EMPTY_UUID
			|| record.firstGearConstraintId == record.secondGearConstraintId))
			return Reject(ConstraintSetValidationCode::InvalidGearReference, index, "Gear constraints require two distinct stable hinge record IDs.");
		if((!std::isfinite(record.breakForce) && !std::isinf(record.breakForce))
			|| (!std::isfinite(record.breakTorque) && !std::isinf(record.breakTorque))
			|| record.breakForce < 0.0F || record.breakTorque < 0.0F
			|| (std::isfinite(record.breakForce) && record.breakForce > kMaximumConstraintMagnitude)
			|| (std::isfinite(record.breakTorque) && record.breakTorque > kMaximumConstraintMagnitude))
			return Reject(ConstraintSetValidationCode::InvalidBreakThreshold, index, "Constraint break thresholds must be nonnegative finite bounds or positive infinity for unbreakable.");
		if(record.resetGeneration == 0U)
			return Reject(ConstraintSetValidationCode::InvalidResetGeneration, index, "Constraint reset generation must be non-zero to distinguish reset requests.");
	}

	return {true, ConstraintSetValidationCode::None, static_cast<std::size_t>(-1), {}};
}

void ConstraintSetComponent::reflect(entt::meta_factory<ConstraintSetComponent>& factory)
{
	TerraEngine::Reflection::RegisterDataProperty<&ConstraintSetComponent::schemaVersion>(factory, "schemaVersion");
	TerraEngine::Reflection::RegisterDataProperty<&ConstraintSetComponent::constraints>(factory, "constraints");
}
