#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptGlue.h"

#include <glm/vec3.hpp>

namespace TerraEngine::Tests {
	void RegisterUnboundedWorldManagedTests(TestRegistry& registry)
	{
		registry.Add("UnboundedWorld.Managed.CanonicalPoseUsesDeferredTypedTransaction", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			const Object object = objects.createObject("Managed canonical transform");

			NativeWorldPose requested{};
			requested.position = {{15'258'789'062LL, -15'258'789'063LL, 0LL}, 1.25, -2.5, 0.001};
			requested.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			requested.scale = {1.0F, 2.0F, 3.0F};
			const NativeRuntimeStatus queued = Native_Transform_SubmitWorldPose(
				static_cast<std::uint32_t>(object.EntityId()), &requested);

			context.Expect(queued == NativeRuntimeStatus::Accepted, "The exact managed pose should enter the typed transaction queue.");
			context.Expect(objects.GetCanonicalWorldPose(object)->position.cell.x == 0LL, "A managed write must remain invisible before the protected flush.");
			const TerraEngine::Scene::SceneMutationBatchResult publication =
				TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "managed-canonical-test");
			context.Expect(publication.applied, "The protected boundary should publish the managed transaction atomically.");

			NativeWorldPose observed{};
			context.Expect(Native_Transform_ReadWorldPose(static_cast<std::uint32_t>(object.EntityId()), &observed)
				== NativeRuntimeStatus::Success, "The managed ABI should read the published canonical pose.");
			context.Expect(observed.position.cell.x == requested.position.cell.x
				&& observed.position.cell.y == requested.position.cell.y
				&& observed.position.offsetX == requested.position.offsetX
				&& observed.position.offsetZ == requested.position.offsetZ,
				"Managed reads must preserve exact cells and bounded double offsets.");
			objects.clear();
		});

		registry.Add("UnboundedWorld.Managed.LegacyVectorAndMalformedCoordinatesAreRejected", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			const Object object = objects.createObject("Strict managed transform");

			const glm::vec3 legacyPosition(40.0F, 50.0F, 60.0F);
			Native_SetComponentProperty(
				static_cast<std::uint32_t>(object.EntityId()),
				"Transform",
				"Position",
				const_cast<glm::vec3*>(&legacyPosition),
				5);
			context.Expect(TerraEngine::Scene::SceneMutationQueue::Instance().Empty(), "A three-number Transform write must not create a compatibility mutation.");
			context.Expect(objects.GetCanonicalWorldPose(object)->position.cell == TerraEngine::Scene::WorldCell{}, "The rejected legacy write must leave live state unchanged.");

			NativeWorldPose malformed{};
			malformed.position = {{0LL, 0LL, 0LL}, TerraEngine::Scene::kWorldCellHalfEdgeMetres, 0.0, 0.0};
			malformed.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			malformed.scale = {1.0F, 1.0F, 1.0F};
			context.Expect(Native_Transform_SubmitWorldPose(static_cast<std::uint32_t>(object.EntityId()), &malformed)
				== NativeRuntimeStatus::InvalidArgument, "An unnormalized managed offset must be rejected instead of silently carried.");
			context.Expect(TerraEngine::Scene::SceneMutationQueue::Instance().Empty(), "Malformed canonical input must not leave partial queued work.");
			objects.clear();
		});
	}
}
