#include "TestHarness.h"

#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryBenchmarkTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Benchmark.CalculatesMeasuredPercentilesVarianceAndRejectsUnavailableSamples",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryBenchmarkRun run{};
				for (std::uint64_t duration : { 10u, 20u, 30u, 40u, 50u })
				{
					TerraEngine::Renderer::VirtualGeometryMeasurementRecord sample{};
					sample.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Measured;
					sample.durationNanoseconds = duration;
					sample.queryIdentity = "query";
					sample.executionEvidenceId = "submission";
					run.samples.push_back(sample);
				}
				TerraEngine::Renderer::VirtualGeometryMeasurementRecord unavailable{};
				unavailable.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Unavailable;
				unavailable.rejectionReason = "unsupported";
				run.samples.push_back(unavailable);

				const TerraEngine::Renderer::VirtualGeometryBenchmarkSampleStatistics statistics =
					TerraEngine::Renderer::BuildVirtualGeometryBenchmarkStatistics(run.samples);
				context.ExpectEqual(std::uint64_t{ 30u }, statistics.medianNanoseconds, "Median must use measured samples only.");
				context.ExpectEqual(std::uint64_t{ 50u }, statistics.p95Nanoseconds, "P95 must select the upper nearest measured rank.");
				context.ExpectEqual(std::uint64_t{ 50u }, statistics.p99Nanoseconds, "P99 must select the upper nearest measured rank.");
				context.ExpectEqual(
					std::uint64_t{ 200u }, statistics.varianceNanosecondsSquared, "Variance must preserve the deterministic population result.");
				context.ExpectEqual(std::uint32_t{ 1u }, statistics.rejectedCount, "Unavailable samples must be rejected rather than folded into zero.");
			});

		registry.Add("VirtualGeometry.Benchmark.HashAndComparabilityRequireEquivalentScopeAndFiveMeasuredPairs",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryBenchmarkRun baseline{};
				baseline.captureId = "baseline";
				baseline.scope = { "vulkan", "adapter", "driver", "mesh", "1920x1080", "high", 1u, "workload" };
				baseline.sourceHash = "source";
				baseline.executableHash = "exe";
				baseline.shaderHash = "shader";
				baseline.assetHash = "asset";
				baseline.buildRecipeHash = "recipe";
				for (std::uint32_t ordinal = 0u; ordinal < 5u; ++ordinal)
				{
					TerraEngine::Renderer::VirtualGeometryMeasurementRecord sample{};
					sample.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Measured;
					sample.durationNanoseconds = 100u + ordinal;
					sample.queryIdentity = "query-" + std::to_string(ordinal);
					sample.executionEvidenceId = "submission-" + std::to_string(ordinal);
					baseline.samples.push_back(sample);
				}

				TerraEngine::Renderer::VirtualGeometryBenchmarkRun candidate = baseline;
				candidate.captureId = "candidate";
				context.ExpectEqual(TerraEngine::Renderer::BuildVirtualGeometryBenchmarkIntegrityHash(baseline),
					TerraEngine::Renderer::BuildVirtualGeometryBenchmarkIntegrityHash(baseline),
					"Equivalent captures must produce an identical deterministic integrity hash.");
				context.Expect(TerraEngine::Renderer::EvaluateVirtualGeometryBenchmarkComparability(baseline, candidate).comparable,
					"Five paired measured runs with identical execution scope should be comparable.");

				candidate.scope.quality = "low";
				const TerraEngine::Renderer::VirtualGeometryBenchmarkComparability incomparable =
					TerraEngine::Renderer::EvaluateVirtualGeometryBenchmarkComparability(baseline, candidate);
				context.Expect(
					!incomparable.comparable && !incomparable.reason.empty(), "Different quality settings must reject a performance claim with a reason.");
			});
	}
} // namespace TerraEngine::Tests
