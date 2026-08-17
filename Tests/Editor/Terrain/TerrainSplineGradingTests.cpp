// TerrainSplineGradingTests.cpp -- Exact cross-area procedural spline contracts.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainSplineGrader.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainHydraulicErosion.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainThermalErosion.h"

#include <atomic>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace TerraEngine::Editor::Terrain;
		using namespace TerraEngine::Terrain;

		TerrainProceduralArea MakeArea(TerrainAreaCoordinate coordinate, std::int32_t height = 10000)
		{
			TerrainProceduralArea area; area.coordinate = coordinate; area.observedGeneration = 7u; area.sourceHash = {11u, 12u}; area.heightMm.assign(kTerrainAreaSampleCount, height); return area;
		}
		TerrainSplineControlPoint Point(double x, double y, double z)
		{
			TerrainSplineControlPoint point; point.position.offsetMetres = {x, y, z}; return point;
		}
		TerrainSplineGradeRequest RoadRequest()
		{
			TerrainSplineGradeRequest request; request.terrain.areas = {MakeArea({0, 0}), MakeArea({1, 0})}; request.settings.sampleSpacingMetres = 1.0; request.settings.widthMetres = 8.0; request.settings.shoulderWidthMetres = 4.0; request.settings.falloffMetres = 8.0; request.settings.bankHeightMm = 500; request.controlPoints = {Point(0.0, 20.0, 128.0), Point(512.0, 20.0, 128.0)}; return request;
		}
	}

	void RegisterTerrainSplineGradingTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainSpline.CrossAreaRoadIsContinuousAndDeterministic", [](TestContext& context) {
			const TerrainSplineGradeRequest request = RoadRequest();
			const TerrainProceduralResult first = ApplyTerrainSplineGrade(request); const TerrainProceduralResult second = ApplyTerrainSplineGrade(request);
			const std::size_t left = 128u * kTerrainAreaExtentSamples + 255u; const std::size_t right = 128u * kTerrainAreaExtentSamples;
			context.Expect(first.Succeeded() && second.Succeeded() && first.outputHash == second.outputHash && first.areas[0].heightMm[left] == 20000 && first.areas[1].heightMm[right] == 20000 && first.areas[0].heightMm[0] == 10000,
				"A canonical-world road must grade neighbouring areas continuously, retain samples outside its falloff, and produce the same immutable output identity.");
		});

		registry.Add("Editor.TerrainSpline.RiverUsesDepthAndSelectionMasks", [](TestContext& context) {
			TerrainSplineGradeRequest request = RoadRequest(); request.settings.kind = TerrainSplineGradeKind::River; request.settings.riverDepthMm = 1500; request.terrain.areas[0].selectionMask.assign(kTerrainAreaSampleCount, 0u);
			const std::size_t centre = 128u * kTerrainAreaExtentSamples + 128u; request.terrain.areas[0].selectionMask[centre] = 255u;
			const TerrainProceduralResult result = ApplyTerrainSplineGrade(request);
			context.Expect(result.Succeeded() && result.areas[0].heightMm[centre] == 18500 && result.areas[0].heightMm[centre - 1u] == 10000,
				"River grading must lower selected samples by its depth while leaving unselected TerrainSelectionQuery-mask samples unchanged.");
		});

		registry.Add("Editor.TerrainSpline.CancellationAndLargeWorldConversionFailClosed", [](TestContext& context) {
			std::atomic_bool cancelled = true; TerrainSplineGradeRequest request = RoadRequest(); request.terrain.cancellation = &cancelled;
			TerraEngine::Scene::WorldPosition distant; distant.cell = {1'000'000, 0, -1'000'000}; distant.offsetMetres = {12.0, 0.0, -12.0};
			const auto sample = TerrainWorldToSample(distant, 1.0); const TerrainProceduralResult result = ApplyTerrainSplineGrade(request);
			context.Expect(sample.has_value() && sample->x == 65'536'000'012LL && sample->z == -65'536'000'012LL && result.code == TerrainResultCode::Cancelled,
				"Spline work must preserve signed million-cell coordinates and stop at a cancellation checkpoint before publication.");
		});

		registry.Add("Editor.TerrainSpline.NegativeLatticeNoiseAndOversizedStampFailClosed", [](TestContext& context) {
			TerrainProceduralRequest request; request.areas = {MakeArea({-1, -1}, 0)};
			const TerrainProceduralResult first = ApplyTerrainSeededNoise(request, {91u, 50, 32u});
			const TerrainProceduralResult second = ApplyTerrainSeededNoise(request, {91u, 50, 32u});
			TerrainStampSettings invalidStamp; invalidStamp.radiusSamples = 3'037'000'500u;
			const TerrainProceduralResult rejected = ApplyTerrainStamp(request, invalidStamp);
			context.Expect(first.Succeeded() && second.Succeeded() && first.outputHash == second.outputHash && rejected.code == TerrainResultCode::InvalidArgument,
				"Negative canonical lattice noise must remain deterministic and an unrepresentable stamp radius must reject before any immutable output is published.");
		});

		registry.Add("Editor.TerrainSpline.ErosionNeverLeaksOutsideSelectionMask", [](TestContext& context) {
			TerrainProceduralRequest request; request.areas = {MakeArea({0, 0}, 0)}; const std::size_t centre = 128u * kTerrainAreaExtentSamples + 128u;
			request.areas[0].heightMm[centre] = 1000; request.areas[0].selectionMask.assign(kTerrainAreaSampleCount, 0u); request.areas[0].selectionMask[centre] = 255u;
			const TerrainProceduralResult hydraulic = ApplyTerrainHydraulicErosion(request, {}); const TerrainProceduralResult thermal = ApplyTerrainThermalErosion(request, {});
			context.Expect(hydraulic.Succeeded() && thermal.Succeeded() && hydraulic.areas[0].heightMm == request.areas[0].heightMm && thermal.areas[0].heightMm == request.areas[0].heightMm,
				"A one-cell selection has no selected transfer destination, so both erosion kernels must retain every sample outside the declared mask.");
		});
	}
}
