// Contract tests for backend-neutral constraint collection authoring.
// Runtime simulation, break events, and provider construction are deliberately
// covered by later tasks; these tests keep durable scene records independent.

#include "../TestHarness.h"

#include "Components/Physics/ConstraintSetComponent.h"

#include <type_traits>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Physics;

	void RegisterPhysicsConstraintComponentTests(TestRegistry& registry)
	{
		registry.Add("Physics.ConstraintComponent.UsesCRTPStableReflectionAndRecordIDs", [](TestContext& context) {
			context.Expect((std::is_base_of_v<Component<ConstraintSetComponent>, ConstraintSetComponent>),
				"ConstraintSetComponent must derive from Component<ConstraintSetComponent>.");
			context.ExpectEqual(std::string_view("ConstraintSetComponent"), ConstraintSetComponent::kComponentName,
				"Constraint set name is a provider-neutral authored contract.");
			ConstraintSetComponent::registerType();
			context.Expect(ComponentAccessRegistry().contains("ConstraintSetComponent"), "Constraint set must self-register for reflection access.");

			ConstraintSetComponent set;
			const TerraEngine::UUID firstRecordId = set.Add().recordId;
			const TerraEngine::UUID secondRecordId = set.Add().recordId;
			context.Expect(firstRecordId != TerraEngine::EMPTY_UUID && secondRecordId != TerraEngine::EMPTY_UUID,
				"New constraint records require stable non-empty IDs.");
			context.Expect(firstRecordId != secondRecordId, "Repeated constraints require independent stable IDs.");
			context.Expect(set.constraints[0].kind == PhysicsConstraintKind::Fixed,
				"Tagged records must preserve an engine-owned default constraint kind.");
		});

		registry.Add("Physics.ConstraintComponent.ValidatesEndpointsRangesAndDuplicateRecordsDeterministically", [](TestContext& context) {
			ConstraintSetComponent set;
			ConstraintRecord valid;
			valid.firstObjectId = TerraEngine::GenerateUUID();
			set.Add(valid);
			ConstraintRecord sameKind = valid;
			sameKind.recordId = TerraEngine::GenerateUUID();
			sameKind.firstObjectId = TerraEngine::GenerateUUID();
			set.Add(sameKind);
			context.Expect(set.Validate().accepted, "Repeated same-kind records with distinct IDs must remain valid.");
			const ConstraintRecord serializerFriendlyCopy = set.constraints.front();
			context.Expect(serializerFriendlyCopy.recordId == valid.recordId
				&& serializerFriendlyCopy.firstObjectId == valid.firstObjectId
				&& serializerFriendlyCopy.kind == valid.kind,
				"Copying an authored record must preserve its stable ID, UUID endpoint, and tagged kind for serialization.");

			ConstraintRecord duplicate = valid;
			set.Add(duplicate);
			const ConstraintSetValidationResult duplicateResult = set.Validate();
			context.Expect(!duplicateResult.accepted && duplicateResult.code == ConstraintSetValidationCode::DuplicateRecordId,
				"The first duplicate in authored order must be rejected as a duplicate record ID.");
			context.ExpectEqual(static_cast<std::size_t>(2), duplicateResult.recordIndex,
				"Duplicate validation must identify the deterministic later record.");

			set.constraints.pop_back();
			set.constraints.front().limits.enabled = true;
			set.constraints.front().limits.minimum = 3.0F;
			set.constraints.front().limits.maximum = 2.0F;
			context.Expect(set.Validate().code == ConstraintSetValidationCode::InvalidLimits,
				"Enabled limits must reject inverted ranges before a physics rebuild.");
			set.constraints.front().limits = {};
			set.constraints.front().secondEndpointKind = PhysicsConstraintEndpointKind::ConnectedObject;
			set.constraints.front().secondObjectId = set.constraints.front().firstObjectId;
			context.Expect(set.Validate().code == ConstraintSetValidationCode::InvalidEndpoint,
				"A record cannot connect an object to itself through two object endpoints.");

			set.constraints.front().secondEndpointKind = PhysicsConstraintEndpointKind::WorldAnchor;
			set.constraints.front().secondObjectId = TerraEngine::EMPTY_UUID;
			set.constraints.front().kind = PhysicsConstraintKind::SixDegreesOfFreedom;
			set.constraints.front().axisLimits[5].enabled = true;
			set.constraints.front().axisLimits[5].minimum = 1.0F;
			set.constraints.front().axisLimits[5].maximum = -1.0F;
			context.Expect(set.Validate().code == ConstraintSetValidationCode::InvalidLimits,
				"Every SixDOF axis limit must reject an inverted range.");
		});
	}
}
