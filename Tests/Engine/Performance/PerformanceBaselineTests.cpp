// PerformanceBaselineTests.cpp — Covers comparable baseline identity and correctness-first acceptance.

#include "TestHarness.h"

#include "Performance/PerformanceTestFixtures.h"
#include "Profiling/PerformanceBaseline.h"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Tests
{
namespace
{
using namespace TerraEngine::Profiling;

PerformanceBaselineIdentityInput MakeBaselineInput(PerformanceWorkloadScale scale)
{
	PerformanceBaselineIdentityInput input;
	input.baselineId = "baseline-2026-07-15";
	input.featureRevision = "043";
	input.buildRevision = "build-a";
	input.installedTreeIdentity = "install-a";
	input.workload = {
		scale == PerformanceWorkloadScale::CanonicalLargeScene ? "canonical-10000-body" : "small-scene-control",
		scale,
		scale == PerformanceWorkloadScale::CanonicalLargeScene ? 10000u : 100u,
		scale == PerformanceWorkloadScale::CanonicalLargeScene ? 1000u : 10u,
		scale == PerformanceWorkloadScale::CanonicalLargeScene ? 1000u : 10u,
		scale == PerformanceWorkloadScale::CanonicalLargeScene ? 8000u : 80u,
		100u,
		"settings-a"
	};
	input.hardwareFingerprint = "hardware-a";
	input.operatingSystem = "windows";
	input.workerCount = 4u;
	input.projectSourceHash = "source-a";
	input.artifactHash = "artifact-a";
	input.captureDetailLevel = "detailed";
	input.editorLayoutHash = "layout-a";
	input.editorPreferenceHash = "preferences-a";
	return input;
}

std::vector<CorrectnessGateResult> PassingCorrectnessGates()
{
	return {
		{CorrectnessGate::Determinism, true, "state hash matches"},
		{CorrectnessGate::Rollback, true, "rejected batch rolled back"},
		{CorrectnessGate::Visibility, true, "declared boundary observed"},
		{CorrectnessGate::Lifecycle, true, "lifecycle completed"},
		{CorrectnessGate::ExportParity, true, "export matches editor"},
		{CorrectnessGate::AuthoredStateRestoration, true, "authored state restored"},
		{CorrectnessGate::NormalShutdown, true, "normal shutdown observed"}
	};
}
}

void RegisterPerformanceBaselineTests(TestRegistry& registry)
{
	registry.Add("Performance.BaselineIdentityRejectsIncompleteOrReducedCanonicalWorkloads", [](TestContext& context) {
		static_assert(!std::is_copy_assignable_v<PerformanceBaselineIdentity>, "Baseline identities must not be assignable after validation.");
		PerformanceBaselineIdentityInput invalid = MakeBaselineInput(PerformanceWorkloadScale::CanonicalLargeScene);
		invalid.workload.staticBodyCount = 7999u;
		BaselineValidationResult validation;
		const auto identity = PerformanceBaselineIdentity::TryCreate(std::move(invalid), validation);
		context.Expect(!identity.has_value(), "A reduced canonical workload must not create a comparable baseline identity.");
		context.Expect(!validation.valid && !validation.diagnostics.empty(), "Rejected baseline identities should explain the validation failure.");
	});

	registry.Add("Performance.BaselineComparisonSeparatesSmallAndLargeWorkloads", [](TestContext& context) {
		BaselineValidationResult validation;
		const auto canonical = PerformanceBaselineIdentity::TryCreate(MakeBaselineInput(PerformanceWorkloadScale::CanonicalLargeScene), validation);
		context.Expect(canonical.has_value(), "A complete canonical workload should create a baseline identity.");
		const auto small = PerformanceBaselineIdentity::TryCreate(MakeBaselineInput(PerformanceWorkloadScale::SmallScene), validation);
		context.Expect(small.has_value(), "A complete small-scene control should create a baseline identity.");
		if (canonical.has_value() && small.has_value())
		{
			const PerformanceComparisonEligibility eligibility = AssessBaselineComparisonEligibility(*canonical, *small, {});
			context.Expect(!eligibility.eligible, "Small-scene controls must not be compared directly with canonical large-scene evidence.");
			context.Expect(!eligibility.mismatches.empty(), "Incomparable workloads should report their mismatch.");
		}
	});

	registry.Add("Performance.BaselineUnavailableMetricsRemainMissingEvidence", [](TestContext& context) {
		const PerformanceMetric unavailable{"scene_view_p95", PerformanceMetricUnit::Nanoseconds, PerformanceMetricAvailability::Unavailable, std::nullopt, "Scene View capture is disabled."};
		context.Expect(ValidatePerformanceMetric(unavailable).valid, "Unavailable metrics with a reason should remain valid missing evidence.");
		const PerformanceMetric invalidUnavailable{"scene_view_p95", PerformanceMetricUnit::Nanoseconds, PerformanceMetricAvailability::Unavailable, 0u, "Scene View capture is disabled."};
		context.Expect(!ValidatePerformanceMetric(invalidUnavailable).valid, "Unavailable metrics must not be represented as a measured zero.");

		PerformanceAcceptanceOutcome outcome;
		outcome.eligibility.eligible = true;
		outcome.correctness = PassingCorrectnessGates();
		outcome.metrics = {{"scene_view_p95", true, PerformanceMetricComparisonStatus::Unavailable, "not captured"}};
		context.Expect(!outcome.IsAccepted(), "A required unavailable metric must reject the acceptance outcome.");
	});

	registry.Add("Performance.BaselineAcceptanceRequiresEveryCorrectnessGate", [](TestContext& context) {
		PerformanceAcceptanceOutcome outcome;
		outcome.eligibility.eligible = true;
		outcome.correctness = PassingCorrectnessGates();
		outcome.metrics = {{"frame_p95", true, PerformanceMetricComparisonStatus::Met, "within threshold"}};
		context.Expect(outcome.IsAccepted(), "Comparable evidence with every passing correctness gate should be accepted.");
		outcome.correctness.front().passed = false;
		context.Expect(!outcome.IsAccepted(), "A performance result must fail when determinism or any other correctness gate fails.");
	});

	registry.Add("Performance.FixturesBuildDeterministicCanonicalAndSmallStateHashes", [](TestContext& context) {
		const PerformanceFixture canonicalFirst = BuildCanonicalPerformanceFixture();
		const PerformanceFixture canonicalSecond = BuildCanonicalPerformanceFixture();
		const PerformanceFixture small = BuildSmallScenePerformanceFixture();
		context.ExpectEqual(10000u, static_cast<unsigned int>(canonicalFirst.bodies.size()), "Canonical fixtures should preserve the accepted 10,000-body workload.");
		context.ExpectEqual(100u, static_cast<unsigned int>(small.bodies.size()), "Small fixtures should remain a separate 100-body control workload.");
		context.ExpectEqual(ComputePerformanceFixtureStateHash(canonicalFirst), ComputePerformanceFixtureStateHash(canonicalSecond), "Repeated fixture builders should produce the same state hash.");
		context.Expect(ComputePerformanceFixtureStateHash(canonicalFirst) != ComputePerformanceFixtureStateHash(small), "Small and canonical fixtures must not share a state hash.");
	});
}
}
