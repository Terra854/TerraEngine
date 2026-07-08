#include "TestHarness.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"
#include "Renderer/RenderSystem.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		std::filesystem::path FindRepositoryRoot()
		{
			std::filesystem::path cursor = std::filesystem::absolute(__FILE__).parent_path();
			while (!cursor.empty())
			{
				// Engine source files are always present in the repository root,
				// so this keeps the source-contract test independent of the
				// current working directory used by CTest.
				if (std::filesystem::exists(cursor / "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContext.cpp"))
				{
					return cursor;
				}
				const std::filesystem::path parent = cursor.parent_path();
				if (parent == cursor)
				{
					break;
				}
				cursor = parent;
			}
			return {};
		}

		std::string ReadSourceFile(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}
	}

	void RegisterRenderFailureContainmentTests(TestRegistry& registry)
	{
		registry.Add("Renderer.FailureContainment.DeviceLostLatchesFrameSubmission", [](TestContext& context) {
			const std::filesystem::path root = FindRepositoryRoot();
			context.Expect(!root.empty(), "Repository root should be discoverable for Vulkan source contract checks.");

			const std::string implHeader = ReadSourceFile(root / "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContextImpl.h");
			const std::string contextSource = ReadSourceFile(root / "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContext.cpp");
			const std::string initSource = ReadSourceFile(root / "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContextInit.cpp");
			const std::string resourcesSource = ReadSourceFile(root / "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContextResources.cpp");

			context.Expect(implHeader.find("bool deviceLost = false;") != std::string::npos, "Vulkan backend should retain a device-lost latch in private state.");
			context.Expect(implHeader.find("bool MarkDeviceLost(VkResult result, std::string_view operation);") != std::string::npos, "Vulkan backend should expose one helper for detecting device-loss results.");
			context.Expect(contextSource.find("result != VK_ERROR_DEVICE_LOST") != std::string::npos, "Device-loss helper should test the Vulkan device-lost result explicitly.");
			context.Expect(contextSource.find("Rendering is disabled for this Vulkan context") != std::string::npos, "Device-loss helper should log a clear one-time disabled-rendering message.");
			context.Expect(contextSource.find("if (!impl->initialized || impl->deviceLost) return;") != std::string::npos, "BeginFrame should stop immediately after device loss.");
			context.Expect(contextSource.find("if (!impl->initialized || impl->skipFrame || impl->deviceLost) return;") != std::string::npos, "RenderFrame and EndFrame should stop immediately after device loss.");
			context.Expect(contextSource.find("if (!impl->editorMode || !impl->initialized || impl->deviceLost) return;") != std::string::npos, "PresentEditor should stop immediately after device loss.");
			context.Expect(contextSource.find("impl->MarkDeviceLost(submitResult, \"editor render queue submit\")") != std::string::npos, "Editor render submit failures should latch device loss before recovery.");
			context.Expect(contextSource.find("impl->MarkDeviceLost(idleResult, \"editor render recovery queue idle\")") != std::string::npos, "Editor render recovery should not loop when the queue cannot prove idle.");
			context.Expect(initSource.find("void VulkanContext::Impl::RecreateSwapChain()") != std::string::npos && initSource.find("if (deviceLost)") != std::string::npos, "Swapchain recreation should not run after the device-lost latch is set.");
			context.Expect(resourcesSource.find("MarkDeviceLost(submitResult, \"immediate command buffer submit\")") != std::string::npos, "Immediate GPU upload submits should respect the same device-lost latch.");
		});

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
