#include "TestHarness.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"

namespace TerraEngine::Tests {

	void RegisterRenderFailureContainmentTests(TestRegistry& registry)
	{
		registry.Add("Renderer.FailureContainment.NonFatalFailureDegradesLocally", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 15u;
			plan.orderedPasses.push_back({ "optional-readback" });
			TerraEngine::Renderer::RenderWorkPacket packet;
			packet.packetId = "optional-readback.0";
			packet.state = TerraEngine::Renderer::RenderPacketState::Failed;
			packet.failurePolicy = TerraEngine::Renderer::RenderFailurePolicy::DiagnosticsOnly;
			plan.recordedWork.push_back(packet);
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 15u);
			context.Expect(plan.valid, "Non-fatal optional work should not reject the whole frame.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.degradedWork.size(), "Degraded work should be reported locally.");
		});
	}

}
