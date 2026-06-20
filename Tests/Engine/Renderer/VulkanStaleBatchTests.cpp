#include "TestHarness.h"

#include "Renderer/Graph/RenderSubmissionPlan.h"

namespace TerraEngine::Tests {

	void RegisterVulkanStaleBatchTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanStaleBatch.CancelledPacketsBlockSubmission", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 12u;
			plan.orderedPasses.push_back({ "draw" });
			TerraEngine::Renderer::RenderWorkPacket packet;
			packet.packetId = "draw.stale";
			packet.passId = "draw";
			packet.state = TerraEngine::Renderer::RenderPacketState::Cancelled;
			plan.recordedWork.push_back(packet);

			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 12u);
			context.Expect(!plan.valid, "Cancelled stale backend batches must not reach final submission.");
		});
	}

}
