// TerrainCoordinateTests.cpp -- Exact lattice edge cases for authored terrain.

#include "TestHarness.h"
#include "Components/TerrainComponent.h"
#include "Terrain/TerrainLayerStack.h"
#include "Terrain/TerrainMaterial.h"
#include "Terrain/TerrainTypes.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>

#include <cmath>
#include <limits>
#include <numbers>

namespace TerraEngine::Tests
{
	void RegisterTerrainCoordinateTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Mapping.DefaultsUseFourMetreTerrainXZ", [](TestContext& context) {
			const Terrain::TerrainLayerMapping mapping{};
			context.Expect(mapping.mode == Terrain::TerrainLayerMappingMode::TerrainXZ
				&& mapping.tileSizeXMetres == 4.0 && mapping.tileSizeZMetres == 4.0
				&& mapping.offsetXMetres == 0.0 && mapping.offsetZMetres == 0.0
				&& mapping.rotationRadians == 0.0 && mapping.triplanarSharpness == 4.0,
				"New terrain layers must default to a four-metre square Terrain-XZ mapping with zero phase transforms.");
		});

		registry.Add("Terrain.Mapping.CanonicalizesOffsetModuloAndRotation", [](TestContext& context) {
			Terrain::TerrainLayerMapping input{};
			input.tileSizeXMetres = 4.0;
			input.tileSizeZMetres = 3.0;
			input.offsetXMetres = 10.0;
			input.offsetZMetres = -1.0;
			input.rotationRadians = 2.0 * std::numbers::pi + std::numbers::pi / 2.0;
			Terrain::TerrainLayerMapping canonical{};
			const auto result = Terrain::CanonicalizeTerrainLayerMapping(input, canonical);
			context.Expect(result.Succeeded()
				&& std::abs(canonical.offsetXMetres - 2.0) <= 1.0e-12
				&& std::abs(canonical.offsetZMetres - 2.0) <= 1.0e-12
				&& std::abs(canonical.rotationRadians - std::numbers::pi / 2.0) <= 1.0e-12,
				"Mapping commits must use positive offset modulo and normalize rotation to one canonical turn.");
		});

		registry.Add("Terrain.Mapping.NonSquareTilesUseIndependentPhysicalAxes", [](TestContext& context) {
			Terrain::TerrainLayerMapping mapping{};
			mapping.tileSizeXMetres = 4.0;
			mapping.tileSizeZMetres = 3.0;
			Terrain::TerrainAreaMaterialPhase phase{};
			const auto prepared = Terrain::PrepareTerrainAreaMaterialPhase({0, 0}, 1.0, mapping, phase);
			const glm::dvec2 uv = Terrain::EvaluateTerrainSurfaceUv(phase, mapping, {8.0, 9.0});
			context.Expect(prepared.Succeeded() && std::abs(uv.x - 2.0) <= 1.0e-12 && std::abs(uv.y - 3.0) <= 1.0e-12,
				"Terrain-XZ surface coordinates must divide each physical axis by its independently authored tile size.");
		});

		registry.Add("Terrain.Mapping.NegativeAreasRetainPositiveBoundedPhase", [](TestContext& context) {
			Terrain::TerrainLayerMapping mapping{};
			mapping.tileSizeXMetres = 10.0;
			mapping.tileSizeZMetres = 10.0;
			Terrain::TerrainAreaMaterialPhase phase{};
			const auto prepared = Terrain::PrepareTerrainAreaMaterialPhase({-1, -2}, 1.0, mapping, phase);
			context.Expect(prepared.Succeeded()
				&& std::abs(phase.phaseMetres.x - 4.0) <= 1.0e-12
				&& std::abs(phase.phaseMetres.y - 8.0) <= 1.0e-12
				&& phase.phaseMetres.x >= 0.0 && phase.phaseMetres.x < mapping.tileSizeXMetres
				&& phase.phaseMetres.y >= 0.0 && phase.phaseMetres.y < mapping.tileSizeZMetres,
				"Signed negative areas must reduce into a positive per-axis phase interval before float upload.");
		});

		registry.Add("Terrain.Mapping.ControlUvIsIndependentFromSurfaceMapping", [](TestContext& context) {
			const glm::dvec2 localSample{64.0, 128.0};
			const glm::dvec2 controlBefore = Terrain::ComputeTerrainControlUv(localSample);
			Terrain::TerrainLayerMapping changed{};
			changed.tileSizeXMetres = 1.25;
			changed.tileSizeZMetres = 9.5;
			changed.offsetXMetres = 7.0;
			changed.offsetZMetres = -3.0;
			changed.rotationRadians = 1.25;
			Terrain::TerrainAreaMaterialPhase phase{};
			const auto prepared = Terrain::PrepareTerrainAreaMaterialPhase({0, 0}, 1.0, changed, phase);
			const glm::dvec2 surface = Terrain::EvaluateTerrainSurfaceUv(phase, changed, localSample);
			const glm::dvec2 controlAfter = Terrain::ComputeTerrainControlUv(localSample);
			context.Expect(prepared.Succeeded() && controlBefore == controlAfter
				&& std::abs(controlAfter.x - 0.25) <= 1.0e-12 && std::abs(controlAfter.y - 0.5) <= 1.0e-12
				&& surface != controlAfter,
				"Material mapping edits must not change the area-local control-map coordinate used for painted IDs and weights.");
		});

		registry.Add("Terrain.Mapping.ComponentPlacementIsTranslationOnly", [](TestContext& context) {
			TerraEngine::Scene::WorldPose translated{};
			translated.position.cell = {17, -3, 9};
			translated.position.offsetMetres = {120.0, -5.0, 31.0};
			TerraEngine::Scene::WorldPose rotated = translated;
			rotated.rotation = glm::angleAxis(0.25F, glm::vec3(0.0F, 1.0F, 0.0F));
			TerraEngine::Scene::WorldPose scaled = translated;
			scaled.scale = {2.0F, 1.0F, 1.0F};
			context.Expect(TerrainComponent::SupportsTranslationOnlyPlacement(translated)
				&& !TerrainComponent::SupportsTranslationOnlyPlacement(rotated)
				&& !TerrainComponent::SupportsTranslationOnlyPlacement(scaled),
				"Terrain translation may carry authored mapping, while instance rotation and scale must remain unsupported.");
		});

		registry.Add("Terrain.Coordinates.NegativeSamplesUseMathematicalFloor", [](TestContext& context) {
			const auto area = Terrain::OwnerArea({-1, -257});
			const auto local = Terrain::LocalSample({-1, -257});
			context.Expect(area.has_value() && area->x == -1 && area->z == -2, "Negative terrain samples should use floor division rather than truncation.");
			context.Expect(local.has_value() && local->x == 255 && local->z == 255, "Negative terrain samples should map to non-negative local coordinates.");
		});

		registry.Add("Terrain.Coordinates.AreaEdgesAreHalfOpen", [](TestContext& context) {
			const Terrain::TerrainSampleBounds bounds{0, 0, 256, 256};
			context.Expect(bounds.Contains({255, 255}), "The final owned sample should be inside a half-open area.");
			context.Expect(!bounds.Contains({256, 255}) && !bounds.Contains({255, 256}), "The positive edge belongs to the neighbouring area.");
		});

		registry.Add("Terrain.Coordinates.Int64MinimumRemainsAddressable", [](TestContext& context) {
			const auto area = Terrain::OwnerArea({std::numeric_limits<std::int64_t>::min(), 0});
			const auto local = Terrain::LocalSample({std::numeric_limits<std::int64_t>::min(), 0});
			context.Expect(area.has_value() && local.has_value(), "Checked floor arithmetic should support the complete signed coordinate range.");
			context.Expect(local.has_value() && local->x >= 0 && local->x < Terrain::kTerrainAreaExtentSamples, "The local coordinate must remain bounded after floor modulo.");
		});
	}
}
