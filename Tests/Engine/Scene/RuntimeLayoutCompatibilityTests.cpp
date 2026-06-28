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
			context.Expect(TerraEngine::Scene::FormatLayoutDiagnosticsSummary(result.diagnostics).find("compatibility-snapshot-mismatch") != std::string::npos, "Formatted diagnostics should include fallback reason.");
		});

		registry.Add("Engine.RuntimeLayout.Compatibility.FallbackCountersFormat", [](TestContext& context) {
			TerraEngine::Scene::LayoutDiagnosticsSummary summary;
			summary.metadataScanFallbackCount = 2;
			summary.fullObjectFacadeFallbackCount = 1;
			summary.objectHandleFallbackCount = 3;
			summary.staleObjectHandleCount = 4;
			TerraEngine::Scene::AddRuntimeLayoutFallback(summary, "metadata-scan", 2);

			const std::string formatted = TerraEngine::Scene::FormatLayoutDiagnosticsSummary(summary);

			context.ExpectEqual(static_cast<std::size_t>(2), summary.fallbackCount, "Fallback helper should accumulate affected fallback count.");
			context.Expect(formatted.find("metadataScans=2") != std::string::npos, "Formatted diagnostics should expose metadata scan fallback count.");
			context.Expect(formatted.find("facadeFallbacks=1") != std::string::npos, "Formatted diagnostics should expose full Object facade fallback count.");
			context.Expect(formatted.find("compactFallbacks=3") != std::string::npos, "Formatted diagnostics should expose compact reference fallback count.");
			context.Expect(formatted.find("staleRefs=4") != std::string::npos, "Formatted diagnostics should expose stale compact reference count.");
		});

		registry.Add("Engine.RuntimeLayout.Compatibility.RuntimeRecordsCarryCompactIdentityFootprint", [](TestContext& context) {
			TerraEngine::Scene::RuntimeActiveMembership membership;
			TerraEngine::Scene::FlatHierarchyEntry hierarchyEntry;
			TerraEngine::Scene::TransformRuntimeRecord transformRecord;
			TerraEngine::Scene::RenderRuntimeRecord renderRecord;

			context.Expect(TerraEngine::ObjectIdentityFootprintBytes() <= 16, "Runtime layout compact identity should stay within the hot snapshot footprint.");
			context.ExpectEqual(TerraEngine::ObjectIdentityFootprintBytes(), sizeof(membership.objectHandle), "Active membership should carry the compact identity value.");
			context.ExpectEqual(TerraEngine::ObjectIdentityFootprintBytes(), sizeof(hierarchyEntry.objectHandle), "Hierarchy entries should carry the compact identity value.");
			context.ExpectEqual(TerraEngine::ObjectIdentityFootprintBytes(), sizeof(transformRecord.objectHandle), "Transform records should carry the compact identity value.");
			context.ExpectEqual(TerraEngine::ObjectIdentityFootprintBytes(), sizeof(renderRecord.objectHandle), "Render records should carry the compact identity value.");
		});
	}
}
