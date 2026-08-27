// TerrainLayerWeightTests.cpp -- stable terrain layers and canonical weight codec coverage.

#include "TestHarness.h"

#include "Terrain/TerrainLayerStack.h"
#include "Terrain/TerrainMaterial.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

		registry.Add("Terrain.Layers.ResolvedContributorsNormalizeToOne", [](TestContext& context) {
			Terrain::TerrainLayerStack stack{};
			stack.generation = 1u;
			stack.layers = {{1u, "Rock"}, {2u, "Grass"}, {3u, "Mud"}, {4u, "Snow"}};
			const Terrain::TerrainWeightSample authored = Terrain::NormalizeTerrainWeights(
				std::vector<Terrain::TerrainWeightContribution>{{1u, 128u}, {2u, 64u}, {3u, 32u}, {4u, 32u}, {5u, 16u}});
			const Terrain::TerrainResolvedWeightSample resolved = Terrain::ResolveTerrainWeightSample(authored, stack);
			double total = 0.0;
			for(std::size_t index = 0u; index < resolved.contributorCount; ++index)
				total += resolved.contributors[index].normalizedWeight;
			context.Expect(resolved.contributorCount == 4u && !resolved.usedDiagnosticFallback
				&& std::abs(total - 1.0) <= 1.0e-6
				&& resolved.contributors[0].stableLayerId == 1u,
				"The four retained stable-ID contributors must reach shading as a deterministic unit-sum value without renderer truncation.");
		});

		registry.Add("Terrain.Layers.UnknownAndDisabledIdsUseOneDiagnosticFallback", [](TestContext& context) {
			Terrain::TerrainLayerStack stack{};
			stack.generation = 1u;
			stack.layers = {{2u, "Grass"}, {3u, "DisabledMud"}};
			stack.layers[1].enabled = false;
			const Terrain::TerrainWeightSample authored = Terrain::NormalizeTerrainWeights(
				std::vector<Terrain::TerrainWeightContribution>{{2u, 100u}, {3u, 50u}, {99u, 50u}});
			const Terrain::TerrainResolvedWeightSample resolved = Terrain::ResolveTerrainWeightSample(authored, stack);
			const auto diagnostic = std::find_if(resolved.contributors.begin(),
				resolved.contributors.begin() + static_cast<std::ptrdiff_t>(resolved.contributorCount),
				[](const Terrain::TerrainResolvedWeightContributor& contributor) {
					return contributor.diagnosticFallback;
				});
			const auto healthy = std::find_if(resolved.contributors.begin(),
				resolved.contributors.begin() + static_cast<std::ptrdiff_t>(resolved.contributorCount),
				[](const Terrain::TerrainResolvedWeightContributor& contributor) {
					return contributor.stableLayerId == 2u;
				});
			context.Expect(resolved.usedDiagnosticFallback && resolved.contributorCount == 2u
				&& diagnostic != resolved.contributors.begin() + static_cast<std::ptrdiff_t>(resolved.contributorCount)
				&& diagnostic->stableLayerId == Terrain::kTerrainDiagnosticFallbackLayerId
				&& healthy != resolved.contributors.begin() + static_cast<std::ptrdiff_t>(resolved.contributorCount)
				&& std::abs(diagnostic->normalizedWeight - healthy->normalizedWeight) <= (1.0 / 255.0 + 1.0e-6),
				"Unknown and disabled painted IDs must combine into one visible diagnostic contributor while retaining healthy neighboring weight.");
		});

		registry.Add("Terrain.Layers.ZeroWeightUsesDeterministicDiagnosticFallback", [](TestContext& context) {
			Terrain::TerrainLayerStack stack{};
			stack.generation = 1u;
			stack.layers = {{7u, "FallbackValid"}};
			const Terrain::TerrainResolvedWeightSample resolved = Terrain::ResolveTerrainWeightSample({}, stack);
			context.Expect(resolved.usedDiagnosticFallback && resolved.contributorCount == 1u
				&& resolved.contributors[0].diagnosticFallback
				&& resolved.contributors[0].stableLayerId == Terrain::kTerrainDiagnosticFallbackLayerId
				&& std::abs(resolved.contributors[0].normalizedWeight - 1.0) <= 1.0e-6,
				"A zero-total control sample must select a deterministic visible diagnostic instead of producing undefined zero shading.");
		});

		registry.Add("Terrain.Layers.ControlUvIgnoresSurfaceLayerMapping", [](TestContext& context) {
			const glm::dvec2 localSample{37.0, 211.0};
			const glm::dvec2 controlBefore = Terrain::ComputeTerrainControlUv(localSample);
			Terrain::TerrainLayerMapping mapping{};
			mapping.tileSizeXMetres = 1.25;
			mapping.tileSizeZMetres = 13.0;
			mapping.offsetXMetres = -8.0;
			mapping.offsetZMetres = 19.0;
			mapping.rotationRadians = 0.83;
			Terrain::TerrainAreaMaterialPhase phase{};
			const auto prepared = Terrain::PrepareTerrainAreaMaterialPhase({-7, 11}, 0.5, mapping, phase);
			const glm::dvec2 surfaceUv = Terrain::EvaluateTerrainSurfaceUv(phase, mapping, localSample);
			const glm::dvec2 controlAfter = Terrain::ComputeTerrainControlUv(localSample);
			context.Expect(prepared.Succeeded() && controlBefore == controlAfter && surfaceUv != controlAfter
				&& std::abs(controlAfter.x - 37.0 / 256.0) <= 1.0e-12
				&& std::abs(controlAfter.y - 211.0 / 256.0) <= 1.0e-12,
				"Control IDs and weights must remain area-local while physical material mapping changes tile, offset, and rotation.");
		});
	}
}
