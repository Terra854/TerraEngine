#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanPresentationLifecycle.h"

namespace TerraEngine::Tests
{
	void RegisterEditorFrameCompletionTrackerTests(TestRegistry& registry)
	{
		registry.Add("Renderer.EditorFrameCompletionTracker.MonotonicFenceEvidence", [](TestContext& context) {
			using TerraEngine::Renderer::Vulkan::EditorFrameCompletionTracker;
			EditorFrameCompletionTracker tracker(3);
			const auto first = tracker.RecordSubmission(0);
			const auto second = tracker.RecordSubmission(1);
			context.Expect(second > first, "Successful submissions must receive monotonic serials.");
			tracker.RecordFenceCompletion(0);
			context.ExpectEqual(first, tracker.Snapshot().completedSerial, "Only the completed slot should advance completion.");
			tracker.RecordFenceCompletion(1);
			context.ExpectEqual(second, tracker.Snapshot().completedSerial, "Completion must catch up monotonically.");
		});

		registry.Add("Renderer.EditorFrameCompletionTracker.ThousandReplacementsRemainBounded", [](TestContext& context) {
			TerraEngine::Renderer::Vulkan::EditorFrameCompletionTracker tracker(3);
			for (std::size_t index = 0; index < 1000; ++index) {
				const std::size_t slot = index % 3;
				(void)tracker.RecordFenceCompletion(slot);
				(void)tracker.RecordSubmission(slot);
			}
			tracker.RecordProtectedIdle();
			const auto progress = tracker.Snapshot();
			context.ExpectEqual(progress.lastSubmittedSerial, progress.completedSerial, "Protected idle must cover every submitted replacement use.");
		});
	}
}
