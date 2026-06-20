#include "TestHarness.h"

#include "Renderer/Graph/RenderWorkerTaskGroup.h"

namespace TerraEngine::Tests {

	void RegisterRenderWorkerTaskGroupTests(TestRegistry& registry)
	{
		registry.Add("Renderer.RenderWorkerTaskGroup.LargeWorkUsesWorkers", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 3u;
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
			profile.maxRecordingWorkers = 4u;
			profile.workerSchedulingThreshold = 8u;

			const auto group = TerraEngine::Renderer::BuildRenderWorkerTaskGroup(snapshot, profile, "Render.Geometry", 64u);
			context.Expect(TerraEngine::Renderer::ShouldRunRenderGroupOnWorkers(group), "Large worker-eligible render work should use render task groups.");
			context.ExpectEqual(static_cast<std::size_t>(4u), group.workerCount, "Worker count should respect backend capacity.");
		});

		registry.Add("Renderer.RenderWorkerTaskGroup.CancellationSerializesSafely", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
			profile.maxRecordingWorkers = 2u;
			profile.workerSchedulingThreshold = 1u;
			auto group = TerraEngine::Renderer::BuildRenderWorkerTaskGroup(snapshot, profile, "Render.Shadow", 12u);

			TerraEngine::Renderer::CancelRenderWorkerTaskGroup(group, "stale viewport");
			context.Expect(!TerraEngine::Renderer::ShouldRunRenderGroupOnWorkers(group), "Cancelled groups must not publish worker state.");
			context.ExpectEqual(std::string("stale viewport"), group.cancellationReason, "Cancellation should explain stale ownership.");
		});
	}

}
