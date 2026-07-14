// RigidBodyComponent.h - backend-neutral authored rigid-body settings.
//
// This component contains creator intent only. PhysicsWorld owns every runtime
// handle, force accumulator, contact cache, and published simulation result.

#pragma once

#include "Core/Object/Component.h"
#include "Physics/PhysicsTypes.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Physics
{
	inline constexpr std::uint32_t kRigidBodyComponentSchemaVersion = 1U;

	// Mass selection is engine-owned so authored files never expose provider flags.
	enum class PhysicsMassMode : std::uint8_t
	{
		AutomaticFromDensity,
		ExplicitMass
	};

	// Interpolation changes presentation only; it does not alter fixed simulation steps.
	enum class PhysicsInterpolationMode : std::uint8_t
	{
		None,
		Interpolate,
		Extrapolate
	};

	// Each bit permits one authored degree of freedom. A value of zero freezes all
	// axes, while kPhysicsAllDegreesOfFreedom permits normal unconstrained motion.
	enum PhysicsDegreeOfFreedom : std::uint32_t
	{
		PhysicsTranslateX = 1U << 0U,
		PhysicsTranslateY = 1U << 1U,
		PhysicsTranslateZ = 1U << 2U,
		PhysicsRotateX = 1U << 3U,
		PhysicsRotateY = 1U << 4U,
		PhysicsRotateZ = 1U << 5U,
		kPhysicsAllDegreesOfFreedom = PhysicsTranslateX | PhysicsTranslateY | PhysicsTranslateZ
			| PhysicsRotateX | PhysicsRotateY | PhysicsRotateZ
	};

	enum class RigidBodyValidationCode : std::uint8_t
	{
		None,
		UnsupportedSchema,
		InvalidMotionType,
		InvalidMassMode,
		InvalidMass,
		InvalidDamping,
		InvalidMotionQuality,
		NonfiniteVelocity,
		InvalidVelocityLimit,
		InvalidDegreesOfFreedom,
		InvalidInterpolationMode,
		InvalidCollisionLayer,
		InvalidCollisionMask
	};

	struct RigidBodyValidationResult
	{
		bool accepted = false;
		RigidBodyValidationCode code = RigidBodyValidationCode::None;
		std::string message;
	};
}

// One optional component describes an object's initial, provider-neutral body
// configuration. It is deliberately safe to copy into an immutable authoring
// snapshot before worker jobs validate or prepare a later protected rebuild.
struct RigidBodyComponent final : public Component<RigidBodyComponent>
{
	static constexpr std::string_view kComponentName = "RigidBodyComponent";

	std::uint32_t schemaVersion = TerraEngine::Physics::kRigidBodyComponentSchemaVersion;
	TerraEngine::Physics::PhysicsMotionType motionType = TerraEngine::Physics::PhysicsMotionType::Dynamic;
	TerraEngine::Physics::PhysicsMassMode massMode = TerraEngine::Physics::PhysicsMassMode::AutomaticFromDensity;
	float mass = 1.0F;
	float linearDamping = 0.0F;
	float angularDamping = 0.05F;
	bool useGravity = true;
	bool allowSleeping = true;
	bool startAwake = true;
	TerraEngine::Physics::PhysicsMotionQuality continuousCollisionMode = TerraEngine::Physics::PhysicsMotionQuality::Discrete;
	TerraEngine::Physics::PhysicsVector3 initialLinearVelocity{};
	TerraEngine::Physics::PhysicsVector3 initialAngularVelocity{};
	float maxLinearVelocity = 500.0F;
	float maxAngularVelocity = 100.0F;
	std::uint32_t allowedDegreesOfFreedom = TerraEngine::Physics::kPhysicsAllDegreesOfFreedom;
	TerraEngine::Physics::PhysicsInterpolationMode interpolationMode = TerraEngine::Physics::PhysicsInterpolationMode::Interpolate;
	std::uint8_t collisionLayerId = 0U;
	bool useCollisionMaskOverride = false;
	std::uint32_t collisionMaskOverride = 0xFFFFFFFFU;
	bool enabled = true;

	// Validation is pure so snapshot jobs can reject invalid authored values
	// without reading live object storage or invoking the physics provider.
	[[nodiscard]] TerraEngine::Physics::RigidBodyValidationResult Validate() const;

	// Reflection uses stable numeric accessors for engine enums. Scene JSON uses
	// symbolic enum names in the physics serializer implemented by its owner.
	static void reflect(entt::meta_factory<RigidBodyComponent>& factory);
	static std::vector<ComponentAutomationDeclaration> automation();
};
