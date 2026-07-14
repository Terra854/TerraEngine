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
			std::string source = buffer.str();
			// Git may materialize these files with CRLF on Windows. Normalize source
			// contracts once so their structural checks remain checkout-independent.
			std::size_t position = 0u;
			while ((position = source.find("\r\n", position)) != std::string::npos)
			{
				source.replace(position, 2u, "\n");
				++position;
			}
			return source;
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
			const std::string editorSource = ReadSourceFile(root / "Editor" / "TerraEngine-Editor" / "Core" / "EditorSystem.cpp");
			const std::string mainEditorSource = ReadSourceFile(root / "Editor" / "TerraEngine-Editor" / "Subsystems" / "MainEditorSubsystem.cpp");
			const std::string viewportRegistrySource = ReadSourceFile(root / "Editor" / "TerraEngine-Editor" / "Core" / "ViewportTextureRegistry.cpp");

			context.Expect(implHeader.find("std::atomic_bool deviceLost = false;") != std::string::npos, "Vulkan backend should retain a thread-safe device-lost latch in private state.");
			context.Expect(contextSource.find("deviceLost.compare_exchange_strong") != std::string::npos, "Concurrent Vulkan failures should publish only the first device-loss incident.");
			context.Expect(implHeader.find("bool MarkDeviceLost(VkResult result, std::string_view operation);") != std::string::npos, "Vulkan backend should expose one helper for detecting device-loss results.");
			context.Expect(contextSource.find("result != VK_ERROR_DEVICE_LOST") != std::string::npos, "Device-loss helper should test the Vulkan device-lost result explicitly.");
			context.Expect(contextSource.find("Rendering is disabled for this Vulkan context") != std::string::npos, "Device-loss helper should log a clear one-time disabled-rendering message.");
			context.Expect(contextSource.find("LogLevel::Critical, \"Vulkan device lost during \"") != std::string::npos, "Terminal Vulkan device loss should be logged at critical severity.");
			context.Expect(contextSource.find("latestRendererRecoverySnapshot = TerraEngine::Renderer::RecordDeviceLossIncident") != std::string::npos, "Device-loss helper should publish a structured recovery snapshot.");
			context.Expect(contextSource.find("if (!impl->initialized || impl->deviceLost) return;") != std::string::npos, "BeginFrame should stop immediately after device loss.");
			context.Expect(contextSource.find("if (!impl->initialized || impl->skipFrame || impl->deviceLost) return;") != std::string::npos, "RenderFrame and EndFrame should stop immediately after device loss.");
			context.Expect(contextSource.find("result.outcome = impl->deviceLost ? EditorPresentOutcome::DeviceLost") != std::string::npos, "PresentEditor should return a typed terminal result after device loss.");
			const std::size_t boundedEditorAcquire = contextSource.find("impl->device, impl->swapchain, 1'000'000ULL");
			const std::size_t retryableEditorAcquire = contextSource.find("acquireResult == VK_TIMEOUT || acquireResult == VK_NOT_READY", boundedEditorAcquire);
			const std::size_t skippedEditorAcquire = contextSource.find("result.outcome = EditorPresentOutcome::Skipped;", retryableEditorAcquire);
			context.Expect(boundedEditorAcquire != std::string::npos && retryableEditorAcquire != std::string::npos
				&& skippedEditorAcquire != std::string::npos,
				"Editor presentation must bound swapchain acquisition and retry later instead of blocking protected editor work behind the compositor.");
			context.Expect(contextSource.find("impl->MarkDeviceLost(submitResult, \"editor render queue submit\")") != std::string::npos, "Editor render submit failures should latch device loss before recovery.");
			context.Expect(contextSource.find("impl->MarkDeviceLost(idleResult, \"editor render recovery queue idle\")") != std::string::npos, "Editor render recovery should not loop when the queue cannot prove idle.");
			context.Expect(initSource.find("void VulkanContext::Impl::RecreateSwapChain()") != std::string::npos && initSource.find("if (deviceLost)") != std::string::npos, "Swapchain recreation should not run after the device-lost latch is set.");
			context.Expect(initSource.find("MakeActiveRecoverySnapshot(renderSessionGeneration)") != std::string::npos, "Fresh Vulkan initialization should reset the CPU recovery snapshot.");
			context.Expect(resourcesSource.find("MarkDeviceLost(submitResult, submitOperation)") != std::string::npos, "Immediate GPU upload submits should respect the same device-lost latch while preserving their operation label.");
			context.Expect(implHeader.find("std::mutex queueAccessMutex;") != std::string::npos, "All Vulkan queue handles should share an external-synchronization mutex.");
			context.Expect(implHeader.find("mutable std::recursive_mutex deviceStateMutex;") != std::string::npos, "Vulkan lifetime and cache ownership should share one recursive protected-boundary gate.");
			// RenderFrame profiles time spent waiting for the state gate, so it now
			// declares a deferred unique lock and acquires it explicitly. Verify the
			// acquisition still happens before any early return; the stack-owned lock
			// then remains held until all worker recording and publication completes.
			const std::size_t renderFrame = contextSource.find("void VulkanContext::RenderFrame(const FrameRenderData& frameData, ViewType view, const RenderOutputRequest& outputRequest) {");
			const std::size_t stateLockDeclaration = contextSource.find("std::unique_lock<std::recursive_mutex> stateLock(impl->deviceStateMutex, std::defer_lock);", renderFrame);
			const std::size_t stateLockAcquisition = contextSource.find("stateLock.lock();", stateLockDeclaration);
			const std::size_t renderFrameEarlyExit = contextSource.find("if (!impl->initialized || impl->skipFrame || impl->deviceLost) return;", stateLockAcquisition);
			context.Expect(renderFrame != std::string::npos
				&& stateLockDeclaration != std::string::npos
				&& stateLockAcquisition != std::string::npos
				&& renderFrameEarlyExit != std::string::npos
				&& renderFrame < stateLockDeclaration
				&& stateLockDeclaration < stateLockAcquisition
				&& stateLockAcquisition < renderFrameEarlyExit,
				"RenderFrame should retain renderer ownership until worker recording has joined and live Vulkan work is published.");
			context.Expect(initSource.find("void VulkanContext::Impl::RecreateSwapChain() {\n\tstd::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);") != std::string::npos, "Swapchain replacement should hold the renderer-state gate across idle, destruction, and candidate publication.");
			context.Expect(implHeader.find("std::recursive_mutex descriptorHostAccessMutex;") != std::string::npos, "Project workers and the render thread should share one descriptor host-synchronization boundary.");
			context.Expect(resourcesSource.find("UpdateGlobalTexture(uint32_t index, const VulkanImage& image) {\n\tstd::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);") != std::string::npos, "Bindless texture publication should hold the descriptor boundary across its idle wait and all frame-set writes.");
			context.Expect(contextSource.find("PrepareDepthForCompute(VkCommandBuffer cmd, VkImage sourceDepthImage, VkImageView depthView, bool updateGlobalDepthDescriptor) {\n\tstd::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);") != std::string::npos, "Depth descriptor publication should not race project-open asset descriptor updates.");
			context.Expect(resourcesSource.find("SubmitGraphicsQueue(1, &submitInfo, immediateFence)") != std::string::npos, "Worker-driven immediate uploads should use the synchronized graphics queue wrapper.");
			context.Expect(contextSource.find("vkQueueSubmit(impl->graphicsQueue") == std::string::npos, "Frame and UI paths should not bypass the synchronized queue wrapper.");
			context.Expect(contextSource.find("vkQueuePresentKHR(impl->presentQueue") == std::string::npos, "Presentation should share queue synchronization with graphics submissions.");
			context.Expect(resourcesSource.find("vkQueueWaitIdle(graphicsQueue)") == std::string::npos, "Resource lifetime waits should not race other host queue access.");
			context.Expect(implHeader.find("std::vector<VulkanBuffer> uiStaticVertexBuffers;") != std::string::npos && implHeader.find("std::vector<VulkanBuffer> uiSdfVertexBuffers;") != std::string::npos, "CPU-written runtime UI vertices should be owned per frame-fence slot.");
			context.Expect(contextSource.find("VulkanBuffer& uiStaticVertexBuffer = uiStaticVertexBuffers[currentFrame];") != std::string::npos && contextSource.find("VulkanBuffer& uiSdfVertexBuffer = uiSdfVertexBuffers[currentFrame];") != std::string::npos, "Runtime UI recording should write only the completed current frame slot.");
			const std::size_t prefixSubmit = resourcesSource.find("recorded view prefix submit before descriptor mutation");
			const std::size_t prefixClear = resourcesSource.find("pendingCommandBuffers.clear();", prefixSubmit);
			context.Expect(prefixSubmit != std::string::npos && prefixClear != std::string::npos && prefixSubmit < prefixClear, "Descriptor mutation should submit and complete recorded views before clearing their ownership records.");
			context.Expect(resourcesSource.find("Treat them as stale snapshots and skip") == std::string::npos, "Descriptor safety must not silently discard an earlier Scene or Game view.");
			const std::size_t shadowSafetyWait = resourcesSource.find("WaitForDescriptorMutationIdle();", resourcesSource.find("bool VulkanContext::Impl::EnsureShadowRenderResources"));
			const std::size_t shadowDeviceIdle = resourcesSource.find("const VkResult idleResult = WaitDeviceIdle();", shadowSafetyWait);
			context.Expect(shadowSafetyWait != std::string::npos && shadowDeviceIdle != std::string::npos && shadowSafetyWait < shadowDeviceIdle, "Shadow replacement should complete unsubmitted view references before destroying GPU objects.");
			context.Expect(editorSource.find("vulkan->RunWithQueueAccess") != std::string::npos, "ImGui-owned texture upload submissions should join the renderer queue synchronization boundary.");
			const std::size_t updateTexture = editorSource.find("ImGui_ImplVulkan_UpdateTexture");
			const std::size_t renderDrawData = editorSource.find("ImGui_ImplVulkan_RenderDrawData");
			const std::size_t captureDrawData = editorSource.find("mainEditor->CaptureViewportDrawDataUses()", updateTexture);
			context.Expect(updateTexture != std::string::npos && captureDrawData != std::string::npos && renderDrawData != std::string::npos && updateTexture < captureDrawData && captureDrawData < renderDrawData, "ImGui glyph upload and viewport descriptor capture should finish before swapchain UI draw recording.");
			context.Expect(contextSource.find("std::lock_guard<std::mutex> queueLock(impl->queueAccessMutex);") != std::string::npos, "PresentEditor must serialize ImGui's hidden texture upload submits with renderer queue access.");
			const std::size_t prepareUi = contextSource.find("if (prepareUI) prepareUI();");
			const std::size_t beginUiCommandBuffer = contextSource.find("VkCommandBufferBeginInfo begin{};", prepareUi);
			context.Expect(prepareUi != std::string::npos && beginUiCommandBuffer != std::string::npos && prepareUi < beginUiCommandBuffer, "Mouse-triggered glyph uploads must run after the frame fence and before beginning the UI command buffer/render pass.");
			context.Expect(implHeader.find("VkDescriptorPool editorDescriptorPool") != std::string::npos, "ImGui should own a descriptor pool that is isolated from asset-loading descriptor allocations.");
			context.Expect(contextSource.find("h.descriptorPool = impl->editorDescriptorPool") != std::string::npos, "Editor handles should publish the dedicated ImGui descriptor pool.");
			context.Expect(mainEditorSource.find("ImGui_ImplVulkan_RemoveTexture") == std::string::npos, "Viewport resize must not immediately free a descriptor still referenced by in-flight UI work.");
			context.Expect(viewportRegistrySource.find("retireAfterSerial > completedSerial") != std::string::npos, "Viewport descriptors should retire only after fence-backed completion.");
			context.Expect(viewportRegistrySource.find("if (descriptor == nullptr) return false;") != std::string::npos, "Viewport registration should reject a failed ImGui descriptor allocation while keeping the active pair.");
			context.Expect(contextSource.find("depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL") != std::string::npos, "Persistent editor depth images should enter each render pass in their restored attachment layout.");
			context.Expect(contextSource.find("VK_ACCESS_SHADER_READ_BIT\n\t\t\t\t| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT") != std::string::npos, "Editor render-pass dependencies should order prior depth restoration before the next clear.");
			context.Expect(contextSource.find("Shadow images persist across frames") != std::string::npos, "Shadow render passes should document their cross-frame attachment synchronization contract.");
			context.Expect(contextSource.find("VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT\n\t\t\t\t| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT") != std::string::npos, "Shadow render-pass dependencies should order prior attachment writes before reuse.");
			context.Expect(contextSource.find("colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL") != std::string::npos, "Persistent shadow color images should not be implicitly discarded on every frame.");
			context.Expect(contextSource.find("attachmentBarriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL") != std::string::npos, "Shadow depth reuse should explicitly synchronize and leave read-only layout before the next pass.");
			context.Expect(contextSource.find("attachmentBarriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL") != std::string::npos, "Shadow depth should explicitly transition to writable attachment layout before a render pass.");
			context.Expect(resourcesSource.find("VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, 1u, 6u") != std::string::npos, "All point-shadow cubemap layers should receive their initial persistent layout.");
			context.Expect(initSource.find("depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL") != std::string::npos, "The shared swapchain depth image should preserve its initialized layout across frames.");
			context.Expect(contextSource.find("Editor UI Present Command Buffer") != std::string::npos, "Temporary UI submissions should remain identifiable in Vulkan validation diagnostics.");
			context.Expect(initSource.find("Keep editor\n\t\t// presentation color-only") != std::string::npos, "Editor presentation should not share a writable depth attachment across swapchain frames in flight.");
			context.Expect(contextSource.find("renderPass.clearValueCount = 1;") != std::string::npos, "The ImGui swapchain pass should clear only its color attachment.");
			context.Expect(implHeader.find("FRAMEBUFFER_RESIZE_REBUILD_INTERVAL{32}") != std::string::npos, "Rapid native resize callbacks should throttle rebuilds without adding a visible trailing delay.");
			context.Expect(contextSource.find("if (!impl->ClaimFramebufferResizeRebuild())") != std::string::npos, "Editor resize handling should rebuild promptly at a protected frame boundary.");
			context.Expect(contextSource.find("recording here\n\t\t// would only resize Scene/Game targets") != std::string::npos, "Editor view recording should pause while a native resize is pending instead of reallocating targets for discarded frames.");
			context.Expect(contextSource.find("result.outcome = EditorPresentOutcome::RecreationRequested") != std::string::npos, "PresentEditor should return a typed stale-draw-data result after recreation.");
			context.Expect(initSource.find("shadows, global buffers, descriptors, caches, scenes, and CPU assets do not") != std::string::npos, "Native and runtime resize should preserve loaded renderer resources.");
			context.Expect(contextSource.find("impl->RecreateSwapChain();\n\t\timpl->RecreateOffscreenResources();") == std::string::npos, "Editor resize paths should not immediately destroy independent Scene/Game targets after rebuilding the native swapchain.");
			context.Expect(implHeader.find("std::uint64_t generation = 0u;") != std::string::npos, "Offscreen targets should expose a logical identity that changes even when Vulkan recycles a raw image-view handle.");
			context.Expect(viewportRegistrySource.find("current->resourceGeneration == generation") != std::string::npos, "ImGui viewport descriptors should compare resource generation as well as raw Vulkan handles.");
			context.Expect(resourcesSource.find("Hi-Z atomic initialization") == std::string::npos, "Hi-Z creation should not perform synchronous startup submissions.");
			context.Expect(contextSource.find("const VkImageLayout hiZOldLayout = hiZHasPriorRecordedLayout") != std::string::npos, "The first normal Hi-Z generation pass should own the UNDEFINED image transition.");
			context.Expect(implHeader.find("void ConfirmHiZWritesSubmitted(uint32_t frameIndex)") != std::string::npos, "Hi-Z images should become readable only after their frame command buffers are submitted.");
			context.Expect(contextSource.find("constexpr bool kEnableHiZMipGeneration = true;") != std::string::npos, "Hi-Z generation should remain enabled after its task-shader workload is bounded.");
			context.Expect(implHeader.find("VkExtent2D sourceDepthExtent") != std::string::npos, "Hi-Z generation should receive the physical source-depth extent explicitly.");
			context.Expect(contextSource.find("hiZ.width != sourceDepthExtent.width") != std::string::npos, "Hi-Z generation should fail open while a resized pyramid and depth attachment have different widths.");
			context.Expect(contextSource.find("hiZ.height != sourceDepthExtent.height") != std::string::npos, "Hi-Z generation should fail open while a resized pyramid and depth attachment have different heights.");
			context.Expect(contextSource.find("hiZ.latestInvalidReason = VulkanHiZInvalidReason::Resize;") != std::string::npos, "A skipped extent-mismatch generation should remain diagnosable as resize warmup.");
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
