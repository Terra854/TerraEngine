// TerrainCoordinateTests.cpp -- Exact lattice edge cases for authored terrain.

#include "TestHarness.h"
#include "Terrain/TerrainTypes.h"

#include <limits>

namespace TerraEngine::Tests
{
	void RegisterTerrainCoordinateTests(TestRegistry& registry)
	{
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
