// Contract tests for the provider-neutral physics authoring components.
// These tests intentionally use only the engine-owned names and fields from
// the physics design; provider handles and enum values must never leak here.
#include "../TestHarness.h"

#include "Core/Object/Component.h"

#if __has_include("Components/Physics/RigidBodyComponent.h")
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#define TERRAENGINE_PHYSICS_COMPONENTS_AVAILABLE 1
#endif

#include <type_traits>

namespace TerraEngine::Tests {

	void RegisterPhysicsComponentTests(TestRegistry& registry)
	{
		registry.Add("Physics.Components.UseCRTPStableNamesAndReflection", [](TestContext& context) {
#if TERRAENGINE_PHYSICS_COMPONENTS_AVAILABLE
			context.Expect((std::is_base_of_v<Component<RigidBodyComponent>, RigidBodyComponent>),
				"RigidBodyComponent must derive from Component<RigidBodyComponent>.");
			context.Expect((std::is_base_of_v<Component<ColliderSetComponent>, ColliderSetComponent>),
				"ColliderSetComponent must derive from Component<ColliderSetComponent>.");
			context.ExpectEqual(std::string_view("RigidBodyComponent"), RigidBodyComponent::kComponentName,
				"Rigid body name is an authored, provider-neutral contract.");
			context.ExpectEqual(std::string_view("ColliderSetComponent"), ColliderSetComponent::kComponentName,
				"Collider set name is an authored, provider-neutral contract.");
			RigidBodyComponent::registerType();
			ColliderSetComponent::registerType();
			context.Expect(ComponentAccessRegistry().contains("RigidBodyComponent"), "Rigid body must self-register.");
			context.Expect(ComponentAccessRegistry().contains("ColliderSetComponent"), "Collider set must self-register.");
#else
			context.Expect(false, "Physics component headers are not available yet (ImplementationCheck/ImplementationCheck contract pending).");
#endif
		});

		registry.Add("Physics.Components.DefaultsAndRepeatedColliderRecords", [](TestContext& context) {
#if TERRAENGINE_PHYSICS_COMPONENTS_AVAILABLE
			RigidBodyComponent body{};
			context.Expect(body.useGravity, "Rigid bodies use project gravity by default.");
			context.Expect(body.mass > 0.0F, "Default rigid-body mass must be positive.");
			ColliderSetComponent colliders{};
			context.Expect(colliders.colliders.empty(), "A new collider set starts with no authored records.");
			using Record = typename decltype(colliders.colliders)::value_type;
			Record first{};
			Record second{};
			first.recordId = TerraEngine::GenerateUUID();
			second.recordId = TerraEngine::GenerateUUID();
			colliders.colliders.push_back(first);
			colliders.colliders.push_back(second);
			context.ExpectEqual(static_cast<std::size_t>(2), colliders.colliders.size(),
				"Collection storage must preserve repeated collider records.");
			context.Expect(colliders.colliders[0].recordId != colliders.colliders[1].recordId,
				"Each repeated collider receives an independent stable record identity.");
#else
			context.Expect(false, "Physics component headers are not available yet (ImplementationCheck/ImplementationCheck contract pending).");
#endif
		});
	}
}
