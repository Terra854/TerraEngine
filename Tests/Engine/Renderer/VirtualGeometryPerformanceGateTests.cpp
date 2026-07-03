#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {

	namespace
	{
		std::vector<PerformanceGateResult>::const_iterator FindGate(
			const std::vector<PerformanceGateResult>& gates,
			std::string_view gateId)
		{
			return std::find_if(gates.begin(), gates.end(), [gateId](const PerformanceGateResult& gate) {
				return gate.gateId == gateId;
			});
		}
	}

	void RegisterVirtualGeometryPerformanceGateTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.PerformanceGate.Foundation.ResultCarriesFixtureMetricAndFailureDetail", [](TestContext& context) {
			PerformanceGateResult gate;
			gate.gateId = "dense-cluster-batch-ratio";
			gate.fixtureId = "virtual-geometry-dense-batching";
			gate.metricName = "submittedBatchRatio";
			gate.threshold = 0.1f;
			gate.actual = 0.22f;
			gate.unit = "ratio";
			gate.status = VirtualGeometryPerformanceGateStatus::Fail;
			gate.source = VirtualGeometryPerformanceGateSource::Prepared;
			gate.diagnostic = "Submitted batches exceeded the 10 percent selected-cluster gate.";

			context.ExpectEqual(std::string("virtual-geometry-dense-batching"), gate.fixtureId, "Gate diagnostics should identify the fixture.");
			context.ExpectEqual(std::string("submittedBatchRatio"), gate.metricName, "Gate diagnostics should identify the metric.");
			context.Expect(gate.actual > gate.threshold, "Fixture should represent a failing dense batching gate.");
			context.Expect(gate.status == VirtualGeometryPerformanceGateStatus::Fail, "Failing gates should not pass silently.");
			context.Expect(!gate.diagnostic.empty(), "Failing gates should carry human-readable detail.");
		});

		registry.Add("VirtualGeometry.PerformanceGate.US4.BuildsDenseAndHiZGatesFromViewDiagnostics", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Game;
			diagnostics.counterSource = VirtualGeometryCounterSource::Prepared;
			diagnostics.clusterPacketCount = 500u;
			diagnostics.submittedBatchCount = 42u;
			diagnostics.selectedMeshlets = 1000u;
			diagnostics.dispatchedMeshlets = 640u;
			diagnostics.geometryBudgetBytes = 4096u;
			diagnostics.budgetPressureBytes = 1024u;
			diagnostics.hiZFalseOcclusionRiskWarnings = 0u;

			const std::vector<PerformanceGateResult> gates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);
			const auto findGate = [&](std::string_view gateId) {
				return std::find_if(gates.begin(), gates.end(), [gateId](const PerformanceGateResult& gate) {
					return gate.gateId == gateId;
				});
			};

			const auto denseGate = findGate("dense-cluster-batch-ratio");
			const auto hiZGate = findGate("virtual-geometry-hiz-false-occlusion");
			context.Expect(denseGate != gates.end(), "Dense batch-ratio gate should be generated for 500+ cluster packets.");
			context.Expect(hiZGate != gates.end(), "Hi-Z false-occlusion gate should always be generated.");
			context.Expect(denseGate->status == VirtualGeometryPerformanceGateStatus::Pass, "A 42/500 batch ratio should pass the 10 percent dense gate.");
			context.Expect(hiZGate->status == VirtualGeometryPerformanceGateStatus::Pass, "Zero false-occlusion warnings should pass the Hi-Z gate.");
			context.Expect(!denseGate->fixtureId.empty() && !denseGate->metricName.empty(), "Generated gates should identify fixture and metric names.");
		});

		registry.Add("VirtualGeometry.PerformanceGate.US4.FailsAndSummarizesExceededThresholds", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Game;
			diagnostics.active = true;
			diagnostics.clusterPacketCount = 500u;
			diagnostics.submittedBatchCount = 120u;
			diagnostics.selectedMeshlets = 100u;
			diagnostics.dispatchedMeshlets = 140u;
			diagnostics.geometryBudgetBytes = 1024u;
			diagnostics.budgetPressureBytes = 2048u;
			diagnostics.hiZFalseOcclusionRiskWarnings = 1u;
			diagnostics.virtualGeometryPerformanceGates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);

			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			summary.detailed = true;
			TerraEngine::Renderer::ApplyVirtualGeometryDiagnostics(summary, { diagnostics }, {});

			const auto failed = std::count_if(diagnostics.virtualGeometryPerformanceGates.begin(), diagnostics.virtualGeometryPerformanceGates.end(), [](const PerformanceGateResult& gate) {
				return gate.status == VirtualGeometryPerformanceGateStatus::Fail;
			});
			context.Expect(failed >= 3, "Dense, dispatch-ratio, page-pressure, and Hi-Z gates should fail when thresholds are exceeded.");
			context.Expect(!summary.resourceWarnings.empty(), "Failed virtual geometry gates should publish summary warnings.");
			context.Expect(summary.resourceWarnings.front().find("expected") != std::string::npos, "Gate warnings should include expected threshold text.");
			context.Expect(summary.resourceWarnings.front().find("actual") != std::string::npos, "Gate warnings should include actual metric text.");
		});

		registry.Add("VirtualGeometry.PerformanceGate.US4.UnsupportedMetricsAreExplicit", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Scene;
			diagnostics.clusterPacketCount = 8u;
			diagnostics.selectedMeshlets = 0u;
			diagnostics.geometryBudgetBytes = 0u;

			const std::vector<PerformanceGateResult> gates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);
			const auto unsupported = std::count_if(gates.begin(), gates.end(), [](const PerformanceGateResult& gate) {
				return gate.status == VirtualGeometryPerformanceGateStatus::Unsupported;
			});

			context.Expect(unsupported >= 3, "Small or missing-data fixtures should report unsupported gates instead of passing silent zero values.");
			context.Expect(std::any_of(gates.begin(), gates.end(), [](const PerformanceGateResult& gate) {
				return gate.diagnostic.find("requires") != std::string::npos;
			}), "Unsupported gates should explain which fixture data is required.");
		});

		registry.Add("VirtualGeometry.PerformanceGate.US7.DenseGpuPacketAndPrepBudgetGatesCarryViewSource", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Game;
			diagnostics.viewId = 2u;
			diagnostics.counterSource = VirtualGeometryCounterSource::GPUReadback;
			diagnostics.clusterPacketCount = 600u;
			diagnostics.submittedBatchCount = 48u;
			diagnostics.gpuPacketRecordCount = 600u;
			diagnostics.gpuBatchDescriptorCount = 30u;
			diagnostics.selectedMeshlets = 1200u;
			diagnostics.dispatchedMeshlets = 780u;
			diagnostics.geometryBudgetBytes = 8192u;
			diagnostics.budgetPressureBytes = 1024u;

			const std::vector<PerformanceGateResult> gates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);
			const auto denseGate = FindGate(gates, "dense-cluster-batch-ratio");
			const auto prepGate = FindGate(gates, "virtual-geometry-prep-cost");
			const auto gpuGate = FindGate(gates, "gpu-packet-batch-ratio");

			context.Expect(denseGate != gates.end(), "Dense cluster gate should be generated for 500+ selected packets.");
			context.Expect(prepGate != gates.end(), "CPU prep budget gate should be generated for every diagnostics snapshot.");
			context.Expect(gpuGate != gates.end(), "GPU packet batch-ratio gate should be generated for 500+ GPU packet records.");
			context.Expect(gpuGate->status == VirtualGeometryPerformanceGateStatus::Pass, "Thirty GPU batches for six hundred packets should pass the dense GPU packet gate.");
			context.ExpectEqual(2u, gpuGate->viewId, "GPU packet gates should carry the producing view id.");
			context.Expect(gpuGate->viewType == RenderViewType::Game, "GPU packet gates should carry the producing view type.");
			context.Expect(gpuGate->source == VirtualGeometryPerformanceGateSource::GPUReadback, "GPU packet gates should preserve the GPU readback source.");
		});

		registry.Add("VirtualGeometry.PerformanceGate.US7.CompactionTruthAndGeneratedSubmissionValidityGates", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Scene;
			diagnostics.viewId = 1u;
			diagnostics.counterSource = VirtualGeometryCounterSource::GPUReadback;
			diagnostics.candidateClusterCount = 10u;
			diagnostics.compactedClusterCount = 7u;
			diagnostics.rejectedClusterCount = 3u;
			diagnostics.gpuPacketRecordCount = 10u;
			diagnostics.generatedSubmissionCount = 2u;
			diagnostics.validGeneratedSubmissionCount = 2u;
			diagnostics.invalidGeneratedSubmissionCount = 0u;

			const std::vector<PerformanceGateResult> gates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);
			const auto accountedGate = FindGate(gates, "gpu-compaction-accounted-packets");
			const auto overflowGate = FindGate(gates, "gpu-compaction-overflow-packets");
			const auto generatedGate = FindGate(gates, "generated-submission-invalid-count");

			context.Expect(accountedGate != gates.end(), "Compaction truth gate should be generated when candidate counters exist.");
			context.Expect(overflowGate != gates.end(), "Compaction overflow gate should be generated when candidate counters exist.");
			context.Expect(generatedGate != gates.end(), "Generated submission validity gate should be generated when generated records exist.");
			context.Expect(accountedGate->status == VirtualGeometryPerformanceGateStatus::Pass, "Compacted plus rejected packet counts should account for every candidate.");
			context.Expect(overflowGate->status == VirtualGeometryPerformanceGateStatus::Pass, "Accounted packet counts that match candidates should not report overflow.");
			context.Expect(generatedGate->status == VirtualGeometryPerformanceGateStatus::Pass, "All-valid generated submissions should pass the invalid-count gate.");

			diagnostics.invalidGeneratedSubmissionCount = 1u;
			const std::vector<PerformanceGateResult> failingGates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);
			const auto failingGeneratedGate = FindGate(failingGates, "generated-submission-invalid-count");
			context.Expect(failingGeneratedGate != failingGates.end(), "Failing generated submission gate should still be reported.");
			context.Expect(failingGeneratedGate->status == VirtualGeometryPerformanceGateStatus::Fail, "Invalid generated submissions should fail the generated-submission gate.");
			context.ExpectEqual(1u, failingGeneratedGate->viewId, "Failing generated submission gates should keep the Scene View id.");
		});
	}
}
