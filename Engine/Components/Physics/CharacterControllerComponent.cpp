// CharacterControllerComponent.cpp - character authoring validation.

#include "CharacterControllerComponent.h"

#include <cmath>

namespace TerraEngine::Physics
{
	namespace
	{
		constexpr float kMaximumCharacterExtent = 1000.0F;
		constexpr float kMaximumCharacterStrength = 1000000.0F;

		CharacterControllerValidationResult Reject(CharacterControllerValidationCode code, std::string message)
		{
			return {false, code, std::move(message)};
		}

		bool IsKnown(PhysicsCharacterShapeKind value)
		{
			return value == PhysicsCharacterShapeKind::Capsule || value == PhysicsCharacterShapeKind::Cylinder;
		}

		bool IsKnown(PhysicsCharacterRotationPolicy value)
		{
			return value == PhysicsCharacterRotationPolicy::Free || value == PhysicsCharacterRotationPolicy::AlignToUp
				|| value == PhysicsCharacterRotationPolicy::Locked;
		}

		bool IsPositiveBounded(float value, float maximum = kMaximumCharacterExtent)
		{
			return std::isfinite(value) && value > 0.0F && value <= maximum;
		}

		bool IsNonnegativeBounded(float value, float maximum = kMaximumCharacterExtent)
		{
			return std::isfinite(value) && value >= 0.0F && value <= maximum;
		}

		bool IsNormalizable(const PhysicsVector3& direction)
		{
			if(!direction.IsFinite()) return false;
			const float lengthSquared = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
			return std::isfinite(lengthSquared) && lengthSquared > 0.000001F;
		}
	}
}

TerraEngine::Physics::CharacterControllerValidationResult CharacterControllerComponent::Validate() const
{
	using namespace TerraEngine::Physics;
	if(schemaVersion != kCharacterControllerComponentSchemaVersion)
		return Reject(CharacterControllerValidationCode::UnsupportedSchema, "Character controller schema version requires an explicit migration.");
	if(!IsKnown(shapeKind))
		return Reject(CharacterControllerValidationCode::InvalidShape, "Character shape must be the engine-owned capsule or cylinder value.");
	if(!IsPositiveBounded(radius) || !IsPositiveBounded(height) || height < radius * 2.0F)
		return Reject(CharacterControllerValidationCode::InvalidDimensions, "Character radius and total height must be finite, positive, bounded, and tall enough for the radius.");
	if(!localCenter.IsFinite())
		return Reject(CharacterControllerValidationCode::InvalidCenter, "Character local center must contain finite metres.");
	if(!IsNormalizable(upDirection))
		return Reject(CharacterControllerValidationCode::InvalidUpDirection, "Character up direction must be finite and non-zero.");
	if(!IsNonnegativeBounded(stepHeight) || !IsPositiveBounded(supportVolumeRadius)
		|| !IsNonnegativeBounded(supportVolumeHeight) || !std::isfinite(maximumSlopeAngleDegrees)
		|| maximumSlopeAngleDegrees < 0.0F || maximumSlopeAngleDegrees >= 90.0F
		|| !IsNonnegativeBounded(contactPadding, 10.0F) || contactPadding >= radius)
		return Reject(CharacterControllerValidationCode::InvalidTraversal, "Character traversal values must be finite, bounded, and use a slope below 90 degrees with padding smaller than radius.");
	if(!IsPositiveBounded(mass, kMaximumCharacterStrength) || !IsNonnegativeBounded(maximumStrength, kMaximumCharacterStrength)
		|| !IsNonnegativeBounded(groundAdhesion, kMaximumCharacterStrength))
		return Reject(CharacterControllerValidationCode::InvalidInteraction, "Character mass, strength, and ground adhesion must be finite and within calibrated bounds.");
	if(collisionLayerId >= 32U)
		return Reject(CharacterControllerValidationCode::InvalidFilter, "Character collision layer IDs must be between zero and 31.");
	if(!IsNonnegativeBounded(gravityScale, 100.0F) || !IsNonnegativeBounded(jumpSpeed, kMaximumCharacterStrength))
		return Reject(CharacterControllerValidationCode::InvalidGravityOrJump, "Character gravity scale and jump speed must be finite and nonnegative within calibrated bounds.");
	if(!IsKnown(rotationPolicy))
		return Reject(CharacterControllerValidationCode::InvalidRotationPolicy, "Character rotation policy is not an engine-owned value.");
	return {true, CharacterControllerValidationCode::None, {}};
}

void CharacterControllerComponent::reflect(entt::meta_factory<CharacterControllerComponent>& factory)
{
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::schemaVersion>(factory, "schemaVersion");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::shapeKind>(factory, "shapeKind");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::radius>(factory, "radius");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::height>(factory, "height");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::localCenter>(factory, "localCenter");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::upDirection>(factory, "upDirection");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::stepHeight>(factory, "stepHeight");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::supportVolumeRadius>(factory, "supportVolumeRadius");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::supportVolumeHeight>(factory, "supportVolumeHeight");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::maximumSlopeAngleDegrees>(factory, "maximumSlopeAngleDegrees");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::contactPadding>(factory, "contactPadding");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::mass>(factory, "mass");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::maximumStrength>(factory, "maximumStrength");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::collisionLayerId>(factory, "collisionLayerId");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::collisionMask>(factory, "collisionMask");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::groundAdhesion>(factory, "groundAdhesion");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::allowStairWalking>(factory, "allowStairWalking");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::useGravity>(factory, "useGravity");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::gravityScale>(factory, "gravityScale");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::jumpSpeed>(factory, "jumpSpeed");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::rotationPolicy>(factory, "rotationPolicy");
	TerraEngine::Reflection::RegisterDataProperty<&CharacterControllerComponent::enabled>(factory, "enabled");
}
