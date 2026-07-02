#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::EditorTests {

	void RegisterVirtualGeometryScalingDebugViewTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Diagnostics.Foundation.ShadowSelectionsRemainSeparate", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			ShadowVisibilitySelection shadow;
			shadow.shadowViewId = 3u;
			shadow.candidatePackets = 20u;
			shadow.selectedPackets = 12u;
			shadow.reusedCameraHiZ = false;
			diagnostics.shadowVisibilitySelections.push_back(shadow);
			diagnostics.shadowViewCount = static_cast<std::uint32_t>(diagnostics.shadowVisibilitySelections.size());

			context.ExpectEqual(1u, diagnostics.shadowViewCount, "Debug view diagnostics should expose shadow-view VG selections separately.");
			context.Expect(!diagnostics.shadowVisibilitySelections.front().reusedCameraHiZ, "Shadow VG diagnostics should not report camera Hi-Z reuse.");
		});
	}
}
