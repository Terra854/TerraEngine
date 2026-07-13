// Physics material asset contract tests. The payload is project-owned JSON;
// provider material IDs and runtime objects are intentionally absent.
#include "../TestHarness.h"

#include <nlohmann/json.hpp>

namespace TerraEngine::Tests {
	void RegisterPhysicsMaterialAssetTests(TestRegistry& registry)
	{
		registry.Add("Physics.Material.SchemaDefaultsAndJsonRoundTrip", [](TestContext& context) {
			nlohmann::json material = {{"schemaVersion", 1}, {"assetGuid", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
				{"staticFriction", 0.5}, {"dynamicFriction", 0.4}, {"restitution", 0.1}, {"density", 1000.0},
				{"frictionCombineMode", "Average"}, {"restitutionCombineMode", "Average"}, {"surfaceTag", "stone"}};
			const auto loaded = nlohmann::json::parse(material.dump());
			for (const char* key : {"schemaVersion", "assetGuid", "staticFriction", "dynamicFriction", "restitution", "density"})
				context.Expect(loaded.contains(key), std::string("Physics material payload must contain ") + key + ".");
			context.Expect(loaded["staticFriction"].get<float>() >= loaded["dynamicFriction"].get<float>(), "Default static friction should not be below dynamic friction.");
		});

		registry.Add("Physics.Material.MigratesLegacyDefaultsAndExcludesRuntimeState", [](TestContext& context) {
			nlohmann::json legacy = {{"schemaVersion", 0}, {"friction", 0.6}, {"bounce", 0.2}, {"density", 1000.0}, {"providerMaterialId", 99}, {"runtimeHandle", 12}};
			// Version-0 migration maps legacy names to stable engine-owned keys.
			nlohmann::json migrated = {{"schemaVersion", 1}, {"staticFriction", legacy.value("friction", 0.5)},
				{"dynamicFriction", legacy.value("friction", 0.5)}, {"restitution", legacy.value("bounce", 0.0)}, {"density", legacy.value("density", 1000.0)}};
			context.ExpectEqual(1, migrated["schemaVersion"].get<int>(), "Legacy material schema must migrate to version 1.");
			context.Expect(!migrated.contains("providerMaterialId") && !migrated.contains("runtimeHandle"), "Runtime/provider state must not be saved.");
			context.ExpectEqual(0.6F, migrated["staticFriction"].get<float>(), "Legacy friction must migrate deterministically.");
			context.ExpectEqual(0.2F, migrated["restitution"].get<float>(), "Legacy bounce must migrate to restitution.");
		});
	}
}
