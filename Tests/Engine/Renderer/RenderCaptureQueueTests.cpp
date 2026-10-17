#include "TestHarness.h"
#include "Renderer/RenderSystem.h"
#include "ThreadPool/ThreadPool.h"
#include "../../Support/DecodedCaptureAssertions.h"

#include <chrono>
#include <thread>
#include <limits>
#include <algorithm>

namespace TerraEngine::Tests {
	namespace {
		// Clear renderer-owned jobs before stopping an executor owned by this
		// test. Existing suite executors remain owned by their original fixture.
		struct CaptureFixture {
			bool ownsPool = ThreadPool::instance().getThreadCount() == 0;
			CaptureFixture() { if (ownsPool) ThreadPool::instance().initialize(2); RenderSystem::Instance().Clear(); }
			~CaptureFixture() { RenderSystem::Instance().Clear(); if (ownsPool) ThreadPool::instance().shutdown(); }
		};
		Renderer::RenderCaptureRequest Request(std::string id, glm::uvec2 size = {3u, 2u})
		{
			Renderer::RenderCaptureRequest request;
			request.requestId = std::move(id);
			request.sourceKind = Renderer::RenderCaptureSourceKind::CustomCamera;
			request.sourceName = "Capture contract camera";
			request.targetSize = size;
			request.timeoutMs = 30000;
			return request;
		}
		std::optional<Renderer::RenderCaptureResult> WaitResult(RenderSystem& renderer, std::string_view id)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			do {
				renderer.ProcessQueuedCaptureRequests();
				if (auto result = renderer.PollCaptureResult(id)) return result;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < deadline);
			return {};
		}
		void CheckPixels(TestContext& context, const Renderer::RenderCaptureResult& result,
			glm::uvec2 expected, bool transparent)
		{
			context.Expect(result.ok, "An affordable synthetic capture must produce successful encoded bytes.");
			const auto image = TestSupport::DecodeCaptureBase64(result.base64Data);
			context.ExpectEqual(expected.x, image.width, "Decoded width must match the promised capture width.");
			context.ExpectEqual(expected.y, image.height, "Decoded height must match the promised capture height.");
			context.ExpectEqual(std::size_t(expected.x) * expected.y * 4, image.rgba.size(), "Decoded data must contain the complete requested image.");
			for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
				context.Expect(image.rgba[i] == 0 && image.rgba[i + 1] == 0 && image.rgba[i + 2] == 0,
					"Synthetic pixels must disclose their neutral black fill.");
				context.ExpectEqual(static_cast<std::uint8_t>(transparent ? 0 : 255), image.rgba[i + 3], "Every synthetic alpha byte must follow transparentBackground.");
			}
			context.Expect(result.deterministicFallback && !result.backendReadback && !result.overlaysIncluded,
				"Synthetic metadata must disclose fallback, absence of GPU pixels and absent overlays.");
		}
	}

	void RegisterRenderCaptureQueueTests(TestRegistry& registry)
	{
		// Keep the legacy extents and exercise the exact ordinary/odd sizes in the
		// acceptance contract through the same public facade and independent decoder.
		for (const auto extent : {glm::uvec2(1,1), glm::uvec2(7,3), glm::uvec2(17,9), glm::uvec2(640,360), glm::uvec2(1280,720), glm::uvec2(4096,4096)})
		{
			for (const bool transparent : {false, true})
			{
				const auto name = std::string("Renderer.CaptureQueue.DecodedSynthetic") + std::to_string(extent.x) + "x" + std::to_string(extent.y) + (transparent ? "Transparent" : "Opaque");
				registry.Add(name, [extent, transparent](TestContext& context) {
					CaptureFixture fixture;
					for (const bool overlays : {false, true})
					{
						auto request = Request("exact-synthetic-" + std::to_string(extent.x) + "x" + std::to_string(extent.y)
							+ (transparent ? "-transparent" : "-opaque") + (overlays ? "-overlays" : "-plain"), extent);
						request.transparentBackground = transparent;
						request.includeOverlays = overlays;
						const auto result = RenderSystem::Instance().CaptureImage(request);
						CheckPixels(context, result, extent, transparent);
						context.Expect(result.requestId == request.requestId && result.sourceName == request.sourceName
							&& result.sourceKind == request.sourceKind && result.freshness == request.freshness
							&& result.size == extent && result.errorCategory.empty() && !result.pendingAsync
							&& !result.backendTarget, "Each decoded image must retain its own request context and truthful terminal metadata.");
						if (overlays) {
							context.Expect(std::any_of(result.warnings.begin(), result.warnings.end(), [](const auto& warning) {
								return warning.find("overlays") != std::string::npos;
							}), "An unavailable overlay must be explained for every requested extent and alpha mode.");
						}
						context.Expect(!RenderSystem::Instance().PollCaptureResult(request.requestId),
							"Synchronous matrix captures must consume their only client outcome.");
						std::cout << "[CAPTURE_MATRIX] native " << extent.x << 'x' << extent.y
							<< " transparent=" << transparent << " overlays=" << overlays << " decoded=passed\n";
					}
				});
			}
		}
		registry.Add("Renderer.CaptureQueue.FailuresRemainEmptyAndHealthyRequestProgresses", [](TestContext& context) {
			CaptureFixture fixture;
			auto& renderer = RenderSystem::Instance();
			for (unsigned scenario = 0; scenario < 4; ++scenario) {
				auto request = Request("invalid-" + std::to_string(scenario));
				if (scenario == 0) request.allowBackendFallback = false;
				if (scenario == 1) request.targetSize = {0, 2};
				if (scenario == 2) request.targetSize = {UINT32_MAX, UINT32_MAX};
				if (scenario == 3) request.format = "image/jpeg";
				const auto result = renderer.CaptureImage(request);
				context.Expect(!result.ok && result.base64Data.empty() && !result.errorCategory.empty(),
					"Denied fallback, invalid dimensions and unsupported format must fail without fabricated image bytes.");
				context.Expect(result.requestId == request.requestId && result.sourceKind == request.sourceKind
					&& result.sourceName == request.sourceName && result.size == request.targetSize,
					"Each rejected request must retain its own source and extent.");
				const auto state = renderer.GetCaptureQueueState();
				context.Expect(state.pendingCaptureCount == 0 && state.activeCaptureCount == 0 && state.completedResultCount == 0,
					"These pre-dispatch failures must leave no pending, physical, or delivery charge.");
				for (unsigned poll = 0; poll < 8; ++poll)
					context.Expect(!renderer.PollCaptureResult(request.requestId), "A returned rejection cannot be delivered again.");
				const auto healthyId = "healthy-after-rejection-" + std::to_string(scenario);
				CheckPixels(context, renderer.CaptureImage(Request(healthyId)), {3, 2}, false);
				context.Expect(!renderer.PollCaptureResult(healthyId), "The immediate healthy follow-up must also be consumed once.");
				// Settle the successful worker before the next independent rejection
				// checks its own physical counters; this does not precede recovery.
				renderer.Clear();
			}
			CheckPixels(context, renderer.CaptureImage(Request("healthy-after-rejections")), {3,2}, false);
			context.Expect(!renderer.PollCaptureResult("healthy-after-rejections"), "Synchronous return must consume its only client-terminal result.");
		});
		registry.Add("Renderer.CaptureQueue.AsyncPumpDeliversExactlyOneTerminalResult", [](TestContext& context) {
			CaptureFixture fixture;
			auto& renderer = RenderSystem::Instance();
			std::string error;
			const auto id = renderer.QueueCaptureRequest(Request("one-terminal"), error);
			context.Expect(!id.empty() && error.empty(), "An admitted request must have a usable identity.");
			const auto result = WaitResult(renderer, id);
			context.Expect(result.has_value(), "Bounded pumping must observe the admitted completion.");
			CheckPixels(context, *result, {3,2}, false);
			for (unsigned i = 0; i < 8; ++i) {
				renderer.ProcessQueuedCaptureRequests();
				context.Expect(!renderer.PollCaptureResult(id), "Repeated protected polls must not redeliver a consumed result.");
			}
			// A ready promise can precede destruction of its worker callable. Wait
			// for that separate retirement rather than assuming eight polls join it.
			const auto retirementDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (renderer.GetCaptureQueueState().activeCaptureCount != 0u
				&& std::chrono::steady_clock::now() < retirementDeadline) {
				renderer.ProcessQueuedCaptureRequests();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			context.ExpectEqual(0u, renderer.GetCaptureQueueState().activeCaptureCount, "Physical settlement must release active capture capacity.");
		});
		registry.Add("Renderer.CaptureQueue.CustomCapturePublishesFallbackResult", [](TestContext& context) {
			CaptureFixture fixture;
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
			context.Expect(!result.overlaysIncluded, "Synthetic pixels do not contain requested overlays.");
			context.Expect(result.deterministicFallback, "Headless renderer capture should disclose deterministic fallback usage.");
			context.Expect(!result.base64Data.empty(), "Successful capture should include image bytes.");
			const TerraEngine::Renderer::RenderCaptureResult repeated = RenderSystem::Instance().CaptureImage(request);
			context.Expect(repeated.ok, "Repeated custom camera capture should also succeed.");
			context.ExpectEqual(result.base64Data, repeated.base64Data, "Deterministic fallback captures should return stable image bytes for repeated descriptors.");
			RenderSystem::Instance().Clear();
		});

		registry.Add("Renderer.CaptureQueue.AcceptsLargeResolutionWithoutFixedCap", [](TestContext& context) {
			CaptureFixture fixture;
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
			CaptureFixture fixture;
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
			// The synchronous facade must expose the same rejection as one owned
			// failure without consuming capacity or publishing an asynchronous result.
			const auto beforeRejectedCapture = renderer.GetCaptureQueueState();
			const auto rejected = renderer.CaptureImage(extra);
			const auto afterRejectedCapture = renderer.GetCaptureQueueState();
			context.Expect(!rejected.ok && rejected.errorCategory == "queue_full" && rejected.base64Data.empty()
				&& rejected.requestId == extra.requestId && rejected.sourceName == extra.sourceName,
				"A full queue must return an empty failure tied to the rejected request.");
			context.Expect(beforeRejectedCapture.pendingCaptureCount == afterRejectedCapture.pendingCaptureCount
				&& beforeRejectedCapture.activeCaptureCount == afterRejectedCapture.activeCaptureCount
				&& beforeRejectedCapture.completedResultCount == afterRejectedCapture.completedResultCount,
				"Rejected synchronous admission must preserve all existing queue charges.");
			for (unsigned poll = 0; poll < 8; ++poll)
				context.Expect(!renderer.PollCaptureResult(extra.requestId), "Rejected work has no second terminal delivery.");

			for (const std::string& queuedId : queuedIds)
			{
				const auto result = WaitResult(renderer, queuedId);
				context.Expect(result.has_value(), "Queued capture should publish exactly one bounded result.");
				CheckPixels(context, *result, {32, 32}, false);
				context.Expect(!renderer.PollCaptureResult(queuedId), "A consumed queue result must not be returned twice.");
			}
			renderer.Clear();
			CheckPixels(context, renderer.CaptureImage(Request("healthy-after-queue-full")), {3, 2}, false);
			context.Expect(!renderer.PollCaptureResult("healthy-after-queue-full"), "Recovery capture must also deliver only once.");
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
