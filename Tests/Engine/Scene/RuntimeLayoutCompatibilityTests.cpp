#include "../TestHarness.h"

#include "Scene/RuntimeLayout.h"

namespace TerraEngine::Tests {
	void RegisterRuntimeLayoutCompatibilityTests(TestRegistry& registry)
	{
		registry.Add("Engine.RuntimeLayout.Compatibility.MatchingSnapshotsPass", [](TestContext& context) {
			TerraEngine::Scene::CompatibilitySnapshot optimized;
			optimized.activeObjectIds.push_back(TerraEngine::GenerateUUID());
			optimized.hierarchyOrder = optimized.activeObjectIds;
			TerraEngine::Scene::CompatibilitySnapshot compatibility = optimized;

			const TerraEngine::Scene::RuntimeLayoutValidationResult result = TerraEngine::Scene::ValidateRuntimeLayoutCompatibility(optimized, compatibility);

			context.Expect(result.valid, "Matching optimized and compatibility snapshots should pass.");
			context.ExpectEqual(static_cast<std::size_t>(0), result.diagnostics.validationMismatchCount, "Passing validation should report no mismatches.");
		});

		registry.Add("Engine.RuntimeLayout.Compatibility.MismatchReportsFallback", [](TestContext& context) {
			TerraEngine::Scene::CompatibilitySnapshot optimized;
			TerraEngine::Scene::CompatibilitySnapshot compatibility;
			optimized.activeObjectIds.push_back(TerraEngine::GenerateUUID());
			compatibility.activeObjectIds.push_back(TerraEngine::GenerateUUID());

			const TerraEngine::Scene::RuntimeLayoutValidationResult result = TerraEngine::Scene::ValidateRuntimeLayoutCompatibility(optimized, compatibility);

			context.Expect(!result.valid, "Mismatched snapshots should fail validation.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.diagnostics.validationMismatchCount, "Mismatch diagnostics should count the failing category.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.diagnostics.fallbackCount, "Mismatch diagnostics should request compatibility fallback.");
			context.Expect(result.mismatches.front().fallbackRecommendation.find("compatibility") != std::string::npos, "Mismatch should provide fallback guidance.");
		});
	}
}
