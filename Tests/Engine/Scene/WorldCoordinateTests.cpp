// WorldCoordinateTests.cpp - property-style regression tests for canonical positions.

#include "../TestHarness.h"
#include "UnboundedSceneTestFixtures.h"

#include "Scene/WorldCoordinate.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace TerraEngine::Tests {
	void RegisterWorldCoordinateTests(TestRegistry& registry)
	{
		registry.Add("WorldCoordinate.NormalizesPositiveAndNegativeCarries", [](TestContext& context) {
			const Scene::WorldCoordinateResult positive = Scene::NormalizeWorldPosition({ {}, { 32'768.25, 0.0, 0.0 } });
			const Scene::WorldCoordinateResult negative = Scene::NormalizeWorldPosition({ {}, { -32'768.25, 0.0, 0.0 } });

			context.Expect(positive.Succeeded(), "An offset at the positive edge should carry into the next cell.");
			context.ExpectEqual(std::int64_t{1}, positive.position.cell.x, "Positive carry should advance the canonical cell.");
			context.Expect(std::abs(positive.position.offsetMetres.x + 32'767.75) < 1e-9, "Positive carry should retain the exact local remainder.");
			context.Expect(negative.Succeeded(), "A negative offset below the interval should carry into the previous cell.");
			context.ExpectEqual(std::int64_t{-1}, negative.position.cell.x, "Negative carry must use floor semantics instead of truncation.");
			context.Expect(std::abs(negative.position.offsetMetres.x - 32'767.75) < 1e-9, "Negative carry should retain the exact local remainder.");
		});

		registry.Add("WorldCoordinate.CanonicalNormalizationIsUnique", [](TestContext& context) {
			const Scene::WorldCoordinateResult left = Scene::NormalizeWorldPosition({ { 8, 0, 0 }, { 32'768.0, 0.0, 0.0 } });
			const Scene::WorldCoordinateResult right = Scene::NormalizeWorldPosition({ { 9, 0, 0 }, { -32'768.0, 0.0, 0.0 } });

			context.Expect(left.Succeeded() && right.Succeeded(), "Equivalent positions should normalize successfully.");
			context.Expect(left.position == right.position, "Every physical point must have one canonical cell-plus-offset representation.");
		});

		registry.Add("WorldCoordinate.RejectsNonFiniteAndOverflow", [](TestContext& context) {
			const Scene::WorldCoordinateResult nonFinite = Scene::NormalizeWorldPosition(UnboundedSceneFixtures::MakeNonFiniteOffsetPosition());
			const Scene::WorldCoordinateResult overflow = Scene::NormalizeWorldPosition(UnboundedSceneFixtures::MakeOverflowPosition());

			context.ExpectEqual(static_cast<int>(Scene::WorldCoordinateError::NonFiniteOffset), static_cast<int>(nonFinite.error), "NaN and infinity must fail before reaching live scene state.");
			context.ExpectEqual(static_cast<int>(Scene::WorldCoordinateError::CellOverflow), static_cast<int>(overflow.error), "Cell carry overflow must fail rather than clamp.");
		});

		registry.Add("WorldCoordinate.LocalizesMillimetresAtExtremeDistance", [](TestContext& context) {
			const Scene::WorldPosition origin = UnboundedSceneFixtures::MakeExtremePositivePosition();
			const Scene::WorldCoordinateResult destinationResult = Scene::ApplyWorldDelta(origin, { {}, { 0.001, 0.0, 0.0 } });
			context.Expect(destinationResult.Succeeded(), "A millimetre delta at the supported extreme should remain representable.");

			const Scene::WorldLocalizationResult local = Scene::LocalizeWorldPosition(destinationResult.position, origin, 8'192.0);
			context.Expect(local.Succeeded(), "Nearby positions should localize inside the declared bounded range.");
			context.Expect(std::abs(local.offsetMetres.x - 0.001) < 1e-9, "Localization must preserve millimetre deltas without absolute-coordinate rounding.");
		});

		registry.Add("WorldCoordinate.RejectsUnboundedDifference", [](TestContext& context) {
			const Scene::WorldPosition left = UnboundedSceneFixtures::MakeExtremePositivePosition();
			const Scene::WorldPosition right = UnboundedSceneFixtures::MakeExtremeNegativePosition();
			const Scene::WorldLocalizationResult local = Scene::LocalizeWorldPosition(right, left, 8'192.0);

			context.ExpectEqual(static_cast<int>(Scene::WorldCoordinateError::UnsafeLocalRange), static_cast<int>(local.error), "Consumers must supply a safe bounded localization range.");
		});

		registry.Add("WorldCoordinate.LocalizesAdjacentExtremeCellsWithoutCancellation", [](TestContext& context) {
			const Scene::WorldPosition nearMaximumLeft{ { std::numeric_limits<std::int64_t>::max() - 1, 0, 0 }, { 0.0, 0.0, 0.0 } };
			const Scene::WorldPosition nearMaximumRight{ { std::numeric_limits<std::int64_t>::max(), 0, 0 }, { 0.0, 0.0, 0.0 } };
			const Scene::WorldPosition nearMinimumLeft{ { std::numeric_limits<std::int64_t>::min(), 0, 0 }, { 0.0, 0.0, 0.0 } };
			const Scene::WorldPosition nearMinimumRight{ { std::numeric_limits<std::int64_t>::min() + 1, 0, 0 }, { 0.0, 0.0, 0.0 } };

			const Scene::WorldLocalizationResult maximum = Scene::LocalizeWorldPosition(nearMaximumRight, nearMaximumLeft, 65'536.0);
			const Scene::WorldLocalizationResult minimum = Scene::LocalizeWorldPosition(nearMinimumRight, nearMinimumLeft, 65'536.0);
			context.Expect(maximum.Succeeded() && minimum.Succeeded(), "Adjacent extreme cells must remain localizable.");
			context.ExpectEqual(65'536.0, maximum.offsetMetres.x, "Maximum-adjacent cells should differ by exactly one cell edge.");
			context.ExpectEqual(65'536.0, minimum.offsetMetres.x, "Minimum-adjacent cells should differ by exactly one cell edge.");
		});
	}
}
