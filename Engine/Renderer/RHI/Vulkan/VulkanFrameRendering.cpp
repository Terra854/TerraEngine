// VulkanFrameRendering.cpp — Per-view render command recording and scene submission preparation
//
// This file owns one focused VulkanContext responsibility. State remains in VulkanContext::Impl,
// so the split changes compilation boundaries without changing resource lifetime or synchronization.

#include "VulkanContextSource.h"
#include "ThreadPool/ScopedTaskSettlement.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"


void VulkanContext::RenderFrame(
	std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication> publication,
	std::uint32_t publicationIndex) {
	// Measure lock contention separately from backend work. A long RenderFrame
	// sample can otherwise make a blocked caller look like expensive rendering.
	std::unique_lock<std::recursive_mutex> stateLock(impl->deviceStateMutex, std::defer_lock);
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: RenderFrame State Lock Wait", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		stateLock.lock();
	}
	// Skip rendering if initialization failed so we do not spam validation errors.
	if (!impl->initialized || impl->skipFrame || impl->deviceLost) return;
	if (impl->editorMode && impl->framebufferResized) {
		// Native resize callbacks change both the swapchain and the docked viewport
		// dimensions. EndFrame owns the settled swapchain rebuild; recording here
		// would only resize Scene/Game targets and then discard their command
		// buffers. Pause view recording until that protected boundary completes so
		// one resize drag produces one set of offscreen allocations and descriptors.
		return;
	}

	if(publication == nullptr || publicationIndex >= publication->views.size()
		|| publicationIndex >= publication->presentationInputs.size()) return;
	if(publication->rendererSessionGeneration != impl->renderSessionGeneration
		|| publication->backendResourceGeneration != impl->backendResourceGeneration) {
		// Direct callers must obey the same stale-root rule as the normal sealed
		// publication path; otherwise a resize could bind retired framebuffer data.
		return;
	}
	const TerraEngine::Renderer::RenderViewSelection& selection = publication->views[publicationIndex];
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation =
		publication->presentationInputs[publicationIndex];
	if(!presentation.Matches(selection)) return;
	const TerraEngine::Renderer::RenderViewCommandResolver commandResolver =
		TerraEngine::Renderer::MakeRenderViewCommandResolver(*publication, publicationIndex);
	if(commandResolver.Count() == 0u && !selection.VisibleOrdinals().empty()) return;
	impl->activeRenderFramePublication = publication;
	const auto renderFrameStart = BackendClock::now();
	const uint32_t currentFrame = impl->currentFrame;
	const ViewType view = selection.viewType == RenderViewType::Scene ? ViewType::Scene : ViewType::Game;
	const RenderViewType renderViewType = selection.viewType;
	const RenderOutputRequest& outputRequest = presentation.output;
	const std::uint32_t diagnosticViewId = selection.outputId;
	const TerraEngine::Renderer::RenderViewPresentationInput*
		publicationPresentation = &presentation;
	const std::uint32_t publicationViewIndex = publicationIndex;


	if (currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	if (currentFrame >= impl->pendingPrimaryCommandBuffersSubmitted.size()) {
		impl->pendingPrimaryCommandBuffersSubmitted.resize(impl->MAX_FRAMES_IN_FLIGHT, 0u);
	}
	auto& pendingCmdsForSnapshot = impl->pendingPrimaryCommandBuffers[currentFrame];
	uint32_t snapshotSlotIndex = VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME;
	const auto& lightingControl = presentation.efficiencyFixtureIntent.lightingControls;
	const bool controlledSnapshot = presentation.efficiencyFixtureIntent.controlsApplied
		&& lightingControl && lightingControl->snapshotSlot;
	const bool requestedSnapshotView = controlledSnapshot
		&& lightingControl->snapshotViewType == renderViewType
		&& lightingControl->snapshotOutputId == diagnosticViewId;
	const auto chooseSnapshotSlot = [&]() {
		const auto& occupied = impl->renderSnapshotSlotsUsed[currentFrame];
		if (requestedSnapshotView) {
			const auto requested = *lightingControl->snapshotSlot;
			return requested < occupied.size() && !occupied[requested] ? requested
				: VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME;
		}
		for (std::uint32_t candidate = 0; candidate < occupied.size(); ++candidate)
			if (!occupied[candidate] && (!controlledSnapshot || candidate != *lightingControl->snapshotSlot)) return candidate;
		return VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME;
	};
	snapshotSlotIndex = chooseSnapshotSlot();

	if (snapshotSlotIndex >= VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME) {
		TERRAENGINE_PROFILE_SCOPE("Vulkan: RenderFrame Snapshot Budget Recovery", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RenderSnapshotFallbackEvent fallback{};
		fallback.kind = outputRequest.kind;
		fallback.reason = RenderSnapshotFallbackReason::BudgetExhausted;
		fallback.outputId = outputRequest.outputId;
		fallback.required = outputRequest.priority == RenderOutputPriority::Required;


		if (outputRequest.priority == RenderOutputPriority::Optional) {
			fallback.result = RenderSnapshotFallbackResult::SkippedOptionalOutput;
			impl->pendingRenderSnapshotDiagnostics.fallbackEvents.push_back(fallback);
			return;
		}


		if (!impl->editorMode) {
			fallback.result = RenderSnapshotFallbackResult::FrameRecovery;
			impl->pendingRenderSnapshotDiagnostics.fallbackEvents.push_back(fallback);
			TERRAENGINE_LOG(LogLevel::Error, "Render snapshot budget exhausted for a required runtime output; skipping the pass to avoid shared-buffer corruption.");
			return;
		}

		// Budget exhaustion is the only remaining correctness fallback that may
		// serialize editor views. Normal scene/game rendering stays queued and
		// uses separate snapshot ranges below.
		VkSubmitInfo pendingSubmitInfo{};
		pendingSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		pendingSubmitInfo.commandBufferCount = static_cast<uint32_t>(pendingCmdsForSnapshot.size());
		pendingSubmitInfo.pCommandBuffers = pendingCmdsForSnapshot.data();

		impl->lightingUploadFrameSlotSafe[currentFrame] = false;
		const VkResult submitResult = impl->SubmitGraphicsQueue(1, &pendingSubmitInfo, VK_NULL_HANDLE);
		if (submitResult != VK_SUCCESS) {
			impl->ResetLightingUploadMirrors();
			impl->DiscardRecordedShadowWrites(currentFrame);
		}
		if (impl->MarkDeviceLost(submitResult, "intermediate editor view submit after snapshot budget exhaustion")) {
			return;
		}
		if (submitResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to submit intermediate editor view command buffers after snapshot budget exhaustion: " << submitResult);
			return;
		}
		if (impl->animationComputeRecordedForCurrentFrame
			&& !impl->animationComputeSubmissionMarkedForCurrentFrame) {
			// This exceptional editor prefix contains the first primary command
			// buffer, so it owns the animation generation's submission receipt.
			impl->MarkVulkanAnimationGenerationSubmitted(impl->pendingAnimationCommandPlan.rendererGeneration);
			impl->animationComputeSubmissionMarkedForCurrentFrame = true;
		}
		if (currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[currentFrame] = 1u;
		}
		impl->ConfirmHiZWritesSubmitted(currentFrame);
		impl->ConfirmShadowWritesSubmitted(currentFrame, pendingCmdsForSnapshot);
		const auto forcedIdleStart = BackendClock::now();
		const VkResult forcedIdleResult = impl->WaitGraphicsQueueIdle();
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::QueueIdleWait, TerraEngine::Renderer::BackendSubmitStage::ForcedIdle, renderViewType, diagnosticViewId, "Queue idle after snapshot budget exhaustion", false, true },
			ElapsedMilliseconds(forcedIdleStart),
			0.25f,
			"Snapshot budget exhaustion forced serialization; increase snapshot capacity or reduce required editor outputs.");
		if (forcedIdleResult != VK_SUCCESS)
		{
			// Failed prefix completion cannot preserve a skip authorization, even
			// if a later frame eventually establishes a new safe-write boundary.
			impl->ResetLightingUploadMirrors();
			if (impl->MarkDeviceLost(forcedIdleResult, "intermediate editor view queue idle after snapshot budget exhaustion")) {
				return;
			}
			TERRAENGINE_LOG(LogLevel::Error, "failed to wait for intermediate editor view command buffers after snapshot budget exhaustion: " << forcedIdleResult);
			return;
		}
		impl->CompleteShadowWrites(currentFrame); // Existing queue-idle proof above.
		impl->lightingUploadFrameSlotSafe[currentFrame] = true;
		impl->renderSnapshotSlotsUsed[currentFrame].fill(false);
		pendingCmdsForSnapshot.clear();
		if (currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[currentFrame] = 0u;
		}
		fallback.result = RenderSnapshotFallbackResult::SerializedSafely;
		impl->pendingRenderSnapshotDiagnostics.fallbackEvents.push_back(fallback);
		snapshotSlotIndex = chooseSnapshotSlot();
	}
	// Reserve on recording admission, including failed recordings: releasing a
	// partially overwritten slice inside this frame could authorize unsafe reuse.
	if (snapshotSlotIndex >= VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME) return;
	impl->renderSnapshotSlotsUsed[currentFrame][snapshotSlotIndex] = true;

	// `OffscreenTexture` is also the diagnostic classification used by shadow
	// outputs. Only camera-like Game/Scene outputs own captureView: letting a
	// later shadow submission select that framebuffer would clear its paired
	// typed identity attachment after an MCP capture has rendered.
	const bool renderingDedicatedCaptureTarget =
		outputRequest.kind == RenderOutputKind::OffscreenTexture
		&& (outputRequest.diagnosticsView == RenderViewType::Game
			|| outputRequest.diagnosticsView == RenderViewType::Scene);
	// Normal runtime views require an acquired swapchain image. A dedicated
	// capture owns a separate offscreen image and can therefore remain valid
	// independently of the presentation target.
	if (!impl->editorMode
		&& !renderingDedicatedCaptureTarget
		&& !impl->swapchainImageAcquired) {
		return;
	}
	VkExtent2D requestedCaptureExtent = impl->swapchainExtent;
	if (renderingDedicatedCaptureTarget)
	{
		// MCP object/custom captures are not editor viewport snapshots. Allocate
		// their private render target at the requested dimensions so projection,
		// culling, rasterization, and readback all agree on the same pixel grid.
		requestedCaptureExtent.width = outputRequest.targetSize.x > 0u
			? outputRequest.targetSize.x
			: std::max<std::uint32_t>(impl->swapchainExtent.width, 1u);
		requestedCaptureExtent.height = outputRequest.targetSize.y > 0u
			? outputRequest.targetSize.y
			: std::max<std::uint32_t>(impl->swapchainExtent.height, 1u);
	}
	const auto ResolveRequestedEditorExtent = [&]() {
		VkExtent2D extent = impl->swapchainExtent;
		extent.width = outputRequest.targetSize.x > 0u
			? outputRequest.targetSize.x
			: std::max<std::uint32_t>(impl->swapchainExtent.width, 1u);
		extent.height = outputRequest.targetSize.y > 0u
			? outputRequest.targetSize.y
			: std::max<std::uint32_t>(impl->swapchainExtent.height, 1u);
		return extent;
	};
	const VkExtent2D requestedEditorExtent = ResolveRequestedEditorExtent();

	// Editor views and dedicated exported-runtime captures both use lazily sized
	// offscreen targets. Runtime captures intentionally allocate only captureView;
	// Scene/Game editor targets remain editor-owned.

	if (impl->editorMode || renderingDedicatedCaptureTarget) {
		TERRAENGINE_PROFILE_SCOPE("Vulkan: RenderFrame Ensure Offscreen Targets", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto ensureOffscreenResources = [&](VulkanContext::Impl::OffscreenResources& resources, VkExtent2D targetExtent) {
			const bool extentMismatch = resources.extent.width != targetExtent.width
				|| resources.extent.height != targetExtent.height;
			if (resources.framebuffer == VK_NULL_HANDLE || extentMismatch)
			{
				// Replacement is transactional. The old target moves to the shared
				// editor submission/readback retirement domain after publication.
				impl->CreateOffscreenResources(resources, targetExtent);
			}
		};
		if (!renderingDedicatedCaptureTarget)
		{
			if (view == ViewType::Scene)
			{
				ensureOffscreenResources(impl->sceneView, requestedEditorExtent);
			}
			else
			{
				ensureOffscreenResources(impl->gameView, requestedEditorExtent);
			}
		}
		if (impl->deviceLost)
		{
			return;
		}
		if (renderingDedicatedCaptureTarget
			&& (impl->captureView.framebuffer == VK_NULL_HANDLE
				|| impl->captureView.extent.width != requestedCaptureExtent.width
				|| impl->captureView.extent.height != requestedCaptureExtent.height)) {
			impl->CreateOffscreenResources(impl->captureView, requestedCaptureExtent);
		}
	}

	// Apply the long-term preset policy on the CPU before upload so clustered
	// lighting receives a predictable retained-light list and an independent
	// shadow subset while the lightweight GPU analysis path observes a larger
	// uploaded candidate set for the next frame.
	const VulkanContext::Impl::LightBudgetApplicationResult budgetResult =
		impl->ApplyFrameLightBudgets(*publicationPresentation, renderViewType);
	const uint32_t requestedShadowSlotCount = std::min(budgetResult.diagnostics.shadowLightBudget, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	VulkanContext::Impl::ShadowResourceRequest requestedShadowResources{};
	requestedShadowResources.directionalCount = std::min(budgetResult.diagnostics.allocatedDirectionalRegionCount, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	requestedShadowResources.spotCount = std::min(budgetResult.diagnostics.allocatedSpotRegionCount, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	requestedShadowResources.areaCount = std::min(budgetResult.diagnostics.allocatedAreaRegionCount, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	requestedShadowResources.pointCount = std::min(budgetResult.diagnostics.allocatedPointRegionCount, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);

	std::optional<TerraEngine::Renderer::RendererEfficiencyFixtureActionReceipt> shadowResourceRecreationAction;
	const auto EnsureShadowRenderResources = [&]() -> bool {

		if (requestedShadowSlotCount == 0u || requestedShadowResources.TotalCount() == 0u) {
			return true;
		}
		const bool directionalMissing = requestedShadowResources.directionalCount > 0u
			&& (impl->directionalShadowDepths[0].view == VK_NULL_HANDLE || impl->directionalShadowColors[0].view == VK_NULL_HANDLE);
		const bool spotMissing = requestedShadowResources.spotCount > 0u
			&& (impl->spotShadowDepths[0].view == VK_NULL_HANDLE || impl->spotShadowColors[0].view == VK_NULL_HANDLE);
		const bool areaMissing = requestedShadowResources.areaCount > 0u
			&& (impl->areaShadowDepths[0].view == VK_NULL_HANDLE || impl->areaShadowColors[0].view == VK_NULL_HANDLE);
		const bool pointMissing = requestedShadowResources.pointCount > 0u
			&& impl->pointShadowCubeDepths[0].view == VK_NULL_HANDLE;
		bool forceOwnedRecreation = false;
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
		const auto recreationKey = presentation.efficiencyFixtureIntent.correlation.operationId + ":" + presentation.efficiencyFixtureIntent.correlation.fixtureStepId;
		forceOwnedRecreation = !presentation.efficiencyFixtureIntent.correlation.operationId.empty()
			&& presentation.efficiencyFixtureIntent.injection == TerraEngine::Renderer::RendererEfficiencyFixtureInjection::ShadowResourceRecreation
			&& recreationKey != impl->lastShadowValidationResourceRecreationKey;
#endif

		if (!(impl->activeShadowResourceRequest == requestedShadowResources)
			|| directionalMissing
			|| spotMissing
			|| areaMissing
			|| pointMissing || forceOwnedRecreation) {
			const auto oldAllocation = impl->shadowAllocationGeneration;
			const auto oldLogicalEntries = impl->shadowPublicationLedger.RetainedLogicalEntryCount();

			if (!impl->EnsureShadowRenderResources(requestedShadowResources)) {
				return false;
			}
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
			if (forceOwnedRecreation) {
				impl->lastShadowValidationResourceRecreationKey = recreationKey;
				TerraEngine::Renderer::RendererEfficiencyFixtureActionReceipt action;
				action.injection = TerraEngine::Renderer::RendererEfficiencyFixtureInjection::ShadowResourceRecreation;
				action.correlation = presentation.efficiencyFixtureIntent.correlation;
				action.frameSerial = publication->frameId; action.commandSerial = 0; action.recorded = true;
				action.oldAllocationGeneration = oldAllocation; action.newAllocationGeneration = impl->shadowAllocationGeneration;
				action.priorOwnerRejected = oldLogicalEntries != 0 && impl->shadowPublicationLedger.RetainedLogicalEntryCount() == 0
					&& action.newAllocationGeneration > oldAllocation;
				action.reason = action.priorOwnerRejected ? "Actual shadow resource replacement revoked prior allocation claims."
					: "Actual shadow resource replacement occurred; no prior retained owner was available to reject.";
				shadowResourceRecreationAction = std::move(action);
			}
#endif
		}


		if (impl->shadowRenderPass == VK_NULL_HANDLE) {
			VkAttachmentDescription colorAttachment{};
			colorAttachment.format = impl->swapchainImageFormat;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// Shadow images are explicitly initialized and persist across frames.
			// Preserve their real attachment layout instead of discarding it again.
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = impl->depthFormat;
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// RecordShadowPass explicitly transitions the reused depth layer back to
			// attachment layout. Keeping both layouts equal prevents an implicit
			// begin-pass transition from racing a write in an earlier queue submit.
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorAttachmentRef{};
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depthAttachmentRef{};
			depthAttachmentRef.attachment = 1;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass{};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &colorAttachmentRef;
			subpass.pDepthStencilAttachment = &depthAttachmentRef;

			std::array<VkSubpassDependency, 2> dependencies{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			// Shadow images persist across frames. Order the previous render-pass
			// writes and fragment sampling before clearing the same attachments again;
			// queue submission order alone is not a memory dependency for these accesses.
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT
				| VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
			VkRenderPassCreateInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassInfo.pAttachments = attachments.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();

			if (vkCreateRenderPass(impl->device, &renderPassInfo, nullptr, &impl->shadowRenderPass) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Warning, "failed to create shadow render pass");
				impl->shadowRenderPass = VK_NULL_HANDLE;
				return false;
			}
		}

		const uint32_t pointShadowSize = VulkanContext::Impl::POINT_SHADOW_MAP_RESOLUTION;
		const auto Shadow2DRegionSizeForSlot = [](std::uint32_t typedSlot) {
			// Slots after the full-quality reserve are backed by smaller images,
			// so their framebuffers must be created at the same reduced extent.
			return typedSlot >= 16u
				? VulkanContext::Impl::SHADOW_MAP_RESOLUTION / 2u
				: VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
		};

		auto EnsureFramebuffer = [&](VkFramebuffer& framebuffer, VkImageView colorView, VkImageView depthView, uint32_t width, uint32_t height) -> bool {

			if (framebuffer != VK_NULL_HANDLE) {
				return true;
			}
			std::array<VkImageView, 2> attachments = { colorView, depthView };
			VkFramebufferCreateInfo framebufferInfo{};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = impl->shadowRenderPass;
			framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			framebufferInfo.pAttachments = attachments.data();
			framebufferInfo.width = width;
			framebufferInfo.height = height;
			framebufferInfo.layers = 1;
			return vkCreateFramebuffer(impl->device, &framebufferInfo, nullptr, &framebuffer) == VK_SUCCESS;
		};


		for (uint32_t slotIndex = 0u; slotIndex < impl->activeShadowResourceRequest.directionalCount; ++slotIndex) {
			const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
			if (!EnsureFramebuffer(impl->directionalShadowFramebuffers[slotIndex], impl->directionalShadowColors[slotIndex].view, impl->directionalShadowDepths[slotIndex].view, regionSize, regionSize)) {
				return false;
			}
		}

		for (uint32_t slotIndex = 0u; slotIndex < impl->activeShadowResourceRequest.spotCount; ++slotIndex) {
			const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
			if (!EnsureFramebuffer(impl->spotShadowFramebuffers[slotIndex], impl->spotShadowColors[slotIndex].view, impl->spotShadowDepths[slotIndex].view, regionSize, regionSize)) {
				return false;
			}
		}

		for (uint32_t slotIndex = 0u; slotIndex < impl->activeShadowResourceRequest.areaCount; ++slotIndex) {
			const std::uint32_t regionSize = Shadow2DRegionSizeForSlot(slotIndex);
			if (!EnsureFramebuffer(impl->areaShadowFramebuffers[slotIndex], impl->areaShadowColors[slotIndex].view, impl->areaShadowDepths[slotIndex].view, regionSize, regionSize)) {
				return false;
			}
		}

		for (uint32_t slotIndex = 0u; slotIndex < impl->activeShadowResourceRequest.pointCount; ++slotIndex) {

			for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {

				if (impl->pointShadowColorFaceViewsBySlot[slotIndex][faceIndex] == VK_NULL_HANDLE || impl->pointShadowDepthFaceViewsBySlot[slotIndex][faceIndex] == VK_NULL_HANDLE) {
					return false;
				}

				if (!EnsureFramebuffer(impl->pointShadowFaceFramebuffersBySlot[slotIndex][faceIndex], impl->pointShadowColorFaceViewsBySlot[slotIndex][faceIndex], impl->pointShadowDepthFaceViewsBySlot[slotIndex][faceIndex], pointShadowSize, pointShadowSize)) {
					return false;
				}
			}
		}

		return true;
	};

	const bool shadowResourcesReady = EnsureShadowRenderResources();

	// Shadow allocation and descriptor replacement happen before any primary is
	// recording. Cold startup/recovery must also create the pass before pipeline
	// prewarm; a null pass cannot establish a reusable shadow pipeline.
	if (impl->deviceLost) return;

	// Retain typed pass provenance; handles are compared directly, never cast to integers.
	const VkRenderPass shadowPrewarmPass = impl->shadowRenderPass;
	const auto shadowPrewarmAllocation = impl->shadowAllocationGeneration;
	// 1. Prepare All Resources (Pipelines, Meshes, Materials) BEFORE starting command recording.
	// This ensures descriptor set updates don't invalidate the recording command buffer.
	const TerraEngine::UUID defaultMeshCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
	const TerraEngine::UUID defaultTaskCollectionId = ResourceManager::Instance().GetDefaultTaskShaderCollectionID();
	const bool defaultTaskPipelineAvailable = impl->taskShaderSupported
		&& (ResourceManager::Instance().GetShaderCollection(defaultTaskCollectionId) != nullptr);

	VkRenderPass currentPass = (impl->editorMode || renderingDedicatedCaptureTarget)
		? impl->offscreenRenderPass
		: impl->renderPass;
	if (currentPass == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Warning, "Skipping Vulkan draw submission because the active render pass is not ready.");
		return;
	}
	VkImage activeDepthImage = impl->depthImage;
	VkImageView activeDepthView = impl->depthImageView;
	VkExtent2D activeRenderExtent = impl->swapchainExtent;
	if (impl->editorMode || renderingDedicatedCaptureTarget) {
		const VulkanContext::Impl::OffscreenResources* activeOffscreenView = renderingDedicatedCaptureTarget
			? &impl->captureView
			: ((view == ViewType::Scene) ? &impl->sceneView : &impl->gameView);
		activeDepthImage = activeOffscreenView->depthImage;
		activeDepthView = activeOffscreenView->depthView;
		activeRenderExtent = activeOffscreenView->extent;
		if (activeOffscreenView->image == VK_NULL_HANDLE
			|| activeOffscreenView->framebuffer == VK_NULL_HANDLE
			|| activeRenderExtent.width == 0u
			|| activeRenderExtent.height == 0u) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping Vulkan draw submission because the editor offscreen color target is not ready.");
			return;
		}
		if (activeDepthImage == VK_NULL_HANDLE || activeDepthView == VK_NULL_HANDLE) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping Vulkan draw submission because the editor view depth attachment is not ready.");
			return;
		}
	}

	struct DrawResourcePreparationKey
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID collectionId = TerraEngine::EMPTY_UUID;
		VkRenderPass renderPass = VK_NULL_HANDLE;
		std::uint32_t materialFlags = 0u;
		bool conventionalVisibilityIdentity = false;

		bool operator==(const DrawResourcePreparationKey&) const = default;
	};
	struct DrawResourcePreparationKeyHash
	{
		std::size_t operator()(const DrawResourcePreparationKey& value) const noexcept
		{
			std::size_t hash = std::hash<TerraEngine::UUID>{}(value.meshId);
			auto combine = [&hash](std::size_t lane) noexcept
			{
				hash ^= lane + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
			};
			combine(std::hash<TerraEngine::UUID>{}(value.materialId));
			combine(std::hash<TerraEngine::UUID>{}(value.collectionId));
			combine(std::hash<VkRenderPass>{}(value.renderPass));
			combine(std::hash<std::uint32_t>{}(value.materialFlags));
			combine(std::hash<bool>{}(value.conventionalVisibilityIdentity));
			return hash;
		}
	};
	std::size_t drawResourceUpperBound = commandResolver.Count();
	for(const ShadowDrawList& shadowDrawList : presentation.shadowDrawLists)
	{
		drawResourceUpperBound += shadowDrawList.stableOrdinals.size();
	}
	std::unordered_set<DrawResourcePreparationKey, DrawResourcePreparationKeyHash>
		preparedDrawResourceKeys;
	// Instance count is usually much larger than unique mesh/material/pass
	// combinations. Keep the bucket array proportional to expected diversity.
	preparedDrawResourceKeys.reserve(std::min<std::size_t>(
		drawResourceUpperBound, 128u));
	// Mesh/material IDs are intentionally empty in this local key: graphics
	// pipeline selection depends only on the exact pass/collection/flags tuple.
	std::unordered_map<DrawResourcePreparationKey, VulkanPipeline*, DrawResourcePreparationKeyHash> preparedFramePipelines;
	preparedFramePipelines.reserve(std::min<std::size_t>(drawResourceUpperBound, 128U));
	const auto PrepareFramePipeline = [&](TerraEngine::UUID collection, VkRenderPass pass, std::uint32_t flags) {
		const bool identity = impl->UsesConventionalVisibilityAttachments(pass);
		impl->EnsurePipeline(collection, pass, flags, identity);
		const auto cachedPass = impl->pipelineCache.find(pass);
		if (cachedPass == impl->pipelineCache.end()) return;
		VulkanPipeline* selected = nullptr;
		std::uint64_t selectedGeneration = 0;
		for (auto& [key, pipeline] : cachedPass->second) {
			if (key.shaderCollectionId == collection && key.materialFlags == flags && key.usesConventionalVisibilityIdentity == identity
				&& pipeline.pipeline != VK_NULL_HANDLE && pipeline.layout != VK_NULL_HANDLE
				&& (!selected || key.collectionGeneration > selectedGeneration)) {
				selected = &pipeline; selectedGeneration = key.collectionGeneration;
			}
		}
		// Valid historical entries remain allocated. If a new shader cannot build,
		// retain the latest real usable GPU pipeline rather than fabricate a new
		// publication or remove complete geometry during command recording.
		if (selected) preparedFramePipelines[{TerraEngine::EMPTY_UUID, TerraEngine::EMPTY_UUID, collection, pass, flags, identity}] = selected;
	};
	TerraEngine::UUID lastPreparationMaterialId = TerraEngine::EMPTY_UUID;
	std::shared_ptr<const Material> lastPreparationMaterialOwner;
	const Material* lastPreparationMaterial = nullptr;
	std::optional<DrawResourcePreparationKey> lastPreparedResourceKey;
	const auto PrepareDrawResources = [&](const RenderItemRecord& command,
		VkRenderPass preparationPass) {
		const Material* material = nullptr;
		if(command.materialId == lastPreparationMaterialId)
		{
			material = lastPreparationMaterial;
		}
		else
		{
			lastPreparationMaterialOwner = ResourceManager::Instance().AcquireMaterialSnapshot(command.materialId);
			material = lastPreparationMaterialOwner.get();
			lastPreparationMaterialId = command.materialId;
			lastPreparationMaterial = material;
		}

		if (!material) return;

		TerraEngine::UUID collectionId = material->header.shaderCollectionId;

		if (collectionId == TerraEngine::EMPTY_UUID) {
			collectionId = defaultMeshCollectionId;
		}

		if (collectionId == defaultMeshCollectionId
			&& defaultTaskPipelineAvailable
			&& !command.animationBinding.RequiresOwnerMeshDeformationFallback()
			&& !(impl->pendingAnimationImmutablePlan != nullptr
				&& impl->pendingAnimationImmutablePlan->WorkPackets().size() > 1u
				&& command.HasCompleteGpuAnimation())) {
			// Owner-mesh CPU/GPU deformation does not publish current per-meshlet
			// bounds to the task stage. Keep those draws on the mesh-only route so
			// the graphics pass fails open over the complete selected range without
			// exposing stale task payload/bounds data to the device.
			collectionId = defaultTaskCollectionId;
		}

		const bool preparationUsesConventionalVisibilityIdentity =
			impl->UsesConventionalVisibilityAttachments(preparationPass);
		const DrawResourcePreparationKey preparationKey{
			command.meshId,
			command.materialId,
			collectionId,
			preparationPass,
			material->header.flags,
			preparationUsesConventionalVisibilityIdentity};
		if(lastPreparedResourceKey.has_value()
			&& *lastPreparedResourceKey == preparationKey)
		{
			return;
		}
		if(!preparedDrawResourceKeys.emplace(preparationKey).second)
		{
			lastPreparedResourceKey = preparationKey;
			return;
		}
		lastPreparedResourceKey = preparationKey;
		// Mesh/material/pipeline preparation is invariant for duplicate commands in
		// one protected view submission. Record the exact key only after collection
		// selection so animated fallback and shadow render-pass differences remain
		// independent.
		impl->EnsureMesh(command.meshId);
		impl->EnsurePipeline(collectionId, preparationPass, material->header.flags,
			preparationUsesConventionalVisibilityIdentity);
		impl->EnsureMaterialDescriptorSets(collectionId, command.materialId);

		// Fallback for failed task pipeline
		auto passIt = impl->pipelineCache.find(preparationPass);
		if (passIt != impl->pipelineCache.end()) {
			const std::optional<PipelineKey> pipeKey =
				impl->BuildPipelineKey(collectionId, material->header.flags,
					preparationUsesConventionalVisibilityIdentity);

			if (!pipeKey.has_value()
				|| passIt->second.find(*pipeKey) == passIt->second.end()
				|| passIt->second.at(*pipeKey).pipeline == VK_NULL_HANDLE) {

				if (collectionId == defaultTaskCollectionId) {
					impl->EnsurePipeline(defaultMeshCollectionId, preparationPass,
						material->header.flags, preparationUsesConventionalVisibilityIdentity);
				}
			}
		}
	};

	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: RenderFrame Prepare Draw Resources", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto descriptorStart = BackendClock::now();
		if(commandResolver.Count() != 0u
			&& !presentation.drawResourcePreparationCommandIndices.empty())
		{
			for(const std::uint32_t commandIndex :
				presentation.drawResourcePreparationCommandIndices)
			{
				if(commandIndex < commandResolver.Count())
				{
					RenderItemRecord command{};
					if(commandResolver.Resolve(commandIndex, command)) {
						PrepareDrawResources(command, currentPass);
					}
				}
			}
		}
		else
		{
			// The resolver always returns a local value backed by sealed roots, so
			// this fallback stays safe without a copied command collection.
			for(std::size_t commandIndex = 0u; commandIndex < commandResolver.Count(); ++commandIndex)
			{
				RenderItemRecord command{};
				if(commandResolver.Resolve(commandIndex, command)) {
					PrepareDrawResources(command, currentPass);
				}
			}
		}
		for (const ShadowDrawList& shadowDrawList : presentation.shadowDrawLists) {
			for(std::size_t itemIndex = 0u;
				itemIndex < shadowDrawList.stableOrdinals.size(); ++itemIndex) {
				RenderItemRecord command{};
				if(!TerraEngine::Renderer::ResolveShadowDrawListItem(
					*publication, presentation.referenceFrame,
					shadowDrawList, itemIndex, command)) {
					continue;
				}
				// Shadow virtual geometry arrives as light-view draw commands with
				// virtualMeshletOffset/Count already selected by RenderSystem. The
				// backend prepares the same mesh-shader resources as the camera path,
				// but shadow visibility never reuses camera Hi-Z decisions.
				if (shadowResourcesReady && impl->shadowRenderPass != VK_NULL_HANDLE)
					PrepareDrawResources(command, impl->shadowRenderPass);
				// Prewarm every real light-qualified cut, including off-camera
				// casters. Resource-key deduplication cannot substitute another
				// light's selected range; page preparation owns its exact cut key.
				const auto mesh = impl->meshCache.find(command.meshId);
				const auto material = impl->materialSourceOwners.find(command.materialId);
				if (mesh != impl->meshCache.end() && material != impl->materialSourceOwners.end() && material->second) {
					TerraEngine::Renderer::Vulkan::ShadowPageCutRequest pageRequest;
					pageRequest.meshId = command.meshId; pageRequest.mesh = mesh->second.sourceOwner;
					pageRequest.cpuPublicationGeneration = mesh->second.cpuPublicationGeneration;
					pageRequest.ownerMeshletOffset = std::min(command.virtualMeshletOffset, mesh->second.meshletCount);
					const auto available = mesh->second.meshletCount - pageRequest.ownerMeshletOffset;
					pageRequest.ownerMeshletCount = command.virtualMeshletCount == 0U ? available : std::min(command.virtualMeshletCount, available);
					pageRequest.selectedLod = command.selectedVirtualLod;
					pageRequest.deformationUnsupported = command.animationBinding.HasCpuDeformation() || command.HasCompleteGpuAnimation();
					const auto collection = material->second->header.shaderCollectionId;
					pageRequest.materialUnsupported = collection != TerraEngine::EMPTY_UUID
						&& collection != defaultMeshCollectionId && collection != defaultTaskCollectionId;
					(void)impl->EnsureShadowAuthoredPagePayload(pageRequest);
				}
			}
		}


		if (presentation.sky.hasTexture && presentation.sky.textureId != TerraEngine::EMPTY_UUID)
		{
			impl->EnsureTexture(presentation.sky.textureId);
		}
		// Texture publication may have rebuilt a material with a newer immutable
		// owner than the initial CPU lookup. Seal pipelines from the final uploaded
		// SSBO owners after all resource preparation, before this primary begins.
		for (const auto& key : preparedDrawResourceKeys) {
			const auto uploaded = impl->materialSourceOwners.find(key.materialId);
			if (uploaded == impl->materialSourceOwners.end() || !uploaded->second) continue;
			auto collection = uploaded->second->header.shaderCollectionId;
			if (collection == TerraEngine::EMPTY_UUID) collection = defaultMeshCollectionId;
			PrepareFramePipeline(collection, key.renderPass, uploaded->second->header.flags);
			if (collection == defaultMeshCollectionId || collection == defaultTaskCollectionId) {
				PrepareFramePipeline(defaultMeshCollectionId, key.renderPass, uploaded->second->header.flags);
				if (defaultTaskPipelineAvailable) PrepareFramePipeline(defaultTaskCollectionId, key.renderPass, uploaded->second->header.flags);
			}
		}
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::DescriptorUpdate, TerraEngine::Renderer::BackendSubmitStage::DescriptorUpdate, renderViewType, diagnosticViewId, "Prepare descriptors/pipelines/resources", false, true },
			ElapsedMilliseconds(descriptorStart));
	}

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	const bool useMainCommandBuffer = impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()
		|| impl->pendingPrimaryCommandBuffers[impl->currentFrame].empty();
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: RenderFrame Acquire Command Buffer", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);

		if (useMainCommandBuffer) {
			cmd = impl->commandBuffers[impl->currentFrame];
			const auto resetCommandStart = BackendClock::now();
			const VkResult resetCommandResult = vkResetCommandBuffer(cmd, 0);
			if (impl->MarkDeviceLost(
				resetCommandResult, "render frame primary command buffer reset"))
			{
				return;
			}
			if (resetCommandResult != VK_SUCCESS)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"RenderFrame could not reset the completed primary command buffer: "
						<< resetCommandResult);
				impl->skipFrame = true;
				return;
			}
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::RhiBeginFrame, TerraEngine::Renderer::BackendSubmitStage::CommandBufferReset, renderViewType, diagnosticViewId, "Reset primary command buffer", false, true },
				ElapsedMilliseconds(resetCommandStart));
		} else {
			VkCommandBufferAllocateInfo allocInfo{};
			allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			allocInfo.commandPool = impl->commandPools[impl->currentFrame];
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = 1;

			const auto allocateCommandStart = BackendClock::now();
			if (vkAllocateCommandBuffers(impl->device, &allocInfo, &cmd) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to allocate command buffer for additional camera pass");
				return;
			}
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::QueueIdleWait, TerraEngine::Renderer::BackendSubmitStage::CommandBufferLifetime, renderViewType, diagnosticViewId, "Allocate additional camera command buffer", false, true },
				ElapsedMilliseconds(allocateCommandStart),
				0.25f);

			if (impl->currentFrame >= impl->extraPrimaryCommandBuffers.size()) {
				impl->extraPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
			}
			impl->extraPrimaryCommandBuffers[impl->currentFrame].push_back(cmd);
		}
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;


	const auto commandRecordingStart = BackendClock::now();
	// This scope intentionally spans the complete primary command recording
	// interval. Resetting the optional immediately after vkEndCommandBuffer keeps
	// later submission bookkeeping out of the measurement without reindenting
	// the large Vulkan recording routine.
	std::optional<TerraEngine::Profiling::ProfilerScope> commandRecordingProfile;
	commandRecordingProfile.emplace("Vulkan: RenderFrame Record Primary Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
		return;
	}
	const std::uint64_t shadowCommandSerial = impl->nextShadowCommandSerial++;
	impl->shadowCommandOwnership[cmd] = {shadowCommandSerial, currentFrame};
	// The resource replacement already occurred at protected preparation. Bind
	// its receipt to this real command only after recording has begun.
	if (shadowResourceRecreationAction)
		shadowResourceRecreationAction->commandSerial = shadowCommandSerial;
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
	if(presentation.efficiencyFixtureIntent.injection == TerraEngine::Renderer::RendererEfficiencyFixtureInjection::RecordedSubmitAbort
		&& !presentation.efficiencyFixtureIntent.correlation.operationId.empty())
	{
		impl->shadowCommandOwnership[cmd].validationAbortKey = presentation.efficiencyFixtureIntent.correlation.operationId
			+ ":" + presentation.efficiencyFixtureIntent.correlation.fixtureStepId;
		impl->shadowCommandOwnership[cmd].validationAbortCorrelation = presentation.efficiencyFixtureIntent.correlation;
		impl->shadowCommandOwnership[cmd].validationFrameSerial = publication->frameId;
	}
