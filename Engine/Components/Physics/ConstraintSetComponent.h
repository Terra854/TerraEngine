// ConstraintSetComponent.h - provider-neutral, ordered constraint authoring.
//
// A single object may own several constraints of the same kind.  Stable child
// IDs keep those records addressable by prefab overrides, undo, and deferred
// physics commands without exposing a provider handle in scene data.

#pragma once

#include "Core/Object/Component.h"
#include "Physics/PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Physics
{
	inline constexpr std::uint32_t kConstraintSetComponentSchemaVersion = 1U;
	inline constexpr std::uint32_t kConstraintRecordSchemaVersion = 1U;

	// An endpoint either names another authored object or uses the immutable
	// physics-world frame. Runtime resolution verifies object generations later.
	enum class PhysicsConstraintEndpointKind : std::uint8_t
	{
		ConnectedObject,
		WorldAnchor
	};

	// Limits are deliberately generic: the selected constraint kind determines
	// whether their values represent metres, degrees, or an engine-defined ratio.
	struct PhysicsConstraintLimits
	{
		bool enabled = false;
		float minimum = 0.0F;
		float maximum = 0.0F;
	};

	struct PhysicsConstraintSpring
	{
		bool enabled = false;
		float stiffness = 0.0F;
		float damping = 0.0F;
	};

	enum class PhysicsConstraintMotorMode : std::uint8_t
	{
		Disabled,
		Position,
		Velocity
	};

	struct PhysicsConstraintMotor
	{
		PhysicsConstraintMotorMode mode = PhysicsConstraintMotorMode::Disabled;
		float targetPosition = 0.0F;
		float targetVelocity = 0.0F;
		float maximumForce = 0.0F;
	};

	// The tagged kind plus common fields forms a stable serializer-friendly
	// record. Provider-specific layouts and runtime broken state remain private.
	struct ConstraintRecord
	{
		TerraEngine::UUID recordId = TerraEngine::GenerateUUID();
		std::uint32_t recordVersion = kConstraintRecordSchemaVersion;
		PhysicsConstraintKind kind = PhysicsConstraintKind::Fixed;
		PhysicsConstraintEndpointKind firstEndpointKind = PhysicsConstraintEndpointKind::ConnectedObject;
		TerraEngine::UUID firstObjectId = TerraEngine::EMPTY_UUID;
		PhysicsConstraintEndpointKind secondEndpointKind = PhysicsConstraintEndpointKind::WorldAnchor;
		TerraEngine::UUID secondObjectId = TerraEngine::EMPTY_UUID;
		PhysicsPose firstFrame{};
		PhysicsPose secondFrame{};
		PhysicsVector3 firstAxis{0.0F, 1.0F, 0.0F};
		PhysicsVector3 secondAxis{0.0F, 1.0F, 0.0F};
		PhysicsConstraintLimits limits{};
		PhysicsConstraintSpring spring{};
		PhysicsConstraintMotor motor{};
		// SixDOF consumes these entries in translation XYZ then rotation XYZ order.
		// Simpler joints use the scalar settings above, which keeps their JSON concise.
		std::array<PhysicsConstraintLimits, 6U> axisLimits{};
		std::array<PhysicsConstraintSpring, 6U> axisSprings{};
		std::array<PhysicsConstraintMotor, 6U> axisMotors{};
		// A Jolt gear constraint couples two authored hinges. Stable record IDs keep
		// that relationship provider-neutral and remappable by prefab composition.
		TerraEngine::UUID firstGearConstraintId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID secondGearConstraintId = TerraEngine::EMPTY_UUID;
		float gearRatio = 1.0F;
		bool collideConnected = false;
		float breakForce = std::numeric_limits<float>::infinity();
		float breakTorque = std::numeric_limits<float>::infinity();
		bool enabled = true;
		// Incrementing this authoring token requests a backend reset. It is never
		// overwritten by transient broken state published by the simulation.
		std::uint32_t resetGeneration = 1U;
	};

	enum class ConstraintSetValidationCode : std::uint8_t
	{
		None,
		UnsupportedSchema,
		EmptyRecordId,
		DuplicateRecordId,
		UnsupportedRecordSchema,
		InvalidConstraintKind,
		InvalidEndpoint,
		InvalidFrame,
		InvalidAxis,
		InvalidLimits,
		InvalidSpring,
		InvalidMotor,
		InvalidGearRatio,
		InvalidGearReference,
		InvalidBreakThreshold,
		InvalidResetGeneration
	};

	struct ConstraintSetValidationResult
	{
		bool accepted = false;
		ConstraintSetValidationCode code = ConstraintSetValidationCode::None;
		std::size_t recordIndex = static_cast<std::size_t>(-1);
		std::string message;
	};
}

struct ConstraintSetComponent final : public Component<ConstraintSetComponent>
{
	static constexpr std::string_view kComponentName = "ConstraintSetComponent";

	std::uint32_t schemaVersion = TerraEngine::Physics::kConstraintSetComponentSchemaVersion;
	std::vector<TerraEngine::Physics::ConstraintRecord> constraints;

	// Appending deliberately preserves order. Validation, rather than insertion,
	// rejects duplicate serialized IDs deterministically at the first duplicate.
	TerraEngine::Physics::ConstraintRecord& Add(TerraEngine::Physics::ConstraintRecord record = {});
	[[nodiscard]] bool Remove(const TerraEngine::UUID& recordId);
	[[nodiscard]] TerraEngine::Physics::ConstraintSetValidationResult Validate() const;

	// The collection is reflected for editor/automation discovery; the dedicated
	// physics serializer owns the nested tagged-record JSON representation.
	static void reflect(entt::meta_factory<ConstraintSetComponent>& factory);
};
