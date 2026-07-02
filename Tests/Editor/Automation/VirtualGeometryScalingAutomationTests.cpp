#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::EditorTests {

	void RegisterVirtualGeometryScalingAutomationTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Automation.Foundation.DiagnosticsReadContractNamesSourceData", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Game;
			diagnostics.counterSource = VirtualGeometryCounterSource::Prepared;
			diagnostics.clusterPacketCount = 12u;
			diagnostics.candidateClusterCount = 14u;
			diagnostics.compactedClusterCount = 10u;
			diagnostics.rejectedClusterCount = 4u;
			diagnostics.failOpenClusterCount = 1u;
			diagnostics.submittedBatchCount = 2u;
			diagnostics.residentPages = 3u;
			diagnostics.requestedPages = 2u;
			diagnostics.missingPages = 1u;
			diagnostics.pinnedFallbackPages = 1u;
			diagnostics.hiZBaselineMatched = true;

			context.Expect(diagnostics.counterSource == VirtualGeometryCounterSource::Prepared, "Automation diagnostics should carry source labels instead of authoritative-looking zeros.");
			context.ExpectEqual(12u, diagnostics.clusterPacketCount, "Automation diagnostics should expose packet counts.");
			context.ExpectEqual(14u, diagnostics.candidateClusterCount, "Automation diagnostics should expose candidate packet counts.");
			context.ExpectEqual(10u, diagnostics.compactedClusterCount, "Automation diagnostics should expose compacted packet counts.");
			context.ExpectEqual(4u, diagnostics.rejectedClusterCount, "Automation diagnostics should expose rejected packet counts.");
			context.ExpectEqual(1u, diagnostics.failOpenClusterCount, "Automation diagnostics should expose fail-open packet counts.");
			context.ExpectEqual(2u, diagnostics.submittedBatchCount, "Automation diagnostics should expose submitted batch counts.");
			context.ExpectEqual(3u, diagnostics.residentPages, "Automation diagnostics should expose resident page counts.");
			context.ExpectEqual(2u, diagnostics.requestedPages, "Automation diagnostics should expose requested page counts.");
			context.ExpectEqual(1u, diagnostics.missingPages, "Automation diagnostics should expose missing page counts.");
			context.ExpectEqual(1u, diagnostics.pinnedFallbackPages, "Automation diagnostics should expose pinned fallback counts.");
			context.Expect(diagnostics.hiZBaselineMatched, "Automation diagnostics should report the preserved Hi-Z baseline state.");
		});
	}
}
