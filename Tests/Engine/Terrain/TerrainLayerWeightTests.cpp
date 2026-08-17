// TerrainLayerWeightTests.cpp -- stable terrain layers and canonical weight codec coverage.

#include "TestHarness.h"

#include "Terrain/TerrainLayerStack.h"

#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainLayerWeightTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Layers.StableMetadataSurvivesPresentationMoves", [](TestContext& context) {
			Terrain::TerrainLayerStack stack{};
			stack.generation = 1u;
			stack.layers = {{1u, "Rock"}, {2u, "Grass"}};
			Terrain::TerrainLayerPatch move{}; move.kind = Terrain::TerrainLayerPatchKind::Move; move.expectedGeneration = 1u; move.targetStableId = 2u; move.destinationIndex = 0u;
			Terrain::TerrainLayerStack moved{};
			const auto result = Terrain::ApplyTerrainLayerPatch(stack, move, moved);
			context.Expect(result.Succeeded() && moved.layers[0].stableId == 2u && moved.layers[1].stableId == 1u && moved.generation == 2u,
				"Layer presentation order may change without changing stable painted layer identities.");
		});

		registry.Add("Terrain.Layers.NormalizesFourSlotsAndRoundTripsCanonicalCodec", [](TestContext& context) {
			const std::vector<Terrain::TerrainWeightContribution> contributions = {{5u, 2u}, {2u, 2u}, {8u, 1u}, {3u, 1u}, {9u, 1u}};
			const Terrain::TerrainWeightSample sample = Terrain::NormalizeTerrainWeights(contributions);
			std::array<std::uint8_t, 12u> bytes{};
			Terrain::TerrainWeightSample decoded{};
			const auto encoded = Terrain::EncodeTerrainWeightSample(sample, bytes);
			const auto decodedResult = Terrain::DecodeTerrainWeightSample(bytes, decoded);
			std::uint32_t total = 0u; for (const auto& slot : sample.slots) total += slot.weight;
			context.Expect(sample.IsCanonical() && total == 255u && sample.slots[0].layerId == 2u && sample.slots[3].layerId != 9u,
				"Canonical weights retain four contributors, break equal ties by stable layer id, and displace the fifth contributor.");
			context.Expect(encoded.Succeeded() && decodedResult.Succeeded() && decoded == sample,
				"Canonical four-slot weights must round-trip through the .tearea group record without changing bytes.");
		});
	}
}
