// RigidBodyComponent.cpp - validation and self-described automation metadata.

#include "RigidBodyComponent.h"

#include <cmath>

namespace TerraEngine::Physics
{
	namespace
	{
		RigidBodyValidationResult Reject(RigidBodyValidationCode code, std::string message)
		{
			return {false, code, std::move(message)};
		}

		bool IsKnown(PhysicsMotionType value)
		{
			return value == PhysicsMotionType::Static
				|| value == PhysicsMotionType::Dynamic
				|| value == PhysicsMotionType::Kinematic;
		}

		bool IsKnown(PhysicsMassMode value)
		{
			return value == PhysicsMassMode::AutomaticFromDensity || value == PhysicsMassMode::ExplicitMass;
		}

		bool IsKnown(PhysicsMotionQuality value)
		{
			return value == PhysicsMotionQuality::Discrete || value == PhysicsMotionQuality::LinearCast;
		}

		bool IsKnown(PhysicsInterpolationMode value)
		{
			return value == PhysicsInterpolationMode::None
				|| value == PhysicsInterpolationMode::Interpolate
				|| value == PhysicsInterpolationMode::Extrapolate;
		}
	}

}

TerraEngine::Physics::RigidBodyValidationResult RigidBodyComponent::Validate() const
	{
	using namespace TerraEngine::Physics;
		if(schemaVersion != kRigidBodyComponentSchemaVersion)
			return Reject(RigidBodyValidationCode::UnsupportedSchema, "Rigid body schema version requires an explicit migration.");
		if(!IsKnown(motionType))
			return Reject(RigidBodyValidationCode::InvalidMotionType, "Rigid body motion type is not an engine-owned value.");
		if(!IsKnown(massMode))
			return Reject(RigidBodyValidationCode::InvalidMassMode, "Rigid body mass mode is not an engine-owned value.");
		if(!std::isfinite(mass) || mass <= 0.0F || mass > 1000000.0F)
			return Reject(RigidBodyValidationCode::InvalidMass, "Rigid body mass must be finite, positive, and within calibrated bounds.");
		if(!std::isfinite(linearDamping) || !std::isfinite(angularDamping)
			|| linearDamping < 0.0F || angularDamping < 0.0F
			|| linearDamping > 100.0F || angularDamping > 100.0F)
			return Reject(RigidBodyValidationCode::InvalidDamping, "Rigid body damping must be finite and between zero and 100.");
		if(!IsKnown(continuousCollisionMode))
			return Reject(RigidBodyValidationCode::InvalidMotionQuality, "Motion quality must be Discrete or LinearCast.");
		if(!initialLinearVelocity.IsFinite() || !initialAngularVelocity.IsFinite())
			return Reject(RigidBodyValidationCode::NonfiniteVelocity, "Initial linear and angular velocities must be finite.");
		if(!std::isfinite(maxLinearVelocity) || !std::isfinite(maxAngularVelocity)
			|| maxLinearVelocity < 0.0F || maxAngularVelocity < 0.0F
			|| maxLinearVelocity > 100000.0F || maxAngularVelocity > 100000.0F)
			return Reject(RigidBodyValidationCode::InvalidVelocityLimit, "Maximum velocities must be finite, nonnegative, and within calibrated bounds.");
		if((allowedDegreesOfFreedom & ~kPhysicsAllDegreesOfFreedom) != 0U)
			return Reject(RigidBodyValidationCode::InvalidDegreesOfFreedom, "Allowed degrees of freedom contains unsupported axis bits.");
		if(!IsKnown(interpolationMode))
			return Reject(RigidBodyValidationCode::InvalidInterpolationMode, "Interpolation mode is not an engine-owned value.");
		if(collisionLayerId >= 32U)
			return Reject(RigidBodyValidationCode::InvalidCollisionLayer, "Collision layer IDs must be between zero and 31.");
		// uint32_t already provides the exact 32-bit collision-mask domain.
		return {true, RigidBodyValidationCode::None, {}};
	}

void RigidBodyComponent::reflect(entt::meta_factory<RigidBodyComponent>& factory)
{
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::schemaVersion>(factory, "schemaVersion");
	TerraEngine::Reflection::RegisterAccessorProperty<&RigidBodyComponent::SetMotionTypeValue, &RigidBodyComponent::GetMotionTypeValue>(factory, "motionType");
	TerraEngine::Reflection::RegisterAccessorProperty<&RigidBodyComponent::SetMassModeValue, &RigidBodyComponent::GetMassModeValue>(factory, "massMode");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::mass>(factory, "mass");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::linearDamping>(factory, "linearDamping");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::angularDamping>(factory, "angularDamping");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::useGravity>(factory, "useGravity");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::allowSleeping>(factory, "allowSleeping");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::startAwake>(factory, "startAwake");
	TerraEngine::Reflection::RegisterAccessorProperty<&RigidBodyComponent::SetMotionQualityValue, &RigidBodyComponent::GetMotionQualityValue>(factory, "continuousCollisionMode");
	TerraEngine::Reflection::RegisterAccessorProperty<&RigidBodyComponent::SetInitialLinearVelocityValue, &RigidBodyComponent::GetInitialLinearVelocityValue>(factory, "initialLinearVelocity");
	TerraEngine::Reflection::RegisterAccessorProperty<&RigidBodyComponent::SetInitialAngularVelocityValue, &RigidBodyComponent::GetInitialAngularVelocityValue>(factory, "initialAngularVelocity");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::maxLinearVelocity>(factory, "maxLinearVelocity");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::maxAngularVelocity>(factory, "maxAngularVelocity");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::allowedDegreesOfFreedom>(factory, "allowedDegreesOfFreedom");
	TerraEngine::Reflection::RegisterAccessorProperty<&RigidBodyComponent::SetInterpolationModeValue, &RigidBodyComponent::GetInterpolationModeValue>(factory, "interpolationMode");
	TerraEngine::Reflection::RegisterAccessorProperty<&RigidBodyComponent::SetCollisionLayerValue, &RigidBodyComponent::GetCollisionLayerValue>(factory, "collisionLayerId");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::useCollisionMaskOverride>(factory, "useCollisionMaskOverride");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::collisionMaskOverride>(factory, "collisionMaskOverride");
	TerraEngine::Reflection::RegisterDataProperty<&RigidBodyComponent::enabled>(factory, "enabled");
}

std::vector<ComponentAutomationDeclaration> RigidBodyComponent::automation()
{
	ComponentAutomationDeclaration declaration;
	declaration.name = "physics.rigidbody.authoring_schema";
	declaration.category = "physics";
	declaration.description = "Returns the versioned, backend-neutral rigid-body authoring contract.";
	declaration.inputSchema = {{"type", "object"}};
	declaration.resultSchema = {{"type", "object"}, {"properties", {{"schemaVersion", {{"type", "integer"}}}, {"component", {{"type", "string"}}}}}};
	declaration.requiresProject = true;
	declaration.requiresScene = false;
	declaration.visibilityBoundary = "immutable_physics_snapshot";
	declaration.handler = [](const nlohmann::json&) {
		return nlohmann::json{{"component", std::string(kComponentName)}, {"schemaVersion", TerraEngine::Physics::kRigidBodyComponentSchemaVersion}};
	};
	return {std::move(declaration)};
}
