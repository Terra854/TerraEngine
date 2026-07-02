#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

#include <algorithm>
#include <string_view>

namespace TerraEngine::Tests {

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
	}
}
