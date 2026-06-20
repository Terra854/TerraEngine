#include "TestHarness.h"

#include "Renderer/Graph/RenderGraph.h"

namespace TerraEngine::Tests {
namespace {
	TerraEngine::Renderer::RenderFrameSnapshot MakeWorkload(std::size_t renderableCount)
	{
		TerraEngine::Renderer::RenderFrameSnapshot snapshot;
		snapshot.frameId = 1u;
		snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1920u, 1080u), 1u });
		for (std::size_t index = 0; index < renderableCount; ++index)
		{
			snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });
		}
		return snapshot;
	}
}

	void RegisterRenderWorkSchedulingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.WorkScheduling.LargeScenesUseWorkerPackets", [](TestContext& context) {
			auto snapshot = MakeWorkload(1000u);
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.backendId = "mock";
			profile.supportsParallelRecording = true;
			profile.maxRecordingWorkers = 8u;
			profile.workerSchedulingThreshold = 32u;
			const auto passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			const auto plan = TerraEngine::Renderer::BuildRenderWorkPacketPlan(snapshot, passes, profile);
			const std::size_t total = plan.workerPacketCount + plan.serialPacketCount;
			context.Expect(total > 0u, "The packet plan should contain work.");
			context.Expect((plan.workerPacketCount * 100u) / total >= 70u, "Large scenes should schedule most eligible work as worker packets.");
		});

		registry.Add("Renderer.WorkScheduling.SmallScenesStayCheap", [](TestContext& context) {
			auto snapshot = MakeWorkload(2u);
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.backendId = "mock";
			profile.supportsParallelRecording = true;
			profile.maxRecordingWorkers = 8u;
			profile.workerSchedulingThreshold = 128u;
			const auto passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			const auto plan = TerraEngine::Renderer::BuildRenderWorkPacketPlan(snapshot, passes, profile);
			context.ExpectEqual(static_cast<std::size_t>(0u), plan.workerPacketCount, "Small scenes should avoid worker overhead.");
			context.Expect(plan.serialPacketCount > 0u, "Small scenes should still produce serial work.");
		});
	}

}
