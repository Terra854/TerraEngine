// UnboundedSceneSerializationTests.cpp - strict authored coordinate contracts.

#include "../TestHarness.h"

#include "Scene/Serializer.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/WorldRegion.h"

namespace TerraEngine::Tests {
	void RegisterUnboundedSceneSerializationTests(TestRegistry& registry)
	{
		registry.Add("Engine.UnboundedSceneSerialization.ExactCellsRoundTripAndLegacyIsRejected", [](TestContext& context) {
			TerraEngine::Scene::WorldPosition position;
			position.cell = {INT64_MAX, INT64_MIN, -42};
			position.offsetMetres = {1.25, -2.5, 3.75};
			const nlohmann::json encoded = Serializer::SerializeWorldPosition(position);
			std::string error;
			const auto decoded = Serializer::DeserializeWorldPosition(encoded, &error);
			context.Expect(decoded.has_value() && decoded->cell == position.cell, "Decimal cell strings must preserve every signed int64 bit.");
			context.Expect(!Serializer::ValidateUnboundedSceneDocument({{"objects", nlohmann::json::array()}}, &error), "Unversioned scene input must not select a legacy decoder.");
			context.Expect(!Serializer::DeserializeWorldPosition({{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", {32768.0, 0.0, 0.0}}}, &error), "Non-normalized offsets must be rejected rather than repaired during loading.");
		});

		registry.Add("Engine.UnboundedSceneSerialization.RegionAndTransactionRequireStrictFormats", [](TestContext& context) {
			TerraEngine::Scene::WorldRegionArtifactManifest manifest;
			manifest.key = {-9, 12, 77}; manifest.contentHash = std::string(64U, 'a'); manifest.sourceGeneration = 5U;
			std::string error;
			context.Expect(TerraEngine::Scene::DeserializeWorldRegionArtifactManifest(TerraEngine::Scene::SerializeWorldRegionArtifactManifest(manifest), &error).has_value(), "A region artifact must retain decimal keys and SHA-256 lineage.");
			context.Expect(!TerraEngine::Scene::DeserializeWorldRegionArtifactManifest({{"format", "old"}}, &error), "Historic region formats must be rejected without migration.");
			context.Expect(!TerraEngine::Scene::LowerSpatialTransaction({{"format", "legacy"}}, &error), "Unversioned spatial transactions must never reach typed lowering.");
		});
	}
}
