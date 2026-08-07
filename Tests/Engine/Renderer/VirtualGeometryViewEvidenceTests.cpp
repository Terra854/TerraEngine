#include "TestHarness.h"

#include "Renderer/VirtualGeometry/Diagnostics/VirtualGeometryEvidence.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryViewPolicy.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryViewEvidenceTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.ViewEvidence.CachedPayloadKeepsRequiredSections", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			VirtualGeometryBenchmarkCapture capture;
			capture.identity.captureId = "view-evidence";
			capture.identity.workloadVersion = "v1";
			capture.environment.backend = "vulkan";
			capture.workload.name = "cached-view";
			capture.integrityHash = "sha256:view";
			capture.sections.geometry["selectedClusters"] = "retained";
			capture.sections.geometry["lodTransitions"] = "retained";
			capture.sections.residencyAndStreaming["pageNeeds"] = "retained";
			capture.sections.visibilityAndShading["viewGeneration"] = "42";
			capture.sections.auxiliaryViews["shadow"] = "independent";
			capture.sections.fallbacksAndFailures["fallbackCoverage"] = "complete";
			const std::string serialized = SerializeVirtualGeometryBenchmarkCapture(capture);
			context.Expect(serialized.find("selectedClusters") != std::string::npos && serialized.find("pageNeeds") != std::string::npos, "Cached view evidence must retain selection and page-need fields.");
			context.Expect(serialized.find("viewGeneration") != std::string::npos && serialized.find("fallbackCoverage") != std::string::npos, "Cached view evidence must retain generation and fallback fields.");
		});

		registry.Add("VirtualGeometry.ViewEvidence.ViewHistoryDoesNotCrossBorrow", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			const VirtualGeometryViewPolicyState camera{ VirtualGeometryViewKind::Camera, 1u, 4u, 5u };
			const VirtualGeometryViewPolicyState shadow{ VirtualGeometryViewKind::Shadow, 2u, 4u, 6u };
			const auto decision = EvaluateVirtualGeometryViewPolicy(shadow, camera);
			context.Expect(decision.sharesGeometry && !decision.ownsHistory && !decision.historyValid, "Auxiliary views may reuse immutable geometry but not stale camera history.");
		});
	}
}
