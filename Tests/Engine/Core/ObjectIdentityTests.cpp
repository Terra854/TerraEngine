#include "../TestHarness.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"

#include <entt/entity/entity.hpp>
#include <type_traits>

namespace TerraEngine::Tests {
	void RegisterObjectIdentityTests(TestRegistry& registry)
	{
		registry.Add("Engine.ObjectIdentity.IsCompactAndCopyable", [](TestContext& context) {
			context.Expect(sizeof(Object) <= 16, "Object identities should remain small enough for snapshots.");
			context.Expect(std::is_trivially_copyable_v<Object>, "Object identities should remain trivially copyable for worker snapshots.");
			context.Expect(TerraEngine::IsObjectIdentityWithinFootprint(), "Footprint helper should enforce the compact snapshot target.");
			context.ExpectEqual(sizeof(Object), TerraEngine::ObjectIdentityFootprintBytes(), "Footprint helper should report the real type size.");
			context.ExpectEqual(static_cast<std::size_t>(16), TerraEngine::kObjectIdentityTargetBytes, "Object identity target should stay at the public footprint.");
			Object reference;
			reference.entityValue = 7;
			reference.generation = 11;
			Object copy = reference;
			context.Expect(reference == copy, "Object identities should be trivially copyable value types.");
		});

		registry.Add("Engine.ObjectIdentity.ValidatesAndRejectsDestroyedObjects", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Reference");
			Object reference = objects.makeObjectHandle(object.EntityId());
			context.Expect(objects.validateObjectHandle(reference).validity == TerraEngine::ObjectIdentityValidity::Valid, "Fresh reference should validate.");
			objects.removeObject(object);
			context.Expect(objects.validateObjectHandle(reference).validity != TerraEngine::ObjectIdentityValidity::Valid, "Destroyed object reference should not validate.");
			objects.clear();
		});

		registry.Add("Engine.ObjectIdentity.EmptyHandleFailsClosed", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			const Object empty;
			const TerraEngine::ObjectIdentityResolution resolution = objects.validateObjectHandle(empty);

			context.Expect(empty.IsEmpty(), "Default compact references should be classified as empty.");
			context.Expect(!TerraEngine::IsResolvedObjectIdentity(resolution), "Default compact references should fail closed.");
			context.Expect(resolution.validity == TerraEngine::ObjectIdentityValidity::Missing, "Default compact references should be reported as missing.");
			objects.clear();
		});

		registry.Add("Engine.ObjectIdentity.RejectsGenerationMismatch", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("GenerationMismatch");
			Object reference = objects.makeObjectHandle(object.EntityId());
			reference.generation = reference.generation == 0 ? 1 : reference.generation + 1;

			const TerraEngine::ObjectIdentityResolution resolution = objects.validateObjectHandle(reference);

			context.Expect(!TerraEngine::IsResolvedObjectIdentity(resolution), "Generation-mismatched compact references should fail closed.");
			context.Expect(resolution.validity == TerraEngine::ObjectIdentityValidity::Stale, "Generation mismatch should be reported as stale.");
			objects.clear();
		});

		registry.Add("Engine.ObjectIdentity.ResolvesProtectedObjectHandle", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Facade");
			Object reference = objects.makeObjectHandle(object.EntityId());
			auto resolved = objects.resolveObjectHandle(reference);
			context.Expect(resolved.has_value(), "Valid object identity should resolve to Object.");
			context.ExpectEqual(entt::to_integral(object.EntityId()), entt::to_integral(resolved->EntityId()), "Resolved object should target the same entity.");
			context.Expect(TerraEngine::FormatObjectIdentity(reference).find("generation=") != std::string::npos, "Formatted reference should include generation.");
			objects.clear();
		});

		registry.Add("Engine.ObjectIdentity.ReloadedSceneInvalidatesOldEntityGeneration", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object original = objects.createObject("Reloaded");
			const TerraEngine::UUID stableId = objects.getUUID(original);
			const Object beforeReload = objects.makeObjectHandle(original.EntityId());
			objects.clear();

			Object reloaded = objects.createObject("Reloaded");
			objects.UpdateObjectUUID(reloaded, stableId);
			const Object afterReload = objects.makeObjectHandle(reloaded.EntityId());

			// Scene reloads can intentionally keep stable UUIDs while replacing
			// the underlying ECS entity. Compact references therefore include
			// entity generation so stale snapshots fail closed after reload.
			context.Expect(beforeReload.stableIdHash == afterReload.stableIdHash, "Reload fixture should preserve the authored stable UUID fingerprint.");
			context.Expect(objects.validateObjectHandle(beforeReload).validity != TerraEngine::ObjectIdentityValidity::Valid, "Old compact reference should be stale after reload replacement.");
			context.Expect(objects.validateObjectHandle(afterReload).validity == TerraEngine::ObjectIdentityValidity::Valid, "New compact reference should validate after reload replacement.");
			objects.clear();
		});

		registry.Add("Engine.ObjectIdentity.RejectsStableUuidMismatch", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("UuidMismatch");
			Object reference = objects.makeObjectHandle(object.EntityId());
			reference.stableIdHash ^= 0x9e3779b97f4a7c15ull;
			if (reference.stableIdHash == 0)
			{
				reference.stableIdHash = 1;
			}

			const TerraEngine::ObjectIdentityResolution resolution = objects.validateObjectHandle(reference);

			context.Expect(!TerraEngine::IsResolvedObjectIdentity(resolution), "UUID-mismatched compact references should fail closed.");
			context.Expect(resolution.validity == TerraEngine::ObjectIdentityValidity::Stale, "UUID mismatch should be reported as stale.");
			context.Expect(TerraEngine::FormatObjectIdentity(reference).find("bytes=") != std::string::npos, "Formatted compact references should expose footprint diagnostics.");
			objects.clear();
		});
	}
}
