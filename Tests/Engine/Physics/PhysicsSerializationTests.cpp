// Scene serialization tests for durable physics authoring data. Runtime
// handles, commands, break state, and contact publications deliberately stay
// outside these project-owned JSON contracts.
#include "../TestHarness.h"

#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/Serializer.h"

#include <limits>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Physics;

		void ResetSerializationFixture()
		{
			// Registration happens before loading so deserialization can select the
			// typed component path rather than the generic reflection fallback.
			ComponentRegistry::RegisterAllComponents();
			ObjectManager::instance().clear();
		}

		ConstraintRecord MakeConstraint(TerraEngine::UUID recordId, TerraEngine::UUID endpoint)
		{
			ConstraintRecord record;
			record.recordId = recordId;
			record.kind = PhysicsConstraintKind::Hinge;
			record.firstObjectId = endpoint;
			record.firstFrame.position = {1.0F, 2.0F, 3.0F};
			record.secondFrame.position = {-1.0F, -2.0F, -3.0F};
			record.firstAxis = {1.0F, 0.0F, 0.0F};
			record.secondAxis = {0.0F, 0.0F, 1.0F};
			record.limits = {true, -20.0F, 45.0F};
			record.spring = {true, 12.0F, 3.0F};
			record.motor = {PhysicsConstraintMotorMode::Velocity, 4.0F, 5.0F, 6.0F};
			record.axisLimits[0] = {true, -1.0F, 1.0F};
			record.axisSprings[1] = {true, 8.0F, 2.0F};
			record.axisMotors[2] = {PhysicsConstraintMotorMode::Position, 3.0F, 0.0F, 4.0F};
			record.gearRatio = 2.0F;
			record.collideConnected = true;
			record.breakForce = 50.0F;
			record.breakTorque = std::numeric_limits<float>::infinity();
			record.enabled = true;
			record.resetGeneration = 7U;
			return record;
		}

		nlohmann::json SerializeFixtureWithPhysics()
		{
			ResetSerializationFixture();
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Physics Serialization Fixture");
			ConstraintSetComponent& constraints = *objects.AddComponent<ConstraintSetComponent>(object);
			const TerraEngine::UUID firstId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID secondId = TerraEngine::GenerateUUID();
			constraints.Add(MakeConstraint(firstId, TerraEngine::GenerateUUID()));
			ConstraintRecord second = MakeConstraint(secondId, TerraEngine::GenerateUUID());
			second.kind = PhysicsConstraintKind::Slider;
			second.resetGeneration = 8U;
			constraints.Add(second);
			ConstraintRecord gear = MakeConstraint(TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID());
			gear.kind = PhysicsConstraintKind::Gear;
			gear.firstGearConstraintId = firstId;
			gear.secondGearConstraintId = secondId;
			gear.gearRatio = -1.5F;
			constraints.Add(gear);

			CharacterControllerComponent& character = *objects.AddComponent<CharacterControllerComponent>(object);
			character.shapeKind = PhysicsCharacterShapeKind::Cylinder;
			character.localCenter = {0.25F, 0.5F, 0.75F};
			character.maximumSlopeAngleDegrees = 40.0F;
			character.rotationPolicy = PhysicsCharacterRotationPolicy::Locked;
			character.enabled = false;
			return Serializer::SerializeObject(object);
		}

	}

	void RegisterPhysicsSerializationTests(TestRegistry& registry)
	{
		registry.Add("Physics.Serialization.ConstraintAndCharacterSchemasRoundTripDurableFields", [](TestContext& context) {
			const nlohmann::json saved = SerializeFixtureWithPhysics();
			const nlohmann::json& constraintJson = saved["components"]["ConstraintSetComponent"];
			const nlohmann::json& characterJson = saved["components"]["CharacterControllerComponent"];
			context.Expect(saved.dump().find("Jolt") == std::string::npos, "Scene JSON must contain only engine-owned physics contracts.");
			context.ExpectEqual(1U, constraintJson["schemaVersion"].get<std::uint32_t>(), "Constraint schema version must be written.");
			context.ExpectEqual(1U, characterJson["schemaVersion"].get<std::uint32_t>(), "Character schema version must be written.");
			context.ExpectEqual(static_cast<std::size_t>(3), constraintJson["constraints"].size(), "Constraint records must retain authored order.");
			context.ExpectEqual(std::string("Hinge"), constraintJson["constraints"][0]["kind"].get<std::string>(), "Constraint kinds must use symbolic engine values.");
			context.ExpectEqual(std::string("Infinity"), constraintJson["constraints"][0]["breakTorque"].get<std::string>(), "Unbreakable thresholds need a stable JSON spelling.");
			context.ExpectEqual(static_cast<std::size_t>(6), constraintJson["constraints"][0]["axisLimits"].size(), "SixDOF limits must keep their ordered six-axis schema.");
			context.ExpectEqual(std::string("Position"), constraintJson["constraints"][0]["axisMotors"][2]["mode"].get<std::string>(), "SixDOF motor modes must use symbolic engine values.");
			context.ExpectEqual(constraintJson["constraints"][0]["recordId"].get<std::string>(), constraintJson["constraints"][2]["firstGearConstraintId"].get<std::string>(), "Gear records must persist their first authored hinge record UUID.");
			context.ExpectEqual(std::string("Cylinder"), characterJson["shapeKind"].get<std::string>(), "Character shape must use a symbolic engine value.");

			ResetSerializationFixture();
			Object loaded = Serializer::DeserializeObject(saved);
			ConstraintSetComponent* constraints = ObjectManager::instance().GetComponent<ConstraintSetComponent>(loaded);
			CharacterControllerComponent* character = ObjectManager::instance().GetComponent<CharacterControllerComponent>(loaded);
			context.Expect(constraints != nullptr && character != nullptr, "Valid typed physics JSON must restore both components.");
			context.ExpectEqual(constraintJson["constraints"][0]["recordId"].get<std::string>(), TerraEngine::ToHexString(constraints->constraints[0].recordId), "Constraint record UUIDs must round-trip exactly.");
			context.Expect(constraints->constraints[0].kind == PhysicsConstraintKind::Hinge && constraints->constraints[1].kind == PhysicsConstraintKind::Slider
				&& constraints->constraints[2].kind == PhysicsConstraintKind::Gear,
				"Constraint record order must remain stable after load.");
			context.Expect(constraints->constraints[0].breakTorque == std::numeric_limits<float>::infinity(), "Infinity threshold must restore as the provider-neutral unbreakable value.");
			context.Expect(constraints->constraints[0].axisLimits[0].minimum == -1.0F
				&& constraints->constraints[0].axisMotors[2].mode == PhysicsConstraintMotorMode::Position,
				"Ordered SixDOF settings must round-trip without provider-specific layouts.");
			context.Expect(constraints->constraints[2].firstGearConstraintId == constraints->constraints[0].recordId
				&& constraints->constraints[2].secondGearConstraintId == constraints->constraints[1].recordId,
				"Gear references must round-trip as stable authored record UUIDs.");
			context.Expect(character->shapeKind == PhysicsCharacterShapeKind::Cylinder && character->rotationPolicy == PhysicsCharacterRotationPolicy::Locked,
				"Character enum authoring must round-trip through the typed schema.");
		});

		registry.Add("Physics.Serialization.ConstraintAndCharacterDefaultsAndRuntimeStateAreExcluded", [](TestContext& context) {
			ResetSerializationFixture();
			ObjectManager& objects = ObjectManager::instance();
			Object object = objects.createObject("Physics Defaults");
			objects.AddComponent<ConstraintSetComponent>(object);
			objects.AddComponent<CharacterControllerComponent>(object);
			const nlohmann::json saved = Serializer::SerializeObject(object);
			const nlohmann::json& constraints = saved["components"]["ConstraintSetComponent"];
			const nlohmann::json& character = saved["components"]["CharacterControllerComponent"];
			context.Expect(constraints["constraints"].empty(), "Default constraint sets may have no authored records.");
			context.ExpectEqual(std::string("Capsule"), character["shapeKind"].get<std::string>(), "Default character shape must be persisted.");
			for(const char* transient : {"broken", "desiredMovement", "desiredJump", "desiredResize", "grounded", "supportVelocity", "supportContact", "contacts"}) {
				context.Expect(!constraints.contains(transient) && !character.contains(transient), "Transient constraint and character state must never enter scene JSON.");
			}
		});

		registry.Add("Physics.Serialization.ConstraintAndCharacterRejectMalformedDataAtomically", [](TestContext& context) {
			nlohmann::json malformedConstraint = SerializeFixtureWithPhysics();
			malformedConstraint["components"]["ConstraintSetComponent"]["constraints"][0]["kind"] = "ProviderConstraint";
			ResetSerializationFixture();
			Object invalidConstraint = Serializer::DeserializeObject(malformedConstraint);
			context.Expect(ObjectManager::instance().GetComponent<ConstraintSetComponent>(invalidConstraint) == nullptr, "Unknown constraint enums must reject the complete temporary component instead of installing partial state.");
			context.Expect(ObjectManager::instance().GetComponent<CharacterControllerComponent>(invalidConstraint) != nullptr, "A rejected constraint payload must not corrupt an independent valid component.");

			nlohmann::json malformedUuid = SerializeFixtureWithPhysics();
			malformedUuid["components"]["ConstraintSetComponent"]["constraints"][0]["firstObjectId"] = "not-a-uuid";
			ResetSerializationFixture();
			Object invalidUuid = Serializer::DeserializeObject(malformedUuid);
			context.Expect(ObjectManager::instance().GetComponent<ConstraintSetComponent>(invalidUuid) == nullptr, "Malformed endpoint UUIDs must reject a constraint payload without a partial record.");

			nlohmann::json unsupportedSchema = SerializeFixtureWithPhysics();
			unsupportedSchema["components"]["ConstraintSetComponent"]["schemaVersion"] = 99U;
			ResetSerializationFixture();
			Object invalidSchema = Serializer::DeserializeObject(unsupportedSchema);
			context.Expect(ObjectManager::instance().GetComponent<ConstraintSetComponent>(invalidSchema) == nullptr, "Unknown constraint schemas must reject the temporary component instead of applying a partial migration.");

			nlohmann::json malformedCharacter = SerializeFixtureWithPhysics();
			malformedCharacter["components"]["CharacterControllerComponent"]["rotationPolicy"] = "ProviderPolicy";
			ResetSerializationFixture();
			Object invalidCharacter = Serializer::DeserializeObject(malformedCharacter);
			context.Expect(ObjectManager::instance().GetComponent<CharacterControllerComponent>(invalidCharacter) == nullptr, "Unknown character enums must fail before replacing component state.");
			context.Expect(ObjectManager::instance().GetComponent<ConstraintSetComponent>(invalidCharacter) != nullptr, "A rejected character payload must leave independently valid constraint data available.");

			nlohmann::json nonfiniteCharacter = SerializeFixtureWithPhysics();
			nonfiniteCharacter["components"]["CharacterControllerComponent"]["radius"] = std::numeric_limits<float>::infinity();
			ResetSerializationFixture();
			Object invalidFiniteCharacter = Serializer::DeserializeObject(nonfiniteCharacter);
			context.Expect(ObjectManager::instance().GetComponent<CharacterControllerComponent>(invalidFiniteCharacter) == nullptr, "Nonfinite character configuration must fail validation before replacing component state.");
		});
	}
}
