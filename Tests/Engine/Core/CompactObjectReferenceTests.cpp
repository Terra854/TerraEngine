#include "../TestHarness.h"

#include "Core/Object/CompactObjectReference.h"
#include "Core/Object/ObjectManager.h"

#include <entt/entity/entity.hpp>

namespace TerraEngine::Tests {
	void RegisterCompactObjectReferenceTests(TestRegistry& registry)
	{
		registry.Add("Engine.CompactObjectReference.IsCompactAndCopyable", [](TestContext& context) {
			context.Expect(sizeof(TerraEngine::CompactObjectReference) <= 40, "Compact object references should remain small enough for snapshots.");
			context.Expect(TerraEngine::IsCompactObjectReferenceWithinFootprint(), "Footprint helper should enforce the compact snapshot target.");
			context.ExpectEqual(sizeof(TerraEngine::CompactObjectReference), TerraEngine::CompactObjectReferenceFootprintBytes(), "Footprint helper should report the real type size.");
			TerraEngine::CompactObjectReference reference;
			reference.entityValue = 7;
			reference.generation = 11;
			TerraEngine::CompactObjectReference copy = reference;
			context.Expect(reference == copy, "Compact object references should be trivially copyable value types.");
		});

		registry.Add("Engine.CompactObjectReference.ValidatesAndRejectsDestroyedObjects", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Reference");
			TerraEngine::CompactObjectReference reference = objects.makeCompactReference(object.EntityId());
			context.Expect(objects.validateCompactReference(reference).validity == TerraEngine::CompactObjectReferenceValidity::Valid, "Fresh reference should validate.");
			objects.removeObject(object);
			context.Expect(objects.validateCompactReference(reference).validity != TerraEngine::CompactObjectReferenceValidity::Valid, "Destroyed object reference should not validate.");
			objects.clear();
		});

		registry.Add("Engine.CompactObjectReference.ResolvesProtectedObjectFacade", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Facade");
			TerraEngine::CompactObjectReference reference = objects.makeCompactReference(object.EntityId());
			auto resolved = objects.resolveCompactReference(reference);
			context.Expect(resolved.has_value(), "Valid compact reference should resolve to Object facade.");
			context.ExpectEqual(entt::to_integral(object.EntityId()), entt::to_integral(resolved->EntityId()), "Resolved facade should target the same entity.");
			context.Expect(TerraEngine::FormatCompactObjectReference(reference).find("generation=") != std::string::npos, "Formatted reference should include generation.");
			objects.clear();
		});

		registry.Add("Engine.CompactObjectReference.ReloadedSceneInvalidatesOldEntityGeneration", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object original = objects.createObject("Reloaded");
			const TerraEngine::UUID stableId = objects.getUUID(original.EntityId());
			const TerraEngine::CompactObjectReference beforeReload = objects.makeCompactReference(original.EntityId());
			objects.clear();

			Object reloaded = objects.createObject("Reloaded");
			objects.UpdateEntityUUID(reloaded.EntityId(), stableId);
			const TerraEngine::CompactObjectReference afterReload = objects.makeCompactReference(reloaded.EntityId());

			// Scene reloads can intentionally keep stable UUIDs while replacing
			// the underlying ECS entity. Compact references therefore include
			// entity generation so stale snapshots fail closed after reload.
			context.Expect(beforeReload.stableId == afterReload.stableId, "Reload fixture should preserve the authored stable UUID.");
			context.Expect(objects.validateCompactReference(beforeReload).validity != TerraEngine::CompactObjectReferenceValidity::Valid, "Old compact reference should be stale after reload replacement.");
			context.Expect(objects.validateCompactReference(afterReload).validity == TerraEngine::CompactObjectReferenceValidity::Valid, "New compact reference should validate after reload replacement.");
			objects.clear();
		});

		registry.Add("Engine.CompactObjectReference.RejectsStableUuidMismatch", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("UuidMismatch");
			TerraEngine::CompactObjectReference reference = objects.makeCompactReference(object.EntityId());
			reference.stableId = TerraEngine::GenerateUUID();

			const TerraEngine::CompactObjectReferenceResolution resolution = objects.validateCompactReference(reference);

			context.Expect(!TerraEngine::IsResolvedCompactObjectReference(resolution), "UUID-mismatched compact references should fail closed.");
			context.Expect(resolution.validity == TerraEngine::CompactObjectReferenceValidity::Stale, "UUID mismatch should be reported as stale.");
			context.Expect(TerraEngine::FormatCompactObjectReference(reference).find("bytes=") != std::string::npos, "Formatted compact references should expose footprint diagnostics.");
			objects.clear();
		});
	}
}
