#include "TestHarness.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"
#include "Renderer/RenderSystem.h"

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

		registry.Add("Renderer.FailureContainment.UnsupportedCaptureFormatPublishesNoImage", [](TestContext& context) {
			RenderSystem::Instance().Clear();
			TerraEngine::Renderer::RenderCaptureRequest request;
			request.requestId = "failure-containment-unsupported-format";
			request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
			request.sourceName = "Unsupported Format Camera";
			request.targetSize = glm::uvec2(64u, 64u);
			request.format = "image/jpeg";

			const TerraEngine::Renderer::RenderCaptureResult result = RenderSystem::Instance().CaptureImage(request);

			context.Expect(!result.ok, "Unsupported capture formats should fail locally.");
			context.ExpectEqual(std::string("unsupported_readback"), result.errorCategory, "Unsupported capture format should map to readback support failure.");
			context.Expect(result.base64Data.empty(), "Unsupported capture failure should not publish partial image bytes.");
			RenderSystem::Instance().Clear();
		});

		registry.Add("Renderer.FailureContainment.QueueOverflowLeavesAcceptedResultsPollable", [](TestContext& context) {
			RenderSystem& renderer = RenderSystem::Instance();
			renderer.Clear();
			const TerraEngine::Renderer::RenderCaptureLimits limits = renderer.GetCaptureLimits();
			std::vector<std::string> acceptedIds;
			for (std::uint32_t index = 0u; index < limits.maxQueuedCaptures; ++index)
			{
				TerraEngine::Renderer::RenderCaptureRequest request;
				request.requestId = "failure-containment-queue-" + std::to_string(index);
				request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
				request.sourceName = "Failure Containment Queue Camera";
				request.targetSize = glm::uvec2(16u, 16u);
				std::string error;
				acceptedIds.push_back(renderer.QueueCaptureRequest(request, error));
				context.Expect(error.empty(), "Accepted queue request should not carry an error.");
			}

			TerraEngine::Renderer::RenderCaptureRequest overflow;
			overflow.requestId = "failure-containment-queue-overflow";
			overflow.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::CustomCamera;
			overflow.sourceName = "Queue Overflow Camera";
			overflow.targetSize = glm::uvec2(16u, 16u);
			std::string overflowError;
			const std::string overflowId = renderer.QueueCaptureRequest(overflow, overflowError);

			context.Expect(overflowId.empty(), "Overflow request should be rejected atomically.");
			context.Expect(!overflowError.empty(), "Overflow request should explain why it was rejected.");

			for (std::uint32_t index = 0u; index < limits.maxQueuedCaptures; ++index)
			{
				renderer.ProcessQueuedCaptureRequests();
			}
			for (const std::string& acceptedId : acceptedIds)
			{
				context.Expect(renderer.PollCaptureResult(acceptedId).has_value(), "Accepted captures should remain pollable after an overflow rejection.");
			}
			renderer.Clear();
		});
	}

}
