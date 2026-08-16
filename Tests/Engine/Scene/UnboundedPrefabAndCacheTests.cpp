// UnboundedPrefabAndCacheTests.cpp - strict prefab and derived-cache lineage.

#include "../TestHarness.h"

#include "Scene/DerivedSceneCache.h"
#include "Scene/PrefabComposition.h"

namespace TerraEngine::Tests {
	void RegisterUnboundedPrefabAndCacheTests(TestRegistry& registry)
	{
		registry.Add("Engine.UnboundedPrefabAndCache.RejectsUnversionedPrefabAndWrongCacheLineage", [](TestContext& context) {
			std::string error;
			context.Expect(!TerraEngine::Scene::ValidateUnboundedPrefabDocument({{"objects", nlohmann::json::array()}}, &error), "Prefab loading must require TerraPrefabUnboundedV1.");
			TerraEngine::Scene::DerivedSceneEnvelope envelope;
			envelope.manifest.identity.formatLineage = "OtherFormat";
			TerraEngine::Scene::DerivedSceneCompatibilityIdentity expected;
			const auto validation = TerraEngine::Scene::ValidateDerivedSceneEnvelope(envelope, expected);
			context.Expect(!validation.accepted, "Derived cache data with another coordinate lineage must be disposable and rejected.");
		});
	}
}
