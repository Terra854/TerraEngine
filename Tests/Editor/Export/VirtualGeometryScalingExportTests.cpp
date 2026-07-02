#include "TestHarness.h"

#include "Renderer/RenderTypes.h"

namespace TerraEngine::EditorTests {

	void RegisterVirtualGeometryScalingExportTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Export.Foundation.PerformanceGateRecordsArePlainData", [](TestContext& context) {
			PerformanceGateResult gate;
			gate.gateId = "export-vg-batch-count";
			gate.fixtureId = "export-runtime";
			gate.metricName = "submittedBatches";
			gate.threshold = 64.0f;
			gate.actual = 12.0f;
			gate.unit = "batches";
			gate.status = VirtualGeometryPerformanceGateStatus::Pass;

			context.ExpectEqual(std::string("export-runtime"), gate.fixtureId, "Export diagnostics should retain fixture identity.");
			context.Expect(gate.status == VirtualGeometryPerformanceGateStatus::Pass, "Export diagnostics should preserve gate status as copied data.");
			context.Expect(gate.actual <= gate.threshold, "Fixture should represent a passing export-side gate.");
		});
	}
}
