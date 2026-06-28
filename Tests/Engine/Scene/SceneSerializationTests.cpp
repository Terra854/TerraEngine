#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/Serializer.h"

namespace TerraEngine::Tests {
	void RegisterSceneSerializationTests(TestRegistry& registry)
	{
		registry.Add("Scene.Serialization.InactiveAuthoredStateRoundTrips", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("Inactive Authored");
			objects.setObjectActive(object, false);

			nlohmann::json serialized = Serializer::SerializeObject(object);
			objects.clear();
			Object loaded = Serializer::DeserializeObject(serialized, std::nullopt);

			context.Expect(serialized.value("active", true) == false, "Serialized object JSON should preserve authored inactive state.");
			context.Expect(!objects.isObjectActive(loaded), "Deserialized object should restore authored inactive state.");
			context.Expect(objects.getObjectsWithComponents<Transform>().empty(), "Runtime active queries should exclude the restored inactive object.");
			context.ExpectEqual(static_cast<std::size_t>(1), objects.getObjectsWithComponentsIncludingInactive<Transform>().size(), "Including-inactive queries should still expose the restored object.");
			objects.clear();
		});
	}
}
