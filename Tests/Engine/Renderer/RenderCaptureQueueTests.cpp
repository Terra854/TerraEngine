#include "TestHarness.h"
#include "Renderer/RenderSystem.h"

namespace TerraEngine::Tests {

	void RegisterRenderCaptureQueueTests(TestRegistry& registry)
	{
		registry.Add("Renderer.CaptureQueue.CustomCapturePublishesFallbackResult", [](TestContext& context) {
			RenderSystem::Instance().Clear();
			TerraEngine::Renderer::RenderCaptureRequest request;
			request.requestId = "queue-custom-fallback";
			request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
			request.sourceName = "Queue Test Camera";
			request.targetSize = glm::uvec2(64u, 32u);
			request.freshness = "nextFrame";
			request.includeOverlays = true;

			const TerraEngine::Renderer::RenderCaptureResult result = RenderSystem::Instance().CaptureImage(request);

			context.Expect(result.ok, "Custom camera capture should publish a deterministic fallback when no readback backend is present.");
			context.ExpectEqual(std::string("queue-custom-fallback"), result.requestId, "Capture result should preserve the caller request id.");
			context.ExpectEqual(std::string("nextFrame"), result.freshness, "Capture result should preserve freshness metadata.");
			context.Expect(result.overlaysIncluded, "Capture result should preserve overlay metadata.");
			context.Expect(result.deterministicFallback, "Headless renderer capture should disclose deterministic fallback usage.");
			context.Expect(!result.base64Data.empty(), "Successful capture should include image bytes.");
			const TerraEngine::Renderer::RenderCaptureResult repeated = RenderSystem::Instance().CaptureImage(request);
			context.Expect(repeated.ok, "Repeated custom camera capture should also succeed.");
			context.ExpectEqual(result.base64Data, repeated.base64Data, "Deterministic fallback captures should return stable image bytes for repeated descriptors.");
			RenderSystem::Instance().Clear();
		});

		registry.Add("Renderer.CaptureQueue.AcceptsLargeResolutionWithoutFixedCap", [](TestContext& context) {
			RenderSystem::Instance().Clear();
			TerraEngine::Renderer::RenderCaptureRequest request;
			request.requestId = "queue-large-resolution";
			request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
			request.sourceName = "Large Resolution Camera";
			request.targetSize = glm::uvec2(4096u, 4096u);

			const TerraEngine::Renderer::RenderCaptureResult result = RenderSystem::Instance().CaptureImage(request);

			context.Expect(result.ok, "Large captures should not be rejected by a fixed maximum resolution policy.");
			context.ExpectEqual(4096u, result.size.x, "Large capture should preserve the requested width.");
			context.ExpectEqual(4096u, result.size.y, "Large capture should preserve the requested height.");
			context.Expect(result.deterministicFallback, "Headless large capture should still disclose fallback usage.");
			RenderSystem::Instance().Clear();
		});

		registry.Add("Renderer.CaptureQueue.EnforcesBoundedPendingQueue", [](TestContext& context) {
			RenderSystem& renderer = RenderSystem::Instance();
			renderer.Clear();
			const TerraEngine::Renderer::RenderCaptureLimits limits = renderer.GetCaptureLimits();
			std::vector<std::string> queuedIds;
			for (std::uint32_t index = 0u; index < limits.maxQueuedCaptures; ++index)
			{
				TerraEngine::Renderer::RenderCaptureRequest request;
				request.requestId = "queue-bounded-" + std::to_string(index);
				request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
				request.sourceName = "Bounded Queue Camera";
				request.targetSize = glm::uvec2(32u, 32u);
				std::string error;
				const std::string queuedId = renderer.QueueCaptureRequest(request, error);
				context.Expect(!queuedId.empty(), "Capture request should fit while pending queue has capacity.");
				context.Expect(error.empty(), "Accepted queue request should not report an error.");
				queuedIds.push_back(queuedId);
			}

			TerraEngine::Renderer::RenderCaptureRequest extra;
			extra.requestId = "queue-bounded-extra";
			extra.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
			extra.sourceName = "Overflow Camera";
			extra.targetSize = glm::uvec2(32u, 32u);
			std::string overflowError;
			const std::string overflowId = renderer.QueueCaptureRequest(extra, overflowError);

			context.Expect(overflowId.empty(), "Capture queue should reject requests beyond the configured bound.");
			context.Expect(overflowError.find("full") != std::string::npos, "Queue overflow should explain that the queue is full.");

			for (std::uint32_t index = 0u; index < limits.maxQueuedCaptures; ++index)
			{
				renderer.ProcessQueuedCaptureRequests();
			}
			for (const std::string& queuedId : queuedIds)
			{
				context.Expect(renderer.PollCaptureResult(queuedId).has_value(), "Queued capture should publish exactly one bounded result.");
			}
			renderer.Clear();
		});

		registry.Add("Renderer.CaptureQueue.CaptureIdentityStateRetainsRequestedTarget", [](TestContext& context) {
			RenderSystem& renderer = RenderSystem::Instance();
			renderer.Clear();
			// No backend is required here: unavailable states must still identify the
			// target that automation asked to inspect.
			const glm::uvec2 pixel(3u, 5u);
			const auto captureState = renderer.GetConventionalVisibilityIdentityPickState(
				RenderViewType::Game, pixel, true);
			const auto viewportState = renderer.GetConventionalVisibilityIdentityPickState(
				RenderViewType::Game, pixel, false);
			context.Expect(captureState.dedicatedCaptureTarget,
				"Unavailable capture identity state must retain the requested capture selector.");
			context.Expect(!viewportState.dedicatedCaptureTarget,
				"Unavailable viewport identity state must not claim capture-target proof.");
			renderer.Clear();
		});
	}

}
