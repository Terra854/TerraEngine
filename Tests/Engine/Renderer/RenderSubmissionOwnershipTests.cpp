#include "TestHarness.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"

namespace TerraEngine::Tests {

	void RegisterRenderSubmissionOwnershipTests(TestRegistry& registry)
	{
		registry.Add("Renderer.SubmissionOwnership.WorkerPresentIsRejected", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 3u;
			plan.orderedPasses.push_back({ "present" });
			TerraEngine::Renderer::RenderWorkPacket packet;
			packet.packetId = "present.0";
			packet.executionPolicy = TerraEngine::Renderer::RenderPacketExecutionPolicy::Worker;
			packet.outputs.push_back({ 9u, TerraEngine::Renderer::RenderAccessMode::Present, TerraEngine::Renderer::RenderStage::Present, "present" });
			plan.recordedWork.push_back(packet);
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 3u);
			context.Expect(!plan.valid, "Worker packets must not own presentation state.");
		});
	}

}