#endif
	// Every exit after recording begins must revoke unsubmitted shadow writes.
	// The guard is released only once the completed primary joins EndFrame's
	// actual ordered command list; exceptions cannot strand reusable candidates.
	struct ShadowRecordingSettlement
	{
		VulkanContext::Impl* owner;
		VkCommandBuffer command;
		std::uint64_t serial;
		bool admitted = false;
		~ShadowRecordingSettlement() noexcept
		{
			if (admitted) return;
			owner->staticShadowLifetime.DiscardUnsubmitted(serial);
			owner->staticShadowDiscardedCommands[owner->staticShadowDiscardCursor++ % owner->staticShadowDiscardedCommands.size()] = serial;
			for (auto& entry : owner->staticShadowEntries) if (!entry->layoutEstablished && entry->initializationCommand == serial) {
				entry->layouts.fill(VK_IMAGE_LAYOUT_UNDEFINED); entry->initializationCommand = 0;
			}
			try { owner->staticShadowPublicationLedger.Discard(serial); }
			catch (...) { owner->staticShadowPublicationLedger.Reset(owner->renderSessionGeneration); }
			try { owner->lightingDepthEvidence.DiscardCommand(serial); owner->shadowValidationReadback.DiscardCommand(serial); owner->shadowPublicationLedger.Discard(serial); }
			catch (...) { owner->shadowPublicationLedger.Reset(owner->renderSessionGeneration); }
			owner->shadowCommandOwnership.erase(command);
		}
	} shadowRecordingSettlement{impl.get(), cmd, shadowCommandSerial};
	// A mixed P2 cohort captures lighting and shadow adjuncts from the same real
	// targeted frame. Untargeted frames still render, but do not seize its token.
	const bool shadowValidationRequested = !presentation.efficiencyFixtureIntent.correlation.operationId.empty()
		&& (!lightingControl || !lightingControl->frameSlot || *lightingControl->frameSlot == currentFrame);
	if(shadowValidationRequested) impl->shadowValidationReadback.Init(impl->allocator, impl->physicalDevice);
	// The outer scope measures the complete primary recording interval, while
	// these sequential child scopes separate CPU preparation from Vulkan command
	// emission. Keeping only one child alive at a time preserves an exact sample
	// hierarchy without changing command order or resource ownership.
	std::optional<TerraEngine::Profiling::ProfilerScope>
		commandRecordingPhaseProfile;
	const auto beginCommandRecordingPhase =
		[&commandRecordingPhaseProfile](const char* name)
		{
			commandRecordingPhaseProfile.reset();
			commandRecordingPhaseProfile.emplace(
				name,
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		};
	beginCommandRecordingPhase(
		"Vulkan: Primary Record Animation");
	impl->RecordDiagnosticCheckpoint(
		cmd,
		renderingDedicatedCaptureTarget
			? "capture.primary.begin"
			: (view == ViewType::Scene ? "scene.primary.begin" : "game.primary.begin"));
	if (useMainCommandBuffer
		&& !impl->animationComputeRecordedForCurrentFrame
		&& impl->pendingAnimationCommandPlan.hasWork
		&& impl->pendingAnimationCommandPlan.valid
		&& impl->pendingAnimationImmutablePlan != nullptr)
	{
		// Record one immutable animation generation before every view. The compute
		// work remains in this primary command buffer, so it shares the normal
		// graphics submission, fence, and present chain with no queue submit or
		// CPU pose readback of its own.
		const bool descriptorsReady = impl->currentFrame < impl->globalDescriptorSets.size()
			&& impl->currentFrame < impl->animationDescriptorSets.size()
			&& impl->globalDescriptorSets[impl->currentFrame] != VK_NULL_HANDLE
			&& impl->animationDescriptorSets[impl->currentFrame] != VK_NULL_HANDLE
			&& impl->animationComputePipelines.layout != VK_NULL_HANDLE;
		if (descriptorsReady)
		{
			const VkDescriptorSet globalSet = impl->globalDescriptorSets[impl->currentFrame];
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
				impl->animationComputePipelines.layout, 0U, 1U, &globalSet, 0U, nullptr);
			const TerraEngine::Renderer::Vulkan::VulkanAnimationRecordingResult result =
				TerraEngine::Renderer::Vulkan::RecordVulkanAnimationCommands(
					cmd,
					impl->pendingAnimationCommandPlan,
					impl->animationComputePipelines,
					impl->animationDescriptorSets[impl->currentFrame]);
			if (result.status == TerraEngine::Renderer::Vulkan::VulkanAnimationRecordingStatus::Recorded
				&& result.recordedDispatchCount != 0U)
			{
				impl->animationComputeRecordedForCurrentFrame = true;
				impl->recordedAnimationDispatchCountForCurrentFrame =
					result.recordedDispatchCount;
			}
		}
		if (!impl->animationComputeRecordedForCurrentFrame)
		{
			// Do not let a graphics draw sample an upload that was not produced by
			// this command buffer. The shader sees invalid bridge indices and leaves
			// vertices in bind pose, which is the safe visual fallback.
			impl->activeAnimationGraphicsBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			impl->pendingAnimationCommandPlan = {};
			impl->pendingAnimationImmutablePlan.reset();
			impl->pendingAnimationFrameId = 0U;
			impl->pendingDeformationCommandPlan = {};
			impl->pendingDeformationImmutablePlan.reset();
			impl->activeDeformationGraphicsBufferIndex =
				VulkanContext::Impl::
					INVALID_BINDLESS_BUFFER_INDEX;
		}
	}
	beginCommandRecordingPhase(
		"Vulkan: Primary Record Deformation");
	if(useMainCommandBuffer
		&& !impl->deformationComputeRecordedForCurrentFrame
		&& impl->pendingDeformationCommandPlan.hasWork
		&& impl->pendingDeformationCommandPlan.valid
		&& impl->pendingDeformationImmutablePlan != nullptr
		&& impl->currentFrame
			< impl->deformationDescriptorSets.size())
	{
		// Deformation, exact dynamic bounds, and refinement share this primary
		// command buffer with animation and every view. The recorder emits all
		// compute-to-compute and compute-to-graphics visibility boundaries with
		// no queue submit or CPU wait between stages.
		TerraEngine::Renderer::Vulkan::
			VulkanDeformationRecordingResult result{};
		result = TerraEngine::Renderer::Vulkan::
			RecordVulkanDeformationCommands(
				cmd,
				impl->pendingDeformationCommandPlan,
				*impl->pendingDeformationImmutablePlan,
				impl->deformationComputePipelines,
				impl->deformationDescriptorSets[
					impl->currentFrame]);
		if(result.recorded)
		{
			impl->deformationComputeRecordedForCurrentFrame =
				true;
			impl->recordedDeformationDispatchCountForCurrentFrame =
				result.deformationDispatchCount;
		}
		else
		{
			// A partial generation is never visible. Graphics remains on the
			// existing conservative owner/fallback route for this frame.
			impl->pendingDeformationCommandPlan = {};
			impl->pendingDeformationImmutablePlan.reset();
		}
	}
	beginCommandRecordingPhase(
		"Vulkan: Primary Prepare Virtual Geometry Resources");
	if (impl->validationLayersEnabled && vkSetDebugUtilsObjectNameEXT != nullptr) {
		// Validation messages otherwise print only an opaque command-buffer handle,
		// which makes a cross-submit fault impossible to associate with Scene, Game,
		// capture, or runtime rendering after a temporary buffer has been freed.
		const std::string commandBufferName = std::string(impl->editorMode ? "Editor " : "Runtime ")
			+ (renderingDedicatedCaptureTarget ? "Capture" : (view == ViewType::Scene ? "Scene" : "Game"))
			+ " Primary Render Command Buffer";
		VkDebugUtilsObjectNameInfoEXT nameInfo{};
		nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		nameInfo.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
		nameInfo.objectHandle = reinterpret_cast<uint64_t>(cmd);
		nameInfo.pObjectName = commandBufferName.c_str();
		vkSetDebugUtilsObjectNameEXT(impl->device, &nameInfo);
	}
	impl->MarkGpuTimestampBegin(cmd, impl->currentFrame, view, impl->backendFrameSerial);

	uint32_t imageIndex = impl->acquiredImageIndex;
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;


	if (!impl->editorMode && !renderingDedicatedCaptureTarget) {
		renderPassInfo.renderPass = impl->renderPass;
		renderPassInfo.framebuffer = impl->framebuffers[imageIndex];
	} else {
		// Editor views and packaged-runtime capture validation render into
		// transfer-readable offscreen targets.
		renderPassInfo.renderPass = impl->offscreenRenderPass;
		renderPassInfo.framebuffer = renderingDedicatedCaptureTarget
			? impl->captureView.framebuffer
			: ((view == ViewType::Scene) ? impl->sceneView.framebuffer : impl->gameView.framebuffer);
	}

	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = activeRenderExtent;

	// Conventional render passes clear color, typed identity, depth, and the
	// optional VRS attachment in that declared attachment order.
	std::array<VkClearValue, 4> clearValues{};
	if (view == ViewType::Scene && presentation.sceneViewOrthographic)
	{
		clearValues[0].color = {{0.36f, 0.43f, 0.50f, 1.0f}};
	}
	else
	{
		clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	}
	clearValues[1].color.uint32[0] = 0u;
	clearValues[1].color.uint32[1] = 0u;
	clearValues[1].color.uint32[2] = 0u;
	clearValues[1].color.uint32[3] = 0u;
	clearValues[2].depthStencil = {1.0f, 0};
	clearValues[3].color = {{0, 0, 0, 0}};
	renderPassInfo.clearValueCount = impl->UsesVrsAttachment() ? 4u : 3u;
	renderPassInfo.pClearValues = clearValues.data();

	// Create Grid Pipeline if needed (lazy init).

	if (view == ViewType::Scene && presentation.sceneViewGridVisible) {
		impl->CreateSceneViewGridPipeline(renderPassInfo.renderPass);
	}
	impl->CreateSkyBackgroundPipeline(renderPassInfo.renderPass);

	const uint32_t viewIndex = (view == ViewType::Scene) ? 1u : 0u;
	const uint32_t statsSlotIndex = impl->GetStatsSlotIndex(impl->currentFrame, view);
	RenderSnapshotHandle renderSnapshot{};
	renderSnapshot.slotIndex = snapshotSlotIndex;
	renderSnapshot.statsSlotIndex = statsSlotIndex;
	renderSnapshot.lightDataOffset = impl->GetSnapshotLightDataOffset(snapshotSlotIndex);
	renderSnapshot.lightAnalysisInputOffset = impl->GetSnapshotLightAnalysisInputOffset(snapshotSlotIndex);
	renderSnapshot.clusterDataOffset = impl->GetSnapshotClusterDataOffset(snapshotSlotIndex);
	renderSnapshot.lightIndexOffset = impl->GetSnapshotLightIndexOffset(snapshotSlotIndex);
	renderSnapshot.clusterDepthOffset = impl->GetSnapshotClusterDepthOffset(snapshotSlotIndex);
	// The indirect range cannot be selected from the view slot: shadow casters
	// are appended after ordinary draw preparation. Reserve the exact range once
	// both lists are complete, immediately before the upload below.
	renderSnapshot.indirectDrawOffset = 0u;
	renderSnapshot.state = RenderSnapshotState::Acquired;
	renderSnapshot.valid = true;
	impl->activeRenderSnapshot = renderSnapshot;
	// Publish immutable per-view packet mirrors at the protected Vulkan
	// boundary. RenderSystem owns the value records; Vulkan owns the backing
	// buffers and may reuse them only when the frame slot and view identity match.
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Publish Virtual Geometry Decoded Pages",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		impl->PublishVirtualGeometryDecodedPageResources(presentation);
	}
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Prepare Virtual Geometry Production Decode",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		impl->PrepareVulkanVirtualGeometryProductionDecode(presentation);
		if(impl->currentFrame < impl->virtualGeometryProductionDecodeResourcesByFrame.size()) {
			auto& decodeResources = impl->virtualGeometryProductionDecodeResourcesByFrame[impl->currentFrame];
			if(decodeResources.plan.valid && !decodeResources.completionPending) {
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResources recordingResources{};
				recordingResources.requestBuffer = decodeResources.requestBuffer.buffer;
				recordingResources.encodedBuffer = decodeResources.encodedBuffer.buffer;
				recordingResources.decodedBuffer = decodeResources.decodedBuffer.buffer;
				recordingResources.resultReadbackBuffer = decodeResources.resultReadbackBuffer.buffer;
				recordingResources.descriptorSet = decodeResources.descriptorSet;
				recordingResources.encodedCapacityBytes = static_cast<std::uint32_t>(decodeResources.encodedBuffer.size);
				recordingResources.decodedCapacityBytes = static_cast<std::uint32_t>(decodeResources.decodedBuffer.size);
				recordingResources.resourcesOwnedByFrame = true;
				const auto decodeRecording = TerraEngine::Renderer::Vulkan::RecordVulkanVirtualGeometryProductionDecode(
					cmd, decodeResources.plan, recordingResources, impl->virtualGeometryProductionDecodePipelines);
				if(decodeRecording.recorded) {
					// Completion is consumed at the next reuse of this exact frame slot.
					decodeResources.completionPending = true;
				}
				else {
					impl->virtualGeometryProductionDecodeUnavailableReason = decodeRecording.failureReason;
				}
			}
		}
	}
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Publish Virtual Geometry Packet Resources",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		impl->PublishVirtualGeometryGpuPacketResources(
			cmd,
			*publicationPresentation,
			*impl->activeRenderFramePublication,
			publicationViewIndex,
			renderViewType,
			diagnosticViewId,
			impl->backendFrameSerial,
			statsSlotIndex,
			renderSnapshot.lightDataOffset,
			renderSnapshot.clusterDataOffset,
			renderSnapshot.lightIndexOffset);
	}
	beginCommandRecordingPhase(
		"Vulkan: Primary Record Virtual Geometry Compute");
	const auto& virtualGeometryGpuPackets =
		publicationPresentation->VirtualGeometryPackets();
	const auto& virtualGeometryGpuBatches =
		publicationPresentation->VirtualGeometryBatches();
	const auto& virtualGeometryGeneratedSubmissions =
		publicationPresentation->VirtualGeometryGeneratedSubmissions();
	const bool hasDirectGeometryCommands = std::any_of(
		impl->activeRenderFramePublication->submissionRanges.begin(),
		impl->activeRenderFramePublication->submissionRanges.end(),
		[publicationViewIndex](const TerraEngine::Renderer::RenderSubmissionRange& range)
		{
			return range.viewIndex == publicationViewIndex
				&& range.route == TerraEngine::Renderer::RenderSubmissionRoute::Direct;
		});
	const bool hasVirtualGeometryTimestampWork = !virtualGeometryGpuPackets.empty()
		|| !virtualGeometryGpuBatches.empty()
		|| !virtualGeometryGeneratedSubmissions.empty();
	if(hasVirtualGeometryTimestampWork || hasDirectGeometryCommands)
	{
		// A direct-only frame still owns a real indexed raster timestamp below.
		// Start the same fence-delayed query frame for either representation so
		// route calibration never substitutes CPU submit or wall-clock timing.
		impl->BeginVirtualGeometryTimestampFrame(
			impl->currentFrame,
			view,
			impl->backendFrameSerial,
			diagnosticViewId,
			impl->renderSessionGeneration);

	}
	bool productionVisibilityRecorded = false;
	if(hasVirtualGeometryTimestampWork)
	{
		// Every live compute stage owns its own timestamp interval. The indirect
		// stage records a real zero-work indirect dispatch when no packets exist;
		// non-empty validated output is consumed by the mesh-task draw path below.
		const auto& executorPlan = impl->latestVirtualGeometryExecutorPlan;
		if(executorPlan.resourcesReady && executorPlan.descriptorReady && executorPlan.pipelinesReady) {
			bool recordedEveryExecutorStage = true;
			const std::array<std::pair<TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorStage,
				TerraEngine::Renderer::VirtualGeometryBackendStage>, 3> stages{{
				{ TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorStage::Traversal, TerraEngine::Renderer::VirtualGeometryBackendStage::Traversal },
				{ TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorStage::Compaction, TerraEngine::Renderer::VirtualGeometryBackendStage::Compaction },
				{ TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorStage::Indirect, TerraEngine::Renderer::VirtualGeometryBackendStage::IndirectGeneration } }};
			for(const auto& [executorStage, measurementStage] : stages) {
				impl->MarkVirtualGeometryTimestampBegin(cmd, impl->currentFrame, view, measurementStage,
					TerraEngine::Renderer::VirtualGeometryBackendRoute::Compute, executorPlan.geometryGeneration,
					executorPlan.residencyGeneration, executorPlan.commandGeneration);
				const auto recordingResult = TerraEngine::Renderer::Vulkan::RecordVulkanVirtualGeometryExecutorStage(
					cmd, executorStage, executorPlan, impl->latestVirtualGeometryExecutorResources, impl->virtualGeometryExecutorPipelines);
				if(!recordingResult.recorded) {
					recordedEveryExecutorStage = false;
					impl->latestVirtualGeometryExecutorPlan.unavailableReason = recordingResult.failureReason;
					impl->latestVirtualGeometryExecutorFailure = TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryFailureReceipt(
						impl->latestVirtualGeometryExecutorPlan, measurementStage);
					break;
				}
				impl->MarkVirtualGeometryTimestampEnd(cmd, impl->currentFrame, view, measurementStage);
			}
			// This flag is a command-recording gate for the draw path. The delayed
			// timestamp receipt remains the stronger diagnostic proof that the
			// recorded command was submitted and completed.
			impl->latestVirtualGeometryExecutorPlan.generatedSubmissionExecutionAvailable =
				recordedEveryExecutorStage && !executorPlan.zeroWork;
		}
		// Visibility becomes production authority only after every compute stage
		// records successfully. The graphics path checks this separate flag before
		// omitting any hardware packet, preserving fail-open coverage.
		const auto& visibilityPlan = impl->latestVirtualGeometryVisibilityPlan;
		if(visibilityPlan.ready) {
			bool recordedEveryVisibilityStage = true;
			const std::array<std::pair<TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityStage,
				TerraEngine::Renderer::VirtualGeometryBackendStage>, 4> visibilityStages{{
				{ TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityStage::TriangleSetup, TerraEngine::Renderer::VirtualGeometryBackendStage::TriangleSetup },
				{ TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityStage::ComputeRaster, TerraEngine::Renderer::VirtualGeometryBackendStage::ComputeRaster },
				{ TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityStage::VisibilityResolve, TerraEngine::Renderer::VirtualGeometryBackendStage::VisibilityResolve },
				{ TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityStage::ShadingBins, TerraEngine::Renderer::VirtualGeometryBackendStage::Shading } }};
			for(std::size_t stageIndex = 0u; stageIndex < visibilityStages.size(); ++stageIndex) {
				const auto& [visibilityStage, measurementStage] = visibilityStages[stageIndex];
				impl->MarkVirtualGeometryTimestampBegin(cmd, impl->currentFrame, view, measurementStage,
					TerraEngine::Renderer::VirtualGeometryBackendRoute::Compute, visibilityPlan.geometryGeneration,
					visibilityPlan.geometryGeneration, visibilityPlan.geometryGeneration);
				const auto visibilityRecordingResult = TerraEngine::Renderer::Vulkan::RecordVulkanVirtualGeometryVisibilityStage(
					cmd, visibilityStage, visibilityPlan, impl->latestVirtualGeometryVisibilityResources,
					impl->virtualGeometryVisibilityPipelines);
				if(!visibilityRecordingResult.recorded) {
					recordedEveryVisibilityStage = false;
					impl->latestVirtualGeometryVisibilityPlan.unavailableReason = visibilityRecordingResult.failureReason;
					break;
				}
				impl->MarkVirtualGeometryTimestampEnd(cmd, impl->currentFrame, view, measurementStage);
				if(!visibilityPlan.zeroWork && stageIndex + 1u < visibilityStages.size()) {
					// The next pass consumes storage writes from this pass on the same queue.
					VkMemoryBarrier visibilityBarrier{};
					visibilityBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
					visibilityBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
					visibilityBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
					vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						0u, 1u, &visibilityBarrier, 0u, nullptr, 0u, nullptr);
				}
			}
			productionVisibilityRecorded = recordedEveryVisibilityStage
				&& !visibilityPlan.zeroWork;
			if(productionVisibilityRecorded)
			{
				// Fullscreen material shading reads resolved records and expanded
				// attributes in the following graphics render pass.
				VkMemoryBarrier compositeBarrier{};
				compositeBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
				compositeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
				compositeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				vkCmdPipelineBarrier(
					cmd,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					0u,
					1u,
					&compositeBarrier,
					0u,
					nullptr,
					0u,
					nullptr);
			}
		}
	}
	beginCommandRecordingPhase(
		"Vulkan: Primary Prepare Lighting Resources");
	const std::uint64_t generatedSubmissionFrameId = virtualGeometryGpuPackets.empty()
		? impl->backendFrameSerial
		: virtualGeometryGpuPackets.front().frameId;
	bool generatedVirtualGeometrySubmissionsUsable = !virtualGeometryGeneratedSubmissions.empty();
	for (const GeneratedVirtualGeometrySubmissionRecord& submission : virtualGeometryGeneratedSubmissions)
	{
		const bool rangeValid = submission.packetCount > 0u
			&& submission.packetOffset <= virtualGeometryGpuPackets.size()
			&& static_cast<std::size_t>(submission.packetOffset) + static_cast<std::size_t>(submission.packetCount) <= virtualGeometryGpuPackets.size();
		generatedVirtualGeometrySubmissionsUsable = generatedVirtualGeometrySubmissionsUsable
			&& submission.frameId == generatedSubmissionFrameId
			&& submission.viewType == renderViewType
			&& submission.viewId == diagnosticViewId
			&& submission.validationState == VirtualGeometryGeneratedSubmissionValidationState::Valid
			&& rangeValid;
	}
	if (!generatedVirtualGeometrySubmissionsUsable)
	{
		// Generated submissions are an optimization authority, not the coverage
		// authority. If they are missing, delayed, or stale, command recording
		// continues with the already expanded CPU-selected draw commands below.
		impl->activeVirtualGeometryGeneratedSubmissionBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	}
	impl->pendingRenderSnapshotDiagnostics.valid = true;
	impl->pendingRenderSnapshotDiagnostics.maxSnapshotsPerFrame = VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME;
	impl->pendingRenderSnapshotDiagnostics.activeSnapshotCount += 1u;
	impl->pendingRenderSnapshotDiagnostics.peakSnapshotCount = std::max(
		impl->pendingRenderSnapshotDiagnostics.peakSnapshotCount,
		impl->pendingRenderSnapshotDiagnostics.activeSnapshotCount);
	RenderSnapshotOwnerSummary ownerSummary{};
	ownerSummary.kind = outputRequest.kind;
	ownerSummary.priority = outputRequest.priority;
	ownerSummary.state = RenderSnapshotState::Acquired;
	ownerSummary.targetSize = outputRequest.targetSize;
	ownerSummary.outputId = outputRequest.outputId;
	ownerSummary.slotIndex = snapshotSlotIndex;
	ownerSummary.lightingIsolated = true;
	ownerSummary.cullingIsolated = true;
	ownerSummary.clusteringIsolated = true;
	ownerSummary.shadowAssignmentsIsolated = true;
	ownerSummary.indirectDrawsIsolated = true;
	impl->pendingRenderSnapshotDiagnostics.ownerSummaries.push_back(ownerSummary);

	std::vector<GPULight> analysisLightingPayload;
	analysisLightingPayload.reserve(budgetResult.uploadedCandidates.size());

	for (const FrameLightCandidate& candidate : budgetResult.uploadedCandidates) {
		analysisLightingPayload.push_back(candidate.gpuLight);
	}

	std::vector<GPULight> lightingPayload;
	lightingPayload.reserve(budgetResult.retainedLights.size());
	const auto FindShadowAssignment = [&](const TerraEngine::UUID& lightId) -> const ShadowResourceAssignmentDiagnostic* {

		for (const ShadowResourceAssignmentDiagnostic& assignment : budgetResult.diagnostics.shadowAssignments) {

			if (assignment.lightId == lightId) {
				return &assignment;
			}
		}
		return nullptr;
	};

	for (const FrameLightCandidate& candidate : budgetResult.retainedLights) {
		GPULight light = candidate.gpuLight;
		if (const ShadowResourceAssignmentDiagnostic* assignment = FindShadowAssignment(candidate.lightId)) {
			const float textureWidth = static_cast<float>(std::max(assignment->regionWidth, 1u));
			const float textureHeight = static_cast<float>(std::max(assignment->regionHeight, 1u));
			// Each current 2D assignment owns its own texture allocation. The
			// region vector still travels through the shader payload so a future
			// shared atlas can use offsets without changing the light layout.
			light.shadowRegion = glm::vec4(
				static_cast<float>(assignment->regionX) / textureWidth,
				static_cast<float>(assignment->regionY) / textureHeight,
				textureWidth / textureWidth,
				textureHeight / textureHeight);
		}
		const uint32_t lightType = static_cast<uint32_t>(std::max(0.0f, light.positionAndType.w));
		const bool castsShadows = light.shadowAndPadding.x > 0.5f;
		const float assignedShadowSlot = light.shadowAndPadding.y;
		light.shadowAndPadding.y = -1.0f;
		light.shadowAndPadding.z = static_cast<float>(impl->ltcAmplitudeTextureIndex);
		light.shadowAndPadding.w = static_cast<float>(impl->ltcInverseTextureIndex);
		// Every retained light must still reach the shader. Only the shadow
		// resource binding is conditional; non-shadowed retained lights still
		// contribute direct lighting in the forward pass.
		if (castsShadows) {
			// ApplyFrameLightBudgets stores the logical shadow slot in y. Keep
			// that CPU-side slot before y is repacked as the shader-visible
			// texture index, otherwise every shadowed light samples slot zero.
			const uint32_t shadowSlotIndex = static_cast<uint32_t>(std::clamp(assignedShadowSlot, 0.0f, static_cast<float>(VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS - 1u)));
			if (lightType == 0u) {
				const glm::mat4 lightViewProjection = BuildDirectionalShadowViewProjection(light, presentation.cameraLocalPosition);
				const glm::vec3 snappedShadowCenter = ComputeDirectionalShadowCenter(light, presentation.cameraLocalPosition);
				light.positionAndType.x = snappedShadowCenter.x;
				light.positionAndType.y = snappedShadowCenter.y;
				light.positionAndType.z = snappedShadowCenter.z;
				light.shadowAndPadding.y = static_cast<float>(impl->directionalShadowTextureIndices[shadowSlotIndex]);
				StoreShadowViewProjection(light, lightViewProjection);
			}
			else if (lightType == 1u) {
				light.shadowAndPadding.y = static_cast<float>(shadowSlotIndex);
			}
			else if (lightType == 2u) {
				light.shadowAndPadding.y = static_cast<float>(impl->spotShadowTextureIndices[shadowSlotIndex]);
				StoreShadowViewProjection(light, BuildSpotShadowViewProjection(light));
			}
			else if (lightType == 3u) {
				light.shadowAndPadding.y = static_cast<float>(impl->areaShadowTextureIndices[shadowSlotIndex]);
				StoreShadowViewProjection(light, BuildAreaShadowViewProjection(light));
			}
		}
		lightingPayload.push_back(light);
	}
	// Canonical serialization reads only immutable copied payloads. Jobs operate
	// on distinct owned blocks and settle before the protected map/write boundary.
	using namespace TerraEngine::Renderer::VulkanLightingUpload;
	auto canonicalFinalLights = std::make_unique<CanonicalBlock>();
	auto canonicalAnalysisLights = std::make_unique<CanonicalBlock>();
	std::array<std::uint64_t, 2> canonicalPreparationNanoseconds{};
	std::vector<std::future<void>> lightingPreparationFutures;
	lightingPreparationFutures.reserve(2);
	TerraEngine::Tasks::ScopedTaskSettlement<void> lightingPreparationSettlement(lightingPreparationFutures);
	const auto prepareLighting = [&](std::uint32_t purpose) {
		const auto started = std::chrono::steady_clock::now();
		CanonicalizeLighting(purpose == 0 ? lightingPayload : analysisLightingPayload,
			purpose == 0 ? *canonicalFinalLights : *canonicalAnalysisLights);
		canonicalPreparationNanoseconds[purpose] = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
	};
	for (std::uint32_t purpose = 0; purpose < 2; ++purpose) {
		if (presentation.efficiencyOptions.options.validationPreparation == TerraEngine::Renderer::RendererEfficiencyPreparation::Concurrent)
			lightingPreparationSettlement.CreateAndEnqueue(ThreadPool::instance(), [&, purpose]() { prepareLighting(purpose); });
		else lightingPreparationSettlement.RunInline([&, purpose]() { prepareLighting(purpose); });
	}
	lightingPreparationSettlement.GetAll();
	std::array<TerraEngine::Renderer::RendererEfficiencyLightingUploadEvidence, 2> lightingUploadEvidence;
	const bool forceFullLighting = presentation.efficiencyOptions.options.lightingUploads
		== TerraEngine::Renderer::RendererEfficiencyLightingUploads::Full;
	const auto EnsureLightingComputePipelines = [&]() {

		if (impl->lightingComputePipelineLayout == VK_NULL_HANDLE) {
			VkPushConstantRange pushRange{};
			pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			pushRange.offset = 0;
			pushRange.size = 64; // Plenty of room for various compute push constants

			VkPipelineLayoutCreateInfo layoutInfo{};
			layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			layoutInfo.setLayoutCount = 1;
			layoutInfo.pSetLayouts = &impl->globalDescriptorSetLayout;
			layoutInfo.pushConstantRangeCount = 1;
			layoutInfo.pPushConstantRanges = &pushRange;

			if (vkCreatePipelineLayout(impl->device, &layoutInfo, nullptr, &impl->lightingComputePipelineLayout) != VK_SUCCESS) {
				impl->lightingComputePipelineLayout = VK_NULL_HANDLE;
				return;
			}
		}

		auto LoadShaderModule = [&](const char* path) -> VkShaderModule {
			const std::filesystem::path shaderPath = ResourceManager::Instance().ResolveEngineAssetPath(path);
			const std::vector<std::uint8_t> shaderBytes = ResourceManager::Instance().ReadFileBytes(shaderPath);
			// SPIR-V shader modules must be non-empty and aligned to 32-bit words.
			if (shaderBytes.empty() || (shaderBytes.size() % sizeof(std::uint32_t)) != 0) {
				return VK_NULL_HANDLE;
			}
			std::vector<std::uint32_t> code(shaderBytes.size() / sizeof(std::uint32_t));
			std::memcpy(code.data(), shaderBytes.data(), shaderBytes.size());

			VkShaderModuleCreateInfo moduleInfo{};
			moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			moduleInfo.codeSize = shaderBytes.size();
			moduleInfo.pCode = code.data();
			VkShaderModule module = VK_NULL_HANDLE;

			if (vkCreateShaderModule(impl->device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
				return VK_NULL_HANDLE;
			}
			return module;
		};

		auto CreateComputePipelineWithLayout = [&](VkPipeline& pipeline, VkPipelineLayout layout, const char* shaderPath) {

			if (pipeline != VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
				return;
			}
			VkShaderModule module = LoadShaderModule(shaderPath);

			if (module == VK_NULL_HANDLE) {
				return;
			}

			VkPipelineShaderStageCreateInfo stageInfo{};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageInfo.module = module;
			stageInfo.pName = "main";

			VkComputePipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			pipelineInfo.stage = stageInfo;
			pipelineInfo.layout = layout;

			if (vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
				pipeline = VK_NULL_HANDLE;
			}
			vkDestroyShaderModule(impl->device, module, nullptr);
		};

		auto CreateComputePipeline = [&](VkPipeline& pipeline, const char* shaderPath) {
			CreateComputePipelineWithLayout(pipeline, impl->lightingComputePipelineLayout, shaderPath);
		};

		if (impl->hiZDescriptorSetLayout == VK_NULL_HANDLE) {
			std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
			bindings[0].binding = 0u;
			bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			bindings[0].descriptorCount = 1u;
			bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[1].binding = 1u;
			bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[1].descriptorCount = 1u;
			bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkDescriptorSetLayoutCreateInfo hiZLayoutInfo{};
			hiZLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			hiZLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
			hiZLayoutInfo.pBindings = bindings.data();
			// Hi-Z has a tiny private descriptor set so mip generation never
			// rewrites global bindings that later passes still depend on.
			if (vkCreateDescriptorSetLayout(impl->device, &hiZLayoutInfo, nullptr, &impl->hiZDescriptorSetLayout) != VK_SUCCESS) {
				impl->hiZDescriptorSetLayout = VK_NULL_HANDLE;
			}
		}

		if (impl->hiZPipelineLayout == VK_NULL_HANDLE && impl->hiZDescriptorSetLayout != VK_NULL_HANDLE) {
			VkPushConstantRange hiZPushRange{};
			hiZPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			hiZPushRange.offset = 0u;
			hiZPushRange.size = 32u;

			VkPipelineLayoutCreateInfo hiZPipelineLayoutInfo{};
			hiZPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			hiZPipelineLayoutInfo.setLayoutCount = 1u;
			hiZPipelineLayoutInfo.pSetLayouts = &impl->hiZDescriptorSetLayout;
			hiZPipelineLayoutInfo.pushConstantRangeCount = 1u;
			hiZPipelineLayoutInfo.pPushConstantRanges = &hiZPushRange;
			// The push constants are source width/height and output width/height.
			if (vkCreatePipelineLayout(impl->device, &hiZPipelineLayoutInfo, nullptr, &impl->hiZPipelineLayout) != VK_SUCCESS) {
				impl->hiZPipelineLayout = VK_NULL_HANDLE;
			}
		}

		if (impl->aoDescriptorSetLayout == VK_NULL_HANDLE) {
			std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
			bindings[0].binding = 12u;
			bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			bindings[0].descriptorCount = 1u;
			bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[1].binding = 21u;
			bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			bindings[1].descriptorCount = 1u;
			bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[2].binding = 22u;
			bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			bindings[2].descriptorCount = 1u;
			bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[3].binding = 23u;
			bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[3].descriptorCount = 1u;
			bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[4].binding = 24u;
			bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[4].descriptorCount = 1u;
			bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkDescriptorSetLayoutCreateInfo aoLayoutInfo{};
			aoLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			aoLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
			aoLayoutInfo.pBindings = bindings.data();
			// AO compute uses per-snapshot descriptor sets because editor scene,
			// game, and additional camera passes can be recorded before the GPU
			// executes them. Rewriting the global descriptor set made optimized
			// builds sample whichever AO image was updated last.
			if (vkCreateDescriptorSetLayout(impl->device, &aoLayoutInfo, nullptr, &impl->aoDescriptorSetLayout) != VK_SUCCESS) {
				impl->aoDescriptorSetLayout = VK_NULL_HANDLE;
			}
		}

		if (impl->aoPipelineLayout == VK_NULL_HANDLE && impl->aoDescriptorSetLayout != VK_NULL_HANDLE) {
			VkPushConstantRange aoPushRange{};
			aoPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			aoPushRange.offset = 0u;
			aoPushRange.size = 16u;

			VkPipelineLayoutCreateInfo aoPipelineLayoutInfo{};
			aoPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			aoPipelineLayoutInfo.setLayoutCount = 1u;
			aoPipelineLayoutInfo.pSetLayouts = &impl->aoDescriptorSetLayout;
			aoPipelineLayoutInfo.pushConstantRangeCount = 1u;
			aoPipelineLayoutInfo.pPushConstantRanges = &aoPushRange;
			if (vkCreatePipelineLayout(impl->device, &aoPipelineLayoutInfo, nullptr, &impl->aoPipelineLayout) != VK_SUCCESS) {
				impl->aoPipelineLayout = VK_NULL_HANDLE;
			}
		}

		CreateComputePipeline(impl->clusterGridComputePipeline, "Shaders/Lighting/ClusterGrid.comp.spv");
		if (!impl->clusterDepthDeviceLimitsChecked) {
			VkPhysicalDeviceProperties depthDeviceProperties{};
			vkGetPhysicalDeviceProperties(impl->physicalDevice, &depthDeviceProperties);
			const auto& limits = depthDeviceProperties.limits;
			impl->clusterDepthCooperativeDeviceSupported = limits.maxComputeWorkGroupInvocations >= 64
				&& limits.maxComputeWorkGroupSize[0] >= 64 && limits.maxComputeSharedMemorySize >= 1024
				&& limits.maxComputeWorkGroupCount[0] >= kLightingClusterDimX
				&& limits.maxComputeWorkGroupCount[1] >= kLightingClusterDimY;
			impl->clusterDepthReferenceDeviceSupported = limits.maxComputeWorkGroupInvocations >= 64
				&& limits.maxComputeWorkGroupSize[0] >= 8 && limits.maxComputeWorkGroupSize[1] >= 8
				&& limits.maxComputeWorkGroupCount[0] >= (kLightingClusterDimX + 7) / 8
				&& limits.maxComputeWorkGroupCount[1] >= (kLightingClusterDimY + 7) / 8;
			impl->clusterDepthDeviceLimitsChecked = true;
		}
		if (impl->clusterDepthCooperativeDeviceSupported)
			CreateComputePipeline(impl->clusterDepthMaskComputePipeline, "Shaders/Lighting/ClusterDepthMask.comp.spv");
		if (impl->clusterDepthReferenceDeviceSupported)
			CreateComputePipeline(impl->clusterDepthMaskReferenceComputePipeline, "Shaders/Lighting/ClusterDepthMaskReference.comp.spv");
		CreateComputePipeline(impl->clusterLightCountComputePipeline, "Shaders/Lighting/ClusterLightCount.comp.spv");
		CreateComputePipeline(impl->clusterLightPrefixComputePipeline, "Shaders/Lighting/ClusterLightPrefix.comp.spv");
		CreateComputePipeline(impl->clusterLightFillComputePipeline, "Shaders/Lighting/ClusterLightFill.comp.spv");
		CreateComputePipeline(impl->lightAnalysisComputePipeline, "Shaders/Lighting/LightCull.comp.spv");
		CreateComputePipelineWithLayout(impl->hiZComputePipeline, impl->hiZPipelineLayout, "Shaders/PostProcess/HiZGenerate.comp.spv");
		CreateComputePipeline(impl->vrsComputePipeline, "Shaders/PostProcess/VRSGenerate.comp.spv");
		CreateComputePipelineWithLayout(impl->gtaoComputePipeline, impl->aoPipelineLayout, "Shaders/PostProcess/GTAO.comp.spv");
		CreateComputePipelineWithLayout(impl->gtaoBlurComputePipeline, impl->aoPipelineLayout, "Shaders/PostProcess/GTAOBlur.comp.spv");
	};

	EnsureLightingComputePipelines();
	if (!impl->lightingDepthEvidenceInitializationAttempted) {
		impl->lightingDepthEvidenceInitializationAttempted = true;
		// The collector consumes immutable shader words during initialization. No
		// file lookup, startup query reset submission or GPU wait enters sampling.
		const auto loadDepthOracleWords = [](const char* path) {
			const auto bytes = ResourceManager::Instance().ReadFileBytes(ResourceManager::Instance().ResolveEngineAssetPath(path));
			std::vector<std::uint32_t> words;
			if (!bytes.empty() && bytes.size() % sizeof(std::uint32_t) == 0) {
				words.resize(bytes.size() / sizeof(std::uint32_t));
				std::memcpy(words.data(), bytes.data(), bytes.size());
			}
			return words;
		};
		const auto cooperativeOracleWords = loadDepthOracleWords("Shaders/Lighting/ClusterDepthMaskOracle.comp.spv");
		const auto referenceOracleWords = loadDepthOracleWords("Shaders/Lighting/ClusterDepthMaskReferenceOracle.comp.spv");
		TerraEngine::Renderer::VulkanLightingDepthEvidence::Config config;
		config.device = impl->device; config.physical = impl->physicalDevice; config.allocator = impl->allocator;
		config.rendererSessionGeneration = impl->renderSessionGeneration;
		config.backendResourceGeneration = impl->lightingUploadAllocationGeneration;
		config.timestampValidBits = impl->lightingDepthTimestampValidBits;
		config.periodNanoseconds = impl->gpuTimestampPeriodNanoseconds;
		config.cooperativeOracleSpirv = cooperativeOracleWords; config.referenceOracleSpirv = referenceOracleWords;
		impl->lightingDepthEvidenceInitialized = impl->lightingDepthEvidence.Initialize(config);
	}

	const glm::mat4 currentViewProjection = presentation.projectionMatrix * presentation.viewMatrix;
	VulkanContext::Impl::ViewDepthHistoryState& depthHistory = impl->depthHistoryByView[viewIndex];
	// The first command buffer can belong to an independently sized capture.
	// Runtime AO history owns swapchain-sized depth, so captures must neither
	// sample that history nor replace it with their private depth attachment.
	const bool runtimeAoHistoryPass = useMainCommandBuffer && !renderingDedicatedCaptureTarget;
	const bool aoDepthHistorySlotReady = impl->currentFrame < depthHistory.ambientOcclusionDepthByFrame.size()
		&& depthHistory.ambientOcclusionDepthByFrame[impl->currentFrame].view != VK_NULL_HANDLE;
	const bool aoDepthHistoryMatchesView = !impl->editorMode
		&& runtimeAoHistoryPass
		&& aoDepthHistorySlotReady
		&& depthHistory.ambientOcclusionDepthValidByFrame[impl->currentFrame]
		&& MatricesApproximatelyEqual(
			currentViewProjection,
			depthHistory.ambientOcclusionDepthViewProjectionByFrame[impl->currentFrame],
			kDepthHistoryMatrixEpsilon);
	VkImageView aoDepthHistoryView = aoDepthHistoryMatchesView
		? depthHistory.ambientOcclusionDepthByFrame[impl->currentFrame].view
		: VK_NULL_HANDLE;
	const std::uint32_t hiZTargetWidth = std::max<std::uint32_t>(renderPassInfo.renderArea.extent.width, 1u);
	const std::uint32_t hiZTargetHeight = std::max<std::uint32_t>(renderPassInfo.renderArea.extent.height, 1u);
	const uint32_t hiZViewIndex = impl->HiZViewIndexForView(view);
	if (hiZViewIndex < VulkanContext::Impl::kHiZViewCount)
	{
		// Record the true per-view render target size. BeginFrame consumes this
		// at a fence-safe boundary before any new command buffer can bind the
		// global Hi-Z descriptors for the current frame slot.
		impl->desiredHiZExtentByView[hiZViewIndex] = glm::uvec2(hiZTargetWidth, hiZTargetHeight);
	}
	const std::uint64_t hiZProjectionSignature = BuildVulkanHiZProjectionSignature(currentViewProjection);
	// Visibility preparation already hashes the exact view, world/resource
	// revisions, selected route/LOD evidence, and stable command membership. Reuse
	// that sealed proof for Hi-Z diagnostics instead of resolving and hashing every
	// visible command again on the protected Vulkan recording path. Legacy callers
	// without publication evidence retain the complete fail-open scan.
	const std::uint64_t hiZDepthSceneSignature =
		publicationPresentation->virtualGeometryPacketEvidenceSignature != 0U
			? publicationPresentation->virtualGeometryPacketEvidenceSignature
			: BuildVulkanHiZDepthSceneSignature(commandResolver);
	VulkanHiZ& currentFrameHiZ = impl->CurrentHiZ(hiZViewIndex);
	VulkanHiZ& writableFrameHiZ = impl->WritableHiZForFrame(hiZViewIndex, impl->currentFrame);
	const VulkanHiZHistoryMetadata& readableHiZHistory = impl->readableHiZHistoryByViewFrame[hiZViewIndex][impl->currentFrame];
	const VulkanHiZViewClass hiZViewClass = outputRequest.kind == RenderOutputKind::MainRuntimeView
		? VulkanHiZViewClass::ExportRuntime
		: (renderViewType == RenderViewType::Scene ? VulkanHiZViewClass::Scene : VulkanHiZViewClass::Game);
	const std::uint32_t hiZTargetId = outputRequest.outputId;
	VulkanHiZHistoryMetadata currentHiZHistory{};
	currentHiZHistory.viewId = viewIndex;
	currentHiZHistory.viewClass = hiZViewClass;
	currentHiZHistory.targetId = hiZTargetId;
	currentHiZHistory.width = hiZTargetWidth;
	currentHiZHistory.height = hiZTargetHeight;
	currentHiZHistory.projectionSignature = hiZProjectionSignature;
	currentHiZHistory.depthSceneSignature = hiZDepthSceneSignature;
	currentHiZHistory.frameId = impl->pendingBackendFrameBreakdown.frameId;
	const bool hiZResourcesReady =
		currentFrameHiZ.width == hiZTargetWidth
		&& currentFrameHiZ.height == hiZTargetHeight
		&& currentFrameHiZ.image != VK_NULL_HANDLE
		&& currentFrameHiZ.fullView != VK_NULL_HANDLE
		&& currentFrameHiZ.sampler != VK_NULL_HANDLE
		&& currentFrameHiZ.mipLevels > 0u
		&& impl->hiZComputePipeline != VK_NULL_HANDLE
		&& impl->hiZPipelineLayout != VK_NULL_HANDLE
		&& impl->hiZDescriptorSetLayout != VK_NULL_HANDLE;
	VulkanHiZValidationInput hiZValidationInput{};
	hiZValidationInput.viewId = viewIndex;
	hiZValidationInput.viewClass = hiZViewClass;
	hiZValidationInput.targetId = hiZTargetId;
	hiZValidationInput.width = hiZTargetWidth;
	hiZValidationInput.height = hiZTargetHeight;
	hiZValidationInput.projectionSignature = hiZProjectionSignature;
	hiZValidationInput.depthSceneSignature = hiZDepthSceneSignature;
	hiZValidationInput.pyramidReady = hiZResourcesReady;
	hiZValidationInput.viewSupported = true;
	hiZValidationInput.cameraCut = false;
	const VulkanHiZValidationResult hiZValidation = ValidateVulkanHiZHistory(readableHiZHistory, hiZValidationInput);
	const bool hiZOcclusionActive = hiZValidation.occlusionAllowed;
	currentHiZHistory.warmupFrames = hiZValidation.warmupFrames;
	currentFrameHiZ.latestInvalidReason = hiZValidation.invalidReason;
	currentFrameHiZ.latestValidation = hiZValidation;
	currentFrameHiZ.latestOcclusionAllowed = hiZOcclusionActive;
	writableFrameHiZ.latestInvalidReason = hiZValidation.invalidReason;
	writableFrameHiZ.latestValidation = hiZValidation;
	writableFrameHiZ.latestOcclusionAllowed = hiZOcclusionActive;
	currentFrameHiZ.history.warmupFrames = hiZValidation.warmupFrames;
	writableFrameHiZ.history.warmupFrames = hiZValidation.warmupFrames;
	if (!hiZValidation.historyValid)
	{
		currentFrameHiZ.history.invalidReason = hiZValidation.invalidReason;
		writableFrameHiZ.history.invalidReason = hiZValidation.invalidReason;
	}

	FrameCullingDataGPU analysisCullingData{};
	analysisCullingData.frustumPlanes = BuildFrustumPlanes(currentViewProjection);
	analysisCullingData.cameraPositionAndEpsilon = glm::vec4(presentation.cameraLocalPosition, std::max(presentation.cullingEpsilon, 0.0001f));
	analysisCullingData.clusterGridAndLightCount = glm::uvec4(
		kLightingClusterDimX,
		kLightingClusterDimY,
		kLightingClusterDimZ,
		static_cast<uint32_t>(std::min<size_t>(analysisLightingPayload.size(), VulkanContext::Impl::MAX_GLOBAL_LIGHTS)));
	analysisCullingData.clusterDepthAndViewport = glm::vec4(
		0.1f,
		100.0f,
		static_cast<float>(hiZTargetWidth),
		static_cast<float>(hiZTargetHeight));
	analysisCullingData.viewMatrix = presentation.viewMatrix;
	analysisCullingData.invProjection = glm::inverse(presentation.projectionMatrix);
	analysisCullingData.renderSettings0 = glm::uvec4(
		presentation.settings.shadowResolution,
		presentation.settings.shadowFilteringSize,
		budgetResult.diagnostics.retainedLightBudget,
		presentation.settings.enableLTC ? 1u : 0u);
	analysisCullingData.renderSettings1 = glm::uvec4(
		presentation.settings.enableIBL ? 1u : 0u,
		presentation.settings.anisotropicFiltering,
		budgetResult.diagnostics.shadowLightBudget,
		budgetResult.diagnostics.uploadedLightBudget);
	// Upload artist-facing indirect and contact-darkening controls separately
	// from integer feature flags so HLSL receives the same type and alignment
	// that the CPU writes into the frame payload.
	// Runtime GTAO samples an owned depth-history image from the same frame slot
	// after that slot's fence has completed. This keeps AO independent from the
	// live depth attachment that the upcoming render pass will clear and write.
	constexpr bool kEnableRuntimeScreenSpaceAo = true;
	const bool screenSpaceAoActive = kEnableRuntimeScreenSpaceAo && aoDepthHistoryMatchesView;
	const float aoQualityForFrame = screenSpaceAoActive
		? static_cast<float>(presentation.settings.ambientOcclusionQuality)
		: 0.0f;
	analysisCullingData.renderSettings2 = glm::vec4(
		presentation.settings.indirectStrength,
		presentation.settings.ambientOcclusionStrength,
		aoQualityForFrame,
		hiZOcclusionActive ? 1.0f : 0.0f);
	analysisCullingData.globalTaskCullingEnabled = (presentation.globalTaskShaderCullingEnabled && impl->globalTaskShaderCullingEnabled) ? 1u : 0u;
	analysisCullingData.taskShaderSupported = impl->taskShaderSupported ? 1u : 0u;

	FrameCullingDataGPU cullingData = analysisCullingData;
	cullingData.clusterGridAndLightCount.w = static_cast<uint32_t>(std::min<size_t>(lightingPayload.size(), VulkanContext::Impl::MAX_GLOBAL_LIGHTS));

	const uint32_t clusterCount = kLightingClusterDimX * kLightingClusterDimY * kLightingClusterDimZ;
	constexpr uint32_t kClusteredLightingMinimumLightCount = 16u;
	// A requested reference must execute its full-scan shader. Cooperative mode
	// may conservatively use the reference when its pipeline or limits are absent.
	const bool cooperativeDepthRequested = presentation.efficiencyOptions.options.lightingDepth
		== TerraEngine::Renderer::RendererEfficiencyLightingDepth::Cooperative;
	const bool cooperativeDepthSelected = cooperativeDepthRequested
		&& impl->clusterDepthCooperativeDeviceSupported && impl->clusterDepthMaskComputePipeline != VK_NULL_HANDLE;
	const VkPipeline selectedDepthPipeline = cooperativeDepthSelected ? impl->clusterDepthMaskComputePipeline
		: (impl->clusterDepthReferenceDeviceSupported ? impl->clusterDepthMaskReferenceComputePipeline : VK_NULL_HANDLE);
	const bool clusteredLightingPipelineReady = impl->lightingComputePipelineLayout != VK_NULL_HANDLE
		&& impl->clusterGridComputePipeline != VK_NULL_HANDLE
		&& selectedDepthPipeline != VK_NULL_HANDLE
		&& impl->clusterLightCountComputePipeline != VK_NULL_HANDLE
		&& impl->clusterLightPrefixComputePipeline != VK_NULL_HANDLE
		&& impl->clusterLightFillComputePipeline != VK_NULL_HANDLE;
	// Clustered lighting is an acceleration structure. For tiny scenes the full
	// light loop is cheaper and avoids touching the GPU-written cluster buffers,
	// which keeps optimized/export builds deterministic while the scene has only
	// a few authored lights.
	const bool clusteredLightingWorthBuilding = cullingData.clusterGridAndLightCount.w > kClusteredLightingMinimumLightCount;
	const bool clusteredLightingAvailable = clusteredLightingPipelineReady && clusteredLightingWorthBuilding;
	static bool loggedClusteredLightingUnavailable = false;

	if (!clusteredLightingPipelineReady && cullingData.clusterGridAndLightCount.w > kClusteredLightingMinimumLightCount) {
		// Warn once per outage so validation runs clearly explain why fragment
		// shading fell back to the full-light path instead of using cluster bins.

		if (!loggedClusteredLightingUnavailable) {
			TERRAENGINE_LOG(LogLevel::Warning, "Variable light bins are unavailable because one or more clustered-light compute pipelines failed to initialize. Falling back to full-light shading for this frame.");
			loggedClusteredLightingUnavailable = true;
		}
	}
	else if (clusteredLightingPipelineReady) {
		loggedClusteredLightingUnavailable = false;
	}
	const bool gpuLightAnalysisAvailable = impl->lightingComputePipelineLayout != VK_NULL_HANDLE
		&& impl->lightAnalysisComputePipeline != VK_NULL_HANDLE;
	cullingData.clusteredLightingValid = clusteredLightingAvailable ? 1u : 0u;
	cullingData.clusteredLightingOverflowed = 0u;
	analysisCullingData.clusteredLightingValid = clusteredLightingAvailable ? 1u : 0u;
	analysisCullingData.clusteredLightingOverflowed = 0u;

	// Publish and transition the pre-scene depth input before any compute pass
	// binds the global descriptor set. Updating the depth descriptor after a
	// global-set bind can invalidate optimized/export command buffers even when
	// the draw command itself has not reached submission yet.
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.precompute.begin" : "game.precompute.begin");
	const bool needsPreSceneDepthCompute = clusteredLightingAvailable;
	const bool depthPreparedForPreSceneCompute = needsPreSceneDepthCompute
		&& impl->PrepareDepthForCompute(cmd, activeDepthImage, activeDepthView, true);
	if (needsPreSceneDepthCompute && !depthPreparedForPreSceneCompute) {
		// Fail open when the shared depth image cannot be borrowed. The draw
		// pass must not sample AO or cluster metadata that depends on a depth
		// transition which did not happen.
		cullingData.renderSettings2.z = 0.0f;
		cullingData.clusteredLightingValid = 0u;
		impl->UpdateFrameCullingData(statsSlotIndex, cullingData);
	}


	// Even zero lights upload/clear the analysis tail. Failed writes stop this
	// command before shaders can consume partly updated or stale light records.
	if (!impl->UploadLightAnalysisInputData(*canonicalAnalysisLights, renderViewType, diagnosticViewId,
		forceFullLighting, lightingUploadEvidence[1])
		|| !impl->UploadLightingData(*canonicalFinalLights, renderViewType, diagnosticViewId,
			forceFullLighting, lightingUploadEvidence[0])) {
		TERRAENGINE_LOG(LogLevel::Error, "Lighting upload failed; this view is withheld and will retry on the next safe frame.");
		return;
	}
	for (std::uint32_t purpose = 0; purpose < 2; ++purpose)
		lightingUploadEvidence[purpose].mirrorCpuDurationNanoseconds += canonicalPreparationNanoseconds[purpose];
	const bool lightingSampleFrame = !lightingControl || !lightingControl->frameSlot
		|| *lightingControl->frameSlot == currentFrame;
	// Shadow receipts also export the real lighting upload owners. Capture their
	// selected IDs for every validation view, including the original P1 fixtures.
	if (shadowValidationRequested) {
		lightingUploadEvidence[0].payloadLightIds.reserve(budgetResult.retainedLights.size());
		lightingUploadEvidence[1].payloadLightIds.reserve(budgetResult.uploadedCandidates.size());
		for (const auto& selected : budgetResult.retainedLights) lightingUploadEvidence[0].payloadLightIds.push_back(selected.lightId);
		for (const auto& selected : budgetResult.uploadedCandidates) lightingUploadEvidence[1].payloadLightIds.push_back(selected.lightId);
	}
	if (shadowValidationRequested && presentation.efficiencyFixtureIntent.lightingControls && lightingSampleFrame) {
		// These rows are independent of shadow lights, so zero-light illumination
		// still has both physical upload purposes and real same-command color.
		for (std::uint32_t purpose = 0; purpose < 2; ++purpose) {
			TerraEngine::Renderer::RendererEfficiencyEvidence evidence;
			evidence.kind = TerraEngine::Renderer::RendererEfficiencyEvidenceKind::Lighting;
			evidence.analysisPurpose = purpose == 1; evidence.lightingUploads = lightingUploadEvidence;
			evidence.viewType = renderViewType; evidence.viewId = diagnosticViewId;
			evidence.frameSerial = publication->frameId; evidence.commandSerial = shadowCommandSerial;
			evidence.effectiveOptions = presentation.efficiencyOptions;
			evidence.fixtureCorrelation = presentation.efficiencyFixtureIntent.correlation;
			evidence.fixtureControlGeneration = presentation.efficiencyFixtureIntent.controlGeneration;
			evidence.preparationWorkerTasks = presentation.efficiencyOptions.options.validationPreparation
				== TerraEngine::Renderer::RendererEfficiencyPreparation::Concurrent ? 2 : 0;
			evidence.preparationInlineTasks = 2 - evidence.preparationWorkerTasks;
			evidence.preparationSettled = true;
			const auto measured = [](std::uint64_t value) { return TerraEngine::Renderer::RendererEfficiencyCounter{
				TerraEngine::Renderer::RendererEfficiencyEvidenceStatus::Valid, value, {}}; };
			const auto& upload = lightingUploadEvidence[purpose];
			evidence.lightingLogicalWriteBytes = measured(upload.logicalWriteBytes);
			evidence.lightingWriteRecords = measured(upload.logicalWriteRecords);
			evidence.lightingWriteRanges = measured(upload.logicalWriteRanges.size());
			evidence.lightingPhysicalFlushBytes = measured(upload.physicalFlushBytes);
			evidence.lightingFlushRanges = measured(upload.physicalFlushRanges.size());
			const auto& buffer = purpose == 0 ? impl->globalLightSSBOs[currentFrame] : impl->lightAnalysisInputSSBOs[currentFrame];
			// Capture the immutable candidate population before either upload budget
			// truncates it, so validation can distinguish 1,025 sources from 1,024 records.
			(void)impl->shadowValidationReadback.RecordLighting(cmd, buffer.buffer,
				static_cast<std::uint64_t>(snapshotSlotIndex) * kBlockBytes, kBlockBytes, evidence,
				static_cast<std::uint32_t>(presentation.lightCandidates.size()));
		}
	}
	if (gpuLightAnalysisAvailable && !analysisLightingPayload.empty()) {
		struct AnalysisPushConstants
		{
			std::uint32_t lightCount = 0u;
			std::uint32_t outputOffset = 0u;
			std::uint32_t inputOffset = 0u;
			std::uint32_t cullingSlotIndex = 0u;
			std::uint32_t padding[12]{};
		};

		impl->RecordPendingLightAnalysisCandidates(statsSlotIndex, budgetResult.uploadedCandidates);
		impl->UpdateFrameCullingData(statsSlotIndex, analysisCullingData);

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightingComputePipelineLayout, 0, 1, &impl->globalDescriptorSets[currentFrame], 0, nullptr);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightAnalysisComputePipeline);

		AnalysisPushConstants push{};
		push.lightCount = static_cast<std::uint32_t>(std::min<size_t>(analysisLightingPayload.size(), VulkanContext::Impl::MAX_GLOBAL_LIGHTS));
		push.outputOffset = statsSlotIndex * VulkanContext::Impl::MAX_GLOBAL_LIGHTS;
		push.inputOffset = renderSnapshot.lightAnalysisInputOffset;
		push.cullingSlotIndex = statsSlotIndex;
		vkCmdPushConstants(cmd, impl->lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(cmd, std::max<uint32_t>((push.lightCount + 63u) / 64u, 1u), 1u, 1u);

		VkMemoryBarrier analysisBarrier{};
		analysisBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		analysisBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		analysisBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &analysisBarrier, 0, nullptr, 0, nullptr);
	}
	else {
		impl->RecordPendingLightAnalysisCandidates(statsSlotIndex, {});
	}

	cullingData.clusteredLightingValid = clusteredLightingAvailable ? 1u : 0u;
	cullingData.clusteredLightingOverflowed = 0u;

	impl->UpdateFrameCullingData(statsSlotIndex, cullingData);
	impl->ResetMeshletCounters(statsSlotIndex);

	FrameBudgetDiagnosticsSnapshot pendingBudgetDiagnostics = budgetResult.diagnostics;
	pendingBudgetDiagnostics.viewType = renderViewType;
	impl->RecordPendingBudgetDiagnostics(statsSlotIndex, pendingBudgetDiagnostics);


	if (impl->clusterValidationCaptureEnabled) {
		ClusterValidationSnapshot pendingClusterValidation{};
		pendingClusterValidation.valid = true;
		pendingClusterValidation.viewType = renderViewType;
		pendingClusterValidation.buildMetadata.clusterCount = clusterCount;
		pendingClusterValidation.buildMetadata.activeLightCount = cullingData.clusterGridAndLightCount.w;
		pendingClusterValidation.buildMetadata.lightIndexCapacity = static_cast<uint32_t>(std::min<std::uint64_t>(
			static_cast<std::uint64_t>(clusterCount) * static_cast<std::uint64_t>(cullingData.clusterGridAndLightCount.w),
			static_cast<std::uint64_t>(VulkanContext::Impl::MAX_LIGHT_INDEX_ENTRIES)));
		pendingClusterValidation.buildMetadata.clusterDataOffset = renderSnapshot.clusterDataOffset;
		pendingClusterValidation.buildMetadata.lightIndexOffset = renderSnapshot.lightIndexOffset;
		pendingClusterValidation.buildMetadata.clusteredLightingAvailable = clusteredLightingAvailable;
		impl->RecordPendingClusterValidationSnapshot(currentFrame, pendingClusterValidation);
	}

	// Prepare Indirect Buffer
	std::size_t shadowCommandSourceCount = 0u;
	for (const ShadowDrawList& shadowDrawList : presentation.shadowDrawLists) {
		shadowCommandSourceCount += shadowDrawList.stableOrdinals.size();
	}
	// RenderFrame is serialized by the backend state mutex. Thread-local scratch
	// retains the large crowd capacities without sharing mutable command bytes
	// between coordinator threads or VulkanContext instances.
	thread_local std::vector<VkDrawMeshTasksIndirectCommandEXT> indirectCommands;
	indirectCommands.clear();
	// Reserve both the main-view commands and the light-specific shadow lists so
	// per-light culling does not thrash the indirect-command buffer on CPU.
	indirectCommands.reserve(commandResolver.Count() + shadowCommandSourceCount);

	if (screenSpaceAoActive) {
		const bool generatedAmbientOcclusion = impl->GenerateAmbientOcclusion(cmd, cullingData, snapshotSlotIndex, aoDepthHistoryView);
		if (!generatedAmbientOcclusion) {
			cullingData.renderSettings2.z = 0.0f;
			impl->UpdateFrameCullingData(statsSlotIndex, cullingData);
			impl->ambientOcclusionHistoryValidByFrame[impl->currentFrame] = false;
		}
	} else {
		impl->ambientOcclusionHistoryValidByFrame[impl->currentFrame] = false;
	}

	// Build the variable-length light list before graphics so clustered shading
	// reads a fresh contiguous range for every cluster.
	if (clusteredLightingAvailable && depthPreparedForPreSceneCompute) {
		struct ClusterBuildPushConstants
		{
			std::uint32_t cullingSlotIndex = 0u;
			std::uint32_t lightDataOffset = 0u;
			std::uint32_t clusterDataOffset = 0u;
			std::uint32_t lightIndexOffset = 0u;
			std::uint32_t clusterDepthOffset = 0u;
			std::uint32_t padding[11]{};
		};
		ClusterBuildPushConstants clusterPush{};
		clusterPush.cullingSlotIndex = statsSlotIndex;
		clusterPush.lightDataOffset = renderSnapshot.lightDataOffset;
		clusterPush.clusterDataOffset = renderSnapshot.clusterDataOffset;
		clusterPush.lightIndexOffset = renderSnapshot.lightIndexOffset;
		clusterPush.clusterDepthOffset = renderSnapshot.clusterDepthOffset;

		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightingComputePipelineLayout, 0, 1, &impl->globalDescriptorSets[currentFrame], 0, nullptr);
		vkCmdPushConstants(cmd, impl->lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(clusterPush), &clusterPush);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterGridComputePipeline);
		vkCmdDispatch(cmd, (kLightingClusterDimX + 7u) / 8u, (kLightingClusterDimY + 7u) / 8u, std::max<uint32_t>(kLightingClusterDimZ, 1u));

		VkMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		TerraEngine::Renderer::VulkanLightingDepthEvidence::Request depthRequest;
		depthRequest.frameSlot = currentFrame; depthRequest.snapshotSlot = snapshotSlotIndex;
		depthRequest.width = activeRenderExtent.width; depthRequest.height = activeRenderExtent.height;
		depthRequest.cooperative = cooperativeDepthSelected;
		depthRequest.validation = shadowValidationRequested && presentation.efficiencyFixtureIntent.controlsApplied
			&& presentation.efficiencyFixtureIntent.depthControls.has_value();
		auto& depthEvidence = depthRequest.evidence;
		depthEvidence.kind = TerraEngine::Renderer::RendererEfficiencyEvidenceKind::LightingDepth;
		depthEvidence.viewType = renderViewType; depthEvidence.viewId = diagnosticViewId;
		depthEvidence.frameSerial = publication->frameId; depthEvidence.commandSerial = shadowCommandSerial;
		depthEvidence.resolution = {activeRenderExtent.width, activeRenderExtent.height};
		depthEvidence.effectiveOptions = presentation.efficiencyOptions;
		if (depthRequest.validation) {
			// Large selected-light identity lists are portable validation evidence.
			// Ordinary high-frequency timestamp samples need only typed query
			// ownership and avoid copying these two 1,024-record identity lists.
			depthEvidence.lightingUploads = lightingUploadEvidence;
			depthEvidence.fixtureCorrelation = presentation.efficiencyFixtureIntent.correlation;
			depthEvidence.fixtureControlGeneration = presentation.efficiencyFixtureIntent.controlGeneration;
			depthRequest.oracleRecipe = presentation.efficiencyFixtureIntent.depthControls->oracleRecipe;
			depthEvidence.preparationWorkerTasks = presentation.efficiencyOptions.options.validationPreparation
				== TerraEngine::Renderer::RendererEfficiencyPreparation::Concurrent ? 2 : 0;
			depthEvidence.preparationInlineTasks = 2 - depthEvidence.preparationWorkerTasks;
			depthEvidence.preparationSettled = true;
		}
		if (cooperativeDepthRequested && !cooperativeDepthSelected)
			depthEvidence.fallbackReason = "Cooperative depth pipeline or device limits unavailable; complete-scan reference selected.";
		// Both compute markers follow the grid barrier and enclose only depth
		// dispatch. Count/fill and validation copies remain outside the interval.
		const bool depthTimingRecorded = impl->lightingDepthEvidenceInitialized
			&& impl->lightingDepthEvidence.BeginProduction(cmd, depthRequest);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selectedDepthPipeline);
		// Cooperative work distributes every tile's pixels across one 64-lane
		// group; the reference keeps one serial full scan per 8x8 invocation.
		vkCmdDispatch(cmd, cooperativeDepthSelected ? kLightingClusterDimX : (kLightingClusterDimX + 7u) / 8u,
			cooperativeDepthSelected ? kLightingClusterDimY : (kLightingClusterDimY + 7u) / 8u, 1u);
		if (depthTimingRecorded) impl->lightingDepthEvidence.EndProduction(cmd, depthRequest);
		if (depthTimingRecorded && depthRequest.validation) {
			// This transfer sits outside the measured dispatch and copies only
			// this command's physical snapshot slice. The collector restores
			// compute visibility before the following count/fill passes.
			constexpr std::uint64_t tileBytes = sizeof(float) * 2u * kLightingClusterDimX * kLightingClusterDimY;
			impl->lightingDepthEvidence.RecordProductionBounds(cmd, depthRequest,
				impl->clusterDepthSSBOs[currentFrame].buffer,
				std::uint64_t(renderSnapshot.clusterDepthOffset) * sizeof(float) * 2u,
				tileBytes * VulkanContext::Impl::MAX_RENDER_SNAPSHOTS_PER_FRAME);
		}
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterLightCountComputePipeline);
		vkCmdDispatch(cmd, std::max<uint32_t>(clusterCount, 1u), 1u, 1u);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterLightPrefixComputePipeline);
		vkCmdDispatch(cmd, 1u, 1u, 1u);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterLightFillComputePipeline);
		vkCmdDispatch(cmd, std::max<uint32_t>(clusterCount, 1u), 1u, 1u);

		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT,
			0,
			1, &barrier,
			0, nullptr,
			0, nullptr
		);
		if (depthTimingRecorded && depthRequest.validation && depthRequest.oracleRecipe
			&& presentation.efficiencyDepthOracleInput) {
			// Isolated oracle bindings replace compute state only after every
			// production count/fill dispatch. Its upload, timing and readback do
			// not alter production lighting buffers or enter the speed interval.
			impl->lightingDepthEvidence.RecordOracle(cmd, depthRequest,
				*depthRequest.oracleRecipe, *presentation.efficiencyDepthOracleInput, cullingData);
		}
	}
	if (depthPreparedForPreSceneCompute) {
		// Pre-scene AO and cluster building sample the previous frame depth.
		// The upcoming scene render pass clears and writes depth again, so hand
		// the shared depth image back to graphics before vkCmdBeginRenderPass.
		impl->RestoreDepthForAttachment(cmd, activeDepthImage);
	}
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.precompute.end" : "game.precompute.end");

	// Prepared draws keep resource validation and command recording separate:
	// the expensive lookups happen once, then worker command buffers consume a
	// compact list of already-resolved Vulkan handles.
	beginCommandRecordingPhase(
		"Vulkan: Primary Prepare Draw Commands");
	struct PreparedDraw {
		// Copy Vulkan handles and immutable mesh facts into the frame-owned draw.
		// Backend cache containers may refresh entries while later resources are
		// prepared, so recorded work must not retain pointers into those containers.
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		std::uint32_t meshletCount = 0u;
		std::array<VkPushConstantRange,
			kMaxPreparedPushConstantRanges> pushConstantRanges{};
		std::uint32_t pushConstantRangeCount = 0u;
		// Every mesh draw binds exactly the global heap and one mesh payload set.
		// Inline storage avoids two heap allocations per visible/shadow draw.
		std::array<VkDescriptorSet, 2u> descriptorSets{};
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		PushConstants push{};
		std::uint32_t materialFlags = 0u;
		uint32_t indirectBufferIndex = 0;
		bool shadowGrouped = false;
		std::uint32_t shadowMemberIndex = 0, shadowGroupIndex = 0;
		// CPU keeps pipeline/descriptors/push ownership. This selects only the
		// command source after the GPU executor produced a bounded uint4 record.
		bool useGeneratedVirtualGeometryIndirect = false;
		std::uint32_t generatedVirtualGeometryIndirectIndex = 0u;
		TerraEngine::Renderer::VirtualGeometryRasterRoute rasterRoute =
			TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware;
	};
	thread_local std::vector<PreparedDraw> prepared;
	prepared.clear();
	prepared.reserve(commandResolver.Count());
	// Validate and publish the whole crowd stream before any grouped command is
	// suppressed. A malformed range, capacity overflow, allocation failure, or
	// descriptor publication failure leaves every original command available for
	// ordinary per-object recording in this same frame.
	thread_local std::vector<std::int32_t> groupedSubmissionByCommand;
	groupedSubmissionByCommand.assign(commandResolver.Count(), -1);
	thread_local std::vector<std::uint8_t> groupedCommandSuppressed;
	groupedCommandSuppressed.assign(commandResolver.Count(), 0u);
	const auto groupedRecordMatchesRepresentative = [](
		const RenderInstanceRecord& member,
		const RenderItemRecord& representative) {
		// These are exactly the values still shared through one push packet. The
		// packet/page byte selectors intentionally remain per-instance and therefore
		// are not equal. RenderSystem sealed every compared lane transactionally.
		return member.materialAndTexture[
				RenderInstanceRecord::kMaterialSelectorWord]
				== representative.materialIndex
			&& member.materialAndTexture[
				RenderInstanceRecord::kTextureSelectorWord]
				== representative.textureIndex
			&& member.objectIdentity[
				RenderInstanceRecord::kGeometryRouteWord]
				== static_cast<std::uint32_t>(representative.geometryRoute)
			&& member.objectIdentity[
				RenderInstanceRecord::kVirtualGeometryFlagsWord]
				== representative.virtualGeometryFlags
			&& member.objectIdentity[
				RenderInstanceRecord::kCompatibilityPolicyWord]
				== RenderInstanceRecord::PackCompatibilityPolicy(
					representative.taskShaderCullingEnabled,
					static_cast<std::uint32_t>(
						representative.animationBinding.source),
					representative.animationBinding.
						RequiresConservativeCoarseVisibility())
			&& member.geometrySelectors[
				RenderInstanceRecord::kSelectedLodWord]
				== representative.selectedVirtualLod
			&& member.geometrySelectors[
				RenderInstanceRecord::kMeshletCountWord]
				== representative.virtualMeshletCount
			&& member.virtualGeometryPayloadSelectors.x
				== static_cast<std::uint32_t>(
					representative.virtualGeometryPayloadSource)
			&& member.editorTint == representative.editorTint;
	};
	thread_local std::vector<RenderItemRecord> groupedRepresentatives;
	groupedRepresentatives.clear();
	groupedRepresentatives.reserve(presentation.groupedSubmissions.size());
	bool groupedInstanceStreamReady = false;
	bool groupedInstanceAllocationReplaced = false;
	if(presentation.renderOptimization.valid != 0u
		&& !presentation.groupedSubmissions.empty()
		&& !presentation.groupedInstanceRecords.empty()) {
		bool complete = true;
		std::size_t coveredInstanceCount = 0u;
		for(std::size_t submissionIndex = 0u;
			submissionIndex < presentation.groupedSubmissions.size(); ++submissionIndex) {
			const GroupedRenderSubmission& submission =
				presentation.groupedSubmissions[submissionIndex];
			const SharedRenderGroup& group = submission.group;
			if(!group.executable || !group.key.IsComplete()
				|| submission.representativeCommandIndex >= commandResolver.Count()
				|| group.instanceCount < 2u
				|| group.firstInstance != coveredInstanceCount
				|| group.firstInstance
					> presentation.groupedInstanceRecords.size()
				|| group.instanceCount > presentation.groupedInstanceRecords.size()
					- group.firstInstance) {
				complete = false;
				break;
			}
			RenderItemRecord representative{};
			if(!commandResolver.Resolve(
					submission.representativeCommandIndex, representative)
				|| group.key.meshId != representative.meshId
				|| group.key.materialId != representative.materialId
				|| group.key.geometryRoute != static_cast<std::uint32_t>(
					representative.geometryRoute))
			{
				complete = false;
				break;
			}
			groupedRepresentatives.push_back(std::move(representative));
			coveredInstanceCount += group.instanceCount;
			groupedSubmissionByCommand[submission.representativeCommandIndex] =
				static_cast<std::int32_t>(submissionIndex);
		}
		const std::size_t recordCount = presentation.groupedInstanceRecords.size();
		complete = complete && coveredInstanceCount == recordCount
			&& groupedRepresentatives.size()
				== presentation.groupedSubmissions.size();
		const bool sizeRepresentable = recordCount <=
			std::numeric_limits<VkDeviceSize>::max() / sizeof(RenderInstanceRecord);
		const VkDeviceSize requiredBytes = sizeRepresentable
			? static_cast<VkDeviceSize>(recordCount * sizeof(RenderInstanceRecord)) : 0u;
		if(complete && requiredBytes != 0u) {
			try {
				VulkanBuffer& instanceBuffer = impl->groupedInstanceBuffers[currentFrame];
				VkDeviceSize& capacity = impl->groupedInstanceCapacities[currentFrame];
				if(instanceBuffer.buffer == VK_NULL_HANDLE || capacity < requiredBytes) {
					const VkDeviceSize growth = std::max(requiredBytes,
						capacity + capacity / 2u + sizeof(RenderInstanceRecord));
					impl->RetireBuffer(instanceBuffer);
					instanceBuffer = impl->CreateBuffer(growth,
						VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
						VMA_MEMORY_USAGE_CPU_TO_GPU, "Grouped Render Instance Stream");
					capacity = growth;
					groupedInstanceAllocationReplaced = true;
					impl->groupedInstanceUploadMirrors[currentFrame].clear();
				}
				if(impl->groupedInstanceBufferIndex
					== VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX) {
					impl->groupedInstanceBufferIndex = impl->AllocateBufferIndex();
				}
				// Do not publish bytes yet. Animated records receive their exact
				// bridge selectors below; one final map/copy/descriptor update then
				// makes the complete stream visible atomically to grouped draws.
				groupedInstanceStreamReady = true;
			}
			catch(const std::exception&) {
				// The fallback loop below remains complete; resource pressure is not
				// allowed to make a partially uploaded crowd visible.
				groupedInstanceStreamReady = false;
			}
		}
	}
	std::uint32_t cameraVisibleShadowCasterCount = 0U;
	// This exact immutable membership is independent of grouped main-pass draw
	// counts. It proves off-camera casters even when camera submissions collapse.
	std::unordered_set<std::uint32_t> cameraVisibleShadowOrdinals;
	for (const auto& key : selection.VisibleOrdinals()) cameraVisibleShadowOrdinals.insert(key.ordinal);
	struct PreparedShadowList {
		TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
		std::vector<PreparedDraw> draws;
		std::vector<PreparedDraw> groupedDraws;
		std::vector<TerraEngine::Renderer::ShadowBatchMember> batchMembers;
		std::vector<TerraEngine::Renderer::RendererEfficiencyShadowBatchSource> batchSources;
		std::vector<RenderInstanceRecord> expectedInstances;
		TerraEngine::Renderer::ShadowBatchPlan batch;
		TerraEngine::Renderer::Vulkan::ShadowGroupedStreamPlan streamPlan;
		TerraEngine::Renderer::Vulkan::ShadowGroupedExecutionSelection execution;
		std::string batchFallback;
		std::vector<TerraEngine::Renderer::ShadowCasterStampInput> casterInputs;
		TerraEngine::Renderer::ShadowCasterMembership membership;
		TerraEngine::Renderer::StaticShadowPartition staticPartition;
		bool recordingReady = true;
		VulkanContext::Impl::ShadowPreparationRejection rejection;
		std::size_t sourceOrdinalCount = 0;
	};
	thread_local std::vector<PreparedShadowList> preparedShadowLists;
	if(preparedShadowLists.size() > presentation.shadowDrawLists.size()) {
		preparedShadowLists.resize(presentation.shadowDrawLists.size());
	}
	while(preparedShadowLists.size() < presentation.shadowDrawLists.size()) {
		preparedShadowLists.emplace_back();
	}
	for(PreparedShadowList& preparedList : preparedShadowLists) {
		preparedList.lightId = TerraEngine::EMPTY_UUID;
		preparedList.draws.clear();
		preparedList.groupedDraws.clear(); preparedList.batchMembers.clear();
		preparedList.batchSources.clear();
		preparedList.expectedInstances.clear(); preparedList.batch = {};
		preparedList.streamPlan = {}; preparedList.execution = {}; preparedList.batchFallback.clear();
		preparedList.casterInputs.clear();
		preparedList.membership = {};
		preparedList.staticPartition = {};
		preparedList.recordingReady = true;
		preparedList.rejection = {}; preparedList.sourceOrdinalCount = 0;
	}
	struct PrimitiveEstimateKey
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint32_t meshletOffset = 0U;
		std::uint32_t meshletCount = 0U;
		std::uint32_t fallbackPrimitiveCount = 0U;

		bool operator==(const PrimitiveEstimateKey&) const noexcept = default;
	};
	struct PrimitiveEstimateKeyHash
	{
		std::size_t operator()(const PrimitiveEstimateKey& value) const noexcept
		{
			std::size_t hash = std::hash<TerraEngine::UUID>{}(value.meshId);
			const auto combine = [&hash](std::uint32_t lane) noexcept {
				hash ^= static_cast<std::size_t>(lane) + 0x9e3779b9U
					+ (hash << 6U) + (hash >> 2U);
			};
			combine(value.meshletOffset);
			combine(value.meshletCount);
			combine(value.fallbackPrimitiveCount);
			return hash;
		}
	};
	thread_local std::unordered_map<PrimitiveEstimateKey, std::uint32_t,
		PrimitiveEstimateKeyHash> primitiveEstimateCache;
	primitiveEstimateCache.clear();
	primitiveEstimateCache.reserve(std::min<std::size_t>(
		commandResolver.Count(), 128U));
	const auto estimateSelectedPrimitives = [&](const RenderItemRecord& command,
		std::uint32_t fallbackPrimitiveCount,
		std::uint32_t meshletOffset,
		std::uint32_t meshletCount) {
		const PrimitiveEstimateKey key{
			command.meshId, meshletOffset, meshletCount, fallbackPrimitiveCount};
		if(const auto cached = primitiveEstimateCache.find(key);
			cached != primitiveEstimateCache.end()) {
			return cached->second;
		}
		const std::uint32_t primitiveCount = EstimateSelectedPrimitiveCount(
			command.meshId, fallbackPrimitiveCount, meshletOffset, meshletCount);
		primitiveEstimateCache.emplace(key, primitiveCount);
		return primitiveCount;
	};

	RenderStatsSnapshot pendingStats{};
	const auto groupedRecordingBegin = std::chrono::steady_clock::now();
	pendingStats.viewType = (view == ViewType::Scene) ? RenderViewType::Scene : RenderViewType::Game;
	pendingStats.outputKind = outputRequest.kind;
	pendingStats.outputId = outputRequest.outputId;
	pendingStats.dedicatedCaptureOutput = renderingDedicatedCaptureTarget;
	pendingStats.publicationFrameId = publication->frameId;
	pendingStats.publicationGeneration = publication->generation;
	pendingStats.rendererSessionGeneration = publication->rendererSessionGeneration;
	pendingStats.renderSize = glm::uvec2(hiZTargetWidth, hiZTargetHeight);
	pendingStats.frameTimeMs = SafeFrameTimeMs(presentation.deltaTimeSeconds);
	pendingStats.fps = pendingStats.frameTimeMs > 0.0f ? (1000.0f / pendingStats.frameTimeMs) : 0.0f;
	pendingStats.valid = true;
	pendingStats.optimization = presentation.renderOptimization;
	pendingStats.optimization.instanceStreamCapacityBytes = static_cast<std::uint32_t>(
		std::min<VkDeviceSize>(impl->groupedInstanceCapacities[currentFrame],
			std::numeric_limits<std::uint32_t>::max()));
	if(!groupedInstanceStreamReady && !presentation.groupedSubmissions.empty()) {
		pendingStats.optimization.fallbackAllocationFailureCount = 1u;
	}
	std::uint32_t recordedGroupedInstanceCount = 0u;
	std::uint32_t recordedGroupedSubmissionCount = 0u;

	const auto ResolvePipeline = [&](const TerraEngine::UUID& shaderCollectionId, VkRenderPass currentPass, uint32_t materialFlags) -> VulkanPipeline* {
		if (currentPass == VK_NULL_HANDLE) {
			// Material edits can invalidate resources while editor offscreen
			// targets are being rebuilt. A missing render pass means this frame
			// has nothing valid to bind against, so fail closed without asking
			// EnsurePipeline to log the lower-level error repeatedly.
			return nullptr;
		}
		const bool pipelineUsesConventionalVisibilityIdentity =
			impl->UsesConventionalVisibilityAttachments(currentPass);
		// Resource publication happens before vkBeginCommandBuffer. A shader
		// reload arriving during recording waits for next frame's preparation;
		// this lookup must never replace resources used by the active command.
		const DrawResourcePreparationKey selection{TerraEngine::EMPTY_UUID, TerraEngine::EMPTY_UUID,
			shaderCollectionId, currentPass, materialFlags, pipelineUsesConventionalVisibilityIdentity};
		const auto pipelineIt = preparedFramePipelines.find(selection);

		if (pipelineIt == preparedFramePipelines.end() || pipelineIt->second == nullptr) {
			return nullptr;
		}

		if (pipelineIt->second->pipeline == VK_NULL_HANDLE
			|| pipelineIt->second->layout == VK_NULL_HANDLE) {
			return nullptr;
		}
		if (pipelineUsesConventionalVisibilityIdentity
			&& !pipelineIt->second->usesConventionalVisibilityIdentity) {
			// A visible mesh drawn through target0-only SPIR-V would leave the
			// integer attachment clear and turn an object hit into false zero data.
			// Reject that command rather than letting automation misreport it.
			TERRAENGINE_LOG(LogLevel::Warning,
				"Skipping conventional draw because its active material pipeline does not write the typed identity attachment.");
			return nullptr;
		}

		return pipelineIt->second;
	};
	thread_local std::unordered_map<TerraEngine::UUID, std::shared_ptr<const Material>>
		materialLookupCache;
	materialLookupCache.clear();
	materialLookupCache.reserve(std::min<std::size_t>(
		commandResolver.Count(), 128u));
	TerraEngine::UUID lastMaterialId = TerraEngine::EMPTY_UUID;
	const Material* lastMaterial = nullptr;
	const auto ResolveMaterial = [&](const TerraEngine::UUID& materialId) -> const Material* {
		// Draw behavior must come from the immutable owner actually uploaded to
		// the material SSBO before recording began. A newer CPU publication is
		// considered at the next preparation boundary, not mixed into old GPU data.
		if(materialId == lastMaterialId)
		{
			return lastMaterial;
		}
		auto existing = materialLookupCache.find(materialId);
		if (existing != materialLookupCache.end()) {
			lastMaterialId = materialId;
			lastMaterial = existing->second.get();
			return lastMaterial;
		}
		const auto uploaded = impl->materialSourceOwners.find(materialId);
		auto materialOwner = uploaded == impl->materialSourceOwners.end()
			? std::shared_ptr<const Material>{} : uploaded->second;
		const Material* material = materialOwner.get();
		materialLookupCache.emplace(materialId, std::move(materialOwner));
		lastMaterialId = materialId;
		lastMaterial = material;
		return lastMaterial;
	};
	const auto HasMeshletPayload = [](const VulkanMesh& mesh) {
		// Mesh shaders need the packed vertex data, remap table, decode info,
		// primitive indices, and meshlet records to agree. Treat any missing
		// buffer as "not renderable yet" instead of recording a partial draw.
		return mesh.meshShaderVertexBuffer.buffer != VK_NULL_HANDLE
			&& mesh.meshletVertexRemapBuffer.buffer != VK_NULL_HANDLE
			&& mesh.meshDecodeInfoBuffer.buffer != VK_NULL_HANDLE
			&& mesh.indexBuffer.buffer != VK_NULL_HANDLE
			&& mesh.meshletBuffer.buffer != VK_NULL_HANDLE
			&& mesh.meshletCount != 0u;
	};
	const auto ResolveCanonicalGraphicsGroup =
		[&](const RenderItemRecord& command)
			-> std::optional<std::uint32_t>
	{
		if(!impl->deformationComputeRecordedForCurrentFrame
			|| impl->pendingDeformationImmutablePlan == nullptr
			|| impl->activeDeformationGraphicsBufferIndex
				== VulkanContext::Impl::
					INVALID_BINDLESS_BUFFER_INDEX
			|| impl->currentFrame
				>= impl->deformationFrameResources.size())
		{
			return std::nullopt;
		}
		const TerraEngine::Renderer::PublishedAnimationRenderBinding& binding =
			command.animationBinding;
		const std::uint64_t stableInstanceId =
			command.animation.stableInstanceId;
		const auto& resources =
			impl->deformationFrameResources[impl->currentFrame];
		if(!resources.live
			|| resources.frameId
				!= impl->pendingDeformationImmutablePlan->frameId
			|| resources.graphicsBridgeBindlessIndex
				!= impl->activeDeformationGraphicsBufferIndex)
		{
			return std::nullopt;
		}
		const std::uint64_t slot = stableInstanceId >> 32u;
		if(slot < resources.graphicsGroupSlotHints.size())
		{
			const auto& hint = resources.graphicsGroupSlotHints[
				static_cast<std::size_t>(slot)];
			if(!hint.ambiguous
				&& hint.stableInstanceId == stableInstanceId
				&& hint.meshId == command.meshId)
			{
				return hint.headerWordOffset;
			}
		}
		const auto group = resources.graphicsGroupOffsets.find(
			VulkanContext::Impl::VulkanDeformationFrameResources::
				GraphicsGroupKey{stableInstanceId, command.meshId});
		return group != resources.graphicsGroupOffsets.end()
			? std::optional<std::uint32_t>(group->second)
			: std::nullopt;
	};
	const auto RequiresConservativeDeformableVisibility =
		[&](const RenderItemRecord& command,
			const std::optional<std::uint32_t>& canonicalGroup)
	{
		// Canonical deformation publishes current dynamic cluster bounds. Legacy
		// owner deformation does not, so only that route must fail open.
		return command.animationBinding.
				RequiresOwnerMeshDeformationFallback()
			&& !canonicalGroup.has_value();
	};
	const auto ApplyAnimationGraphicsBridge = [&](const RenderItemRecord& command,
		PushConstants& push, bool preferOwnerPalette,
		const std::optional<std::uint32_t>& resolvedCanonicalGroup) {
		// Static and rejected animation draws publish invalid bridge fields. A
		// validated CPU pose selects its own frame upload; GPU/Hybrid work then
		// follows the separately generation-checked compute bridge below.
		push.meshDataIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
		push.textureIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
		const TerraEngine::Renderer::PublishedAnimationRenderBinding& binding =
			command.animationBinding;
		const std::optional<std::uint32_t> canonicalGroup = preferOwnerPalette
			? std::nullopt
			: resolvedCanonicalGroup;
		if(canonicalGroup.has_value())
		{
			// Every canonical owner/mesh pair owns one contiguous bridge slice.
			// Publish its immutable header offset so independently animated crowd
			// instances consume their own deformed page range without mutating
			// shared mesh resources or assuming the frame contains one owner.
			push.meshDataIndex =
				impl->activeDeformationGraphicsBufferIndex;
			push.textureIndex = canonicalGroup.value();
			return;
		}
		if(binding.HasCpuDeformation()
			&& impl->pendingCpuAnimationPlan != nullptr
			&& impl->activeCpuAnimationGraphicsBufferIndex
				!= VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX
			&& impl->currentFrame < impl->cpuAnimationFrameResources.size()
			&& binding.cpuDeformationRecordIndex
				< impl->pendingCpuAnimationPlan->records.size()) {
			const VulkanContext::Impl::VulkanCpuAnimationFrameResources&
				resources =
					impl->cpuAnimationFrameResources[impl->currentFrame];
			const TerraEngine::Renderer::CpuAnimationDeformationRecord&
				record = impl->pendingCpuAnimationPlan->records[
					binding.cpuDeformationRecordIndex];
			const std::uint64_t stableInstanceId =
				command.animation.stableInstanceId;
			if(resources.live
				&& resources.frameId
					== impl->pendingCpuAnimationPlan->frameId
				&& resources.rendererGeneration
					== impl->pendingCpuAnimationPlan->rendererGeneration
				&& resources.graphicsBridgeBindlessIndex
					== impl->activeCpuAnimationGraphicsBufferIndex
				&& record.IsComplete()
				&& record.stableInstanceId == stableInstanceId
				&& record.meshId == command.meshId
				&& record.poseGeneration == command.animation.poseGeneration
				&& record.deformablePayloadGeneration
					== binding.deformablePayloadGeneration
				&& record.rendererGeneration
					== command.animation.rendererGeneration
				&& record.skeletonCompatibilityFingerprint
					== binding.skeletonCompatibilityFingerprint
				&& record.data == binding.cpuDeformation) {
				push.meshDataIndex =
					impl->activeCpuAnimationGraphicsBufferIndex;
				push.textureIndex =
					binding.cpuDeformationRecordIndex;
				return;
			}
		}
		const RenderItemRecord::PublishedAnimationGeneration& gpu =
			command.animation;
		if (!command.HasCompleteGpuAnimation()
			|| impl->activeAnimationGraphicsBufferIndex == VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX
			|| impl->currentFrame >= impl->animationFrameResources.size())
		{
			return;
		}

		// The bridge buffer is frame-owned. Compare immutable generations before
		// using its copied packet index so a stale render snapshot falls back to
		// bind pose rather than sampling a newer instance's palette range.
		const VulkanContext::Impl::VulkanAnimationFrameResources& resources =
			impl->animationFrameResources[impl->currentFrame];
		if (!resources.live
			|| resources.graphicsBridgeBindlessIndex != impl->activeAnimationGraphicsBufferIndex
			|| resources.planGeneration != gpu.animationPlanGeneration
			|| resources.rendererGeneration != gpu.rendererGeneration
			|| impl->pendingAnimationImmutablePlan == nullptr
			|| impl->pendingAnimationImmutablePlan->generation
				!= gpu.animationPlanGeneration
			|| impl->pendingAnimationImmutablePlan->RenderIdentityCount()
				!= impl->pendingAnimationImmutablePlan->WorkPackets().size()
			|| gpu.firstGpuWorkPacket
				>= impl->pendingAnimationImmutablePlan->WorkPackets().size())
		{
			return;
		}
		const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
			impl->pendingAnimationImmutablePlan->WorkPackets()[
				gpu.firstGpuWorkPacket];
		const TerraEngine::Animation::GpuAnimationRenderIdentity& renderIdentity =
			impl->pendingAnimationImmutablePlan->RenderIdentity(
				gpu.firstGpuWorkPacket);
		if (packet.generationRecordIndex >= impl->pendingAnimationImmutablePlan->GenerationRecordCount())
		{
			return;
		}
		const TerraEngine::Animation::GpuAnimationGenerationRecord& generation =
			impl->pendingAnimationImmutablePlan->GenerationRecord(packet.generationRecordIndex);
		const bool packetIdentityMatches =
			packet.stableInstanceId == gpu.stableInstanceId
			&& packet.stageMask == gpu.gpuStageMask
			&& generation.rendererGeneration == gpu.rendererGeneration
			&& generation.workGeneration == gpu.poseGeneration
			&& generation.programGeneration == gpu.programGeneration
			&& renderIdentity.meshId == command.meshId
			&& renderIdentity.deformablePayloadGeneration
				== command.animationBinding.deformablePayloadGeneration
			&& renderIdentity.skeletonCompatibilityFingerprint
				== command.animationBinding.skeletonCompatibilityFingerprint
			&& TerraEngine::Animation::HasGpuAnimationFlag(packet.flags,
				TerraEngine::Animation::GpuAnimationWorkFlag::HybridCpuAuthority)
				== (static_cast<TerraEngine::Renderer::AnimationRenderEvaluationSource>(
					gpu.source)
					== TerraEngine::Renderer::AnimationRenderEvaluationSource::
						HybridAuthoritative);
		if (!packetIdentityMatches)
		{
			// An in-range packet index is not sufficient identity: deterministic
			// sorting can assign that index to another animator in a newer snapshot.
			// Reject the whole copied envelope and leave the draw in bind pose.
			return;
		}

		push.meshDataIndex = impl->activeAnimationGraphicsBufferIndex;
		push.textureIndex = gpu.firstGpuWorkPacket;
	};
	// Resolve the exact animation bridge for every grouped owner before the
	// second (authoritative) stream publication. The first allocation/upload
	// above only reserves a frame-slot buffer; this pass is the all-or-nothing
	// boundary that prevents one animator from borrowing its representative's
	// palette record.
	const auto instanceStreamUploadBegin = std::chrono::steady_clock::now();
	const bool useSealedDenseGroupedAnimationSelectors =
		groupedInstanceStreamReady
		&& impl->pendingAnimationFullImmutablePlanCoverage
		&& impl->pendingAnimationImmutablePlan != nullptr
		&& impl->activeAnimationGraphicsBufferIndex
			!= VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Prepare Grouped Instance Stream",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(groupedInstanceStreamReady) {
			// Validate the backend-owned selectors in compact ordinary memory first.
			// Once every record succeeds, upload the authoritative stream once and patch
			// only the two words that cannot be known before backend publication.
			const bool useSealedDenseSelectors =
				useSealedDenseGroupedAnimationSelectors;
			thread_local std::vector<glm::uvec2> resolvedAnimationSelectors;
			if(!useSealedDenseSelectors) {
				try {
					resolvedAnimationSelectors.resize(
						presentation.groupedInstanceRecords.size());
				} catch(const std::bad_alloc&) {
					groupedInstanceStreamReady = false;
				}
			}
			bool resolvedEveryAnimatedRecord = groupedInstanceStreamReady;
			for(std::size_t submissionIndex = 0u;
				resolvedEveryAnimatedRecord
					&& submissionIndex < presentation.groupedSubmissions.size();
				++submissionIndex) {
				const SharedRenderGroup& group =
					presentation.groupedSubmissions[submissionIndex].group;
				const RenderItemRecord& representative =
					groupedRepresentatives[submissionIndex];
				for(std::uint32_t instance = 0u;
					resolvedEveryAnimatedRecord && instance < group.instanceCount;
					++instance) {
					const std::size_t recordIndex = group.firstInstance + instance;
					const RenderInstanceRecord& record =
						presentation.groupedInstanceRecords[recordIndex];
					const std::uint32_t sourceCommandIndex =
						record.materialAndTexture[
							RenderInstanceRecord::kPickingIdentityWord];
					if(sourceCommandIndex >= commandResolver.Count()
						|| !groupedRecordMatchesRepresentative(
							record, representative)) {
						resolvedEveryAnimatedRecord = false;
						break;
					}
					if(useSealedDenseSelectors) {
						// Full coverage proves that every dynamic lane names exactly one
						// packet in the immutable plan. Recheck the compact generation tuple
						// carried by this presentation record, then publish its packet index.
						const std::uint32_t packetIndex = record.animationSelectors.x;
						if((record.animationSelectors.y != 0u
								&& record.animationSelectors.y
									!= impl->pendingAnimationImmutablePlan->generation)
							|| record.animationSelectors.z
								!= impl->pendingAnimationCommandPlan.rendererGeneration
							|| packetIndex >= impl->pendingAnimationImmutablePlan->
								WorkPackets().size()) {
							resolvedEveryAnimatedRecord = false;
						}
						continue;
					}
					RenderItemRecord source{};
					if(!commandResolver.Resolve(sourceCommandIndex, source)) {
						resolvedEveryAnimatedRecord = false;
						break;
					}
					if(!source.HasCompleteGpuAnimation()) {
						resolvedAnimationSelectors[recordIndex] = glm::uvec2(
							VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX);
						continue;
					}
					const std::optional<std::uint32_t> canonicalGroup =
						ResolveCanonicalGraphicsGroup(source);
					PushConstants animationPush{};
					ApplyAnimationGraphicsBridge(source, animationPush,
						RequiresConservativeDeformableVisibility(
							source, canonicalGroup), canonicalGroup);
					if(animationPush.meshDataIndex
							== VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX
						|| animationPush.textureIndex
							== VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX) {
						resolvedEveryAnimatedRecord = false;
						break;
					}
					resolvedAnimationSelectors[recordIndex] = glm::uvec2(
						animationPush.meshDataIndex, animationPush.textureIndex);
				}
			}
			VulkanBuffer& instanceBuffer = impl->groupedInstanceBuffers[currentFrame];
			const VkDeviceSize bytes = static_cast<VkDeviceSize>(
				presentation.groupedInstanceRecords.size() * sizeof(RenderInstanceRecord));
			if(resolvedEveryAnimatedRecord) {
				thread_local std::vector<RenderInstanceRecord> resolvedRecords;
				std::span<const RenderInstanceRecord> uploadRecords =
					presentation.groupedInstanceRecords;
				if(!useSealedDenseSelectors) {
					try {
						resolvedRecords.assign(presentation.groupedInstanceRecords.begin(),
							presentation.groupedInstanceRecords.end());
					}
					catch(const std::bad_alloc&) {
						resolvedEveryAnimatedRecord = false;
					}
					for(std::size_t recordIndex = 0u;
						resolvedEveryAnimatedRecord
							&& recordIndex < resolvedAnimationSelectors.size(); ++recordIndex) {
						resolvedRecords[recordIndex].animationSelectors.x =
							resolvedAnimationSelectors[recordIndex].x;
						resolvedRecords[recordIndex].animationSelectors.y =
							resolvedAnimationSelectors[recordIndex].y;
					}
					uploadRecords = resolvedRecords;
				}

				std::vector<RenderInstanceRecord>& mirror =
					impl->groupedInstanceUploadMirrors[currentFrame];
				const TerraEngine::Renderer::Vulkan::GroupedInstanceDirtyPlan dirtyPlan =
					TerraEngine::Renderer::Vulkan::BuildGroupedInstanceDirtyPlan(
					mirror, uploadRecords, groupedInstanceAllocationReplaced);
				void* mapped = instanceBuffer.mappedData;
				const bool temporaryMapping = mapped == nullptr;
				if(resolvedEveryAnimatedRecord && !dirtyPlan.ranges.empty()
					&& ((temporaryMapping
							&& vmaMapMemory(impl->allocator, instanceBuffer.allocation, &mapped)
								!= VK_SUCCESS)
						|| mapped == nullptr)) {
					resolvedEveryAnimatedRecord = false;
				}
				for(const TerraEngine::Renderer::Vulkan::GroupedInstanceDirtyRange& range :
					dirtyPlan.ranges) {
					if(!resolvedEveryAnimatedRecord) {
						break;
					}
					const VkDeviceSize byteOffset = static_cast<VkDeviceSize>(
						range.firstRecord * sizeof(RenderInstanceRecord));
					const VkDeviceSize byteCount = static_cast<VkDeviceSize>(
						range.recordCount * sizeof(RenderInstanceRecord));
					std::memcpy(static_cast<std::byte*>(mapped) + byteOffset,
						uploadRecords.data() + range.firstRecord,
						static_cast<std::size_t>(byteCount));
					if(vmaFlushAllocation(impl->allocator, instanceBuffer.allocation,
						byteOffset, byteCount) != VK_SUCCESS) {
						resolvedEveryAnimatedRecord = false;
					}
				}
				if(temporaryMapping && mapped != nullptr) {
					vmaUnmapMemory(impl->allocator, instanceBuffer.allocation);
				}
				if(resolvedEveryAnimatedRecord) {
					try {
						mirror.assign(uploadRecords.begin(), uploadRecords.end());
					}
					catch(const std::bad_alloc&) {
						// Rendering remains correct after a successful upload. Dropping only
						// this optimization proof forces a complete refresh for the slot.
						mirror.clear();
					}
				}
				else {
					mirror.clear();
				}
			}
			if(resolvedEveryAnimatedRecord) {
				impl->UpdateGlobalBufferForFrame(currentFrame,
					impl->groupedInstanceBufferIndex, instanceBuffer.buffer, 0u,
					bytes, true);
			}
			else {
				groupedInstanceStreamReady = false;
			}
		}
	}
	if(pendingStats.optimization.valid != 0u) {
		// This covers selector resolution plus the single authoritative map/copy/
		// flush/publication boundary. It deliberately excludes command recording.
		pendingStats.optimization.instanceStreamUploadNanoseconds =
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - instanceStreamUploadBegin).count());
	}
	struct StaticDrawPreparationKey
	{
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID requestedCollectionId = TerraEngine::EMPTY_UUID;
		VkRenderPass renderPass = VK_NULL_HANDLE;
		std::uint32_t materialFlags = 0U;

		bool operator==(const StaticDrawPreparationKey&) const noexcept = default;
	};
	struct StaticDrawPreparationKeyHash
	{
		std::size_t operator()(const StaticDrawPreparationKey& value) const noexcept
		{
			std::size_t hash = std::hash<TerraEngine::UUID>{}(value.meshId);
			const auto combine = [&hash](std::size_t lane) noexcept
			{
				hash ^= lane + static_cast<std::size_t>(0x9e3779b9U)
					+ (hash << 6U) + (hash >> 2U);
			};
			combine(std::hash<TerraEngine::UUID>{}(value.materialId));
			combine(std::hash<TerraEngine::UUID>{}(value.requestedCollectionId));
			combine(std::hash<VkRenderPass>{}(value.renderPass));
			combine(std::hash<std::uint32_t>{}(value.materialFlags));
			return hash;
		}
	};
	struct StaticDrawPreparation
	{
		const char* rejectionStage = nullptr;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkDescriptorSet meshDescriptorSet = VK_NULL_HANDLE;
		TerraEngine::UUID selectedCollectionId = TerraEngine::EMPTY_UUID;
		std::uint64_t collectionGeneration = 0;
		std::shared_ptr<const Mesh> meshOwner;
		std::uint64_t meshPublicationGeneration = 0, meshAllocationGeneration = 0;
		std::uint32_t meshletCount = 0u;
		std::uint32_t indexCount = 0u;
		bool hasMeshletBounds = false;
		bool hasTaskStage = false;
		std::array<VkPushConstantRange,
			kMaxPreparedPushConstantRanges> pushConstantRanges{};
		std::uint32_t pushConstantRangeCount = 0u;
	};
	thread_local std::unordered_map<StaticDrawPreparationKey, StaticDrawPreparation,
		StaticDrawPreparationKeyHash> staticDrawPreparationCache;
	staticDrawPreparationCache.clear();
	// Repeated instances share immutable mesh, material, pipeline, and descriptor
	// resources. Resolve that static envelope once per exact pass/route key; the
	// following loop still builds unique model and animation push data per owner.
	staticDrawPreparationCache.reserve(std::min<std::size_t>(
		commandResolver.Count() + presentation.shadowDrawLists.size(), 128U));
	std::optional<StaticDrawPreparationKey> lastStaticPreparationKey;
	std::optional<StaticDrawPreparation> lastStaticPreparation;
	const char* staticPreparationFailure = nullptr;
	const auto ResolveStaticDrawPreparation = [&](const RenderItemRecord& command,
		TerraEngine::UUID requestedCollectionId,
		VkRenderPass renderPass,
		std::uint32_t materialFlags) -> const StaticDrawPreparation*
	{
		const StaticDrawPreparationKey key{
			command.meshId,
			command.materialId,
			requestedCollectionId,
			renderPass,
			materialFlags};
		if(lastStaticPreparationKey.has_value()
			&& *lastStaticPreparationKey == key)
		{
			return lastStaticPreparation.has_value()
				? &*lastStaticPreparation : nullptr;
		}
		if(const auto cached = staticDrawPreparationCache.find(key);
			cached != staticDrawPreparationCache.end())
		{
			lastStaticPreparationKey = key;
			lastStaticPreparation = cached->second;
			staticPreparationFailure = cached->second.rejectionStage;
			return lastStaticPreparation->pipeline != VK_NULL_HANDLE
				&& lastStaticPreparation->pipelineLayout != VK_NULL_HANDLE
				&& lastStaticPreparation->meshDescriptorSet != VK_NULL_HANDLE
				&& lastStaticPreparation->meshletCount != 0u
				? &*lastStaticPreparation : nullptr;
		}

		// All normal camera commands and every off-camera light caster were
		// prepared before command recording. Missing resources reject this draw;
		// reacquiring a newer CPU owner here could invalidate earlier copied GPU
		// handles while the same primary is still being recorded.
		auto meshIt = impl->meshCache.find(command.meshId);
		if(meshIt == impl->meshCache.end() || !HasMeshletPayload(meshIt->second)) {
			staticPreparationFailure = meshIt == impl->meshCache.end() ? "MeshCacheMissing" : "MeshletPayloadIncomplete";
			StaticDrawPreparation rejected; rejected.rejectionStage = staticPreparationFailure;
			staticDrawPreparationCache.emplace(key, std::move(rejected));
			lastStaticPreparationKey = key;
			lastStaticPreparation.reset();
			return nullptr;
		}

		TerraEngine::UUID selectedCollectionId = requestedCollectionId;
		VulkanPipeline* pipeline = ResolvePipeline(
			selectedCollectionId, renderPass, materialFlags);
		if(pipeline == nullptr && selectedCollectionId == defaultTaskCollectionId) {
			selectedCollectionId = defaultMeshCollectionId;
			pipeline = ResolvePipeline(selectedCollectionId, renderPass, materialFlags);
		}
		if(pipeline == nullptr) {
			staticPreparationFailure = "FrozenPipeline";
			StaticDrawPreparation rejected; rejected.rejectionStage = staticPreparationFailure;
			staticDrawPreparationCache.emplace(key, std::move(rejected));
			lastStaticPreparationKey = key;
			lastStaticPreparation.reset();
			return nullptr;
		}

		if (!impl->materialIndexMap.contains(command.materialId)
			|| !impl->materialSourceOwners.contains(command.materialId)) {
			staticPreparationFailure = !impl->materialIndexMap.contains(command.materialId) ? "MaterialIndexMissing" : "MaterialOwnerMissing";
			StaticDrawPreparation rejected; rejected.rejectionStage = staticPreparationFailure;
			staticDrawPreparationCache.emplace(key, std::move(rejected));
			lastStaticPreparationKey = key;
			lastStaticPreparation.reset();
			return nullptr;
		}
		const VkDescriptorSet meshDescriptorSet =
			impl->EnsureMeshDescriptorSet(command.meshId, *pipeline);
		if(meshDescriptorSet == VK_NULL_HANDLE) {
			staticPreparationFailure = "MeshDescriptorSet";
			StaticDrawPreparation rejected; rejected.rejectionStage = staticPreparationFailure;
			staticDrawPreparationCache.emplace(key, std::move(rejected));
			lastStaticPreparationKey = key;
			lastStaticPreparation.reset();
			return nullptr;
		}
		if(pipeline->pushConstants.size() > kMaxPreparedPushConstantRanges)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Skipping draw because the reflected pipeline exposes more push-constant ranges than the prepared draw can retain.");
			staticPreparationFailure = "PushConstantRange";
			StaticDrawPreparation rejected; rejected.rejectionStage = staticPreparationFailure;
			staticDrawPreparationCache.emplace(key, std::move(rejected));
			lastStaticPreparationKey = key;
			lastStaticPreparation.reset();
			return nullptr;
		}
		StaticDrawPreparation preparation{};
		preparation.pipeline = pipeline->pipeline;
		preparation.pipelineLayout = pipeline->layout;
		preparation.meshDescriptorSet = meshDescriptorSet;
		preparation.selectedCollectionId = selectedCollectionId;
		preparation.collectionGeneration = pipeline->collectionGeneration;
		preparation.meshOwner = meshIt->second.sourceOwner;
		preparation.meshPublicationGeneration = meshIt->second.cpuPublicationGeneration;
		preparation.meshAllocationGeneration = meshIt->second.allocationGeneration;
		preparation.meshletCount = meshIt->second.meshletCount;
		preparation.indexCount = meshIt->second.indexCount;
		preparation.hasMeshletBounds = meshIt->second.hasMeshletBounds;
		preparation.hasTaskStage = pipeline->hasTaskStage;
		preparation.pushConstantRangeCount = static_cast<std::uint32_t>(
			pipeline->pushConstants.size());
		std::copy(
			pipeline->pushConstants.begin(),
			pipeline->pushConstants.end(),
			preparation.pushConstantRanges.begin());
		auto [inserted, wasInserted] = staticDrawPreparationCache.emplace(
			key,
			std::move(preparation));
		(void)wasInserted;
		lastStaticPreparationKey = key;
		lastStaticPreparation = inserted->second;
		staticPreparationFailure = nullptr;
		return &*lastStaticPreparation;
	};


	for (std::uint32_t commandIndex = 0u;
		commandIndex < commandResolver.Count(); ++commandIndex) {
		if(groupedCommandSuppressed[commandIndex] != 0u) {
			continue;
		}
		RenderItemRecord command{};
		if(!commandResolver.Resolve(commandIndex, command)) continue;
		const Material* material = ResolveMaterial(command.materialId);
		if (!material) continue;

		VkRenderPass currentPass = (impl->editorMode || renderingDedicatedCaptureTarget)
			? impl->offscreenRenderPass
			: impl->renderPass;
		uint32_t materialFlags = material->header.flags;

		TerraEngine::UUID collectionId = material->header.shaderCollectionId;

		if (collectionId == TerraEngine::EMPTY_UUID) {
			collectionId = defaultMeshCollectionId;
		}
		const std::optional<std::uint32_t> canonicalGraphicsGroup =
			ResolveCanonicalGraphicsGroup(command);
		const bool canonicalGraphicsAvailable = canonicalGraphicsGroup.has_value();
		const bool groupedPerInstanceAnimationBridgeAvailable =
			groupedInstanceStreamReady
			&& groupedSubmissionByCommand[commandIndex] >= 0;
		const bool multiOwnerCanonicalFallback =
			impl->pendingAnimationImmutablePlan != nullptr
			&& impl->pendingAnimationImmutablePlan->WorkPackets().size() > 1u
			&& command.HasCompleteGpuAnimation()
			&& !canonicalGraphicsAvailable;

		// Route default materials through the task pipeline whenever supported.

		if (collectionId == defaultMeshCollectionId
			&& defaultTaskPipelineAvailable
			&& (groupedPerInstanceAnimationBridgeAvailable
				|| canonicalGraphicsAvailable
				|| !command.animationBinding.
					RequiresOwnerMeshDeformationFallback())
			&& (!multiOwnerCanonicalFallback
				|| groupedPerInstanceAnimationBridgeAvailable)) {
			// Canonical page deformation requires the task stage to initialize
			// packet-local payload identity. Legacy owner deformation stays on
			// the complete owner-mesh route until it publishes that same proof.
			// A validated grouped stream is the one safe exception: every member
			// carries its own generation-checked animation buffer and record index,
			// so the task/mesh stages never borrow the representative's palette.
			collectionId = defaultTaskCollectionId;
		}

		const StaticDrawPreparation* staticPreparation =
			ResolveStaticDrawPreparation(
				command, collectionId, currentPass, materialFlags);
		if(staticPreparation == nullptr) {
			TERRAENGINE_LOG(LogLevel::Warning,
				"Skipping draw because its static Vulkan resources are unavailable.");
			continue;
		}
		collectionId = staticPreparation->selectedCollectionId;
		const VkDescriptorSet meshDescriptorSet =
			staticPreparation->meshDescriptorSet;

		// Let task shaders reject meshlets once the mesh payload has CPU-built
		// bounds. Hi-Z visibility is guarded separately, so this flag only
		// controls the ordinary per-meshlet culling path.
		constexpr bool kEnableGpuMeshletCulling = true;
		const bool conservativeDeformableVisibility =
			RequiresConservativeDeformableVisibility(command, canonicalGraphicsGroup);
		const bool activeTaskCulling = kEnableGpuMeshletCulling
			&& staticPreparation->hasTaskStage
			&& presentation.globalTaskShaderCullingEnabled
			&& impl->globalTaskShaderCullingEnabled
			&& command.taskShaderCullingEnabled
			&& staticPreparation->hasMeshletBounds
			&& !conservativeDeformableVisibility;
		const auto& executorPlan = impl->latestVirtualGeometryExecutorPlan;
		const bool deviceAuthoredCommand =
			executorPlan.resourcesReady
			&& executorPlan.descriptorReady
			&& executorPlan.pipelinesReady
			&& executorPlan.generatedSubmissionExecutionAvailable
			&& generatedVirtualGeometrySubmissionsUsable
			&& !executorPlan.zeroWork
			&& command.virtualGeometryPacketDescriptorIndex
				< executorPlan.boundedPacketCount
			&& impl->latestVirtualGeometryExecutorResources
				.generatedSubmissionBuffer != VK_NULL_HANDLE;
		const bool ownerPaletteDeformation =
			(command.virtualGeometryFlags
				& kVirtualGeometryFlagOwnerPaletteDeformation) != 0u
			|| (command.animationBinding.RequiresConservativeCoarseVisibility()
				&& !deviceAuthoredCommand);

		PushConstants push{};
		// Every draw in this immutable view uses the same camera transform.
		// Reusing the matrix avoids repeated identical 4x4 multiplies for a shared crowd.
		push.viewProj = currentViewProjection;
		push.model = command.modelMatrix;
		push.materialIndex = 0;

		if(const auto materialIndex = impl->materialIndexMap.find(command.materialId);
			materialIndex != impl->materialIndexMap.end()) {
			push.materialIndex = materialIndex->second;
		}
		ApplyAnimationGraphicsBridge(command, push,
			ownerPaletteDeformation, canonicalGraphicsGroup);
		push.vrsActive = 0u;
		push.taskCullingEnabled = activeTaskCulling ? 1u : 0u;
		push.statsSlotIndex = statsSlotIndex;
		push.cullingEpsilon = std::max(presentation.cullingEpsilon, 0.0001f);
		push.lightDataOffset = renderSnapshot.lightDataOffset;
		push.clusterDataOffset = renderSnapshot.clusterDataOffset;
		push.lightIndexOffset = renderSnapshot.lightIndexOffset;
		// Indirect recording and upload both address the frame-slot command buffer
		// through this base. It must remain the snapshot offset: replacing it with
		// per-object metadata makes vkCmdDrawMeshTasksIndirectEXT read an
		// uninitialised zero-command slot and silently dispatch no mesh work.
		push.indirectDrawOffset = renderSnapshot.indirectDrawOffset;
		// The default task and mesh shaders do not consume the page-need offset;
		// page-need count remains available for their diagnostics. Reuse this ABI
		// lane only for the conventional identity fragment's one-based immutable
		// snapshot-table key. This avoids increasing the 256-byte push-constant
		// contract while keeping the indirect-command addressing channel intact.
		push.selectedVirtualPageNeedOffset = command.conventionalVisibilityIdentityIndex;
		// RenderSystem has already selected the packet/cluster meshlet range.
		// Owner-mesh draws use the baked global meshlet offset. Resident page and
		// fallback draws may use a compact page-local stream, but the original
		// owner offset is still pushed so invalid page descriptors can fail open
		// to the correct owner range instead of restarting at meshlet zero.
		const bool requestedPageLocalGeometry =
			command.virtualGeometryPayloadSource == VirtualGeometryPagePayloadSource::ResidentPagePayload
			|| command.virtualGeometryPayloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		const bool pageLocalGeometry =
			requestedPageLocalGeometry
			&& !ownerPaletteDeformation
			&& impl->activeVirtualGeometryPagePayloadBufferIndex != VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
		// A validated compute bridge is compatible with a self-contained demanded-page
		// packet: the mesh shader resolves the packet's page id through that
		// bridge and reads current deformed vertices. Only an absent/corrupt page
		// descriptor falls back to the owner range below.
		const uint32_t ownerMeshletOffset =
			std::min(command.virtualMeshletOffset,
				staticPreparation->meshletCount);
		const uint32_t ownerMeshletCount =
			command.virtualMeshletCount == 0u
				? (staticPreparation->meshletCount - ownerMeshletOffset)
				: std::min(command.virtualMeshletCount,
					staticPreparation->meshletCount - ownerMeshletOffset);
		const uint32_t selectedMeshletCount = pageLocalGeometry
			? command.virtualMeshletCount
			: ownerMeshletCount;
		const GroupedRenderSubmission* groupedSubmission = nullptr;
		if(groupedInstanceStreamReady
			&& groupedSubmissionByCommand[commandIndex] >= 0
			&& command.UsesGroupableMeshTaskRoute()
			&& staticPreparation->hasTaskStage) {
			const GroupedRenderSubmission& candidate = presentation.groupedSubmissions[
				static_cast<std::size_t>(groupedSubmissionByCommand[commandIndex])];
			const SharedRenderGroup& group = candidate.group;
			const uint32_t baseTaskGroups = (selectedMeshletCount + 31u) / 32u;
			const bool taskCountFits = baseTaskGroups != 0u
				&& group.instanceCount <= std::numeric_limits<uint32_t>::max()
					/ baseTaskGroups;
			const bool membersMatch = taskCountFits
				&& group.key.meshId == command.meshId
				&& group.key.materialId == command.materialId
				&& group.key.geometryRoute
					== static_cast<std::uint32_t>(command.geometryRoute);
			if(membersMatch) {
				groupedSubmission = &candidate;
			}
		}
		// Always push the owner-mesh offset. Task/mesh shaders ignore it when a
		// page-local descriptor validates, but they need the original range when
		// a descriptor is missing or rejected and the draw falls back to the
		// owner mesh payload for correctness.
		push.virtualMeshletOffset = ownerMeshletOffset;
		push.virtualMeshletCount = selectedMeshletCount;
		push.selectedVirtualLod = command.selectedVirtualLod;
		push.virtualGeometryFlags = command.virtualGeometryFlags;
		if(ownerPaletteDeformation)
		{
			// Bind the route choice and deformation source to the same immutable
			// executor decision. Backend-wide capability support alone cannot prove
			// that this packet will execute through generated device work.
			push.virtualGeometryFlags |=
				kVirtualGeometryFlagOwnerPaletteDeformation;
		}
		if (staticPreparation->hasTaskStage)
		{
			// Task payload addressing remains authoritative when culling is off.
			// A mesh-stage group id is local to each DispatchMesh call, so using it
			// directly would repeat the first 32 meshlets of every larger range.
			push.virtualGeometryFlags |= kVirtualGeometryFlagTaskPayloadValid;
		}
		if (conservativeDeformableVisibility)
		{
			// A whole-mesh envelope may be valid while per-cluster dynamic bounds
			// are still unavailable.  Mark the visible fail-open policy and remove
			// cone proof so every task/mesh pass agrees that it must not reject a
			// moving cluster before the shader-visible bounds generation arrives.
			push.virtualGeometryFlags |= kVirtualGeometryFlagFailOpen;
			push.virtualGeometryFlags &= ~kVirtualGeometryFlagConeCullingSafe;
		}
		push.selectedVirtualClusterOffset = command.selectedVirtualClusterOffset;
		push.selectedVirtualClusterCount = command.selectedVirtualClusterCount;
		push.selectedVirtualPageNeedCount = command.selectedVirtualPageNeedCount;
		push.virtualGeometryPacketBufferIndex = impl->activeVirtualGeometryPacketBufferIndex;
		push.virtualGeometryBatchBufferIndex = impl->activeVirtualGeometryBatchBufferIndex;
		push.virtualGeometryPagePayloadBufferIndex = impl->activeVirtualGeometryPagePayloadBufferIndex;
		push.virtualGeometryPayloadSource = static_cast<std::uint32_t>(command.virtualGeometryPayloadSource);
		push.virtualGeometryEmittedSource = static_cast<std::uint32_t>(command.virtualGeometryEmittedSource);
		push.virtualGeometryPagePayloadByteOffset = static_cast<std::uint32_t>(command.virtualGeometryPagePayloadByteOffset & 0xffffffffull);
		push.virtualGeometryPagePayloadByteSize = static_cast<std::uint32_t>(command.virtualGeometryPagePayloadByteSize & 0xffffffffull);
		push.virtualGeometryPacketDescriptorIndex = command.virtualGeometryPacketDescriptorIndex;
		push.editorTint = command.editorTint;
		if(groupedSubmission != nullptr) {
			// Every member has the representative's exact tint, validated above, so
			// the shared push constant preserves Scene prefab-context coloring while
			// per-instance selectors remain in the transient stream.
			constexpr std::uint32_t kRenderInstanceGroupedFlag = 0x80000000u;
			constexpr std::uint32_t kRenderInstanceSharedAnimationBufferFlag =
				0x40000000u;
			push.virtualGeometryFlags |= kRenderInstanceGroupedFlag;
			if(useSealedDenseGroupedAnimationSelectors)
			{
				// Every dense member uses the same frame-owned animation bridge buffer.
				// Keep its packet index immutable in the instance record and publish the
				// common buffer selector once through the representative push packet.
				push.virtualGeometryFlags |=
					kRenderInstanceSharedAnimationBufferFlag;
			}
			// Grouped mesh work never consumes the representative model transform;
			// retain tint/VG fields and carry the transient stream locator in GLM
			// column 3. The HLSL ABI reads it as model[0..3].w because HLSL's
			// first matrix index is a row, even for column_major storage.
			push.model[3] = glm::vec4(
				std::bit_cast<float>(impl->groupedInstanceBufferIndex),
				std::bit_cast<float>(groupedSubmission->group.firstInstance),
				std::bit_cast<float>(groupedSubmission->group.instanceCount),
				std::bit_cast<float>(static_cast<std::uint32_t>(
					presentation.groupedInstanceRecords.size())));
		}

		PreparedDraw draw{};
		draw.pipeline = staticPreparation->pipeline;
		draw.pipelineLayout = staticPreparation->pipelineLayout;
		draw.meshletCount = staticPreparation->meshletCount;
		draw.pushConstantRangeCount =
			staticPreparation->pushConstantRangeCount;
		std::copy_n(
			staticPreparation->pushConstantRanges.begin(),
			staticPreparation->pushConstantRangeCount,
			draw.pushConstantRanges.begin());
		draw.meshId = command.meshId;
		draw.materialId = command.materialId;
		// Set 0 is the global bindless heap; set 1 is per-mesh payload for mesh shader pipelines.
		draw.descriptorSets = {impl->globalDescriptorSets[currentFrame], meshDescriptorSet};
		draw.push = push;
		draw.materialFlags = materialFlags;
		draw.generatedVirtualGeometryIndirectIndex = command.virtualGeometryPacketDescriptorIndex;
		draw.rasterRoute = static_cast<
			TerraEngine::Renderer::VirtualGeometryRasterRoute>(
				(command.virtualGeometryFlags
					>> kVirtualGeometryRasterRouteShift)
				& kVirtualGeometryRasterRouteMask);
		// Generated packet commands encode one selected packet's dispatch count.
		// A validated crowd range instead needs the normal frame-slot indirect
		// command with its count multiplied by members. Retaining the
		// representative's device-authored/page-local selectors in push constants
		// gives grouped work the same geometry source without a vendor-specific
		// execution path.
		// Preserve the canonical device-authored decision for ordinary packets.
		// A validated grouped stream instead uses the standard frame-slot task
		// indirect range, whose instance records select each member's exact packet.
		draw.useGeneratedVirtualGeometryIndirect = deviceAuthoredCommand;
		if (groupedSubmission != nullptr)
		{
			draw.useGeneratedVirtualGeometryIndirect = false;
		}

		// RenderSystem has already collapsed compatible contiguous virtual
		// geometry cluster packets into this selected meshlet range. Vulkan
		// submits that prepared packet batch as one indirect task draw instead
		// of exploding back into one command per cluster.
		const uint32_t kTaskBatchSize = 32u;
		uint32_t taskGroupCount = staticPreparation->hasTaskStage
			? ((selectedMeshletCount + (kTaskBatchSize - 1u)) / kTaskBatchSize)
			: selectedMeshletCount;
		if(groupedSubmission != nullptr) {
			taskGroupCount *= groupedSubmission->group.instanceCount;
			for(uint32_t instance = 0u;
				instance < groupedSubmission->group.instanceCount; ++instance) {
				const uint32_t memberCommandIndex = presentation.groupedInstanceRecords[
					groupedSubmission->group.firstInstance + instance]
					.materialAndTexture[RenderInstanceRecord::kPickingIdentityWord];
				if(memberCommandIndex != commandIndex) {
					groupedCommandSuppressed[memberCommandIndex] = 1u;
				}
			}
			recordedGroupedInstanceCount += groupedSubmission->group.instanceCount;
			++recordedGroupedSubmissionCount;
		}

		VkDrawMeshTasksIndirectCommandEXT indirectCmd{};
		indirectCmd.groupCountX = taskGroupCount;
		indirectCmd.groupCountY = 1;
		indirectCmd.groupCountZ = 1;
		draw.indirectBufferIndex = static_cast<uint32_t>(indirectCommands.size());
		indirectCommands.push_back(indirectCmd);

		prepared.push_back(std::move(draw));

		// Per-light shadow lists below own the only shadow draw preparations that
		// can be submitted. The former camera-list copy built another complete draw
		// and indirect command per visible instance solely to count it, doubling
		// preparation for a crowd before discarding those commands.
		if(shadowResourcesReady
			&& ShouldMaterialCastShadow(material->shadowBehavior)
			&& cameraVisibleShadowCasterCount
				< std::numeric_limits<std::uint32_t>::max()) {
			++cameraVisibleShadowCasterCount;
		}

		pendingStats.drawCalls += 1u;
		const uint32_t groupedInstanceCount = groupedSubmission != nullptr
			? groupedSubmission->group.instanceCount : 1u;
		pendingStats.triangleCount += groupedInstanceCount * estimateSelectedPrimitives(
			command,
			staticPreparation->indexCount / 3u,
			ownerMeshletOffset,
			selectedMeshletCount);
		pendingStats.meshletsTotal += groupedInstanceCount * selectedMeshletCount;
		pendingStats.taskCullingActive = pendingStats.taskCullingActive || activeTaskCulling;
		pendingStats.hiZActive = pendingStats.hiZActive || hiZOcclusionActive;
		pendingStats.vrsActive = pendingStats.vrsActive || impl->UsesVrsAttachment();

		// Mesh-only and pass-through fallback paths keep conservative CPU estimates.

		if (!staticPreparation->hasTaskStage || !activeTaskCulling) {
			pendingStats.meshletsDispatched += groupedInstanceCount * selectedMeshletCount;
		}
	}


	for(std::size_t shadowListIndex = 0U;
		shadowListIndex < presentation.shadowDrawLists.size(); ++shadowListIndex) {
		const ShadowDrawList& shadowDrawList =
			presentation.shadowDrawLists[shadowListIndex];
		PreparedShadowList& preparedList =
			preparedShadowLists[shadowListIndex];
		preparedList.lightId = shadowDrawList.lightId;
		preparedList.sourceOrdinalCount = shadowDrawList.stableOrdinals.size();
		preparedList.draws.reserve(shadowDrawList.stableOrdinals.size());

		// The renderer already asked the spatial index for one list per light.
		// Rebuilding the shadow draws from that list preserves the culling win
		// all the way through pipeline selection and indirect submission.
		for(std::size_t itemIndex = 0u;
			itemIndex < shadowDrawList.stableOrdinals.size(); ++itemIndex) {
			RenderItemRecord command{};
			TerraEngine::Renderer::RendererEfficiencyShadowBatchSource source;
			source.sourceOrdinal = shadowDrawList.stableOrdinals[itemIndex];
			preparedList.batchSources.push_back(source);
			auto& batchSource = preparedList.batchSources.back();
			if(!TerraEngine::Renderer::ResolveShadowDrawListItem(
				*publication, presentation.referenceFrame,
				shadowDrawList, itemIndex, command)) {
				preparedList.recordingReady = false;
				preparedList.rejection.Capture("ResolveShadowItem", shadowDrawList.stableOrdinals[itemIndex]);
				continue;
			}
			batchSource.objectId = command.objectId; batchSource.lifetimeGeneration = command.objectLifetimeGeneration;
			batchSource.cameraVisible = cameraVisibleShadowOrdinals.contains(batchSource.sourceOrdinal);
			const Material* material = ResolveMaterial(command.materialId);
			if (material == nullptr || !shadowResourcesReady || !ShouldMaterialCastShadow(material->shadowBehavior)) {
				batchSource.excludedByMaterial = material && !ShouldMaterialCastShadow(material->shadowBehavior);
				batchSource.resolved = batchSource.excludedByMaterial;
				if (material == nullptr || !shadowResourcesReady) {
					preparedList.recordingReady = false;
					preparedList.rejection.Capture(material == nullptr ? "MaterialOwner" : "ShadowResources", shadowDrawList.stableOrdinals[itemIndex], &command);
				}
				continue;
			}

			uint32_t materialFlags = material->header.flags;
			TerraEngine::UUID collectionId = material->header.shaderCollectionId;

			if (collectionId == TerraEngine::EMPTY_UUID) {
				collectionId = defaultMeshCollectionId;
			}

			const std::optional<std::uint32_t> canonicalGraphicsGroup =
				ResolveCanonicalGraphicsGroup(command);
			if (collectionId == defaultMeshCollectionId
				&& defaultTaskPipelineAvailable
				&& (canonicalGraphicsGroup.has_value()
					|| !command.animationBinding.
						RequiresOwnerMeshDeformationFallback())
				&& !(impl->pendingAnimationImmutablePlan != nullptr
					&& impl->pendingAnimationImmutablePlan->WorkPackets().size() > 1u
					&& command.HasCompleteGpuAnimation()
					&& !canonicalGraphicsGroup.has_value())) {
				// Shadow rendering follows the same immutable deformation policy
				// as the visible pass. Canonical page payloads require a task
				// stage; stale legacy owner bounds still remain fail-open.
				collectionId = defaultTaskCollectionId;
			}

			const StaticDrawPreparation* shadowPreparation =
				ResolveStaticDrawPreparation(
					command, collectionId, impl->shadowRenderPass, materialFlags);
			if(shadowPreparation == nullptr) {
				preparedList.recordingReady = false;
				preparedList.rejection.Capture(staticPreparationFailure ? staticPreparationFailure : "StaticPreparation", shadowDrawList.stableOrdinals[itemIndex], &command);
				continue;
			}
			const VkDescriptorSet shadowMeshDescriptorSet =
				shadowPreparation->meshDescriptorSet;

			PreparedDraw shadowDraw{};
			shadowDraw.pipeline = shadowPreparation->pipeline;
			shadowDraw.pipelineLayout = shadowPreparation->pipelineLayout;
			shadowDraw.meshletCount = shadowPreparation->meshletCount;
			shadowDraw.pushConstantRangeCount =
				shadowPreparation->pushConstantRangeCount;
			std::copy_n(
				shadowPreparation->pushConstantRanges.begin(),
				shadowPreparation->pushConstantRangeCount,
				shadowDraw.pushConstantRanges.begin());
			shadowDraw.meshId = command.meshId;
			shadowDraw.materialId = command.materialId;
			shadowDraw.descriptorSets = {impl->globalDescriptorSets[currentFrame], shadowMeshDescriptorSet};
			shadowDraw.push.viewProj = currentViewProjection;
			shadowDraw.push.model = command.modelMatrix;
			shadowDraw.push.materialIndex = 0u;

			if(const auto materialIndex = impl->materialIndexMap.find(command.materialId);
				materialIndex != impl->materialIndexMap.end()) {
				shadowDraw.push.materialIndex = materialIndex->second;
			}
			ApplyAnimationGraphicsBridge(command, shadowDraw.push,
				(command.virtualGeometryFlags
					& kVirtualGeometryFlagOwnerPaletteDeformation) != 0u,
				canonicalGraphicsGroup);
			shadowDraw.push.vrsActive = 2u;
			shadowDraw.push.taskCullingEnabled = 0u;
			shadowDraw.push.statsSlotIndex = statsSlotIndex;
			shadowDraw.push.cullingEpsilon = std::max(presentation.cullingEpsilon, 0.0001f);
			shadowDraw.push.lightDataOffset = renderSnapshot.lightDataOffset;
			shadowDraw.push.clusterDataOffset = renderSnapshot.clusterDataOffset;
			shadowDraw.push.lightIndexOffset = renderSnapshot.lightIndexOffset;
			shadowDraw.push.indirectDrawOffset = renderSnapshot.indirectDrawOffset;
			const uint32_t selectedShadowMeshletOffset = std::min(
				command.virtualMeshletOffset, shadowPreparation->meshletCount);
			const uint32_t selectedShadowMeshletCount = command.virtualMeshletCount == 0u
				? (shadowPreparation->meshletCount - selectedShadowMeshletOffset)
				: std::min(command.virtualMeshletCount,
					shadowPreparation->meshletCount - selectedShadowMeshletOffset);
			batchSource.ownerFirstMeshlet = selectedShadowMeshletOffset;
			batchSource.ownerMeshletCount = selectedShadowMeshletCount;
			batchSource.resolved = selectedShadowMeshletCount != 0;
			shadowDraw.push.virtualMeshletOffset = selectedShadowMeshletOffset;
			shadowDraw.push.virtualMeshletCount = selectedShadowMeshletCount;
			shadowDraw.push.selectedVirtualLod = command.selectedVirtualLod;
			shadowDraw.push.virtualGeometryFlags = command.virtualGeometryFlags;
			if (shadowPreparation->hasTaskStage)
			{
				// Shadow task stages still publish pass-through payload indices
				// while camera-based visibility rejection is intentionally off.
				shadowDraw.push.virtualGeometryFlags |=
					kVirtualGeometryFlagTaskPayloadValid;
			}
			if (RequiresConservativeDeformableVisibility(
				command, canonicalGraphicsGroup))
			{
				// Shadow maps must use the same immutable fail-open decision as the
				// visible pass.  Their task culling is already disabled below because
				// camera Hi-Z cannot prove light-view visibility.
				shadowDraw.push.virtualGeometryFlags |= kVirtualGeometryFlagFailOpen;
				shadowDraw.push.virtualGeometryFlags &= ~kVirtualGeometryFlagConeCullingSafe;
			}
			shadowDraw.push.selectedVirtualClusterOffset = command.selectedVirtualClusterOffset;
			shadowDraw.push.selectedVirtualClusterCount = command.selectedVirtualClusterCount;
			shadowDraw.push.selectedVirtualPageNeedOffset = command.selectedVirtualPageNeedOffset;
			shadowDraw.push.selectedVirtualPageNeedCount = command.selectedVirtualPageNeedCount;
			shadowDraw.push.virtualGeometryPacketBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			shadowDraw.push.virtualGeometryBatchBufferIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			shadowDraw.push.virtualGeometryPacketDescriptorIndex = VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX;
			shadowDraw.push.editorTint = glm::vec4(0.0f);
			shadowDraw.materialFlags = materialFlags;
			// Qualify the exact range that the validated owner-buffer path consumes,
			// rather than a zero meaning 'all meshlets' or a camera packet offset.
			RenderItemRecord semanticCommand = command;
			semanticCommand.virtualMeshletOffset = selectedShadowMeshletOffset;
			semanticCommand.virtualMeshletCount = selectedShadowMeshletCount;
			semanticCommand.virtualGeometryFlags = shadowDraw.push.virtualGeometryFlags;
			const std::uint64_t segmentId = (static_cast<std::uint64_t>(selectedShadowMeshletOffset) << 32U)
				| selectedShadowMeshletCount;
			auto casterInput = TerraEngine::Renderer::BuildShadowCasterStampInput(
				semanticCommand, presentation.referenceFrame, segmentId);
			// This protected preparation already resolved the actual shadow pipeline
			// and owner buffers. Authored/unknown shader proof remains conservative.
			casterInput.semantic.shaderId = shadowPreparation->selectedCollectionId;
			casterInput.semantic.shaderRevision = shadowPreparation->collectionGeneration;
			if (!shadowPreparation->collectionGeneration)
				casterInput.semantic.evidenceMask &= ~static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Shader);
			const auto currentShaderCollection = ResourceManager::Instance().AcquireShaderCollectionSnapshot(shadowPreparation->selectedCollectionId);
			if (!currentShaderCollection || currentShaderCollection->collectionGeneration != shadowPreparation->collectionGeneration)
				casterInput.semantic.evidenceMask &= ~static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Shader);
			// A publication can race the end of immutable capture. The actual GPU
			// preparation must not authorize an older semantic row under new data.
			const auto currentResourceGeneration = ResourceManager::Instance().GetRendererResourcePublicationGeneration();
			if (casterInput.semantic.meshRevision != currentResourceGeneration
				|| casterInput.semantic.materialRevision != currentResourceGeneration)
				casterInput.semantic.evidenceMask &= ~(static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Geometry)
					| static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Material)
					| static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Residency));
			if (shadowPreparation->selectedCollectionId != defaultMeshCollectionId
				&& shadowPreparation->selectedCollectionId != defaultTaskCollectionId)
				casterInput.semantic.evidenceMask &= ~static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Shader);
			if (command.animationBinding.HasCpuDeformation()
				&& (impl->activeCpuAnimationGraphicsBufferIndex == VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX
					|| shadowDraw.push.meshDataIndex != impl->activeCpuAnimationGraphicsBufferIndex
					|| shadowDraw.push.textureIndex != command.animationBinding.cpuDeformationRecordIndex))
			{
				// A rejected upload selects bind pose. Exact intended CPU arrays cannot
				// authorize caching that fallback under a future successful palette.
				casterInput.semantic.evidenceMask &= ~static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Deformation);
			}
			casterInput.semantic.meshRevision = shadowPreparation->meshPublicationGeneration;
			// These owners are the successfully uploaded material and pixels bound
			// by this draw. Current CPU publications alone cannot prove GPU content.
			const auto materialOwner = impl->materialSourceOwners.find(command.materialId);
			const bool actualMaterial = materialOwner != impl->materialSourceOwners.end()
				&& materialOwner->second && materialOwner->second.get() == material
				&& ResourceManager::Instance().AcquireMaterialSnapshot(command.materialId) == materialOwner->second
				&& materialOwner->second->publicationGeneration != 0;
			if (actualMaterial) casterInput.semantic.materialRevision = materialOwner->second->publicationGeneration;
			else casterInput.semantic.evidenceMask &= ~static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Material);
			casterInput.semantic.textureId = TerraEngine::EMPTY_UUID;
			casterInput.semantic.textureRevision = 0;
			bool textureProof = actualMaterial && material->header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Opaque);
			if (actualMaterial && material->header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Mask)) {
				const auto dependencies = impl->materialTextureSourceOwners.find(command.materialId);
				textureProof = dependencies != impl->materialTextureSourceOwners.end();
				if (textureProof) for (std::size_t binding = 0; binding < dependencies->second.size(); ++binding) {
					const auto& dependency = dependencies->second[binding];
					if (dependency.first == TerraEngine::EMPTY_UUID) continue;
					const auto uploaded = impl->textureCache.find(dependency.first);
					if (!dependency.second || !dependency.second->publicationGeneration
						|| uploaded == impl->textureCache.end() || uploaded->second.sourceOwner != dependency.second
						|| ResourceManager::Instance().AcquireTextureSnapshot(dependency.first) != dependency.second) {
						textureProof = false; break;
					}
					casterInput.semantic.texturePublications.push_back({dependency.first, dependency.second->publicationGeneration, static_cast<std::uint32_t>(binding)});
				}
				if (!casterInput.semantic.texturePublications.empty()) {
					casterInput.semantic.textureId = casterInput.semantic.texturePublications.front().textureId;
					casterInput.semantic.textureRevision = casterInput.semantic.texturePublications.front().publicationGeneration;
				}
			}
			if (textureProof) casterInput.semantic.evidenceMask |= static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Texture);
			else casterInput.semantic.evidenceMask &= ~static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Texture);
			casterInput.semantic.assetRevision = shadowPreparation->meshPublicationGeneration;
			casterInput.semantic.residencyGeneration = shadowPreparation->meshAllocationGeneration;
			if (!shadowPreparation->meshOwner || !shadowPreparation->meshPublicationGeneration
				|| !shadowPreparation->meshAllocationGeneration
				|| ResourceManager::Instance().AcquireMeshSnapshot(command.meshId) != shadowPreparation->meshOwner)
				casterInput.semantic.evidenceMask &= ~(static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Geometry)
					| static_cast<std::uint64_t>(TerraEngine::Renderer::ShadowCasterEvidence::Residency));
			if (selectedShadowMeshletCount == 0U) {
				preparedList.recordingReady = false;
				preparedList.rejection.Capture("EmptySelectedMeshletRange", shadowDrawList.stableOrdinals[itemIndex], &command);
			}

			// The complete light-selected owner cut is independent of camera
			// visibility. Only a fully validated/uploaded page set replaces it.
			TerraEngine::Renderer::Vulkan::ShadowPageCutRequest pageRequest;
			pageRequest.meshId = command.meshId; pageRequest.mesh = shadowPreparation->meshOwner;
			pageRequest.cpuPublicationGeneration = shadowPreparation->meshPublicationGeneration;
			pageRequest.ownerMeshletOffset = selectedShadowMeshletOffset;
			pageRequest.ownerMeshletCount = selectedShadowMeshletCount;
			pageRequest.selectedLod = command.selectedVirtualLod;
			pageRequest.deformationUnsupported = command.animationBinding.HasCpuDeformation() || command.HasCompleteGpuAnimation();
			// Page-local geometry needs initialized task payload indices; a mesh-only
			// fallback must retain the complete owner-buffer path instead.
			pageRequest.materialUnsupported = !shadowPreparation->hasTaskStage
				|| (shadowPreparation->selectedCollectionId != defaultTaskCollectionId
					&& shadowPreparation->selectedCollectionId != defaultMeshCollectionId);
			const auto pageSet = impl->EnsureShadowAuthoredPagePayload(pageRequest, false);
			const auto emit = [&](PreparedDraw draw, TerraEngine::Renderer::ShadowCasterStampInput input, std::uint32_t count) {
				// Static admission follows accepted native owners, never an authored
				// flag or empty animation palette alone. Unknown proof stays dynamic.
				auto& support = input.semantic.staticSupport;
				support.knownDefaultShadowAbi = support.knownDefaultShadowAbi
					&& (shadowPreparation->selectedCollectionId == defaultTaskCollectionId
						|| shadowPreparation->selectedCollectionId == defaultMeshCollectionId);
				support.ownerPublicationsComplete = input.semantic.evidenceMask
					== TerraEngine::Renderer::kCompleteShadowCasterEvidence;
				support.materialShadowSupported = actualMaterial && textureProof
					&& (material->header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Opaque)
						|| material->header.alphaMode == static_cast<std::uint32_t>(AlphaMode::Mask));
				support.geometrySupported = shadowPreparation->meshOwner
					&& shadowPreparation->meshAllocationGeneration != 0 && count != 0
					&& command.geometryRoute != TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute;
				support.deformationAbsent = support.deformationAbsent
					&& !command.animationBinding.HasCpuDeformation() && !command.HasCompleteGpuAnimation();
				const auto classification = TerraEngine::Renderer::ClassifyStaticShadowCaster(input.semantic);
				input.semantic.staticEligible = classification.eligible;
				input.semantic.staticClassificationReason = static_cast<std::uint32_t>(classification.reason);
				// Copy accepted native proof; worker planning below has no cache,
				// Vulkan or scene lookups. Preserve each page segment as its own member.
				TerraEngine::Renderer::ShadowBatchMember member;
				const auto& stamp = input.semantic;
				member.sourceOrdinal = shadowDrawList.stableOrdinals[itemIndex];
				member.objectId = stamp.objectId; member.lifetimeGeneration = stamp.lifetimeGeneration;
				// Compatibility uses the native payload-relative range. The independent
				// source/stamp receipt retains its full owner-space coverage, including
				// page cuts whose native meshlet stream starts at zero.
				member.segmentId = stamp.segmentId; member.firstMeshlet = draw.push.virtualMeshletOffset;
				member.meshletCount = stamp.meshletCount; member.firstCluster = stamp.firstCluster;
				member.clusterCount = stamp.clusterCount; member.pageId = stamp.pageId;
				member.payloadOffset = stamp.payloadByteOffset; member.payloadSize = stamp.payloadByteSize;
				auto& key = member.key; key.meshId = stamp.meshId; key.materialId = stamp.materialId;
				key.shaderId = stamp.shaderId;
				key.pipeline = reinterpret_cast<std::uintptr_t>(draw.pipeline);
				key.layout = reinterpret_cast<std::uintptr_t>(draw.pipelineLayout);
				key.pipelineGeneration = key.layoutGeneration = shadowPreparation->collectionGeneration;
				key.descriptors = {reinterpret_cast<std::uintptr_t>(draw.descriptorSets[0]),
					reinterpret_cast<std::uintptr_t>(draw.descriptorSets[1])};
				key.descriptorGenerations = {impl->backendResourceGeneration, shadowPreparation->meshAllocationGeneration};
				key.meshGeneration = stamp.meshRevision; key.materialGeneration = stamp.materialRevision;
				key.shaderGeneration = stamp.shaderRevision; key.residencyGeneration = stamp.residencyGeneration;
				key.materialSelector = draw.push.materialIndex; key.textureSelector = draw.push.textureIndex;
				key.materialFlags = draw.materialFlags; key.shadowPolicy = static_cast<std::uint32_t>(stamp.shadowBehavior);
				key.geometryRoute = stamp.geometryRoute; key.deformationPolicy = static_cast<std::uint32_t>(command.animationBinding.source);
				key.selectionFlags = draw.push.virtualGeometryFlags; key.selectedLod = draw.push.selectedVirtualLod;
				key.dispatchMeshletCount = count; key.pushRangeCount = draw.pushConstantRangeCount;
				if (key.pushRangeCount <= key.pushRanges.size()) for (std::size_t range = 0; range < key.pushRangeCount; ++range)
					key.pushRanges[range] = {draw.pushConstantRanges[range].stageFlags,
						draw.pushConstantRanges[range].offset, draw.pushConstantRanges[range].size};
				member.knownDefaultTaskAbi = shadowPreparation->hasTaskStage
					&& shadowPreparation->selectedCollectionId == defaultTaskCollectionId;
				member.supportedRoute = command.geometryRoute != TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute;
				member.conservativeDeformation = command.animationBinding.RequiresConservativeCoarseVisibility()
					|| command.animationBinding.HasCpuDeformation();
				member.deformationBridgeComplete = !command.HasCompleteGpuAnimation()
					|| (canonicalGraphicsGroup.has_value() && draw.push.meshDataIndex != VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX
						&& draw.push.textureIndex != VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX);
				const bool sourceCurrent = TerraEngine::Renderer::ShadowPreparedInstanceInputsCurrent(*publication, presentation, shadowDrawList);
				member.knownDefaultTaskAbi = member.knownDefaultTaskAbi && sourceCurrent;
				RenderInstanceRecord record;
				if (sourceCurrent) record = shadowDrawList.instanceInputs->records[itemIndex];
				// The protected lowering patches every actually consumed selector after
				// accepted native resource/animation/page preparation, preserving identity.
				record.modelMatrix = draw.push.model; record.editorTint = draw.push.editorTint;
				record.materialAndTexture.x = draw.push.materialIndex; record.materialAndTexture.y = draw.push.textureIndex;
				record.animationSelectors.x = draw.push.meshDataIndex; record.animationSelectors.y = draw.push.textureIndex;
				record.objectIdentity.z = draw.push.virtualGeometryFlags;
				record.geometrySelectors = glm::uvec4(draw.push.selectedVirtualLod, draw.push.virtualGeometryPacketDescriptorIndex,
					draw.push.virtualMeshletOffset, draw.push.virtualMeshletCount);
				record.virtualGeometrySelectors = glm::uvec4(draw.push.selectedVirtualClusterOffset, draw.push.selectedVirtualClusterCount,
					draw.push.selectedVirtualPageNeedOffset, draw.push.selectedVirtualPageNeedCount);
				record.virtualGeometryPayloadSelectors = glm::uvec4(draw.push.virtualGeometryPayloadSource,
					draw.push.virtualGeometryEmittedSource, draw.push.virtualGeometryPagePayloadByteOffset,
					draw.push.virtualGeometryPagePayloadByteSize);
				preparedList.batchMembers.push_back(member); preparedList.expectedInstances.push_back(record);
				draw.shadowMemberIndex = static_cast<std::uint32_t>(preparedList.draws.size());
				VkDrawMeshTasksIndirectCommandEXT indirect{};
				indirect.groupCountX = shadowPreparation->hasTaskStage ? (count + 31U) / 32U : count;
				indirect.groupCountY = 1U; indirect.groupCountZ = 1U;
				draw.indirectBufferIndex = static_cast<std::uint32_t>(indirectCommands.size());
				indirectCommands.push_back(indirect);
				preparedList.casterInputs.push_back(std::move(input));
				preparedList.draws.push_back(std::move(draw));
			};
			// Validate the entire native page cohort before suppressing any owner
			// meshlet. One missing byte span or stale binding keeps whole coverage.
			std::vector<TerraEngine::Renderer::ShadowCasterStamp> pageStamps;
			bool pageBindingsReady = pageSet.complete && !pageSet.bindings.empty();
			if (pageBindingsReady) {
				pageStamps.reserve(pageSet.bindings.size());
				for (const auto& binding : pageSet.bindings) {
					auto stamp = casterInput.semantic;
					if (!TerraEngine::Renderer::Vulkan::ApplyShadowAuthoredPageBinding(binding, stamp)) {
						pageBindingsReady = false;
						break;
					}
					pageStamps.push_back(std::move(stamp));
				}
			}
			if (pageBindingsReady) {
				for (std::size_t bindingIndex = 0U; bindingIndex < pageSet.bindings.size(); ++bindingIndex) {
					const auto& binding = pageSet.bindings[bindingIndex];
					auto pageDraw = shadowDraw; auto pageInput = casterInput;
					const auto& segment = binding.segment;
					// Descriptor stream meshlets are page-local. Original owner ranges
					// remain in semantic identity, rather than transient GPU offsets.
					pageDraw.push.virtualMeshletOffset = 0;
					pageDraw.push.virtualMeshletCount = segment.ownerMeshletCount;
					pageDraw.push.virtualGeometryPagePayloadBufferIndex = binding.payloadBufferIndex;
					pageDraw.push.virtualGeometryPacketDescriptorIndex = binding.descriptorIndex;
					pageDraw.push.virtualGeometryPayloadSource = static_cast<std::uint32_t>(VirtualGeometryPagePayloadSource::ResidentPagePayload);
					pageDraw.push.virtualGeometryEmittedSource = static_cast<std::uint32_t>(VirtualGeometryEmittedPageSource::HighDetailPage);
					pageInput.semantic = std::move(pageStamps[bindingIndex]);
					emit(std::move(pageDraw), std::move(pageInput), segment.ownerMeshletCount);
				}
			} else {
				// Unknown page topology/deformation/upload keeps every owner meshlet.
				// No partially prepared page list can suppress complete geometry.
				emit(std::move(shadowDraw), std::move(casterInput), selectedShadowMeshletCount);
			}
		}

	}
	// Exact row normalization is worker-safe: inputs own copied canonical values
	// and immutable resource proof, with no Vulkan or live scene lookups in jobs.
	std::vector<std::future<void>> shadowMembershipFutures;
	std::uint32_t shadowPreparationWorkerTasks = 0, shadowPreparationInlineTasks = 0;
	bool shadowPreparationSettled = false;
	shadowMembershipFutures.reserve(preparedShadowLists.size());
	TerraEngine::Tasks::ScopedTaskSettlement<void> shadowMembershipSettlement(shadowMembershipFutures);
	const bool parallelShadowProof = presentation.efficiencyOptions.options.validationPreparation
		== TerraEngine::Renderer::RendererEfficiencyPreparation::Concurrent && preparedShadowLists.size() > 1U;
	try
	{
		for (auto& list : preparedShadowLists)
		{
			const auto normalize = [&list]() { list.membership = TerraEngine::Renderer::NormalizeShadowCasterMembership(list.casterInputs); };
			if (parallelShadowProof) {
				shadowMembershipSettlement.CreateAndEnqueue(ThreadPool::instance(), normalize);
				++shadowPreparationWorkerTasks;
			} else {
				shadowMembershipSettlement.RunInline(normalize);
				++shadowPreparationInlineTasks;
			}
		}
		shadowMembershipSettlement.GetAll();
		shadowPreparationSettled = true;
	}
	catch (...)
	{
		shadowMembershipSettlement.Wait();
		for (auto& list : preparedShadowLists) { list.membership = {}; list.recordingReady = false; list.rejection.Capture("MembershipSettlementException", UINT32_MAX); }
	}

	pendingBudgetDiagnostics.cameraVisibleShadowCasterCount =
		cameraVisibleShadowCasterCount;
	pendingBudgetDiagnostics.lightVisibleShadowCasterCount = 0u;

	for (const PreparedShadowList& preparedList : preparedShadowLists) {
		pendingBudgetDiagnostics.lightVisibleShadowCasterCount += static_cast<std::uint32_t>(preparedList.draws.size());
	}

	if(pendingStats.optimization.valid != 0u) {
		pendingStats.optimization.groupedCommandRecordingNanoseconds =
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - groupedRecordingBegin).count());
		pendingStats.optimization.groupedInstanceCount = recordedGroupedInstanceCount;
		pendingStats.optimization.groupedSubmissionCount = recordedGroupedSubmissionCount;
		pendingStats.optimization.fallbackInstanceCount =
			pendingStats.optimization.visibleInstanceCount - recordedGroupedInstanceCount;
		pendingStats.optimization.fallbackSubmissionCount =
			pendingStats.optimization.fallbackInstanceCount;
		pendingStats.optimization.avoidedSubmissionCount = recordedGroupedInstanceCount
			- recordedGroupedSubmissionCount;
		if(recordedGroupedSubmissionCount == 0u && !presentation.groupedSubmissions.empty()
			&& groupedInstanceStreamReady) {
			pendingStats.optimization.rejectedUnsupportedVariantCount =
				static_cast<std::uint32_t>(std::min<std::size_t>(
					presentation.groupedSubmissions.size(),
					std::numeric_limits<std::uint32_t>::max()));
		}
	}
	// Capture-only automation has no visible Game View to populate another slot.
	// Publish its counters with explicit output provenance so diagnostics remain
	// available without pretending the offscreen target was an editor viewport.
	impl->RecordPendingStats(statsSlotIndex, pendingStats);

	const auto HashCombine = [](std::uint64_t& seed, std::uint64_t value) {
		seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
	};
	const auto HashBytes = [&](std::uint64_t& seed, const void* data,
		std::size_t byteCount) {
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		for(std::size_t byteIndex = 0u; byteIndex < byteCount; ++byteIndex) {
			HashCombine(seed, bytes[byteIndex]);
		}
	};
	const auto HashFloat = [&](std::uint64_t& seed, float value) {
		std::uint32_t bits = 0u;
		std::memcpy(&bits, &value, sizeof(bits));
		HashCombine(seed, bits);
	};
	const auto HashVec4 = [&](std::uint64_t& seed, const glm::vec4& value) {
		HashFloat(seed, value.x);
		HashFloat(seed, value.y);
		HashFloat(seed, value.z);
		HashFloat(seed, value.w);
	};
	const auto HashMat4 = [&](std::uint64_t& seed, const glm::mat4& value) {

		for (int column = 0; column < 4; ++column) {
			HashVec4(seed, value[column]);
		}
	};

	// A previous primary may have rendered only a private capture target. Order
	// earlier color writes without claiming that the swapchain is already in a
	// presentation layout; this pass's render pass owns its discard/transition.
	if (!useMainCommandBuffer && !impl->editorMode && !renderingDedicatedCaptureTarget) {
		VkMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0,
			1, &barrier,
			0, nullptr,
			0, nullptr
		);
	}


	struct ShadowLightRecord
	{
		const GPULight* light = nullptr;
		TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
		const std::vector<PreparedDraw>* preparedDraws = nullptr;
		uint32_t type = 0u;
		uint32_t slot = VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS;
		std::uint32_t atlasId = 0u;
		std::uint32_t regionX = 0u;
		std::uint32_t regionY = 0u;
		std::uint32_t regionWidth = VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
		std::uint32_t regionHeight = VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
		PreparedShadowList* preparedList = nullptr;
		TerraEngine::Renderer::Vulkan::ShadowGroupedRecordedCounts recordedBatchCounts;
		TerraEngine::Renderer::RendererEfficiencyShadowBatchExecution recordedBatchExecution;
		TerraEngine::Renderer::ShadowViewKey qualifiedKey{};
		std::uint64_t contentToken = 0U, physicalRegionKey = 0U;
		VulkanContext::Impl::StaticShadowEntry* staticEntry = nullptr;
		std::vector<PreparedDraw> staticDraws, dynamicDraws;
		std::string staticFallback;
		double staticPriority = 0;
		bool staticReused = false, staticComposition = false;
		std::uint8_t staticRecordedFaceMask = 0, copiedFaceMask = 0;
		std::uint64_t staticSourceCommand = 0, staticSourceSubmit = 0;
		std::uint32_t staticRecordedDraws = 0, dynamicRecordedDraws = 0;
		std::array<std::uint32_t, 6> staticFaceDraws{}, dynamicFaceDraws{};
		std::array<TerraEngine::Renderer::RendererEfficiencyShadowBatchExecution, 6> staticFaceExecutions;
		std::uint8_t recordedFaceMask = 0U;
		bool reusedCachedResult = false;
		bool invalidatedThisFrame = false;
		bool validationIncompletePointFaces = false;
		std::uint64_t validationPriorContentToken = 0;
		TerraEngine::Renderer::ShadowViewKey validationPriorPhysicalKey{};
	};
	std::vector<ShadowLightRecord> shadowLightRecords;
	shadowLightRecords.reserve(VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS);
	auto FindShadowSlotFromTextureIndex = [](const std::array<uint32_t, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS>& textureIndices, uint32_t textureIndex) -> uint32_t {

		for (uint32_t slotIndex = 0u; slotIndex < VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS; ++slotIndex) {

			if (textureIndices[slotIndex] == textureIndex) {
				return slotIndex;
			}
		}
		return VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS;
	};

	for (std::size_t payloadIndex = 0u; payloadIndex < lightingPayload.size(); ++payloadIndex) {
		const GPULight& light = lightingPayload[payloadIndex];
		if (light.shadowAndPadding.x <= 0.5f) {
			continue;
		}
		const FrameLightCandidate& retainedCandidate = budgetResult.retainedLights[payloadIndex];
		const uint32_t lightType = static_cast<uint32_t>(std::max(0.0f, light.positionAndType.w));
		const uint32_t shadowPayloadIndex = static_cast<uint32_t>(std::max(0.0f, light.shadowAndPadding.y));
		uint32_t slotIndex = VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS;

		if (lightType == 0u) {
			slotIndex = FindShadowSlotFromTextureIndex(impl->directionalShadowTextureIndices, shadowPayloadIndex);
		}
		else if (lightType == 1u) {
			slotIndex = std::min(shadowPayloadIndex, VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS - 1u);
		}
		else if (lightType == 2u) {
			slotIndex = FindShadowSlotFromTextureIndex(impl->spotShadowTextureIndices, shadowPayloadIndex);
		}
		else if (lightType == 3u) {
			slotIndex = FindShadowSlotFromTextureIndex(impl->areaShadowTextureIndices, shadowPayloadIndex);
		}

		if (slotIndex < VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS) {
			ShadowLightRecord record{};
			record.light = &light;
			record.lightId = retainedCandidate.lightId;
			record.staticPriority = retainedCandidate.gpuLight.extraParams.w;
			record.type = lightType;
			record.slot = slotIndex;

			for (PreparedShadowList& preparedList : preparedShadowLists) {

				if (preparedList.lightId == retainedCandidate.lightId) {
					record.preparedDraws = &preparedList.draws;
					record.preparedList = &preparedList;
					break;
				}
			}

			if (record.preparedDraws == nullptr) {
				static const std::vector<PreparedDraw> kEmptyPreparedShadowDraws{};
				record.preparedDraws = &kEmptyPreparedShadowDraws;
			}

			if (const ShadowResourceAssignmentDiagnostic* assignment = FindShadowAssignment(retainedCandidate.lightId)) {
				record.atlasId = assignment->atlasId;
				record.regionX = assignment->regionX;
				record.regionY = assignment->regionY;
				record.regionWidth = std::max(assignment->regionWidth, 1u);
				record.regionHeight = std::max(assignment->regionHeight, 1u);
			}
			shadowLightRecords.push_back(record);
		}
	}

	pendingBudgetDiagnostics.cacheReuseCount = 0u;
	pendingBudgetDiagnostics.cacheRefreshCount = 0u;
	pendingBudgetDiagnostics.cacheInvalidationCount = 0u;
	const auto ShadowKindFromRecord = [](std::uint32_t lightType) {
		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (lightType)
		{
		case 0u: return ShadowResourceKind::DirectionalCascade;
		case 1u: return ShadowResourceKind::PointCube;
		case 2u: return ShadowResourceKind::SpotAtlas;
		case 3u: return ShadowResourceKind::AreaAtlas;
		default: return ShadowResourceKind::Unsupported;
		}
	};
	const auto exactMatrix = [](const glm::mat4& source) {
		std::array<double, 16> result{};
		for (std::size_t column = 0; column < 4; ++column)
			for (std::size_t row = 0; row < 4; ++row) result[column * 4 + row] = source[column][row];
		return result;
	};
	std::string incompletePointFacesKey;
	bool selectIncompletePointFaces = false;
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
	incompletePointFacesKey = presentation.efficiencyFixtureIntent.correlation.operationId + ":" + presentation.efficiencyFixtureIntent.correlation.fixtureStepId;
	selectIncompletePointFaces = !presentation.efficiencyFixtureIntent.correlation.operationId.empty()
		&& presentation.efficiencyFixtureIntent.injection == TerraEngine::Renderer::RendererEfficiencyFixtureInjection::IncompletePointFaces
		&& incompletePointFacesKey != impl->lastShadowValidationIncompletePointFacesKey;
