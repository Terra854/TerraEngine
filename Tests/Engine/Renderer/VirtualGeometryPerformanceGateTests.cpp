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
			gate.viewType = RenderViewType::Game;
			gate.viewId = 3u;
			gate.assetId = TerraEngine::GenerateUUID();
			gate.pageId = 7u;
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
			context.ExpectEqual(3u, gate.viewId, "Failure reports should carry the view id.");
			context.Expect(gate.assetId != TerraEngine::EMPTY_UUID, "Failure reports should carry the scoped asset id when known.");
			context.ExpectEqual(7u, gate.pageId, "Failure reports should carry the scoped page id when known.");
			context.Expect(gate.source == VirtualGeometryPerformanceGateSource::Prepared, "Failure reports should carry the counter source.");
		});

		registry.Add("Renderer.VisibleCommandReuse.CopyCountGateUsesCompletedStats", [](TestContext& context) {
			RenderStatsSnapshot stats;
			stats.viewType = RenderViewType::Game;
			stats.valid = true;
			stats.visibleCommandCacheHit = true;
			stats.visibleCommandCount = 48u;
			stats.visibleCommandCopiedCount = 48u;
			stats.visibleStreamingUniqueMeshes = 1u;
			stats.visibleStreamingUniqueMaterials = 2u;
			stats.visibleStreamingUniqueTextures = 3u;
			stats.visibleStreamingDuplicateTouchesEliminated = 144u;

			const std::vector<PerformanceGateResult> gates = TerraEngine::Renderer::BuildVisibleCommandReusePerformanceGates(stats);
			const auto copyGate = FindGate(gates, "visible-command-copy-count");

			context.Expect(copyGate != gates.end(), "Visible-command reuse should publish a copy-count gate.");
			context.Expect(copyGate->status == VirtualGeometryPerformanceGateStatus::Pass, "Copying no more than the completed command count should pass.");
			context.ExpectEqual(std::string("visible-command-reuse-static"), copyGate->fixtureId, "Gate should identify the visible-command reuse fixture.");
		});

		registry.Add("Renderer.VisibleCommandReuse.ConservativeRefreshPublishesWarningGate", [](TestContext& context) {
			RenderStatsSnapshot stats;
			stats.viewType = RenderViewType::Scene;
			stats.valid = true;
			stats.visibleCommandCount = 8u;
			stats.visibleCommandCopiedCount = 8u;
			stats.visibleCommandConservativeRefresh = true;
			stats.visibleCommandReuseFallbackReason = "cached visible streaming touch set was unavailable";

			const std::vector<PerformanceGateResult> gates = TerraEngine::Renderer::BuildVisibleCommandReusePerformanceGates(stats);
			const auto warningGate = FindGate(gates, "visible-command-conservative-refresh");

			context.Expect(warningGate != gates.end(), "Conservative refresh should be visible to performance diagnostics.");
			context.Expect(warningGate->status == VirtualGeometryPerformanceGateStatus::Warning, "Conservative refresh should warn instead of pretending the optimized path ran.");
			context.Expect(warningGate->diagnostic.find("touch set") != std::string::npos, "Warning should explain the fallback reason.");
		});

		registry.Add("VirtualGeometry.PerformanceGate.BuildsDenseAndHiZGatesFromViewDiagnostics", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.PerformanceGate.FailsAndSummarizesExceededThresholds", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.PerformanceGate.UnsupportedMetricsAreExplicit", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.PerformanceGate.DenseGpuPacketAndPrepBudgetGatesCarryViewSource", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.PerformanceGate.CompactionTruthAndGeneratedSubmissionValidityGates", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.PerformanceGate.DensePageBudgetGatesResidentBytesAndCoverage", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Game;
			diagnostics.viewId = 3u;
			diagnostics.counterSource = VirtualGeometryCounterSource::GPUReadback;
			diagnostics.geometryBudgetBytes = 4u * 1024u * 1024u;
			diagnostics.estimatedGeometryBytes = 16u * 1024u * 1024u;
			diagnostics.pageResidency.residentHighDetailBytes = 2u * 1024u * 1024u;
			diagnostics.pageResidency.residentFallbackBytes = 1u * 1024u * 1024u;
			diagnostics.pageSources.residentPayloads = 6u;
			diagnostics.pageSources.emittedHighDetailSources = 5u;
			diagnostics.pageSources.emittedFallbackSources = 1u;
			diagnostics.pageSources.missingSources = 0u;
			diagnostics.pageSources.emittedEmbeddedFallbackSources = 0u;
			TerraEngine::Renderer::VirtualGeometryBudgetContributor contributor;
			contributor.assetId = TerraEngine::GenerateUUID();
			diagnostics.budgetContributors.push_back(contributor);

			const std::vector<PerformanceGateResult> gates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);
			const auto budgetGate = FindGate(gates, "virtual-geometry-resident-page-budget-ratio");
			const auto fullMeshGate = FindGate(gates, "virtual-geometry-full-owning-mesh-residency");
			const auto owningFallbackGate = FindGate(gates, "virtual-geometry-owning-mesh-fallback-bytes");
			const auto missingGate = FindGate(gates, "virtual-geometry-missing-page-coverage");
			const auto embeddedGate = FindGate(gates, "virtual-geometry-embedded-fallback-uses");

			context.Expect(budgetGate != gates.end(), "Dense page-budget gate should be generated from exact resident page bytes.");
			context.Expect(fullMeshGate != gates.end(), "Full-owning-mesh residency gate should be generated from exact resident/full byte facts.");
			context.Expect(owningFallbackGate != gates.end(), "Owning-mesh fallback bytes should be reported as an explicit gate.");
			context.Expect(missingGate != gates.end(), "Missing coverage gate should be generated from page-source diagnostics.");
			context.Expect(embeddedGate != gates.end(), "Embedded fallback gate should be generated from page-source diagnostics.");
			context.Expect(budgetGate->status == VirtualGeometryPerformanceGateStatus::Pass, "Resident bytes below the page budget should pass.");
			context.Expect(fullMeshGate->status == VirtualGeometryPerformanceGateStatus::Pass, "Resident bytes far below full mesh size should pass.");
			context.Expect(owningFallbackGate->status == VirtualGeometryPerformanceGateStatus::Pass, "Zero owning-mesh fallback bytes should pass the page-streaming gate.");
			context.Expect(missingGate->status == VirtualGeometryPerformanceGateStatus::Pass, "No missing page sources should pass coverage gates.");
			context.Expect(embeddedGate->status == VirtualGeometryPerformanceGateStatus::Pass, "No embedded fallback sources should pass page-source gates.");
			context.Expect(budgetGate->assetId == contributor.assetId, "Page-budget gate should identify the asset family that contributed resident bytes.");
			context.ExpectEqual(0u, budgetGate->pageId, "Aggregate page-source gates should carry a page-scope marker when page data exists.");
			context.Expect(budgetGate->source == VirtualGeometryPerformanceGateSource::GPUReadback, "Page-budget gate should preserve the counter source.");

			diagnostics.pageResidency.residentHighDetailBytes = 4u * 1024u * 1024u;
			diagnostics.pageResidency.residentFallbackBytes = 1u * 1024u * 1024u;
			diagnostics.pageResidency.owningMeshFallbackBytes = 256u;
			diagnostics.pageSources.missingSources = 1u;
			diagnostics.pageSources.emittedEmbeddedFallbackSources = 1u;
			const std::vector<PerformanceGateResult> failingGates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);
			const auto failingBudgetGate = FindGate(failingGates, "virtual-geometry-resident-page-budget-ratio");
			const auto failingOwningFallbackGate = FindGate(failingGates, "virtual-geometry-owning-mesh-fallback-bytes");
			const auto failingMissingGate = FindGate(failingGates, "virtual-geometry-missing-page-coverage");
			const auto failingEmbeddedGate = FindGate(failingGates, "virtual-geometry-embedded-fallback-uses");
			context.Expect(failingBudgetGate != failingGates.end(), "Failing resident page-budget gate should still be reported.");
			context.Expect(failingOwningFallbackGate != failingGates.end(), "Failing owning-mesh fallback gate should still be reported.");
			context.Expect(failingMissingGate != failingGates.end(), "Failing missing coverage gate should still be reported.");
			context.Expect(failingEmbeddedGate != failingGates.end(), "Failing embedded fallback gate should still be reported.");
			if (failingBudgetGate != failingGates.end())
			{
				context.Expect(failingBudgetGate->status == VirtualGeometryPerformanceGateStatus::Fail, "Resident bytes above budget should fail.");
			}
			if (failingOwningFallbackGate != failingGates.end())
			{
				context.Expect(failingOwningFallbackGate->status == VirtualGeometryPerformanceGateStatus::Fail, "Any owning-mesh fallback bytes should fail the true page-streaming gate.");
			}
			if (failingMissingGate != failingGates.end())
			{
				context.Expect(failingMissingGate->status == VirtualGeometryPerformanceGateStatus::Fail, "Missing page-source coverage should fail.");
			}
			if (failingEmbeddedGate != failingGates.end())
			{
				context.Expect(failingEmbeddedGate->status == VirtualGeometryPerformanceGateStatus::Fail, "Embedded fallback use should fail the page-streaming gate.");
			}
		});

		registry.Add("VirtualGeometry.PerformanceGate.FailsFullOwningMeshResidencyRegression", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Game;
			diagnostics.viewId = 4u;
			diagnostics.counterSource = VirtualGeometryCounterSource::GPUReadback;
			diagnostics.geometryBudgetBytes = 4u * 1024u * 1024u;
			diagnostics.estimatedGeometryBytes = 12u * 1024u * 1024u;
			diagnostics.pageResidency.residentHighDetailBytes = 10u * 1024u * 1024u;
			diagnostics.pageResidency.residentFallbackBytes = 2u * 1024u * 1024u;
			diagnostics.pageResidency.owningMeshFallbackBytes = 12u * 1024u * 1024u;
			diagnostics.pageSources.residentPayloads = 4u;

			const std::vector<PerformanceGateResult> gates = TerraEngine::Renderer::BuildVirtualGeometryPerformanceGates(diagnostics);
			const auto fullMeshGate = FindGate(gates, "virtual-geometry-full-owning-mesh-residency");
			const auto owningFallbackGate = FindGate(gates, "virtual-geometry-owning-mesh-fallback-bytes");

			context.Expect(fullMeshGate != gates.end(), "Full-owning-mesh regression gate should be generated for dense page-streamed assets.");
			context.Expect(owningFallbackGate != gates.end(), "Owning-mesh fallback regression gate should be generated for dense page-streamed assets.");
			context.Expect(fullMeshGate->status == VirtualGeometryPerformanceGateStatus::Fail, "Resident bytes equal to full mesh bytes should fail the page-streaming gate.");
			context.Expect(owningFallbackGate->status == VirtualGeometryPerformanceGateStatus::Fail, "Owning mesh fallback bytes should fail until physical page decode is active.");
			context.ExpectEqual(4u, fullMeshGate->viewId, "Full-owning-mesh gate should identify the failing view.");
			context.Expect(fullMeshGate->diagnostic.find("actual") != std::string::npos, "Full-owning-mesh gate should include actual metric detail.");
		});
	}
}
