// TerrainProceduralOperationTests.cpp -- bounded deterministic operation coverage.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainHydraulicErosion.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainThermalErosion.h"
#include "Terrain/TerrainSelectionQuery.h"

#include <array>
#include <cstdint>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		Editor::Terrain::TerrainProceduralRequest MakeProceduralRequest(std::int32_t height = 0)
		{
			Editor::Terrain::TerrainProceduralArea area;
			area.coordinate = {-1, -1};
			area.observedGeneration = 3u;
			area.sourceHash = {7u, 9u};
			area.heightMm.assign(Terrain::kTerrainAreaSampleCount, height);
			Editor::Terrain::TerrainProceduralRequest request;
			request.areas.push_back(std::move(area));
			return request;
		}
	}

	void RegisterTerrainProceduralOperationTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Procedural.SelectionCombinesCanonicalCriteriaExactly", [](TestContext& context) {
			// The 5x5 storage includes a halo around a 3x3 evaluation region. Its
			// centre has a 2 m rise to the east, allowing an exact 2000-per-mille
			// slope comparison without reading another authored area.
			std::array<std::int32_t, 25u> heights{};
			heights[2u * 5u + 2u] = 1000;
			heights[2u * 5u + 3u] = 3000;
			std::array<std::uint8_t, 25u> painted{};
			painted[2u * 5u + 2u] = 220u;
			std::array<Terrain::TerrainWeightSample, 25u> weights{};
			weights[2u * 5u + 2u].slots = {{{7u, 200u}, {2u, 55u}, {}, {}}};

			Terrain::TerrainSelectionGrid grid{};
			grid.storageOrigin = {-1, -1};
			grid.storageWidth = 5u;
			grid.storageHeight = 5u;
			grid.evaluationBounds = {0, 0, 3, 3};
			grid.heightMm = heights;
			grid.paintedMask = painted;
			grid.weights = weights;
			Terrain::TerrainSelectionCriteria criteria{};
			criteria.worldBounds = Terrain::TerrainSampleBounds{1, 1, 2, 2};
			criteria.minimumHeightMm = 1000;
			criteria.maximumHeightMm = 1000;
			criteria.minimumSlopePermille = 2000u;
			criteria.maximumSlopePermille = 2000u;
			criteria.minimumPaintedMask = 200u;
			criteria.layerStableId = 7u;
			criteria.minimumLayerWeight = 128u;
			criteria.maximumEvaluatedSamples = 9u;

			const Terrain::TerrainSelectionResult result = Terrain::EvaluateTerrainSelection(grid, criteria);
			context.Expect(result.Succeeded() && result.selectedSampleCount == 1u && result.mask.size() == 9u && result.mask[4u] == 255u,
				"World, height, slope, painted-mask, and stable-layer criteria must combine without selecting an outside sample.");
			context.Expect(result.selectedBounds.has_value() && result.selectedBounds->minX == 1 && result.selectedBounds->minZ == 1
				&& result.selectedBounds->maxXExclusive == 2 && result.selectedBounds->maxZExclusive == 2,
				"Selection output bounds must be the exact half-open extent of selected canonical samples.");
		});

		registry.Add("Terrain.Procedural.SelectionRejectsBudgetsAndMissingChannels", [](TestContext& context) {
			const std::array<std::int32_t, 4u> heights{};
			Terrain::TerrainSelectionGrid grid{};
			grid.storageWidth = 2u;
			grid.storageHeight = 2u;
			grid.evaluationBounds = {0, 0, 2, 2};
			grid.heightMm = heights;
			Terrain::TerrainSelectionCriteria budgeted{};
			budgeted.maximumEvaluatedSamples = 3u;
			const Terrain::TerrainSelectionResult budgetResult = Terrain::EvaluateTerrainSelection(grid, budgeted);
			Terrain::TerrainSelectionCriteria missingLayer{};
			missingLayer.layerStableId = 4u;
			const Terrain::TerrainSelectionResult missingLayerResult = Terrain::EvaluateTerrainSelection(grid, missingLayer);
			context.Expect(budgetResult.code == Terrain::TerrainResultCode::BudgetExceeded && budgetResult.mask.empty(),
				"A procedural selection must reject its complete batch before allocation when its sample budget is exceeded.");
			context.Expect(missingLayerResult.code == Terrain::TerrainResultCode::InvalidArgument && missingLayerResult.mask.empty(),
				"Layer selection must fail closed when stable weight data is unavailable.");
		});

		registry.Add("Terrain.Procedural.HeightOperatorsAreDeterministicAndSelectionBounded", [](TestContext& context) {
			using namespace Editor::Terrain;
			auto request = MakeProceduralRequest(-1500);
			request.areas[0].selectionMask.assign(Terrain::kTerrainAreaSampleCount, 0u);
			request.areas[0].selectionMask[0u] = 255u;
			TerrainHeightRemapSettings remap;
			remap.multiplyNumerator = 2;
			remap.addMm = 500;
			const TerrainProceduralResult remapped = ApplyTerrainHeightRemap(request, remap);
			TerrainTerraceSettings terrace;
			terrace.stepMm = 1000;
			terrace.blendPercent = 100u;
			const TerrainProceduralResult terraced = ApplyTerrainTerrace(request, terrace);
			const TerrainProceduralResult noiseA = ApplyTerrainSeededNoise(request, {42u, 75, 16u});
			const TerrainProceduralResult noiseB = ApplyTerrainSeededNoise(request, {42u, 75, 16u});
			context.Expect(remapped.Succeeded() && remapped.areas[0].heightMm[0u] == -2500 && remapped.areas[0].heightMm[1u] == -1500,
				"Height remap must modify only selected samples and retain every outside value exactly.");
			context.Expect(terraced.Succeeded() && terraced.areas[0].heightMm[0u] == -2000 && terraced.areas[0].heightMm[1u] == -1500,
				"Negative terraces must use mathematical floor steps without leaking beyond the selection mask.");
			context.Expect(noiseA.Succeeded() && noiseB.Succeeded() && noiseA.outputHash == noiseB.outputHash
				&& noiseA.areas[0].heightMm[1u] == -1500,
				"Seeded noise on a negative canonical lattice must be byte-deterministic and selection bounded.");
		});

		registry.Add("Terrain.Procedural.StampAndErosionPreserveEveryOutsideSample", [](TestContext& context) {
			using namespace Editor::Terrain;
			auto request = MakeProceduralRequest(0);
			const std::size_t centre = 128u * Terrain::kTerrainAreaExtentSamples + 128u;
			request.areas[0].heightMm[centre] = 1000;
			request.areas[0].selectionMask.assign(Terrain::kTerrainAreaSampleCount, 0u);
			// A selected 3x3 patch gives erosion a legal transfer destination while
			// leaving its immediate outside ring available for leak detection.
			for(std::size_t z = 127u; z <= 129u; ++z)
				for(std::size_t x = 127u; x <= 129u; ++x)
					request.areas[0].selectionMask[z * Terrain::kTerrainAreaExtentSamples + x] = 255u;
			TerrainStampSettings stamp;
			stamp.centre = {-128, -128};
			stamp.radiusSamples = 2u;
			stamp.heightDeltaMm = 250;
			const TerrainProceduralResult stamped = ApplyTerrainStamp(request, stamp);
			const TerrainProceduralResult hydraulicA = ApplyTerrainHydraulicErosion(request, {});
			const TerrainProceduralResult hydraulicB = ApplyTerrainHydraulicErosion(request, {});
			const TerrainProceduralResult thermalA = ApplyTerrainThermalErosion(request, {});
			const TerrainProceduralResult thermalB = ApplyTerrainThermalErosion(request, {});
			const std::size_t outside = 126u * Terrain::kTerrainAreaExtentSamples + 128u;
			context.Expect(stamped.Succeeded() && stamped.areas[0].heightMm[centre] == 1250 && stamped.areas[0].heightMm[outside] == 0,
				"A canonical stamp must affect its selected bounded footprint and no outside sample.");
			context.Expect(hydraulicA.Succeeded() && hydraulicB.Succeeded() && hydraulicA.outputHash == hydraulicB.outputHash
				&& hydraulicA.areas[0].heightMm[outside] == request.areas[0].heightMm[outside],
				"Hydraulic-style erosion must be deterministic and cannot deposit outside its declared selection.");
			context.Expect(thermalA.Succeeded() && thermalB.Succeeded() && thermalA.outputHash == thermalB.outputHash
				&& thermalA.areas[0].heightMm[outside] == request.areas[0].heightMm[outside],
				"Thermal-style erosion must be deterministic and cannot deposit outside its declared selection.");
			TerrainStampSettings oversized;
			oversized.radiusSamples = 3'037'000'500u;
			context.Expect(ApplyTerrainStamp(request, oversized).code == Terrain::TerrainResultCode::InvalidArgument,
				"A stamp whose squared radius cannot be represented must reject before allocating output.");
		});
	}
}