#endif
	for (ShadowLightRecord& record : shadowLightRecords)
	{
		// Full qualified inputs include actual allocation lifetime, canonical view
		// origin and every point face. Per-frame/upload bookkeeping is excluded.
		auto& key = record.qualifiedKey;
		key.rendererSessionGeneration = impl->renderSessionGeneration;
		key.viewType = renderViewType; key.viewId = diagnosticViewId;
		const auto& origin = presentation.referenceFrame.canonicalOrigin;
		key.referenceOrigin = {{origin.cell.x, origin.cell.y, origin.cell.z},
			{origin.offsetMetres.x, origin.offsetMetres.y, origin.offsetMetres.z}};
		key.lightId = record.lightId; key.resourceKind = ShadowKindFromRecord(record.type);
		key.allocationGeneration = impl->shadowAllocationGeneration; key.regionId = record.slot;
		key.extent = record.type == 1U
			? std::array<std::uint32_t, 2>{VulkanContext::Impl::POINT_SHADOW_MAP_RESOLUTION, VulkanContext::Impl::POINT_SHADOW_MAP_RESOLUTION}
			: std::array<std::uint32_t, 2>{record.regionWidth, record.regionHeight};
		key.depthFormat = static_cast<std::uint32_t>(impl->depthFormat != VK_FORMAT_UNDEFINED ? impl->depthFormat : impl->FindDepthFormat());
		key.lightProjectionStamp.viewProjection = exactMatrix(record.type == 1U
			? BuildPointShadowViewProjection(*record.light, 0U) : LoadShadowViewProjection(*record.light));
		key.lightProjectionStamp.lightParameters = {record.light->positionAndType.w, record.light->directionAndRange.w,
			record.light->coneAnglesAndArea.x, record.light->coneAnglesAndArea.y};
		key.lightProjectionStamp.shadowFilterParameters = {record.light->coneAnglesAndArea.z, record.light->coneAnglesAndArea.w,
			record.light->extraParams.x, record.light->extraParams.y};
		if (record.type == 1U)
		{
			for (std::uint32_t face = 0U; face < 6U; ++face)
				key.lightProjectionStamp.cubeViewProjections[face] = exactMatrix(BuildPointShadowViewProjection(*record.light, face));
			key.lightProjectionStamp.cubeProjectionEvidenceComplete = true;
		}
		record.physicalRegionKey = (static_cast<std::uint64_t>(record.type) << 32U) | record.slot;
		const auto previous = impl->shadowPublicationLedger.Find(key);
		if (record.type == 1U && selectIncompletePointFaces) {
			record.validationIncompletePointFaces = true; selectIncompletePointFaces = false;
		}
		if (record.validationIncompletePointFaces) {
			const auto physicalOwner = impl->shadowPublicationLedger.FindSubmittedPhysicalOwner(key);
			if (physicalOwner && impl->shadowPublicationLedger.CanReuse(physicalOwner->view, physicalOwner->contentToken)) {
				record.validationPriorContentToken = physicalOwner->contentToken;
				record.validationPriorPhysicalKey = physicalOwner->view;
			}
		}
		record.invalidatedThisFrame = previous.has_value();
		const bool reuseRequested = presentation.efficiencyOptions.options.shadowReuse
			== TerraEngine::Renderer::RendererEfficiencyShadowReuse::Enabled;
		if (reuseRequested && !record.validationIncompletePointFaces && record.preparedList != nullptr && record.preparedList->recordingReady
			&& record.preparedList->membership.reusable)
		{
			// The physical token is only a hint. Alias promotion independently checks
			// all exact inputs, current submitted ownership and pending overwrites.
			const auto hint = impl->shadowPhysicalTokenHints.find(record.physicalRegionKey);
			if (hint != impl->shadowPhysicalTokenHints.end())
			{
				record.contentToken = hint->second;
				try
				{
					record.reusedCachedResult = impl->shadowPublicationLedger.AliasSubmittedContent(
						key, record.contentToken, record.preparedList->membership)
						&& impl->shadowPublicationLedger.CanReuse(key, record.contentToken);
				}
				catch (...) { impl->shadowPublicationLedger.Reset(impl->renderSessionGeneration); record.reusedCachedResult = false; }
			}
		}
		if (record.reusedCachedResult) ++pendingBudgetDiagnostics.cacheReuseCount;
		else if (record.invalidatedThisFrame) ++pendingBudgetDiagnostics.cacheInvalidationCount;
		if(!record.reusedCachedResult) record.contentToken = impl->nextShadowContentToken++;
		// Recording below supplies actual refresh counts and completeness. No
		// valid flag is published merely because preparation selected a refresh.
	}

	// Partition jobs consume sealed native proofs and original singleton indices.
	// Unknown dynamic semantic inputs retain drawable coverage, independently of
	// fully proven static content. No job allocates or touches live scene storage.
	std::vector<std::future<void>> staticPartitionFutures;
	TerraEngine::Tasks::ScopedTaskSettlement<void> staticPartitionSettlement(staticPartitionFutures);
	staticPartitionFutures.reserve(shadowLightRecords.size());
	try {
		for (auto& record : shadowLightRecords) {
			const auto partition = [&record]() {
				if (record.preparedList) record.preparedList->staticPartition =
					TerraEngine::Renderer::BuildStaticShadowPartition(record.qualifiedKey, record.preparedList->casterInputs);
			};
			if (parallelShadowProof) { staticPartitionSettlement.CreateAndEnqueue(ThreadPool::instance(), partition); ++shadowPreparationWorkerTasks; }
			else { staticPartitionSettlement.RunInline(partition); ++shadowPreparationInlineTasks; }
		}
		staticPartitionSettlement.GetAll();
	} catch (...) {
		staticPartitionSettlement.Wait();
		for (auto& record : shadowLightRecords) if (record.preparedList) record.preparedList->staticPartition = {};
	}
	// Physical charges include all views and retirements. Preserve another view's
	// qualified owners; only obsolete keys for this current view are replaced.
	impl->staticShadowRequestedBudget = presentation.efficiencyOptions.options.staticShadowCacheBudgetBytes;
	impl->ReleaseCompletedStaticShadowEntries();
	std::vector<TerraEngine::Renderer::StaticShadowAdmissionRequest> staticRequests;
	std::vector<TerraEngine::Renderer::StaticShadowAllocationCharge> staticCharges;
	std::vector<ShadowLightRecord*> staticRequestRecords;
	try {
		// The owned failure seam exercises a fresh allocator admission. Retire
		// this view's inherited cache logically first, while exact native users
		// keep its physical storage charged until their real fence/discard proof.
		if (presentation.efficiencyFixtureIntent.staticShadowControls
			&& presentation.efficiencyFixtureIntent.staticShadowControls->failAllocation)
			for (auto& entry : impl->staticShadowEntries)
				if (!entry->retiring && entry->finalView.viewType == renderViewType
					&& entry->finalView.viewId == diagnosticViewId) {
					entry->retiring = true;
					impl->staticShadowPublicationLedger.InvalidatePhysicalRegion(entry->staticView);
					impl->staticShadowLifetime.RequestRetirement(entry->owner);
				}
		for (auto& record : shadowLightRecords) {
			if (!record.preparedList || !record.preparedList->recordingReady) { record.staticFallback = "IncompleteCasterPreparation"; continue; }
			auto& partition = record.preparedList->staticPartition;
			if (!partition.valid) { record.staticFallback = "InvalidStaticPartition"; continue; }
			// Retain one complete all-caster vector for final validity/diagnostics;
			// partition recording needs only its separate category rows and indices.
			record.preparedList->membership = std::move(partition.allMembership);
			if (partition.staticSourceIndices.empty()) { record.staticFallback = "EmptyStaticMembership"; continue; }
			if (!impl->staticShadowRequestedBudget) { record.staticFallback = "StaticBudgetDisabled"; continue; }
			if (record.validationIncompletePointFaces) { record.staticFallback = "IncompletePointValidation"; continue; }
			if (presentation.efficiencyFixtureIntent.staticShadowControls
				&& presentation.efficiencyFixtureIntent.staticShadowControls->forceTransferUnavailable) {
				record.staticFallback = "OwnedTransferUnavailable"; continue;
			}
			TerraEngine::Renderer::StaticShadowAdmissionRequest request;
			if (!impl->QueryStaticShadowRequirements(record.qualifiedKey, request, record.staticFallback)) continue;
			request.retainedLightPriority = record.staticPriority;
			staticRequests.push_back(request); staticRequestRecords.push_back(&record);
		}
		for (const auto& entry : impl->staticShadowEntries) {
			staticCharges.push_back({entry->finalView, entry->owner.allocationGeneration, entry->owner.actualBytes, 0,
				entry->priority, entry->retiring ? TerraEngine::Renderer::StaticShadowChargeState::Retiring : TerraEngine::Renderer::StaticShadowChargeState::Resident});
			if (!entry->retiring && entry->finalView.rendererSessionGeneration == impl->renderSessionGeneration
				&& entry->borrowedColorGeneration == impl->shadowAllocationGeneration
				&& (entry->finalView.viewType != renderViewType || entry->finalView.viewId != diagnosticViewId)) {
				staticRequests.push_back({entry->finalView, entry->priority, entry->owner.actualBytes, 0,
					static_cast<std::uint8_t>(entry->owner.layers == 6 ? 0x3f : 1)});
				staticRequestRecords.push_back(nullptr);
			}
		}
		const auto admission = TerraEngine::Renderer::BuildStaticShadowAdmissionPlan(impl->staticShadowRequestedBudget,
			impl->staticShadowEffectiveBudget, staticRequests, staticCharges);
		if (!admission.valid) { impl->RetireStaticShadowEntries(); }
		else {
			impl->staticShadowEffectiveBudget = admission.effectiveBytes; impl->staticShadowBudgetDraining = admission.draining;
			for (auto index : admission.retireChargeIndices) {
				auto& entry = impl->staticShadowEntries[index]; entry->retiring = true;
				impl->staticShadowPublicationLedger.InvalidatePhysicalRegion(entry->staticView);
				impl->staticShadowLifetime.RequestRetirement(entry->owner);
			}
			// Keep stable pointers until all admission indices have been consumed.
			for (auto index : admission.retainedChargeIndices) {
				auto& entry = impl->staticShadowEntries[index];
				for (auto& record : shadowLightRecords) if (record.qualifiedKey == entry->finalView) record.staticEntry = entry.get();
			}
			for (auto index : admission.admittedRequestIndices) {
				auto* record = staticRequestRecords[index]; if (!record) continue;
				if (presentation.efficiencyFixtureIntent.staticShadowControls
					&& presentation.efficiencyFixtureIntent.staticShadowControls->failAllocation) {
					record->staticFallback = "OwnedAllocationFailure"; continue;
				}
				std::array<VkImageView, 6> colors{};
				if (record->type == 1) colors = impl->pointShadowColorFaceViewsBySlot[record->slot];
				else colors[0] = record->type == 0 ? impl->directionalShadowColors[record->slot].view
					: record->type == 2 ? impl->spotShadowColors[record->slot].view : impl->areaShadowColors[record->slot].view;
				impl->CreateStaticShadowEntry(staticRequests[index], std::span(colors.data(), record->type == 1 ? 6 : 1),
					record->staticPriority, record->staticEntry, record->staticFallback);
			}
		}
		for (auto& record : shadowLightRecords) {
			if (!record.staticEntry || record.reusedCachedResult) continue;
			auto& partition = record.preparedList->staticPartition;
			for (auto index : partition.staticSourceIndices) record.staticDraws.push_back(record.preparedList->draws[index]);
			for (auto index : partition.dynamicSourceIndices) record.dynamicDraws.push_back(record.preparedList->draws[index]);
			auto& entry = *record.staticEntry;
			if (!entry.layoutEstablished && entry.initializationCommand != 0
				&& entry.initializationCommand != shadowCommandSerial) {
				record.staticEntry = nullptr; record.staticFallback = "StaticInitializationPendingSubmit"; continue;
			}
			record.staticReused = entry.contentToken && impl->staticShadowPublicationLedger.AliasSubmittedContent(
				entry.staticView, entry.contentToken, partition.staticMembership)
				&& impl->staticShadowPublicationLedger.CanReuse(entry.staticView, entry.contentToken);
			if (record.staticReused) {
				const auto source = impl->staticShadowPublicationLedger.Find(entry.staticView);
				if (source) { record.staticSourceCommand = source->commandSerial; record.staticSourceSubmit = source->submitSerial; }
			}
			if (!impl->staticShadowLifetime.ReserveRecordedUse(entry.owner, shadowCommandSerial, currentFrame)) {
				record.staticEntry = nullptr; record.staticFallback = "RecordedLifetimeReservationFailed";
			}
		}
	} catch (...) {
		// Complete combined singleton/grouped lists remain available if admission
		// or copied policy allocation fails. No partial partition is recorded.
		for (auto& record : shadowLightRecords) { record.staticEntry = nullptr; record.staticFallback = "StaticAdmissionException"; }
		impl->staticShadowPublicationLedger.Reset(impl->renderSessionGeneration);
	}
	impl->ReleaseCompletedStaticShadowEntries();
	// Native compatibility is now copied and qualified by actual light/region.
	// Pure planning runs in jobs; the protected boundary alone allocates, writes
	// and publishes the physical frame/snapshot stream after the existing fence.
	std::vector<RenderInstanceRecord> shadowStreamRecords;
	std::vector<std::future<void>> shadowBatchFutures;
	TerraEngine::Tasks::ScopedTaskSettlement<void> shadowBatchSettlement(shadowBatchFutures);
	bool shadowStreamPrepared = impl->lightingUploadFrameSlotSafe[currentFrame];
	std::uint64_t shadowStreamRecordCount = 0;
	const auto& batchControls = presentation.efficiencyFixtureIntent.shadowBatchControls;
	try
	{
		for (auto& record : shadowLightRecords)
		{
			auto* list = record.preparedList;
			if (!list) continue;
			TerraEngine::Renderer::ShadowBatchLimits limits;
			limits.firstInstance = static_cast<std::uint32_t>(shadowStreamRecordCount);
			limits.instanceCapacity = static_cast<std::uint32_t>(list->batchMembers.size());
			limits.maxInstancesPerGroup = 65536;
			limits.maxGroups = limits.instanceCapacity;
			limits.maxTaskGroupsX = impl->maximumTaskGroupsX;
			limits.maxTaskGroupsTotal = impl->maximumTaskGroupsTotal;
			if (presentation.efficiencyFixtureIntent.controlsApplied && batchControls)
			{
				if (batchControls->maxInstancesPerGroup) limits.maxInstancesPerGroup = std::min(limits.maxInstancesPerGroup, batchControls->maxInstancesPerGroup);
				if (batchControls->maxTaskGroupsX) limits.maxTaskGroupsX = std::min(limits.maxTaskGroupsX, batchControls->maxTaskGroupsX);
			}
			shadowStreamRecordCount += list->batchMembers.size();
			const auto maximumRecords = std::min<std::uint64_t>(UINT32_MAX / 44U,
				impl->maximumStorageBufferRange / sizeof(RenderInstanceRecord));
			if (shadowStreamRecordCount > maximumRecords) { shadowStreamPrepared = false; break; }
			const auto plan = [list, key = record.qualifiedKey, limits]() {
				list->batch = TerraEngine::Renderer::BuildShadowBatchPlan(key, list->batchMembers, limits);
			};
			if (presentation.efficiencyOptions.options.validationPreparation == TerraEngine::Renderer::RendererEfficiencyPreparation::Concurrent)
			{ shadowBatchSettlement.CreateAndEnqueue(ThreadPool::instance(), plan); ++shadowPreparationWorkerTasks; }
			else { shadowBatchSettlement.RunInline(plan); ++shadowPreparationInlineTasks; }
		}
		shadowBatchSettlement.GetAll();
		if (shadowStreamPrepared)
		{
			shadowStreamRecords.resize(static_cast<std::size_t>(shadowStreamRecordCount));
			for (auto& record : shadowLightRecords) if (record.preparedList)
				for (const auto& group : record.preparedList->batch.groups)
					for (std::size_t lane = 0; lane < group.memberIndices.size(); ++lane)
						shadowStreamRecords[group.firstInstance + lane] = record.preparedList->expectedInstances[group.memberIndices[lane]];
		}
	}
	catch (...) { shadowBatchSettlement.Wait(); shadowStreamPrepared = false; }
	bool shadowInstanceUploaded = false;
	auto& shadowStream = impl->shadowInstanceStreams[currentFrame][snapshotSlotIndex];
	const VkDeviceSize shadowInstanceBytes = static_cast<VkDeviceSize>(shadowStreamRecords.size()) * sizeof(RenderInstanceRecord);
	const bool wantsShadowGroups = presentation.efficiencyOptions.options.shadowSubmission
		== TerraEngine::Renderer::RendererEfficiencyShadowSubmission::Grouped
		&& std::any_of(shadowLightRecords.begin(), shadowLightRecords.end(), [](const auto& record) {
			return record.preparedList && !record.reusedCachedResult && !record.preparedList->batch.groups.empty(); });
	if (shadowStreamPrepared && wantsShadowGroups && shadowInstanceBytes)
	{
		try
		{
			if (shadowStream.buffer.buffer == VK_NULL_HANDLE || shadowStream.capacity < shadowInstanceBytes)
			{
				VulkanBuffer replacement = impl->CreateBuffer(shadowInstanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
					VMA_MEMORY_USAGE_CPU_TO_GPU, "Shadow Frame Snapshot Instance Stream");
				try { impl->RetireBuffer(shadowStream.buffer); }
				catch (...) { impl->DestroyBufferImmediate(replacement); throw; }
				shadowStream.buffer = replacement;
				shadowStream.capacity = shadowInstanceBytes;
				shadowStream.allocationGeneration = impl->nextShadowInstanceAllocationGeneration++;
			}
			if (shadowStream.bindlessIndex == VulkanContext::Impl::INVALID_BINDLESS_BUFFER_INDEX)
				shadowStream.bindlessIndex = impl->AllocateBufferIndex();
			// Inject once at the actual map/write seam, then restore the full
			// original draw list. A planned group is never evidence of execution.
			const bool injectFailure = presentation.efficiencyFixtureIntent.controlsApplied && batchControls
				&& batchControls->failInstanceUpload && impl->shadowInstanceFailureControlGeneration
					!= presentation.efficiencyFixtureIntent.controlGeneration;
			if (injectFailure) impl->shadowInstanceFailureControlGeneration = presentation.efficiencyFixtureIntent.controlGeneration;
			void* mapped = nullptr;
			if (!injectFailure && shadowStream.buffer.buffer && shadowStream.buffer.allocation
				&& vmaMapMemory(impl->allocator, shadowStream.buffer.allocation, &mapped) == VK_SUCCESS && mapped)
			{
				std::memcpy(mapped, shadowStreamRecords.data(), static_cast<std::size_t>(shadowInstanceBytes));
				const bool flushed = vmaFlushAllocation(impl->allocator, shadowStream.buffer.allocation, 0, shadowInstanceBytes) == VK_SUCCESS;
				vmaUnmapMemory(impl->allocator, shadowStream.buffer.allocation);
				shadowInstanceUploaded = flushed && impl->UpdateGlobalBufferForFrame(currentFrame, shadowStream.bindlessIndex,
					shadowStream.buffer.buffer, 0, shadowInstanceBytes, true);
			}
			if (shadowInstanceUploaded)
			{
				VkBufferMemoryBarrier hostVisibility{};
				hostVisibility.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
				hostVisibility.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; hostVisibility.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				hostVisibility.srcQueueFamilyIndex = hostVisibility.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				hostVisibility.buffer = shadowStream.buffer.buffer; hostVisibility.size = shadowInstanceBytes;
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
					VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT, 0, 0, nullptr, 1, &hostVisibility, 0, nullptr);
			}
		}
		catch (...) { shadowInstanceUploaded = false; }
	}
	TerraEngine::Renderer::Vulkan::ShadowGroupedStreamOwner shadowStreamOwner;
	shadowStreamOwner.rendererSessionGeneration = publication->rendererSessionGeneration;
	shadowStreamOwner.backendGeneration = impl->backendResourceGeneration;
	shadowStreamOwner.allocationId = reinterpret_cast<std::uintptr_t>(shadowStream.buffer.buffer);
	shadowStreamOwner.allocationGeneration = shadowStream.allocationGeneration;
	shadowStreamOwner.frameSerial = publication->frameId; shadowStreamOwner.commandSerial = shadowCommandSerial;
	shadowStreamOwner.frameSlot = currentFrame; shadowStreamOwner.snapshotSlot = snapshotSlotIndex;
	for (auto& record : shadowLightRecords) if (record.preparedList)
	{
		auto& list = *record.preparedList;
		const auto originalCommandCount = indirectCommands.size();
		try
		{
		list.batch.view = record.qualifiedKey;
		list.streamPlan = TerraEngine::Renderer::Vulkan::BuildShadowGroupedStreamPlan(list.batch,
			list.batchMembers, list.expectedInstances, shadowStreamRecords, shadowStreamOwner, shadowStream.capacity);
		TerraEngine::Renderer::Vulkan::ShadowGroupedUploadReceipt upload;
		upload.view = record.qualifiedKey; upload.owner = shadowStreamOwner;
		upload.frameSlotSafe = impl->lightingUploadFrameSlotSafe[currentFrame]; upload.ownerCurrent = shadowInstanceUploaded;
		upload.mapSucceeded = upload.writeSucceeded = upload.flushSucceeded = upload.descriptorPublished = shadowInstanceUploaded;
		list.execution = TerraEngine::Renderer::Vulkan::SelectShadowGroupedExecution(list.streamPlan, upload);
		if (!list.execution.grouped)
		{
			list.batchFallback = !wantsShadowGroups ? "SingletonReference" : (!shadowInstanceUploaded
				? "ShadowInstanceUploadUnavailable" : "ShadowGroupProofUnavailable");
			continue;
		}
		// Original singleton commands remain retained. Only a fully accepted
		// execution selection chooses this alternate vector for actual recording.
		for (std::size_t groupIndex = 0; groupIndex < list.execution.groups.size(); ++groupIndex)
		{
			const auto& group = list.execution.groups[groupIndex];
			PreparedDraw draw = list.draws[group.representativeIndex];
			draw.shadowGrouped = true; draw.shadowGroupIndex = static_cast<std::uint32_t>(groupIndex);
			draw.push.virtualGeometryFlags |= 0x80000000U;
			draw.push.virtualGeometryFlags &= ~0x40000000U;
			draw.push.model[3] = glm::vec4(std::bit_cast<float>(shadowStream.bindlessIndex),
				std::bit_cast<float>(group.firstInstance), std::bit_cast<float>(group.instanceCount),
				std::bit_cast<float>(static_cast<std::uint32_t>(shadowStreamRecords.size())));
			VkDrawMeshTasksIndirectCommandEXT command{group.taskGroupsX, 1, 1};
			draw.indirectBufferIndex = static_cast<std::uint32_t>(indirectCommands.size()); indirectCommands.push_back(command);
			list.groupedDraws.push_back(draw);
		}
		for (const auto index : list.execution.singletonIndices) list.groupedDraws.push_back(list.draws[index]);
		record.preparedDraws = &list.groupedDraws;
		}
		catch (...)
		{
			indirectCommands.resize(originalCommandCount); list.groupedDraws.clear();
			// Selection retains capacity for every original member, even when its
			// accepted groups contain no singletons. Keep that storage when alternate
			// draw assembly fails so actual fallback draws retain their exact proof.
			list.execution.groups.clear(); list.execution.grouped = false;
			try
			{
				list.execution.singletonIndices.assign(list.streamPlan.allSingletonIndices.begin(),
					list.streamPlan.allSingletonIndices.end());
			}
			catch (...)
			{
				// If the earlier planner itself exhausted memory, original draws still
				// survive. Missing receipt storage must conservatively disable reuse.
				list.recordingReady = false;
			}
			list.batchFallback = "ShadowGroupSelectionAllocation";
			record.preparedDraws = &list.draws;
		}
	}

	// Shadow-map rendering and the main scene pass both consume the same
	// indirect draw command buffer. Upload it before any shadow pass records
	// indirect draws, otherwise the shadow map can render stale or empty draw
	// commands and blockers will not occlude the light.
	if (!indirectCommands.empty()) {
		const auto rejectIndirectGeometry = [&]() {
			// The primary command buffer is already recording by this point. Keep it
			// structurally valid and reject every consumer of this range together, so
			// no main or shadow pass can reference stale indirect commands.
			prepared.clear();
			for (PreparedShadowList& preparedList : preparedShadowLists) {
				preparedList.draws.clear();
				preparedList.groupedDraws.clear(); preparedList.execution = {};
				preparedList.recordingReady = false;
				preparedList.rejection.Capture("IndirectRangeUpload", UINT32_MAX);
			}
			indirectCommands.clear();
			pendingStats.drawCalls = 0u;
			pendingStats.triangleCount = 0u;
			pendingStats.meshletsTotal = 0u;
			pendingStats.meshletsDispatched = 0u;
			impl->RecordPendingStats(statsSlotIndex, pendingStats);
		};
		std::uint32_t indirectDrawOffset = 0u;
		if (!impl->ReserveIndirectDrawCommands(
			indirectCommands.size(), indirectDrawOffset)) {
			TERRAENGINE_LOG(LogLevel::Error,
				"Vulkan could not reserve a non-overlapping indirect command range; "
					"omitting geometry from this view while preserving its valid background and overlays.");
			rejectIndirectGeometry();
		} else {
			renderSnapshot.indirectDrawOffset = indirectDrawOffset;
			// Draw preparation runs before the complete shadow list is known. Patch
			// the final range into every prepared command together so secondary main
			// recording and primary shadow recording address the same uploaded bytes.
			for (PreparedDraw& draw : prepared) {
				draw.push.indirectDrawOffset = indirectDrawOffset;
			}
			for (PreparedShadowList& preparedList : preparedShadowLists) {
				for (PreparedDraw& draw : preparedList.draws) {
					draw.push.indirectDrawOffset = indirectDrawOffset;
				}
				for (PreparedDraw& draw : preparedList.groupedDraws) draw.push.indirectDrawOffset = indirectDrawOffset;
			}
			const VulkanBuffer& indirectDrawBuffer = impl->indirectDrawBuffers[impl->currentFrame];
			void* data = nullptr;
			const VkResult mapResult = vmaMapMemory(
				impl->allocator, indirectDrawBuffer.allocation, &data);
			if (mapResult != VK_SUCCESS || data == nullptr) {
				TERRAENGINE_LOG(LogLevel::Error,
					"Vulkan could not map the indirect command range; omitting geometry from this view.");
				rejectIndirectGeometry();
			} else {
				auto* snapshotIndirectCommands =
					static_cast<VkDrawMeshTasksIndirectCommandEXT*>(data) + renderSnapshot.indirectDrawOffset;
				std::memcpy(snapshotIndirectCommands, indirectCommands.data(),
					indirectCommands.size() * sizeof(VkDrawMeshTasksIndirectCommandEXT));
				// The indirect command buffer is ordinary mapped CPU_TO_GPU memory, not a
				// staging upload. Release and exported builds can land this allocation in
				// non-coherent memory, so flush the snapshot range before shadow/main
				// passes consume it through vkCmdDrawMeshTasksIndirectEXT.
				const VkDeviceSize indirectByteOffset = static_cast<VkDeviceSize>(renderSnapshot.indirectDrawOffset)
					* sizeof(VkDrawMeshTasksIndirectCommandEXT);
				const VkDeviceSize indirectByteSize = static_cast<VkDeviceSize>(indirectCommands.size())
					* sizeof(VkDrawMeshTasksIndirectCommandEXT);
				const VkResult flushResult = vmaFlushAllocation(impl->allocator,
					indirectDrawBuffer.allocation, indirectByteOffset, indirectByteSize);
				vmaUnmapMemory(impl->allocator, indirectDrawBuffer.allocation);
				if (flushResult != VK_SUCCESS) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Vulkan could not flush the indirect command range; omitting geometry from this view.");
					rejectIndirectGeometry();
				}
			}
		}
	}

	beginCommandRecordingPhase(
		"Vulkan: Primary Record Shadow Commands");
	auto RecordShadowDraws = [&](ShadowLightRecord& record, const std::vector<PreparedDraw>& shadowDraws, const glm::mat4& lightViewProjection, bool partitionDraw = false) {
		std::uint32_t recordedDrawCount = 0u;
		TerraEngine::Renderer::Vulkan::ShadowGroupedRecordingReceipt recording;
		recording.view = record.qualifiedKey; recording.owner = shadowStreamOwner;
		record.recordedBatchExecution = {};
		auto& executionEvidence = record.recordedBatchExecution;
		if (shadowValidationRequested && record.preparedList)
		{
			executionEvidence.available = true; executionEvidence.sources = record.preparedList->batchSources;
			executionEvidence.frameSlotSafe = impl->lightingUploadFrameSlotSafe[currentFrame];
			executionEvidence.uploadAttempted = shadowStreamPrepared && wantsShadowGroups && shadowInstanceBytes != 0;
			executionEvidence.uploadSucceeded = shadowInstanceUploaded;
			executionEvidence.allocationIdentity = shadowStreamOwner.allocationId;
			executionEvidence.allocationGeneration = shadowStreamOwner.allocationGeneration;
			executionEvidence.backendGeneration = shadowStreamOwner.backendGeneration;
			executionEvidence.instanceBytes = shadowInstanceBytes;
			executionEvidence.frameSlot = currentFrame; executionEvidence.snapshotSlot = snapshotSlotIndex;
		}
		VkPipeline boundPipeline = VK_NULL_HANDLE;
		VkPipelineLayout boundDescriptorLayout = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, 2u> boundDescriptorSets{};
		bool descriptorsBound = false;

		for (const PreparedDraw& draw : shadowDraws) {

			if (draw.pipeline == VK_NULL_HANDLE
				|| draw.pipelineLayout == VK_NULL_HANDLE
				|| draw.meshletCount == 0u) {
				continue;
			}

			// Crowds commonly repeat one pipeline and descriptor pair thousands of
			// times. Vulkan keeps this state until another bind changes it, so emit a
			// bind only when the prepared draw actually changes the required state.
			if(boundPipeline != draw.pipeline)
			{
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
				boundPipeline = draw.pipeline;
			}
			if(!descriptorsBound || boundDescriptorLayout != draw.pipelineLayout
				|| boundDescriptorSets != draw.descriptorSets)
			{
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
					draw.pipelineLayout, 0,
					static_cast<uint32_t>(draw.descriptorSets.size()),
					draw.descriptorSets.data(), 0, nullptr);
				boundDescriptorLayout = draw.pipelineLayout;
				boundDescriptorSets = draw.descriptorSets;
				descriptorsBound = true;
			}

			PushConstants shadowPush = draw.push;
			shadowPush.viewProj = lightViewProjection;
			shadowPush.taskCullingEnabled = 0u;
			// Keep Hi-Z diagnostic bits intact. Shadow depth uses its own view
			// projection, but clearing these flags here previously acted like a
			// hidden metadata disable path after editor view changes.
			PushDefaultMeshConstants(
				cmd,
				draw.pipelineLayout,
				draw.pushConstantRanges.data(),
				draw.pushConstantRangeCount,
				shadowPush);

			VkDeviceSize offset = static_cast<VkDeviceSize>(draw.push.indirectDrawOffset + draw.indirectBufferIndex) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
			vkCmdDrawMeshTasksIndirectEXT(cmd, impl->indirectDrawBuffers[impl->currentFrame].buffer, offset, 1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
			recordedDrawCount += 1u;
			// These receipts follow the actual native call. Planned groups alone
			// cannot increment the exported submission or caster coverage counters.
			if (draw.shadowGrouped)
			{
				recording.recordedGroupIndices.push_back(draw.shadowGroupIndex);
				if (shadowValidationRequested && record.preparedList)
				{
					const auto& group = record.preparedList->execution.groups[draw.shadowGroupIndex];
					TerraEngine::Renderer::RendererEfficiencyShadowBatchGroup receipt;
					receipt.firstInstance = group.firstInstance; receipt.instanceCount = group.instanceCount;
					receipt.taskGroupsX = group.taskGroupsX; receipt.pipelineIdentity = group.key.pipeline;
					receipt.layoutIdentity = group.key.layout;
					for (const auto index : group.memberIndices)
					{
						const auto& member = record.preparedList->batchMembers[index];
						receipt.members.push_back({member.objectId, member.lifetimeGeneration, member.segmentId});
					}
					executionEvidence.groups.push_back(std::move(receipt));
				}
			}
			else
			{
				recording.recordedSingletonIndices.push_back(draw.shadowMemberIndex);
				if (shadowValidationRequested && record.preparedList)
				{
					const auto& member = record.preparedList->batchMembers[draw.shadowMemberIndex];
					executionEvidence.singletons.push_back({member.objectId, member.lifetimeGeneration, member.segmentId});
				}
			}
		}
		recording.recorded = true;
		if (record.preparedList && partitionDraw)
		{
			// Partitioned commands retain validated original singleton indices. The
			// whole-list P4 counter must not reject a deliberate static/dynamic subset.
			record.recordedBatchCounts = {};
			record.recordedBatchCounts.valid = recordedDrawCount == shadowDraws.size()
				&& recording.recordedGroupIndices.empty() && record.preparedList->staticPartition.valid;
			record.recordedBatchCounts.singletons = record.recordedBatchCounts.segments = recordedDrawCount;
			record.recordedBatchCounts.draws = recordedDrawCount;
			executionEvidence.recordedComplete = record.recordedBatchCounts.valid;
		}
		else if (record.preparedList)
		{
			record.recordedBatchCounts = TerraEngine::Renderer::Vulkan::CountShadowGroupedRecording(
				record.preparedList->streamPlan, record.preparedList->execution, recording);
			executionEvidence.recordedComplete = record.recordedBatchCounts.valid && record.preparedList->recordingReady;
		}
		return recordedDrawCount;
	};

	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.shadows.begin" : "game.shadows.begin");
	// Copy a face before a later view can overwrite the same physical image.
	// Counts describe actual recorded work and remain Pending until real submit
	// and completion receipts qualify both the depth and matching frame color.
	const auto recordShadowValidationFace = [&](const ShadowLightRecord& record, VkImage depthImage,
		std::uint32_t face, std::uint32_t recordedDraws) {
		if(!shadowValidationRequested) return;
		TerraEngine::Renderer::ShadowValidationObservation observation;
		observation.key = record.qualifiedKey;
		observation.key.faceIndex = face;
		if(record.type == 1U) observation.key.lightProjectionStamp.viewProjection =
			observation.key.lightProjectionStamp.cubeViewProjections[face];
		observation.contentToken = record.contentToken;
		observation.preparationComplete = record.preparedList != nullptr && record.preparedList->recordingReady
			&& (record.reusedCachedResult || record.recordedBatchCounts.valid);
		if(record.preparedList) {
			observation.membership = record.preparedList->membership;
			if(!record.reusedCachedResult && observation.preparationComplete) {
				if (record.staticComposition && record.staticReused) observation.submittedSegments = record.preparedList->staticPartition.dynamicMembership.rows;
				else observation.submittedSegments = observation.membership.rows;
			}
		}
		auto& evidence = observation.evidence;
		evidence.viewType = renderViewType; evidence.viewId = diagnosticViewId;
		evidence.lightId = record.lightId; evidence.faceIndex = face;
		evidence.resolution = {record.regionWidth, record.regionHeight};
		evidence.frameSerial = publication->frameId; evidence.commandSerial = shadowCommandSerial;
		evidence.effectiveOptions = presentation.efficiencyOptions;
		evidence.lightingUploads = lightingUploadEvidence;
		evidence.fixtureCorrelation = presentation.efficiencyFixtureIntent.correlation;
		evidence.fixtureControlGeneration = presentation.efficiencyFixtureIntent.controlGeneration;
		evidence.shadowBatchExecution = record.recordedBatchExecution;
		if (record.reusedCachedResult && record.preparedList)
		{
			evidence.shadowBatchExecution.available = true; evidence.shadowBatchExecution.recordedComplete = true;
			evidence.shadowBatchExecution.frameSlotSafe = impl->lightingUploadFrameSlotSafe[currentFrame];
			evidence.shadowBatchExecution.backendGeneration = impl->backendResourceGeneration;
			evidence.shadowBatchExecution.frameSlot = currentFrame; evidence.shadowBatchExecution.snapshotSlot = snapshotSlotIndex;
			evidence.shadowBatchExecution.sources = record.preparedList->batchSources;
		}
		evidence.preparationWorkerTasks = shadowPreparationWorkerTasks;
		evidence.preparationInlineTasks = shadowPreparationInlineTasks;
		evidence.preparationSettled = shadowPreparationSettled;
		if (shadowResourceRecreationAction) evidence.fixtureActions.push_back(*shadowResourceRecreationAction);
		evidence.contentOwner = std::to_string(record.contentToken);
		evidence.refreshReason = record.reusedCachedResult ? "ExactSubmittedContent"
			: (presentation.efficiencyOptions.options.shadowReuse == TerraEngine::Renderer::RendererEfficiencyShadowReuse::AlwaysRefresh
				? "AlwaysRefresh" : (observation.membership.reusable ? "ContentOrOwnerChanged" : "MissingSemanticProof"));
		const auto measured = [](std::uint64_t value) {
			return TerraEngine::Renderer::RendererEfficiencyCounter{TerraEngine::Renderer::RendererEfficiencyEvidenceStatus::Valid, value, {}};
		};
		evidence.eligibleCasterSegments = measured(observation.membership.rows.size());
		std::uint64_t lightingBytes = 0, lightingRecords = 0, lightingRanges = 0, flushBytes = 0, flushRanges = 0;
		for (const auto& upload : lightingUploadEvidence) {
			lightingBytes += upload.logicalWriteBytes; lightingRecords += upload.logicalWriteRecords;
			lightingRanges += upload.logicalWriteRanges.size(); flushBytes += upload.physicalFlushBytes;
			flushRanges += upload.physicalFlushRanges.size();
		}
		evidence.lightingLogicalWriteBytes = measured(lightingBytes); evidence.lightingWriteRecords = measured(lightingRecords);
		evidence.lightingWriteRanges = measured(lightingRanges); evidence.lightingPhysicalFlushBytes = measured(flushBytes);
		evidence.lightingFlushRanges = measured(flushRanges);
		evidence.submittedCasterSegments = measured(observation.submittedSegments.size());
		evidence.actualDrawSubmissions = measured(recordedDraws);
		evidence.groups = measured(record.reusedCachedResult ? 0 : record.recordedBatchCounts.groups);
		evidence.singletons = measured(record.reusedCachedResult ? 0 : record.recordedBatchCounts.singletons);
		if (record.preparedList) evidence.fallbackReason = record.preparedList->batchFallback;
		const auto* partition = record.preparedList ? &record.preparedList->staticPartition : nullptr;
		auto& staticExecution = evidence.staticShadowExecution;
		staticExecution.available = true; staticExecution.partitionComplete = partition && partition->valid;
		staticExecution.backendGeneration = impl->backendResourceGeneration;
		staticExecution.copyOverlay = record.staticComposition; staticExecution.staticReused = record.staticComposition && record.staticReused;
		impl->CaptureStaticShadowAllocations(staticExecution);
		staticExecution.requiredFaceMask = record.type == 1 ? 0x3f : 1;
		staticExecution.staticRecordedFaceMask = record.staticRecordedFaceMask; staticExecution.copiedFaceMask = record.copiedFaceMask;
		staticExecution.recordedComplete = observation.preparationComplete
			&& (!record.staticComposition || (record.copiedFaceMask == staticExecution.requiredFaceMask
				&& record.recordedFaceMask == staticExecution.requiredFaceMask));
		if (partition && partition->valid) {
			staticExecution.staticMembership = partition->staticMembership.rows;
			staticExecution.dynamicMembership = partition->dynamicMembership.rows;
		}
		staticExecution.staticDraws = record.staticComposition ? record.staticFaceDraws[face]
			: 0;
		staticExecution.dynamicDraws = record.staticComposition ? record.dynamicFaceDraws[face]
			: 0;
		// Fallback counters describe caster segments; grouped draws may contain
		// several of them. Copy-overlay uses original singleton calls explicitly.
		staticExecution.fallbackReason = record.staticComposition ? std::string{} : record.staticFallback;
		if (record.staticComposition && record.staticEntry) {
			const auto& entry = *record.staticEntry; staticExecution.staticView = entry.staticView;
			staticExecution.staticContentToken = entry.contentToken;
			staticExecution.sourceCommandSerial = record.staticSourceCommand; staticExecution.sourceSubmitSerial = record.staticSourceSubmit;
			staticExecution.allocationIdentity = entry.owner.allocationIdentity; staticExecution.allocationGeneration = entry.owner.allocationGeneration;
			staticExecution.backendGeneration = entry.owner.backendGeneration; staticExecution.actualBytes = entry.owner.actualBytes;
		}
		evidence.staticRefreshes = measured(record.staticComposition ? staticExecution.staticDraws
			: record.reusedCachedResult ? 0 : staticExecution.staticMembership.size());
		evidence.dynamicRefreshes = measured(record.staticComposition ? staticExecution.dynamicDraws
			: record.reusedCachedResult ? 0 : staticExecution.dynamicMembership.size());
		evidence.staticReservedBytes = measured(0); // Synchronous admission has no outstanding queried reservation at recording.
		evidence.staticActiveBytes = measured(impl->staticShadowLifetime.ResidentBytes());
		evidence.staticRetiringBytes = measured(impl->staticShadowLifetime.RetiringBytes());
		evidence.staticCacheBudgetRequestedBytes = impl->staticShadowRequestedBudget;
		evidence.staticCacheBudgetEffectiveBytes = impl->staticShadowEffectiveBudget; evidence.staticCacheDraining = impl->staticShadowBudgetDraining;
		evidence.shadowReuses = measured(record.reusedCachedResult ? 1 : 0);
		if(record.preparedList == nullptr || !record.preparedList->recordingReady) {
			// Allocate diagnostic text only after failed typed preparation.
			const auto* list = record.preparedList;
			const auto* failure = list ? &list->rejection : nullptr;
			evidence.fallbackReason = std::string("Caster preparation incomplete; stage=")
				+ (failure && failure->stage ? failure->stage : "MissingPreparedList")
				+ "; ordinal=" + std::to_string(failure ? failure->ordinal : UINT32_MAX)
				+ "; object=" + ToHexString(failure ? failure->objectId : TerraEngine::EMPTY_UUID)
				+ "; mesh=" + ToHexString(failure ? failure->meshId : TerraEngine::EMPTY_UUID)
				+ "; material=" + ToHexString(failure ? failure->materialId : TerraEngine::EMPTY_UUID)
				+ "; sourceOrdinals=" + std::to_string(list ? list->sourceOrdinalCount : 0)
				+ "; prewarmPassMatches=" + (shadowPrewarmPass == impl->shadowRenderPass ? "true" : "false")
				+ "; prewarmPassPresent=" + (shadowPrewarmPass != VK_NULL_HANDLE ? "true" : "false")
				+ "; recordingPassPresent=" + (impl->shadowRenderPass != VK_NULL_HANDLE ? "true" : "false")
				+ "; prewarmAllocation=" + std::to_string(shadowPrewarmAllocation)
				+ "; recordingAllocation=" + std::to_string(impl->shadowAllocationGeneration)
				+ "; requestedSlots=" + std::to_string(requestedShadowResources.TotalCount())
				+ "; activeSlots=" + std::to_string(impl->activeShadowResourceRequest.TotalCount())
				+ "; requestedPointSlots=" + std::to_string(requestedShadowResources.pointCount)
				+ "; activePointSlots=" + std::to_string(impl->activeShadowResourceRequest.pointCount);
		}
		impl->shadowValidationReadback.RecordFace(cmd, depthImage, static_cast<VkFormat>(record.qualifiedKey.depthFormat), face, observation);
	};
	auto RecordShadowPass = [&](VkFramebuffer framebuffer, VkImage colorImage, VkImage depthImage, uint32_t arrayLayer, uint32_t width, uint32_t height, ShadowLightRecord& record, const glm::mat4& lightViewProjection) -> bool {

		if (framebuffer == VK_NULL_HANDLE || colorImage == VK_NULL_HANDLE || depthImage == VK_NULL_HANDLE || impl->shadowRenderPass == VK_NULL_HANDLE) {
			return false;
		}

		// A shadow attachment can be written in one queue submission and reused in
		// another. Use explicit per-image barriers because a render-pass external
		// dependency does not provide that cross-submit memory dependency on every
		// driver. The depth transition also orders any cached shader reads before
		// the next clear and write.
		std::array<VkImageMemoryBarrier, 2> attachmentBarriers{};
		attachmentBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		attachmentBarriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		attachmentBarriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		attachmentBarriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachmentBarriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachmentBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		attachmentBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		attachmentBarriers[0].image = colorImage;
		attachmentBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		attachmentBarriers[0].subresourceRange.baseMipLevel = 0u;
		attachmentBarriers[0].subresourceRange.levelCount = 1u;
		attachmentBarriers[0].subresourceRange.baseArrayLayer = arrayLayer;
		attachmentBarriers[0].subresourceRange.layerCount = 1u;

		attachmentBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		attachmentBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		attachmentBarriers[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		attachmentBarriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		attachmentBarriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachmentBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		attachmentBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		attachmentBarriers[1].image = depthImage;
		attachmentBarriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		attachmentBarriers[1].subresourceRange.baseMipLevel = 0u;
		attachmentBarriers[1].subresourceRange.levelCount = 1u;
		attachmentBarriers[1].subresourceRange.baseArrayLayer = arrayLayer;
		attachmentBarriers[1].subresourceRange.layerCount = 1u;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_DEPENDENCY_BY_REGION_BIT,
			0u, nullptr,
			0u, nullptr,
			static_cast<uint32_t>(attachmentBarriers.size()), attachmentBarriers.data());

		VkRenderPassBeginInfo shadowPassInfo{};
		shadowPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		shadowPassInfo.renderPass = impl->shadowRenderPass;
		shadowPassInfo.framebuffer = framebuffer;
		shadowPassInfo.renderArea.offset = { 0, 0 };
		shadowPassInfo.renderArea.extent = { width, height };

		VkClearValue clearValues[2];
		clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
		clearValues[1].depthStencil = {1.0f, 0};
		shadowPassInfo.clearValueCount = 2;
		shadowPassInfo.pClearValues = clearValues;

		vkCmdBeginRenderPass(cmd, &shadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport shadowViewport{};
		shadowViewport.x = static_cast<float>(record.regionX);
		shadowViewport.y = static_cast<float>(record.regionY);
		shadowViewport.width = static_cast<float>(std::min(record.regionWidth, width));
		shadowViewport.height = static_cast<float>(std::min(record.regionHeight, height));
		shadowViewport.minDepth = 0.0f;
		shadowViewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0u, 1u, &shadowViewport);

		VkRect2D shadowScissor{};
		shadowScissor.offset = { static_cast<std::int32_t>(record.regionX), static_cast<std::int32_t>(record.regionY) };
		shadowScissor.extent = { std::min(record.regionWidth, width), std::min(record.regionHeight, height) };
		vkCmdSetScissor(cmd, 0u, 1u, &shadowScissor);
		static const std::vector<PreparedDraw> kEmptyPreparedShadowDraws{};
		const std::vector<PreparedDraw>& shadowDraws = record.preparedDraws != nullptr
			? *record.preparedDraws
			: kEmptyPreparedShadowDraws;
		const auto recordedDraws = RecordShadowDraws(record, shadowDraws, lightViewProjection);
		pendingBudgetDiagnostics.shadowDrawSubmissionCount += recordedDraws;
		if (record.recordedBatchCounts.valid)
		{
			pendingBudgetDiagnostics.shadowGroupedSubmissionCount += static_cast<std::uint32_t>(record.recordedBatchCounts.groups);
			pendingBudgetDiagnostics.shadowSingletonSubmissionCount += static_cast<std::uint32_t>(record.recordedBatchCounts.singletons);
			pendingBudgetDiagnostics.shadowGroupedCasterSegmentCount += static_cast<std::uint32_t>(record.recordedBatchCounts.segments - record.recordedBatchCounts.singletons);
		}
		vkCmdEndRenderPass(cmd);
		recordShadowValidationFace(record, depthImage, arrayLayer, recordedDraws);
		return true;
	};

	// All operations below remain in the existing primary/queue boundary. Each
	// depth layer moves explicitly through attachment, transfer and sampling
	// layouts; no separate submission or idle is added for the static cache.
	const auto staticDepthBarrier = [&](VkImage image, std::uint32_t face, VkImageLayout oldLayout,
		VkImageLayout newLayout, VkAccessFlags source, VkAccessFlags destination,
		VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		barrier.oldLayout = oldLayout; barrier.newLayout = newLayout;
		barrier.srcAccessMask = source; barrier.dstAccessMask = destination;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, face, 1};
		vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr, 1, &barrier);
	};
	const auto recordPartitionPass = [&](ShadowLightRecord& record, VkRenderPass pass, VkFramebuffer framebuffer,
		std::uint32_t face, const std::vector<PreparedDraw>& draws) {
		// The static and final passes borrow the same unused color attachment.
		// Its clears still write memory, even though shadow pipelines mask color.
		const auto colorImage = record.type == 0 ? impl->directionalShadowColors[record.slot].image
			: record.type == 1 ? impl->pointShadowCubeColors[record.slot].image
			: record.type == 2 ? impl->spotShadowColors[record.slot].image : impl->areaShadowColors[record.slot].image;
		VkImageMemoryBarrier colorBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		colorBarrier.image = colorImage; colorBarrier.oldLayout = colorBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		colorBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		colorBarrier.srcQueueFamilyIndex = colorBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		colorBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr, 1, &colorBarrier);
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		begin.renderPass = pass; begin.framebuffer = framebuffer; begin.renderArea.extent = {record.qualifiedKey.extent[0], record.qualifiedKey.extent[1]};
		std::array<VkClearValue, 2> clear{}; clear[1].depthStencil = {1.0f, 0};
		begin.clearValueCount = 2; begin.pClearValues = clear.data();
		vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport{0, 0, static_cast<float>(begin.renderArea.extent.width), static_cast<float>(begin.renderArea.extent.height), 0, 1};
		VkRect2D scissor{{0, 0}, begin.renderArea.extent};
		vkCmdSetViewport(cmd, 0, 1, &viewport); vkCmdSetScissor(cmd, 0, 1, &scissor);
		const auto matrix = record.type == 1 ? BuildPointShadowViewProjection(*record.light, face) : LoadShadowViewProjection(*record.light);
		const auto count = RecordShadowDraws(record, draws, matrix, true);
		vkCmdEndRenderPass(cmd);
		return count;
	};
	const auto recordStaticComposition = [&](ShadowLightRecord& record) {
		if (!record.staticEntry || !record.preparedList || !impl->staticShadowLoadRenderPass) return false;
		auto& entry = *record.staticEntry;
		const auto layers = record.type == 1 ? 6U : 1U;
		const auto requiredMask = static_cast<std::uint8_t>(layers == 6 ? 0x3f : 1);
		auto& finalDepth = record.type == 0 ? impl->directionalShadowDepths[record.slot]
			: record.type == 1 ? impl->pointShadowCubeDepths[record.slot]
			: record.type == 2 ? impl->spotShadowDepths[record.slot] : impl->areaShadowDepths[record.slot];
		const auto finalFramebuffer = [&](std::uint32_t face) {
			return record.type == 0 ? impl->directionalShadowFramebuffers[record.slot]
				: record.type == 1 ? impl->pointShadowFaceFramebuffersBySlot[record.slot][face]
				: record.type == 2 ? impl->spotShadowFramebuffers[record.slot] : impl->areaShadowFramebuffers[record.slot];
		};
		for (std::uint32_t face = 0; face < layers; ++face)
			if (!finalFramebuffer(face) || !entry.framebuffers[face]) return false;
		if (!finalDepth.image || !finalDepth.allocation || !entry.depth.image) return false;
		if (!record.staticReused) {
			if (!impl->nextShadowContentToken) { record.staticFallback = "StaticTokenExhausted"; return false; }
			entry.contentToken = impl->nextShadowContentToken++;
			if (!entry.layoutEstablished) entry.initializationCommand = shadowCommandSerial;
			for (std::uint32_t face = 0; face < layers; ++face) {
				const auto oldLayout = entry.layouts[face];
				staticDepthBarrier(entry.depth.image, face, oldLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
					oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
					VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
				record.staticFaceDraws[face] = recordPartitionPass(record, impl->shadowRenderPass, entry.framebuffers[face], face, record.staticDraws);
				if (shadowValidationRequested) record.staticFaceExecutions[face] = record.recordedBatchExecution;
				entry.layouts[face] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
				if (record.recordedBatchCounts.valid) record.staticRecordedFaceMask |= static_cast<std::uint8_t>(1U << face);
				pendingBudgetDiagnostics.shadowDrawSubmissionCount += record.staticFaceDraws[face];
				pendingBudgetDiagnostics.shadowSingletonSubmissionCount += record.staticFaceDraws[face];
			}
			TerraEngine::Renderer::ShadowRefreshCandidate candidate;
			candidate.view = entry.staticView; candidate.membership = record.preparedList->staticPartition.staticMembership;
			candidate.contentToken = entry.contentToken; candidate.frameSerial = publication->frameId;
			candidate.commandSerial = shadowCommandSerial; candidate.requiredFaceMask = requiredMask;
			candidate.recordedFaceMask = record.staticRecordedFaceMask; candidate.recordingComplete = record.staticRecordedFaceMask == requiredMask;
			if (!impl->staticShadowPublicationLedger.Record(candidate)) { record.staticFallback = "IncompleteStaticRecording"; return false; }
			record.staticSourceCommand = shadowCommandSerial; record.staticSourceSubmit = 0;
		}
		// Cached source coverage is proved by the submitted publication below.
		// Keep this frame's recorded mask zero when no static pass was recorded,
		// so copied diagnostics distinguish reuse from fresh native work.
		VmaAllocationInfo finalAllocation{}; vmaGetAllocationInfo(impl->allocator, finalDepth.allocation, &finalAllocation);
		TerraEngine::Renderer::VulkanStaticShadow::StaticShadowCompositionRequest request;
		request.staticView = entry.staticView; request.finalView = record.qualifiedKey; request.staticContentToken = entry.contentToken;
		request.currentCommandSerial = shadowCommandSerial; request.sourceRecordedCommandSerial = record.staticReused ? 0 : shadowCommandSerial;
		request.source = entry.owner;
		request.destination = {impl->renderSessionGeneration, impl->backendResourceGeneration,
			reinterpret_cast<std::uint64_t>(finalDepth.allocation), impl->shadowAllocationGeneration, finalAllocation.size,
			record.qualifiedKey.depthFormat, layers, record.qualifiedKey.extent};
		request.requiredFaceMask = request.sourceFaceMask = requiredMask;
		// Creation queried both complete usage descriptors and these pipelines are
		// the original shadow LESS/cutout pipelines with color writes disabled.
		request.sourceTransferSupported = request.destinationTransferSupported = true;
		request.sourceTransferUsage = request.destinationTransferUsage = true;
		request.sameShadowPipeline = request.lessDepthComparison = request.colorWritesDisabled = request.cutoutBehaviorPreserved = true;
		const auto composition = TerraEngine::Renderer::VulkanStaticShadow::BuildStaticShadowCompositionPlan(request, impl->staticShadowPublicationLedger);
		if (composition.route != TerraEngine::Renderer::VulkanStaticShadow::StaticShadowCompositionRoute::CacheCopyOverlay) {
			record.staticFallback = "StaticCompositionProofRejected"; return false;
		}
		for (std::uint32_t face = 0; face < layers; ++face) {
			staticDepthBarrier(entry.depth.image, face, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			staticDepthBarrier(finalDepth.image, face, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, face, 1};
			copy.extent = {record.qualifiedKey.extent[0], record.qualifiedKey.extent[1], 1};
			vkCmdCopyImage(cmd, entry.depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, finalDepth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			record.copiedFaceMask |= static_cast<std::uint8_t>(1U << face);
			staticDepthBarrier(entry.depth.image, face, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
			staticDepthBarrier(finalDepth.image, face, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
			record.dynamicFaceDraws[face] = recordPartitionPass(record, impl->staticShadowLoadRenderPass, finalFramebuffer(face), face, record.dynamicDraws);
			if (!record.staticReused) {
				// Fresh composition records both subsets. Preserve their actual native
				// singleton receipts instead of replacing them with the final overlay.
				if (shadowValidationRequested) {
					const auto& staticExecution = record.staticFaceExecutions[face];
					record.recordedBatchExecution.singletons.insert(record.recordedBatchExecution.singletons.begin(),
						staticExecution.singletons.begin(), staticExecution.singletons.end());
				}
				record.recordedBatchCounts.draws += record.staticFaceDraws[face];
				record.recordedBatchCounts.singletons += record.staticFaceDraws[face];
				record.recordedBatchCounts.segments += record.staticFaceDraws[face];
			}
			if (record.recordedBatchCounts.valid) record.recordedFaceMask |= static_cast<std::uint8_t>(1U << face);
			pendingBudgetDiagnostics.shadowDrawSubmissionCount += record.dynamicFaceDraws[face];
			pendingBudgetDiagnostics.shadowSingletonSubmissionCount += record.dynamicFaceDraws[face];
		}
		record.staticComposition = true;
		// Capture after every face is complete so each observation has the whole
		// atomic cube receipt. Later lights/views cannot overwrite before copies.
		for (std::uint32_t face = 0; face < layers; ++face)
			recordShadowValidationFace(record, finalDepth.image, face, record.staticFaceDraws[face] + record.dynamicFaceDraws[face]);
		return true;
	};
	bool recordedAnyShadowPass = false;
	const uint32_t pointShadowSize = VulkanContext::Impl::POINT_SHADOW_MAP_RESOLUTION;
	pendingBudgetDiagnostics.shadowDrawSubmissionCount = 0u;
	pendingBudgetDiagnostics.shadowGroupedSubmissionCount = pendingBudgetDiagnostics.shadowSingletonSubmissionCount = 0;
	pendingBudgetDiagnostics.shadowGroupedCasterSegmentCount = 0;

	if (shadowResourcesReady && impl->shadowRenderPass != VK_NULL_HANDLE && !shadowLightRecords.empty()) {

		for (ShadowLightRecord& record : shadowLightRecords) {

			if (record.light == nullptr || record.slot >= VulkanContext::Impl::MAX_SHADOW_RESOURCE_SLOTS) {
				continue;
			}

			if (record.reusedCachedResult) {
				const auto image = record.type == 0U ? impl->directionalShadowDepths[record.slot].image
					: (record.type == 1U ? impl->pointShadowCubeDepths[record.slot].image
						: (record.type == 2U ? impl->spotShadowDepths[record.slot].image : impl->areaShadowDepths[record.slot].image));
				for(std::uint32_t face = 0; face < (record.type == 1U ? 6U : 1U); ++face)
					recordShadowValidationFace(record, image, face, 0);
				continue;
			}

			if (recordStaticComposition(record)) continue;
			if (record.type == 0u) {
				const glm::mat4 lightViewProjection = LoadShadowViewProjection(*record.light);
				if (RecordShadowPass(impl->directionalShadowFramebuffers[record.slot], impl->directionalShadowColors[record.slot].image, impl->directionalShadowDepths[record.slot].image, 0u, record.regionWidth, record.regionHeight, record, lightViewProjection)) record.recordedFaceMask |= 1U;
			}
			else if (record.type == 1u) {
				// Point lights render one atlas face per cubemap direction.
				for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
					if (record.validationIncompletePointFaces && faceIndex == 5U) {
						// This owned action omits the sixth native pass/draw/copy. The
						// untouched face cannot qualify a full-cube submitted owner.
						impl->lastShadowValidationIncompletePointFacesKey = incompletePointFacesKey;
						continue;
					}
					const glm::mat4 lightViewProjection = BuildPointShadowViewProjection(*record.light, faceIndex);
					if (RecordShadowPass(impl->pointShadowFaceFramebuffersBySlot[record.slot][faceIndex], impl->pointShadowCubeColors[record.slot].image, impl->pointShadowCubeDepths[record.slot].image, faceIndex, pointShadowSize, pointShadowSize, record, lightViewProjection)) record.recordedFaceMask |= static_cast<std::uint8_t>(1U << faceIndex);
				}
			}
			else if (record.type == 2u) {
				const glm::mat4 lightViewProjection = LoadShadowViewProjection(*record.light);
				if (RecordShadowPass(impl->spotShadowFramebuffers[record.slot], impl->spotShadowColors[record.slot].image, impl->spotShadowDepths[record.slot].image, 0u, record.regionWidth, record.regionHeight, record, lightViewProjection)) record.recordedFaceMask |= 1U;
			}
			else if (record.type == 3u) {
				const glm::mat4 lightViewProjection = LoadShadowViewProjection(*record.light);
				if (RecordShadowPass(impl->areaShadowFramebuffers[record.slot], impl->areaShadowColors[record.slot].image, impl->areaShadowDepths[record.slot].image, 0u, record.regionWidth, record.regionHeight, record, lightViewProjection)) record.recordedFaceMask |= 1U;
			}
		}
	}

	for (auto& record : shadowLightRecords)
	{
		const std::uint8_t requiredFaces = record.type == 1U ? 0x3FU : 1U;
		if (!record.reusedCachedResult && record.recordedFaceMask != 0U)
		{
			recordedAnyShadowPass = true;
			++pendingBudgetDiagnostics.cacheRefreshCount;
			TerraEngine::Renderer::ShadowRefreshCandidate candidate;
			candidate.view = record.qualifiedKey;
			candidate.contentToken = record.contentToken;
			candidate.frameSerial = publication->frameId; candidate.commandSerial = shadowCommandSerial;
			candidate.requiredFaceMask = requiredFaces; candidate.recordedFaceMask = record.recordedFaceMask;
			candidate.recordingComplete = record.recordedFaceMask == requiredFaces && record.preparedList != nullptr && record.preparedList->recordingReady;
			try
			{
				if (record.preparedList != nullptr) candidate.membership = record.preparedList->membership;
				// Record blocks every older alias even if completeness or proof fails.
				// Submission alone can promote a fully validated pending candidate.
				impl->shadowPublicationLedger.Record(candidate);
				impl->shadowPhysicalTokenHints[record.physicalRegionKey] = candidate.contentToken;
			}
			catch (...) { impl->shadowPublicationLedger.Reset(impl->renderSessionGeneration); impl->shadowPhysicalTokenHints.clear(); }
			if (record.validationIncompletePointFaces) {
				TerraEngine::Renderer::RendererEfficiencyFixtureActionReceipt action;
				action.injection = TerraEngine::Renderer::RendererEfficiencyFixtureInjection::IncompletePointFaces;
				action.correlation = presentation.efficiencyFixtureIntent.correlation;
				action.frameSerial = publication->frameId; action.commandSerial = shadowCommandSerial; action.recorded = true;
				action.requiredFaceMask = requiredFaces; action.recordedFaceMask = record.recordedFaceMask;
				action.oldAllocationGeneration = action.newAllocationGeneration = impl->shadowAllocationGeneration;
				action.priorOwnerRejected = record.validationPriorContentToken != 0
					&& !impl->shadowPublicationLedger.CanReuse(record.validationPriorPhysicalKey, record.validationPriorContentToken);
				action.reason = "Sixth native point shadow pass was intentionally omitted; incomplete cube cannot publish cache ownership.";
				impl->shadowValidationReadback.AttachFixtureAction(shadowCommandSerial, action);
			}
		}
		for (auto& assignment : pendingBudgetDiagnostics.shadowAssignments)
		{
			if (assignment.lightId != record.lightId || !assignment.assigned) continue;
			assignment.reusedCachedResult = record.reusedCachedResult;
			assignment.refreshedThisFrame = !record.reusedCachedResult && record.recordedFaceMask == requiredFaces;
			assignment.invalidatedThisFrame = record.invalidatedThisFrame;
			assignment.refreshReason = record.reusedCachedResult ? ShadowRefreshReason::CacheHit
				: (record.invalidatedThisFrame ? ShadowRefreshReason::CacheInvalidated : ShadowRefreshReason::RenderedThisFrame);
			break;
		}
	}


	if (recordedAnyShadowPass) {
		VkMemoryBarrier shadowBarrier{};
		shadowBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		shadowBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		shadowBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0,
			1, &shadowBarrier,
			0, nullptr,
			0, nullptr
		);
	}
	pendingBudgetDiagnostics.shadowPassRecorded = recordedAnyShadowPass;
	impl->RecordPendingBudgetDiagnostics(statsSlotIndex, pendingBudgetDiagnostics);
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.shadows.end" : "game.shadows.end");

	beginCommandRecordingPhase(
		"Vulkan: Primary Record Secondary Commands");
	// Record the composite before worker draw recording decides ownership. If
	// any graphics prerequisite fails, compute packets remain in the ordinary
	// hardware stream for this frame and no geometry can disappear.
	VkCommandBuffer virtualGeometryCompositeSecondary = VK_NULL_HANDLE;
	bool computeCompositeReady = false;
	const bool hasComputeOwnedDraw = std::any_of(
		prepared.begin(),
		prepared.end(),
		[](const PreparedDraw& draw)
		{
			return draw.rasterRoute
				== TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute;
		});
	if(productionVisibilityRecorded && hasComputeOwnedDraw
		&& impl->latestVirtualGeometryVisibilityResources
			.visibilityResolveDescriptorSet != VK_NULL_HANDLE
		&& currentFrame < impl->globalDescriptorSets.size()
		&& impl->globalDescriptorSets[currentFrame] != VK_NULL_HANDLE)
	{
		const VkPipeline compositePipeline =
			impl->EnsureVulkanVirtualGeometryCompositePipeline(
				renderPassInfo.renderPass);
		if(compositePipeline != VK_NULL_HANDLE)
		{
			const std::size_t workerIndex = prepared.size() + 3u;
			impl->EnsureWorkerPools(
				impl->currentFrame, workerIndex + 1u);
			VkCommandBuffer composite = impl->AcquireWorkerCommandBuffer(
				impl->currentFrame, workerIndex);
			if(composite != VK_NULL_HANDLE)
			{
				impl->reusableSecondaryCommandSignatures.erase(composite);
				vkResetCommandBuffer(composite, 0u);
				VkCommandBufferInheritanceInfo inheritance{};
				inheritance.sType =
					VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
				inheritance.renderPass = renderPassInfo.renderPass;
				inheritance.subpass = 0u;
				inheritance.framebuffer = renderPassInfo.framebuffer;
				VkCommandBufferBeginInfo begin{};
				begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				begin.flags =
					VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
				begin.pInheritanceInfo = &inheritance;
				if(vkBeginCommandBuffer(composite, &begin) == VK_SUCCESS)
				{
					VkViewport viewport{};
					viewport.width = static_cast<float>(
						renderPassInfo.renderArea.extent.width);
					viewport.height = static_cast<float>(
						renderPassInfo.renderArea.extent.height);
					viewport.maxDepth = 1.0f;
					vkCmdSetViewport(composite, 0u, 1u, &viewport);
					VkRect2D scissor{};
					scissor.extent = renderPassInfo.renderArea.extent;
					vkCmdSetScissor(composite, 0u, 1u, &scissor);
					vkCmdBindPipeline(
						composite,
						VK_PIPELINE_BIND_POINT_GRAPHICS,
						compositePipeline);
					const std::array<VkDescriptorSet, 2u> descriptorSets{
						impl->globalDescriptorSets[currentFrame],
						impl->latestVirtualGeometryVisibilityResources
							.visibilityResolveDescriptorSet };
					vkCmdBindDescriptorSets(
						composite,
						VK_PIPELINE_BIND_POINT_GRAPHICS,
						impl->virtualGeometryVisibilityPipelines.layout,
						0u,
						static_cast<std::uint32_t>(descriptorSets.size()),
						descriptorSets.data(),
						0u,
						nullptr);
					const std::array<std::uint32_t, 4u> constants{
						impl->latestVirtualGeometryVisibilityPlan.targetWidth,
						impl->latestVirtualGeometryVisibilityPlan.targetHeight,
						static_cast<std::uint32_t>(
							impl->latestVirtualGeometryVisibilityPlan
								.geometryGeneration),
						0u };
					vkCmdPushConstants(
						composite,
						impl->virtualGeometryVisibilityPipelines.layout,
						VK_SHADER_STAGE_COMPUTE_BIT |
							VK_SHADER_STAGE_FRAGMENT_BIT,
						0u,
						static_cast<std::uint32_t>(sizeof(constants)),
						constants.data());
					// One task group dispatches the mesh workgroup that emits the
					// procedural fullscreen composite triangle.
					vkCmdDrawMeshTasksEXT(composite, 1u, 1u, 1u);
					if(vkEndCommandBuffer(composite) == VK_SUCCESS)
					{
						virtualGeometryCompositeSecondary = composite;
						computeCompositeReady = true;
					}
				}
			}
		}
	}

	std::vector<VkCommandBuffer> secondaryBuffers;
	// Mesh chunks plus optional sky/grid/UI command buffers form a small, known
	// upper bound. Reserving once avoids vector growth inside primary recording.
	secondaryBuffers.reserve(prepared.size() + 3u);

	if (!prepared.empty()) {
		const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		// Hardware cores do not imply available task workers. In particular a
		// single executing worker must record inline rather than wait for its own
		// queued children. Zero workers also follows the synchronous fallback.
		const size_t recordingCapacity = std::min(hw,
			std::max<size_t>(1u, ThreadPool::instance().getThreadCount()));
		const size_t usefulWorkerCount = std::max<size_t>(
			1u,
			(prepared.size() + kVulkanDrawsPerRecordingWorker - 1u)
				/ kVulkanDrawsPerRecordingWorker);
		const size_t workerCount = prepared.size()
			< kVulkanParallelRecordingThreshold
			? 1u
			: std::min(usefulWorkerCount, recordingCapacity);
		const size_t chunkSize = (prepared.size() + workerCount - 1) / workerCount;

		impl->EnsureWorkerPools(impl->currentFrame, workerCount);
		std::vector<std::future<VkCommandBuffer>> futures;
		futures.reserve(workerCount);
		std::vector<VkCommandBuffer> futureSecondaryBuffers;
		std::vector<std::uint64_t> futureSecondarySignatures;
		std::vector<VkCommandBuffer> recordedSecondaryBuffers;
		futureSecondaryBuffers.reserve(workerCount);
		futureSecondarySignatures.reserve(workerCount);
		recordedSecondaryBuffers.reserve(workerCount);
		// The scratch vector belongs to the render thread's thread-local storage.
		// Worker jobs must capture that exact storage explicitly; resolving the
		// thread-local name on a worker would select the worker's empty instance.
		const std::vector<PreparedDraw>* preparedForRecording = &prepared;
		const auto BuildSecondaryRangeSignature = [&](VkRenderPass renderPass,
			VkFramebuffer framebuffer, VkExtent2D renderExtent,
			std::size_t start, std::size_t end) {
			std::uint64_t signature = 1469598103934665603ull;
			HashCombine(signature, impl->backendResourceGeneration);
			HashCombine(signature, impl->activeRenderFramePublication != nullptr
				? impl->activeRenderFramePublication->rendererSessionGeneration : 0u);
			HashCombine(signature, std::hash<VkRenderPass>{}(renderPass));
			HashCombine(signature, std::hash<VkFramebuffer>{}(framebuffer));
			HashCombine(signature, renderExtent.width);
			HashCombine(signature, renderExtent.height);
			HashCombine(signature, computeCompositeReady ? 1u : 0u);
			HashCombine(signature, start);
			HashCombine(signature, end);
			HashCombine(signature, std::hash<VkBuffer>{}(
				impl->indirectDrawBuffers[impl->currentFrame].buffer));
			HashCombine(signature, std::hash<VkBuffer>{}(
				impl->latestVirtualGeometryExecutorResources.generatedSubmissionBuffer));
			for(std::size_t drawIndex = start; drawIndex < end; ++drawIndex) {
				const PreparedDraw& draw = prepared[drawIndex];
				HashCombine(signature, std::hash<VkPipeline>{}(draw.pipeline));
				HashCombine(signature, std::hash<VkPipelineLayout>{}(draw.pipelineLayout));
				HashCombine(signature, draw.meshletCount);
				HashCombine(signature, draw.pushConstantRangeCount);
				for(std::uint32_t rangeIndex = 0u;
					rangeIndex < draw.pushConstantRangeCount; ++rangeIndex) {
					const VkPushConstantRange& range = draw.pushConstantRanges[rangeIndex];
					HashCombine(signature, range.stageFlags);
					HashCombine(signature, range.offset);
					HashCombine(signature, range.size);
				}
				for(const VkDescriptorSet descriptor : draw.descriptorSets) {
					HashCombine(signature, std::hash<VkDescriptorSet>{}(descriptor));
				}
				HashBytes(signature, &draw.push, sizeof(draw.push));
				HashCombine(signature, draw.indirectBufferIndex);
				HashCombine(signature, draw.useGeneratedVirtualGeometryIndirect ? 1u : 0u);
				HashCombine(signature, draw.generatedVirtualGeometryIndirectIndex);
				HashCombine(signature, static_cast<std::uint32_t>(draw.rasterRoute));
			}
			return TerraEngine::Renderer::Vulkan::BuildSecondaryCommandReuseSignature({
				impl->activeRenderFramePublication != nullptr
					? impl->activeRenderFramePublication->rendererSessionGeneration : 0u,
				impl->backendResourceGeneration,
				std::hash<VkRenderPass>{}(renderPass),
				std::hash<VkFramebuffer>{}(framebuffer),
				signature,
				renderExtent.width,
				renderExtent.height });
		};
		auto recordSecondaryRange = [this, computeCompositeReady, preparedForRecording](VkCommandBuffer secondary, VkRenderPass renderPass, VkFramebuffer framebuffer, VkExtent2D renderExtent, size_t start, size_t end) -> VkCommandBuffer {

			if (secondary == VK_NULL_HANDLE) return VK_NULL_HANDLE;
			vkResetCommandBuffer(secondary, 0);

			VkCommandBufferInheritanceInfo inherit{};
			inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
			inherit.renderPass = renderPass;
			inherit.subpass = 0;
			inherit.framebuffer = framebuffer;

			VkCommandBufferBeginInfo begin{};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			// Secondary draw buffers are recorded once for the current frame slot and
			// are reset only after that slot's fence completes in BeginFrame. That
			// lifetime means they are never submitted for simultaneous execution, so
			// avoid VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT and the optional
			// nestedCommandBufferSimultaneousUse device feature it requires.
			begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
			begin.pInheritanceInfo = &inherit;

			if(vkBeginCommandBuffer(secondary, &begin) != VK_SUCCESS) {
				return VK_NULL_HANDLE;
			}
			VkViewport viewport{};
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = static_cast<float>(renderExtent.width);
			viewport.height = static_cast<float>(renderExtent.height);
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vkCmdSetViewport(secondary, 0u, 1u, &viewport);

			VkRect2D scissor{};
			scissor.offset = { 0, 0 };
			scissor.extent = renderExtent;
			vkCmdSetScissor(secondary, 0u, 1u, &scissor);
			VkPipeline boundPipeline = VK_NULL_HANDLE;
			VkPipelineLayout boundDescriptorLayout = VK_NULL_HANDLE;
			std::array<VkDescriptorSet, 2u> boundDescriptorSets{};
			bool descriptorsBound = false;


			for (size_t i = start; i < end; ++i) {
				const PreparedDraw& draw = (*preparedForRecording)[i];
				if(computeCompositeReady
					&& draw.rasterRoute
						== TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute)
				{
					continue;
				}
				if (draw.pipeline == VK_NULL_HANDLE
					|| draw.pipelineLayout == VK_NULL_HANDLE
					|| draw.meshletCount == 0u) {
					continue;
				}

				if(boundPipeline != draw.pipeline)
				{
					vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS,
						draw.pipeline);
					boundPipeline = draw.pipeline;
				}
				if(!descriptorsBound || boundDescriptorLayout != draw.pipelineLayout
					|| boundDescriptorSets != draw.descriptorSets)
				{
					vkCmdBindDescriptorSets(secondary,
						VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipelineLayout, 0,
						static_cast<uint32_t>(draw.descriptorSets.size()),
						draw.descriptorSets.data(), 0, nullptr);
					boundDescriptorLayout = draw.pipelineLayout;
					boundDescriptorSets = draw.descriptorSets;
					descriptorsBound = true;
				}
				PushDefaultMeshConstants(
					secondary,
					draw.pipelineLayout,
					draw.pushConstantRanges.data(),
					draw.pushConstantRangeCount,
					draw.push);

				const VkBuffer commandBuffer = draw.useGeneratedVirtualGeometryIndirect
					? impl->latestVirtualGeometryExecutorResources.generatedSubmissionBuffer
					: impl->indirectDrawBuffers[impl->currentFrame].buffer;
				const VkDeviceSize offset = draw.useGeneratedVirtualGeometryIndirect
					? static_cast<VkDeviceSize>(draw.generatedVirtualGeometryIndirectIndex) * 16u
					: static_cast<VkDeviceSize>(draw.push.indirectDrawOffset + draw.indirectBufferIndex) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
				vkCmdDrawMeshTasksIndirectEXT(secondary, commandBuffer, offset, 1, draw.useGeneratedVirtualGeometryIndirect ? 16u : sizeof(VkDrawMeshTasksIndirectCommandEXT));
			}
			return vkEndCommandBuffer(secondary) == VK_SUCCESS
				? secondary : VK_NULL_HANDLE;
		};

		// Construct after the recording callable and every referenced input. Any
		// later admission/result failure joins all recording children before those
		// stack captures expire. Prepared results are accepted only as a whole.
		TerraEngine::Tasks::ScopedTaskSettlement<VkCommandBuffer> recordingSettlement(
			futures, TerraEngine::Tasks::NestedTaskGroup::VulkanSecondaryRecording);


		for (size_t taskIndex = 0; taskIndex < workerCount; ++taskIndex) {
			const size_t start = taskIndex * chunkSize;
			if (start >= prepared.size()) break;
			const size_t end = std::min(prepared.size(), start + chunkSize);
			VkFramebuffer framebuffer = renderPassInfo.framebuffer;
			VkRenderPass renderPass = renderPassInfo.renderPass;
			const VkExtent2D renderExtent = renderPassInfo.renderArea.extent;
			const uint32_t frameIndex = currentFrame;
			VkCommandBuffer secondary = impl->AcquireWorkerCommandBuffer(frameIndex, taskIndex);
			const std::uint64_t secondarySignature = BuildSecondaryRangeSignature(
				renderPass, framebuffer, renderExtent, start, end);
			const auto cachedSecondary =
				impl->reusableSecondaryCommandSignatures.find(secondary);
			if(cachedSecondary != impl->reusableSecondaryCommandSignatures.end()
				&& TerraEngine::Renderer::Vulkan::CanReuseSecondaryCommands(
					cachedSecondary->second, secondarySignature, true)) {
				secondaryBuffers.push_back(secondary);
				continue;
			}
			impl->reusableSecondaryCommandSignatures.erase(secondary);
			if (workerCount == 1u) {
				if (VkCommandBuffer recorded = recordingSettlement.RunInline([&] {
					return recordSecondaryRange(secondary, renderPass, framebuffer, renderExtent, start, end);
				}); recorded != VK_NULL_HANDLE) {
					secondaryBuffers.push_back(recorded);
					impl->reusableSecondaryCommandSignatures[recorded] = secondarySignature;
				}
			}
			else {
				recordingSettlement.CreateAndEnqueue(ThreadPool::instance(), [secondary, renderPass, framebuffer, renderExtent, start, end, &recordSecondaryRange]() -> VkCommandBuffer {
					return recordSecondaryRange(secondary, renderPass, framebuffer, renderExtent, start, end);
				});
				futureSecondaryBuffers.push_back(secondary);
				futureSecondarySignatures.push_back(secondarySignature);
			}
		}

		// Collect every completion before publishing any newly recorded command.
		recordingSettlement.Collect(recordedSecondaryBuffers);
		for (std::size_t futureIndex = 0u; futureIndex < recordedSecondaryBuffers.size(); ++futureIndex) {
			VkCommandBuffer secondary = recordedSecondaryBuffers[futureIndex];
			if (secondary != VK_NULL_HANDLE) {
				secondaryBuffers.push_back(secondary);
				impl->reusableSecondaryCommandSignatures[secondary] =
					futureSecondarySignatures[futureIndex];
			}
			else if(futureIndex < futureSecondaryBuffers.size()) {
				impl->reusableSecondaryCommandSignatures.erase(
					futureSecondaryBuffers[futureIndex]);
			}
		}
	}


	if (presentation.sky.renderBackground)
	{
		const size_t workerIndex = secondaryBuffers.size();
		impl->EnsureWorkerPools(impl->currentFrame, workerIndex + 1);
		VkCommandBuffer skyCmd = impl->AcquireWorkerCommandBuffer(impl->currentFrame, workerIndex);

		if (skyCmd != VK_NULL_HANDLE)
		{
			impl->reusableSecondaryCommandSignatures.erase(skyCmd);
			vkResetCommandBuffer(skyCmd, 0);

			VkCommandBufferInheritanceInfo inherit{};
			inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
			inherit.renderPass = renderPassInfo.renderPass;
			inherit.subpass = 0;
			inherit.framebuffer = renderPassInfo.framebuffer;

			VkCommandBufferBeginInfo begin{};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			// The sky secondary buffer follows the same per-frame fence lifetime as
			// mesh worker buffers, so it can be executed once by the primary command
			// buffer without requesting Vulkan's optional nested simultaneous-use feature.
			begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
			begin.pInheritanceInfo = &inherit;

			vkBeginCommandBuffer(skyCmd, &begin);
			impl->RenderSkyBackground(skyCmd, *publicationPresentation,
				renderPassInfo.renderArea.extent);
			vkEndCommandBuffer(skyCmd);
			secondaryBuffers.insert(secondaryBuffers.begin(), skyCmd);
		}
	}

	// Render grid (Scene View only).

	if (view == ViewType::Scene && presentation.sceneViewGridVisible) {
		const size_t workerIndex = secondaryBuffers.size();
		impl->EnsureWorkerPools(impl->currentFrame, workerIndex + 1);

		VkCommandBuffer gridCmd = impl->AcquireWorkerCommandBuffer(impl->currentFrame, workerIndex);

		if (gridCmd != VK_NULL_HANDLE) {
			impl->reusableSecondaryCommandSignatures.erase(gridCmd);
			vkResetCommandBuffer(gridCmd, 0);

			VkCommandBufferInheritanceInfo inherit{};
			inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
			inherit.renderPass = renderPassInfo.renderPass;
			inherit.subpass = 0;
			inherit.framebuffer = renderPassInfo.framebuffer;

			VkCommandBufferBeginInfo begin{};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			// The scene-grid secondary buffer is reset with the current frame's worker
			// pool after the GPU fence is signaled, so render-pass continuation is the
			// only usage flag needed for valid nested execution.
			begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
			begin.pInheritanceInfo = &inherit;

			vkBeginCommandBuffer(gridCmd, &begin);
			impl->RenderSceneViewGrid(gridCmd, *publicationPresentation,
				renderPassInfo.renderArea.extent);
			vkEndCommandBuffer(gridCmd);
			secondaryBuffers.push_back(gridCmd);
		}
	}

	// Runtime UI belongs to the Game view only. The Scene view must stay free of
	// authored UI so editor camera work, picking, and gizmos are never obscured
	// by the game's overlay.
	if (view == ViewType::Game) {
		const size_t workerIndex = secondaryBuffers.size();
		impl->EnsureWorkerPools(impl->currentFrame, workerIndex + 1);
		VkCommandBuffer uiCmd = impl->AcquireWorkerCommandBuffer(impl->currentFrame, workerIndex);

		if (uiCmd != VK_NULL_HANDLE) {
			impl->reusableSecondaryCommandSignatures.erase(uiCmd);
			vkResetCommandBuffer(uiCmd, 0);

			VkCommandBufferInheritanceInfo inherit{};
			inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
			inherit.renderPass = renderPassInfo.renderPass;
			inherit.subpass = 0;
			inherit.framebuffer = renderPassInfo.framebuffer;

			VkCommandBufferBeginInfo begin{};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			// Runtime UI records into a per-frame secondary command buffer that is not
			// submitted concurrently, so keep the flags limited to render-pass
			// continuation and avoid requiring nestedCommandBufferSimultaneousUse.
			begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
			begin.pInheritanceInfo = &inherit;

			vkBeginCommandBuffer(uiCmd, &begin);
			impl->CreateUIPipelines(renderPassInfo.renderPass);
			// Secondary command buffers do not inherit viewport/scissor state.
			// Use this exact Game target, including offscreen editor targets, so
			// UI geometry and the published logical pointer mapping agree.
			VkViewport uiViewport{};
			uiViewport.width = static_cast<float>(activeRenderExtent.width);
			uiViewport.height = static_cast<float>(activeRenderExtent.height);
			uiViewport.minDepth = 0.0f;
			uiViewport.maxDepth = 1.0f;
			vkCmdSetViewport(uiCmd, 0u, 1u, &uiViewport);
			VkRect2D uiScissor{};
			uiScissor.extent = activeRenderExtent;
			vkCmdSetScissor(uiCmd, 0u, 1u, &uiScissor);
			impl->RenderUI(uiCmd, publicationPresentation->uiBatch);
			vkEndCommandBuffer(uiCmd);
			secondaryBuffers.push_back(uiCmd);
		}
	}
	if(virtualGeometryCompositeSecondary != VK_NULL_HANDLE)
	{
		// Composite after opaque hardware packets so its explicit SV_Depth can
		// resolve against conventional geometry in the shared depth attachment.
		secondaryBuffers.push_back(virtualGeometryCompositeSecondary);
	}

	VkSubpassContents contents = secondaryBuffers.empty() ? VK_SUBPASS_CONTENTS_INLINE : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
	if (!impl->editorMode
		&& !renderingDedicatedCaptureTarget
		&& impl->depthImage != VK_NULL_HANDLE) {
		// Every swapchain framebuffer references the same depth image. Order its
		// previous frame write before this frame clears it, even when a different
		// frame-in-flight fence owns the earlier command buffer.
		VkImageMemoryBarrier depthReuseBarrier{};
		depthReuseBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		depthReuseBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthReuseBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthReuseBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthReuseBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthReuseBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		depthReuseBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		depthReuseBarrier.image = impl->depthImage;
		depthReuseBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depthReuseBarrier.subresourceRange.baseMipLevel = 0u;
		depthReuseBarrier.subresourceRange.levelCount = 1u;
		depthReuseBarrier.subresourceRange.baseArrayLayer = 0u;
		depthReuseBarrier.subresourceRange.layerCount = 1u;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_DEPENDENCY_BY_REGION_BIT,
			0u, nullptr,
			0u, nullptr,
			1u, &depthReuseBarrier);
	}
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.mainpass.begin" : "game.mainpass.begin");
	if(hasDirectGeometryCommands)
	{
		// The direct-resident route uses the ordinary mesh-task/indexed command
		// stream. Bracket its completed main render pass with a dedicated query so
		// low-poly calibration compares real GPU work, not CPU command recording.
		impl->MarkVirtualGeometryTimestampBegin(
			cmd,
			impl->currentFrame,
			view,
			TerraEngine::Renderer::VirtualGeometryBackendStage::HardwareRaster,
			TerraEngine::Renderer::VirtualGeometryBackendRoute::Indexed,
			impl->backendFrameSerial,
			impl->backendFrameSerial,
			impl->backendFrameSerial);
	}
	beginCommandRecordingPhase(
		"Vulkan: Primary Finalize Render Pass");
	vkCmdBeginRenderPass(cmd, &renderPassInfo, contents);

	if (!secondaryBuffers.empty()) {
		vkCmdExecuteCommands(cmd, static_cast<uint32_t>(secondaryBuffers.size()), secondaryBuffers.data());
	}

	vkCmdEndRenderPass(cmd);
	if(hasDirectGeometryCommands)
	{
		impl->MarkVirtualGeometryTimestampEnd(
			cmd,
			impl->currentFrame,
			view,
			TerraEngine::Renderer::VirtualGeometryBackendStage::HardwareRaster);
	}
	if (impl->editorMode || renderingDedicatedCaptureTarget) {
		Impl::OffscreenResources& identityTarget = renderingDedicatedCaptureTarget
			? impl->captureView
			: (view == ViewType::Scene ? impl->sceneView : impl->gameView);
		// This metadata is CPU-side only, but it binds a later fence-completed
		// uint4 to the immutable frame/session that rendered the attachment.
		// Direct meshes may legitimately publish no VG packets. The renderer still
		// owns one immutable frame snapshot, so always stamp its explicit frame id
		// instead of substituting a backend-only serial that automation cannot map.
		identityTarget.identityFrameId = publication->frameId != 0u
			? publication->frameId : impl->backendFrameSerial;
		identityTarget.identityRendererGeneration = impl->renderSessionGeneration;
		identityTarget.identityOriginGeneration =
			presentation.referenceFrame.originGeneration;
		identityTarget.identitySpatialGeneration =
			presentation.referenceFrame.spatialGeneration;
		identityTarget.identityResidencyGeneration =
			presentation.referenceFrame.residencyGeneration;
	}
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.mainpass.end" : "game.mainpass.end");
	if(shadowValidationRequested)
	{
		// The native offscreen attachment has finished its render pass. Record
		// one color copy for this view and share its completed bytes across faces.
		const bool offscreenColor = impl->editorMode || renderingDedicatedCaptureTarget;
		const VkImage color = offscreenColor
			? (renderingDedicatedCaptureTarget ? impl->captureView.image
				: (view == ViewType::Scene ? impl->sceneView.image : impl->gameView.image))
			: impl->swapchainImages[imageIndex];
		const VkImageLayout layout = offscreenColor
			? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		// Match staging bytes to the actual image: an RGBA16F offscreen attachment
		// occupies twice the bytes of a typical BGRA8 swapchain attachment.
		const VkFormat colorFormat = offscreenColor ? impl->offscreenColorFormat : impl->swapchainImageFormat;
		impl->shadowValidationReadback.RecordFrameColor(cmd, color, colorFormat,
			activeRenderExtent.width, activeRenderExtent.height, layout,
			{impl->renderSessionGeneration, publication->frameId, shadowCommandSerial, renderViewType, diagnosticViewId});
	}

	// VRS Generation for next frame

	if (impl->UsesVrsAttachment()) {
		const VkImageView vrsColorView = renderingDedicatedCaptureTarget
			? impl->captureView.view
			: ((view == ViewType::Scene) ? impl->sceneView.view : impl->gameView.view);
		impl->GenerateVRSImage(cmd, vrsColorView);
	}
	if (kEnableRuntimeScreenSpaceAo
		&& !impl->editorMode
		&& runtimeAoHistoryPass
		&& presentation.settings.ambientOcclusionStrength > 0.001f
		&& presentation.settings.ambientOcclusionQuality > 0u) {
		impl->StoreAmbientOcclusionDepthHistory(cmd, activeDepthImage, activeRenderExtent, viewIndex, currentViewProjection);
	}
	// Generate the pyramid after scene depth is complete. Its task-shader
	// consumer has a fixed four-load proof budget, so enabling Hi-Z cannot create
	// the unbounded startup workload that previously triggered driver watchdogs.
	constexpr bool kEnableHiZMipGeneration = true;
	if (kEnableHiZMipGeneration) {
		impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.hiz.begin" : "game.hiz.begin");
		// Build Hi-Z after the depth attachment contains the final scene depth.
		// The next frame can then use this stable pyramid for conservative occlusion.
		if (impl->GenerateHiZPyramid(writableFrameHiZ, cmd, activeDepthImage, activeDepthView, activeRenderExtent, snapshotSlotIndex, currentHiZHistory)) {
			impl->MarkHiZWritePending(hiZViewIndex, impl->currentFrame);
		}
		impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.hiz.end" : "game.hiz.end");
	}
	impl->MarkGpuTimestampEnd(cmd, impl->currentFrame, view);
	impl->RecordDiagnosticCheckpoint(cmd, view == ViewType::Scene ? "scene.primary.end" : "game.primary.end");

	const VkResult primaryRecordingResult = vkEndCommandBuffer(cmd);
	if (primaryRecordingResult != VK_SUCCESS)
	{
		impl->MarkDeviceLost(primaryRecordingResult, "primary shadow/view recording completion");
		return;
	}
	commandRecordingPhaseProfile.reset();
	commandRecordingProfile.reset();
	const float commandRecordingMs = ElapsedMilliseconds(commandRecordingStart);
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::PrimaryCommandRecording, TerraEngine::Renderer::BackendSubmitStage::CommandRecording, renderViewType, diagnosticViewId, "Primary command recording", false, true },
		commandRecordingMs);
	TERRAENGINE_PROFILE_SCOPE("Vulkan: RenderFrame Finalize View Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::ViewSubmission, TerraEngine::Renderer::BackendSubmitStage::ViewSubmit, renderViewType, diagnosticViewId, "View Submission", false, true },
		ElapsedMilliseconds(renderFrameStart));
	std::uint64_t passSetHash = 1469598103934665603ull;
	HashCombine(passSetHash, static_cast<std::uint64_t>(
		impl->activeRenderFramePublication->submissionRanges.size()));
	HashCombine(passSetHash, static_cast<std::uint64_t>(publicationPresentation->shadowDrawLists.size()));
	HashCombine(passSetHash, static_cast<std::uint64_t>(publicationPresentation->uiBatch.staticQuads.size()));
	HashCombine(passSetHash, static_cast<std::uint64_t>(publicationPresentation->uiBatch.textRuns.size()));

	std::uint64_t cameraHash = 1469598103934665603ull;
	HashMat4(cameraHash, publicationPresentation->viewMatrix);
	HashMat4(cameraHash, publicationPresentation->projectionMatrix);
	HashVec4(cameraHash, glm::vec4(publicationPresentation->cameraLocalPosition, 1.0f));

	std::uint64_t targetHash = 1469598103934665603ull;
	HashCombine(targetHash, static_cast<std::uint64_t>(outputRequest.kind));
	HashCombine(targetHash, static_cast<std::uint64_t>(renderViewType));
	HashCombine(targetHash, static_cast<std::uint64_t>(outputRequest.outputId));
	HashCombine(targetHash, static_cast<std::uint64_t>(outputRequest.targetSize.x));
	HashCombine(targetHash, static_cast<std::uint64_t>(outputRequest.targetSize.y));

	std::uint64_t overlayHash = 1469598103934665603ull;
	HashVec4(overlayHash, presentation.editorContextTint);
	std::string sharedWorkKey = std::to_string(passSetHash) + ":"
		+ std::to_string(cameraHash) + ":"
		+ std::to_string(targetHash);
	TerraEngine::Renderer::ViewSubmitWorkItem submitWork = TerraEngine::Renderer::Vulkan::BuildVulkanViewSubmitWorkItem(
		impl->pendingBackendFrameBreakdown.frameId,
		{ TerraEngine::Renderer::BackendFrameCostCategory::ViewSubmission, TerraEngine::Renderer::BackendSubmitStage::ViewSubmit, renderViewType, diagnosticViewId, "View submit work", false, true },
		passSetHash,
		overlayHash,
		commandRecordingMs,
		0.0f,
		std::move(sharedWorkKey));
	submitWork.cameraId = cameraHash;
	submitWork.targetId = targetHash;
	if (!impl->pendingBackendFrameBreakdown.viewSubmitWork.empty())
	{
		std::string reuseReason;
		submitWork.reuseDecision = TerraEngine::Renderer::CompareViewSubmitWork(impl->pendingBackendFrameBreakdown.viewSubmitWork.front(), submitWork, &reuseReason);
		submitWork.reuseReason = reuseReason;
	}
	else
	{
		submitWork.reuseDecision = TerraEngine::Renderer::ViewSubmitReuseDecision::Separate;
		submitWork.reuseReason = "First submitted view owns the initial backend work for this frame.";
	}
	TerraEngine::Renderer::AddViewSubmitWorkItem(impl->pendingBackendFrameBreakdown, std::move(submitWork));
	// Record the command buffer to be submitted at EndFrame so multiple cameras
	// render sequentially before a single present.

	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffersSubmitted.size()) {
		impl->pendingPrimaryCommandBuffersSubmitted.resize(impl->MAX_FRAMES_IN_FLIGHT, 0u);
	}
	impl->pendingPrimaryCommandBuffers[impl->currentFrame].push_back(cmd);
	shadowRecordingSettlement.admitted = true;
	impl->activeRenderSnapshot.state = RenderSnapshotState::Recorded;

	if (!impl->pendingRenderSnapshotDiagnostics.ownerSummaries.empty()) {
		impl->pendingRenderSnapshotDiagnostics.ownerSummaries.back().state = RenderSnapshotState::Recorded;
	}

	depthHistory.lastViewProjectionMatrix = currentViewProjection;
	depthHistory.valid = true;
}
