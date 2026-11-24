// VulkanGpuDiagnostics.cpp — GPU timestamps, completed statistics, and validation capture
//
// This file owns one focused Vulkan resource responsibility. Allocations and retirement
// still flow through VulkanContext::Impl so frame-fence lifetime rules remain unchanged.

#include "VulkanResourceSource.h"
#include "VulkanPostProcessPasses.h"
#include <cmath>

namespace
{
	using namespace TerraEngine::Renderer;
	using namespace TerraEngine::Renderer::GraphicsSettings;

	PostProcessEffectIdentity ColorTimingEffect(const RenderViewPresentationInput& view, std::uint32_t stage)
	{
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::SpatialAA))
			return {"spatial-aa", PostProcessPassKind::SpatialAA};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::Presentation))
			return {"presentation-lens", PostProcessPassKind::PresentationEffects};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::Temporal))
			return {"temporal-resolve", PostProcessPassKind::TemporalResolve};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::Atmosphere))
			return {"volumetric-fog-and-shafts", PostProcessPassKind::HdrCamera};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::OutputDither))
			return {"output-dither", PostProcessPassKind::OverlayComposite};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::DepthOfField))
			return {"depth-of-field", PostProcessPassKind::HdrCamera};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::LensRadiance))
			return {"lens-radiance", PostProcessPassKind::HdrCamera};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::LensDirt))
			return {"lens-dirt", PostProcessPassKind::HdrCamera};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::LensFlare))
			return {"lens-flare", PostProcessPassKind::HdrCamera};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::MotionBlur))
			return {"motion-blur", PostProcessPassKind::HdrCamera};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::MotionDepthHistory))
			return {"motion-depth-history", PostProcessPassKind::HdrCamera};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::ScreenAO))
			return {"gtao", PostProcessPassKind::ScreenSpaceLighting};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::ScreenReflections))
			return {"ssr", PostProcessPassKind::ScreenSpaceLighting};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::ScreenIndirect))
			return {"ssgi", PostProcessPassKind::ScreenSpaceLighting};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::ScreenComposite))
			return {"screen-space-composite", PostProcessPassKind::ScreenSpaceLighting};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::ScreenDepthHierarchy))
			return {"screen-depth-hierarchy", PostProcessPassKind::ScreenSpaceLighting};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::CustomHdr))
			return {"custom-hdr", PostProcessPassKind::CustomFullscreen};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::CustomDisplay))
			return {"custom-display", PostProcessPassKind::CustomFullscreen};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::CustomBeforeUI))
			return {"custom-before-ui", PostProcessPassKind::CustomFullscreen};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::ToneAndGrade))
			return {"tone-and-grade", PostProcessPassKind::ToneAndGrade};
		if(stage == static_cast<std::uint32_t>(Vulkan::PostProcessStage::Bloom))
			return {"bloom", PostProcessPassKind::HdrCamera};
		const auto color = view.graphicsSettings
			? ResolvePostProcessColorSettings(view.graphicsSettings->values) : PostProcessColorSettings{};
		if(stage == 0 && view.graphicsSettings && BuildPostProcessGraphSettings(*view.graphicsSettings).spatialAA)
			return {"post-process-chain", PostProcessPassKind::SpatialAA};
		return {stage == 0 && (color.HasBloom() || color.HasToneAndGrading() || color.exposure == ExposureMode::Automatic)
			? "color-chain" : "exposure", PostProcessPassKind::HdrCamera};
	}
}

TerraEngine::Renderer::GraphicsSettings::PostProcessDiagnosticsSnapshot
VulkanContext::GetPostProcessDiagnostics(
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
	const TerraEngine::Renderer::GraphicsSettings::GraphicsRequestReceipt* correlatedRequest) const
{
	using namespace TerraEngine::Renderer;
	using namespace TerraEngine::Renderer::GraphicsSettings;
	PostProcessDiagnosticsSnapshot snapshot;
	snapshot.view.viewType = presentation.output.diagnosticsView;
	snapshot.view.outputKind = presentation.output.kind;
	snapshot.view.outputId = presentation.output.outputId;
	snapshot.frameId = presentation.referenceFrame.frameId;
	if(presentation.graphicsSettings)
	{
		snapshot.lifecycle = presentation.graphicsSettings->lifecycle;
		snapshot.settingsRevision = presentation.graphicsSettings->revision;
		if(presentation.graphicsSettings->target.view)
			snapshot.view.lifetimeGeneration = presentation.graphicsSettings->target.view->lifetimeGeneration;
	}
	if(presentation.graphicsView) snapshot.view = *presentation.graphicsView;
	PostProcessEffectDiagnostic diagnostic;
	diagnostic.effect = ColorTimingEffect(presentation, 0u);
	diagnostic.timing.effect = diagnostic.effect;
	diagnostic.timing.owner.lifecycle = snapshot.lifecycle;
	diagnostic.timing.owner.view = snapshot.view;
	diagnostic.timing.owner.settingsRevision = snapshot.settingsRevision;
	diagnostic.timing.owner.frameId = snapshot.frameId;
	const auto finish = [&]() {
		snapshot.effects.push_back(std::move(diagnostic));
		return snapshot;
	};
	// Rejection describes the requested operation. Its current view can still
	// have an active prior setting, whose GPU duration belongs to that setting.
	if(correlatedRequest && correlatedRequest->state == GraphicsRequestState::Rejected
		&& std::any_of(correlatedRequest->affectedSettings.begin(), correlatedRequest->affectedSettings.end(),
			[](const auto id) { return id >= GraphicsSettingId::AntiAliasingMode; }))
	{
		diagnostic.state = PostProcessEffectState::Rejected;
		diagnostic.reason = correlatedRequest->reason == GraphicsRequestReason::Unsupported
			? PostProcessDiagnosticReason::UnsupportedDevice : PostProcessDiagnosticReason::InvalidConfiguration;
		diagnostic.requestId = correlatedRequest->requestId;
		diagnostic.detail = correlatedRequest->detail;
		diagnostic.timing.reason = diagnostic.reason;
		return finish();
	}
	const auto plan = presentation.graphicsSettings
		? BuildPostProcessGraphSettings(*presentation.graphicsSettings) : PostProcessGraphSettings{};
	if(!plan.hdrCamera && !plan.toneAndGrade && !plan.spatialAA && !plan.coverageAA
		&& !plan.temporalResolve && !plan.screenSpaceLighting && !plan.presentationEffects && !plan.customStages && !plan.outputDithering)
	{
		diagnostic.detail = "Color processing is disabled in this view's effective settings.";
		diagnostic.timing.reason = PostProcessDiagnosticReason::Disabled;
		return finish();
	}
	diagnostic.state = PostProcessEffectState::Unavailable;
	diagnostic.reason = PostProcessDiagnosticReason::MissingResources;
	diagnostic.timing.availability = PostProcessTimingAvailability::Unavailable;
	const auto unavailable = [&](PostProcessDiagnosticReason reason, const char* detail) {
		diagnostic.reason = reason;
		diagnostic.detail = detail;
		diagnostic.timing.reason = reason;
		return finish();
	};
	if(!impl) return unavailable(PostProcessDiagnosticReason::MissingResources, "The native renderer owner is unavailable.");
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if(!impl->initialized || impl->deviceLost
		|| snapshot.lifecycle.rendererSessionGeneration != impl->renderSessionGeneration
		|| snapshot.lifecycle.backendResourceGeneration != impl->backendResourceGeneration)
		return unavailable(PostProcessDiagnosticReason::StaleLifetime, "This effective view belongs to an unavailable renderer lifetime.");
	if(!std::isfinite(plan.fixedExposureCompensation) || !Vulkan::SupportsPostProcessPlan(plan))
		return unavailable(PostProcessDiagnosticReason::InvalidConfiguration, "This view requests an unsupported color configuration.");
	if(!plan.executionAvailable)
		return unavailable(PostProcessDiagnosticReason::MissingResources, "This effective setting has no certified native resource generation.");
	const auto key = Vulkan::MakePostProcessTargetKey(presentation, snapshot.lifecycle.rendererSessionGeneration);
	const auto* target = impl->FindPostProcessTargets(key);
	if(!target || !target->passes)
		return unavailable(PostProcessDiagnosticReason::MissingResources, "This view has no prepared native image pipeline.");
	const auto& source = presentation.output.kind == RenderOutputKind::OffscreenTexture
		? impl->captureView : (presentation.output.diagnosticsView == RenderViewType::Scene
			? impl->sceneView : impl->gameView);
	const auto& output = target->passes->output;
	if(!Vulkan::IsPostProcessOutputCurrent(output, source.image, source.generation, source.colorFrameId)
		|| output.publicationFrameId != snapshot.frameId || output.settingsRevision != snapshot.settingsRevision)
		return unavailable(PostProcessDiagnosticReason::MissingResources, "This view has no matching accepted output for its effective frame and revision.");
	diagnostic.state = PostProcessEffectState::Active;
	diagnostic.reason = PostProcessDiagnosticReason::None;
	diagnostic.detail = "The configured color stages executed in the accepted native view.";
	diagnostic.timing.owner.targetGeneration = target->generation;
	diagnostic.timing.availability = PostProcessTimingAvailability::Unavailable;
	diagnostic.timing.reason = PostProcessDiagnosticReason::MissingMarkers;
	const auto matches = [&](const PostProcessTimingSample& sample) {
		return !sample.effect.stableId.empty() && sample.owner.lifecycle == snapshot.lifecycle
			&& sample.owner.view == snapshot.view && sample.owner.settingsRevision == snapshot.settingsRevision
			&& sample.owner.frameId == snapshot.frameId && sample.owner.targetGeneration == target->generation;
	};
	// An on-demand query may expose pending evidence but never a numeric duration
	// before the exact submitted command's completion and query availability.
	std::vector<PostProcessEffectDiagnostic> stages;
	const auto keepTiming = [&](const PostProcessTimingSample& sample) {
		if(!matches(sample)) return;
		if(sample.effect == diagnostic.effect)
		{
			diagnostic.timing = sample;
			return;
		}
		const auto previous = std::find_if(stages.begin(), stages.end(), [&](const auto& stage) { return stage.effect == sample.effect; });
		if(previous != stages.end())
		{
			previous->timing = sample;
			return;
		}
		PostProcessEffectDiagnostic effect;
		effect.effect = sample.effect;
		effect.state = PostProcessEffectState::Active;
		effect.timing = sample;
		stages.push_back(std::move(effect));
	};
	for(const auto& slot : impl->postProcessTimestampSlots)
		if(slot.occupied) keepTiming(slot.sample);
	{
		std::lock_guard timingLock(impl->statsMutex);
		for(const auto& sample : impl->completedPostProcessTimingSamples)
			keepTiming(sample);
	}
	snapshot.effects.push_back(std::move(diagnostic));
	for(auto& stage : stages) snapshot.effects.push_back(std::move(stage));
	return snapshot;
}

bool VulkanContext::Impl::BeginPostProcessTimestamp(VkCommandBuffer command,
	std::uint32_t frameSlot, std::uint32_t snapshotSlot,
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
	std::uint64_t targetGeneration, std::uint32_t stage)
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	if(!command || frameSlot >= MAX_FRAMES_IN_FLIGHT || snapshotSlot >= MAX_RENDER_SNAPSHOTS_PER_FRAME
		|| stage >= kPostProcessStagesPerView || !presentation.graphicsSettings || !targetGeneration) return false;
	const auto index = (frameSlot * MAX_RENDER_SNAPSHOTS_PER_FRAME + snapshotSlot) * kPostProcessStagesPerView + stage;
	PostProcessTimingSample sample;
	sample.effect = ColorTimingEffect(presentation, stage);
	sample.owner.lifecycle = presentation.graphicsSettings->lifecycle;
	sample.owner.view.viewType = presentation.output.diagnosticsView;
	sample.owner.view.outputId = presentation.output.outputId;
	sample.owner.view.outputKind = presentation.output.kind;
	// Camera/view and executable image lifetimes advance independently. Keep an
	// unknown game-root view lifetime explicit while retaining the actual target.
	if(presentation.graphicsSettings->target.view)
		sample.owner.view.lifetimeGeneration = presentation.graphicsSettings->target.view->lifetimeGeneration;
	if(presentation.graphicsView) sample.owner.view = *presentation.graphicsView;
	sample.owner.targetGeneration = targetGeneration;
	sample.owner.settingsRevision = presentation.graphicsSettings->revision;
	sample.owner.frameId = presentation.referenceFrame.frameId;
	sample.owner.commandSerial = nextPostProcessTimestampCommandSerial++;
	sample.owner.frameSlot = frameSlot;
	sample.owner.snapshotSlot = snapshotSlot;
	sample.queries = {index * 2, 2};
	sample.nonemptyWork = true;
	const auto unavailable = [&](PostProcessDiagnosticReason reason) {
		sample.availability = PostProcessTimingAvailability::Unavailable;
		sample.reason = reason;
		std::lock_guard lock(statsMutex);
		completedPostProcessTimingSamples[index] = std::move(sample);
		return false;
	};
	if(deviceLost || sample.owner.lifecycle.rendererSessionGeneration != renderSessionGeneration
		|| sample.owner.lifecycle.backendResourceGeneration != backendResourceGeneration)
		return unavailable(PostProcessDiagnosticReason::StaleLifetime);
	if(postProcessTimestampSlots[index].occupied)
		return unavailable(PostProcessDiagnosticReason::QueryCapacityExceeded);
	if(!postProcessTimestampPoolAttempted)
	{
		postProcessTimestampPoolAttempted = true;
		// Format and resource preparation already qualified the executed pass.
		// Timing additionally requires the selected queue's actual timestamp bits.
		if(device && lightingDepthTimestampValidBits > 0 && lightingDepthTimestampValidBits <= 64
			&& std::isfinite(gpuTimestampPeriodNanoseconds) && gpuTimestampPeriodNanoseconds > 0)
		{
			VkQueryPoolCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			info.queryType = VK_QUERY_TYPE_TIMESTAMP;
			info.queryCount = kPostProcessTimingSlotCount * 2;
			if(vkCreateQueryPool(device, &info, nullptr, &postProcessTimestampQueryPool) == VK_SUCCESS)
				++postProcessTimestampPoolGeneration;
		}
	}
	if(!postProcessTimestampQueryPool)
	{
		const bool supported = device && lightingDepthTimestampValidBits > 0 && lightingDepthTimestampValidBits <= 64
			&& std::isfinite(gpuTimestampPeriodNanoseconds) && gpuTimestampPeriodNanoseconds > 0;
		return unavailable(supported ? PostProcessDiagnosticReason::MissingResources
			: PostProcessDiagnosticReason::UnsupportedDevice);
	}
	sample.owner.queryPoolGeneration = postProcessTimestampPoolGeneration;
	sample.timestampValidBits = lightingDepthTimestampValidBits;
	sample.timestampPeriodNanoseconds = gpuTimestampPeriodNanoseconds;
	sample.availability = PostProcessTimingAvailability::Pending;
	auto& slot = postProcessTimestampSlots[index];
	slot.sample = std::move(sample);
	slot.command = command;
	slot.fence = VK_NULL_HANDLE;
	slot.occupied = true;
	slot.ended = false;
	slot.acceptedUnfenced = false;
	// Reset only this admitted pair. A pending accepted range cannot be recycled.
	vkCmdResetQueryPool(command, postProcessTimestampQueryPool, slot.sample.queries.firstQuery, 2);
	// The preceding geometry must reach completion before the timed copy and
	// fullscreen draws. Top-of-pipe would include geometry still in flight.
	vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		postProcessTimestampQueryPool, slot.sample.queries.firstQuery);
	return true;
}

void VulkanContext::Impl::EndPostProcessTimestamp(VkCommandBuffer command,
	std::uint32_t frameSlot, std::uint32_t snapshotSlot, std::uint32_t stage)
{
	if(frameSlot >= MAX_FRAMES_IN_FLIGHT || snapshotSlot >= MAX_RENDER_SNAPSHOTS_PER_FRAME || stage >= kPostProcessStagesPerView) return;
	auto& slot = postProcessTimestampSlots[(frameSlot * MAX_RENDER_SNAPSHOTS_PER_FRAME + snapshotSlot) * kPostProcessStagesPerView + stage];
	if(!slot.occupied || slot.command != command || slot.ended || slot.sample.submitSerial) return;
	vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		postProcessTimestampQueryPool, slot.sample.queries.firstQuery + 1);
	slot.ended = true;
}

void VulkanContext::Impl::MarkPostProcessTimestampCommandsSubmitted(
	std::span<const VkCommandBuffer> commands, std::uint32_t frameSlot, VkFence fence) noexcept
{
	if(!postProcessTimestampQueryPool || frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
	std::uint64_t submitSerial = 0;
	for(auto& slot : postProcessTimestampSlots)
	{
		if(!slot.occupied || slot.sample.owner.frameSlot != frameSlot) continue;
		const bool included = std::find(commands.begin(), commands.end(), slot.command) != commands.end();
		if(included && !slot.sample.submitSerial)
		{
			if(!submitSerial) submitSerial = nextPostProcessTimestampSubmitSerial++;
			slot.sample.submitSerial = submitSerial;
			slot.acceptedUnfenced = true;
		}
		// The later fence-bearing UI submit covers an accepted offscreen prefix
		// on the same queue. A previously signaled slot fence never grants proof.
		if(fence && slot.acceptedUnfenced)
		{
			slot.fence = fence;
			slot.acceptedUnfenced = false;
		}
	}
}

void VulkanContext::Impl::DiscardPostProcessTimestampCommands(std::span<const VkCommandBuffer> commands) noexcept
{
	if(!postProcessTimestampQueryPool) return;
	using namespace TerraEngine::Renderer::GraphicsSettings;
	for(auto& slot : postProcessTimestampSlots)
	{
		if(!slot.occupied || slot.sample.submitSerial
			|| std::find(commands.begin(), commands.end(), slot.command) == commands.end()) continue;
		slot.sample.availability = PostProcessTimingAvailability::Abandoned;
		slot.sample.reason = PostProcessDiagnosticReason::DiscardedCommand;
		slot.sample.durationMilliseconds.reset();
		{
			std::lock_guard lock(statsMutex);
			completedPostProcessTimingSamples[slot.sample.queries.firstQuery / 2u] = std::move(slot.sample);
		}
		slot.occupied = false;
	}
}

void VulkanContext::Impl::ResolvePostProcessTimestampSlot(PostProcessTimestampSlot& slot)
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	if(!slot.occupied || !slot.sample.submitSerial || !slot.sample.completionSerial) return;
	auto& sample = slot.sample;
	sample.durationMilliseconds.reset();
	if(deviceLost || sample.owner.lifecycle.rendererSessionGeneration != renderSessionGeneration
		|| sample.owner.lifecycle.backendResourceGeneration != backendResourceGeneration
		|| sample.owner.queryPoolGeneration != postProcessTimestampPoolGeneration)
	{
		sample.availability = PostProcessTimingAvailability::Cancelled;
		sample.reason = PostProcessDiagnosticReason::StaleLifetime;
	}
	else if(!slot.ended)
	{
		sample.availability = PostProcessTimingAvailability::Unavailable;
		sample.reason = PostProcessDiagnosticReason::MissingMarkers;
	}
	else
	{
		struct Query { std::uint64_t ticks = 0; std::uint64_t available = 0; };
		std::array<Query, 2> result{};
		const auto read = vkGetQueryPoolResults(device, postProcessTimestampQueryPool,
			sample.queries.firstQuery, 2, sizeof(result), result.data(), sizeof(Query),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		sample.beginTicks = result[0].ticks;
		sample.endTicks = result[1].ticks;
		sample.beginAvailable = result[0].available != 0;
		sample.endAvailable = result[1].available != 0;
		if(read != VK_SUCCESS || !sample.beginAvailable || !sample.endAvailable)
		{
			sample.availability = PostProcessTimingAvailability::Unavailable;
			sample.reason = read == VK_SUCCESS || read == VK_NOT_READY
				? PostProcessDiagnosticReason::MissingAvailability : PostProcessDiagnosticReason::QueryReadFailed;
		}
		else
		{
			const auto bits = sample.timestampValidBits;
			const auto mask = bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
			sample.elapsedTicks = (sample.endTicks - sample.beginTicks) & mask;
			const double milliseconds = static_cast<double>(sample.elapsedTicks)
				* sample.timestampPeriodNanoseconds / 1000000.0;
			if(sample.elapsedTicks && std::isfinite(milliseconds) && milliseconds > 0)
			{
				sample.availability = PostProcessTimingAvailability::Measured;
				sample.reason = PostProcessDiagnosticReason::None;
				sample.durationMilliseconds = milliseconds;
			}
			else
			{
				sample.availability = PostProcessTimingAvailability::Invalid;
				sample.reason = PostProcessDiagnosticReason::InvalidDuration;
			}
		}
	}
	{
		std::lock_guard lock(statsMutex);
		completedPostProcessTimingSamples[sample.queries.firstQuery / 2u] = std::move(sample);
	}
	// The accepted command's completion proof releases the physical query pair,
	// including unavailable readbacks. A new range starts with its own reset.
	slot.occupied = false;
}

void VulkanContext::Impl::CompletePostProcessTimestampFrame(std::uint32_t frameSlot, VkFence fence)
{
	if(!postProcessTimestampQueryPool || !device || deviceLost || !fence
		|| frameSlot >= MAX_FRAMES_IN_FLIGHT || vkGetFenceStatus(device, fence) != VK_SUCCESS) return;
	for(auto& slot : postProcessTimestampSlots)
	{
		if(!slot.occupied || slot.sample.owner.frameSlot != frameSlot || slot.fence != fence
			|| !slot.sample.submitSerial) continue;
		slot.sample.completionSerial = slot.sample.submitSerial;
		ResolvePostProcessTimestampSlot(slot);
	}
}

void VulkanContext::Impl::CompletePostProcessTimestampDeviceIdle()
{
	if(!postProcessTimestampQueryPool || !device || deviceLost) return;
	for(auto& slot : postProcessTimestampSlots)
	{
		if(!slot.occupied || !slot.sample.submitSerial) continue;
		slot.sample.completionSerial = slot.sample.submitSerial;
		ResolvePostProcessTimestampSlot(slot);
	}
}

void VulkanContext::Impl::DestroyPostProcessTimestampQueries() noexcept
{
	using namespace TerraEngine::Renderer::GraphicsSettings;
	// Existing teardown has already established idle or terminal device loss.
	// Preserve explicit cancellation for outstanding ranges before pool release.
	for(auto& slot : postProcessTimestampSlots)
	{
		if(!slot.occupied) continue;
		slot.sample.availability = PostProcessTimingAvailability::Cancelled;
		slot.sample.reason = PostProcessDiagnosticReason::StaleLifetime;
		slot.sample.durationMilliseconds.reset();
		{
			std::lock_guard lock(statsMutex);
			completedPostProcessTimingSamples[slot.sample.queries.firstQuery / 2u] = std::move(slot.sample);
		}
		slot.occupied = false;
	}
	if(device && postProcessTimestampQueryPool)
		vkDestroyQueryPool(device, postProcessTimestampQueryPool, nullptr);
	postProcessTimestampQueryPool = VK_NULL_HANDLE;
	postProcessTimestampPoolAttempted = false;
}

std::vector<TerraEngine::Renderer::GraphicsSettings::PostProcessTimingSample> VulkanContext::Impl::GetCompletedPostProcessTimings() const
{
	std::vector<TerraEngine::Renderer::GraphicsSettings::PostProcessTimingSample> result;
	std::lock_guard lock(statsMutex);
	for(const auto& sample : completedPostProcessTimingSamples)
		if(!sample.effect.stableId.empty()) result.push_back(sample);
	return result;
}


bool VulkanContext::Impl::CreateGpuTimestampQueries() {
	gpuTimestampQueriesSupported = false;
	lightingDepthTimestampValidBits = 0;
	gpuTimestampPeriodNanoseconds = 0.0f;
	completedVirtualGeometryExecutionReceipts.clear();
	completedVirtualGeometryMeasurements.clear();
	completedVirtualGeometryStageEvidence.fill(false);
	for (auto& frameSamples : completedGpuTimestampSamplesByFrame) {
		for (GpuTimestampViewSample& sample : frameSamples) {
			sample = {};
			sample.reason = "GPU timestamp queries have not produced a completed sample yet.";
		}
	}
	for (auto& frameWrites : pendingGpuTimestampWritesByFrame) {
		frameWrites.fill(false);
	}
	for (auto& frameIds : pendingGpuTimestampFrameIdsByFrame) {
		frameIds.fill(0u);
	}
	for(auto& frameSamples : virtualGeometryGpuTimestampSamplesByFrame) {
		for(auto& viewSamples : frameSamples) {
			for(VirtualGeometryGpuTimestampStageSample& sample : viewSamples) {
				sample = {};
				sample.reason = "No live Vulkan command owns this virtual-geometry stage yet.";
			}
		}
	}

	if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
		return true;
	}

	VkPhysicalDeviceProperties deviceProperties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
	gpuTimestampPeriodNanoseconds = deviceProperties.limits.timestampPeriod;

	const QueueFamilyIndices indices = FindQueueFamilies(physicalDevice);
	std::uint32_t queueFamilyCount = 0u;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	if (queueFamilyCount > 0u) {
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());
	}

	const bool graphicsQueueHasTimestampBits = indices.graphicsFamily.has_value()
		&& *indices.graphicsFamily < queueFamilies.size()
		&& queueFamilies[*indices.graphicsFamily].timestampValidBits > 0u;
	// The isolated collector creates its own pool without the legacy profiler's
	// startup reset submission. Both read the actual selected graphics queue.
	if (deviceProperties.limits.timestampComputeAndGraphics && graphicsQueueHasTimestampBits)
		lightingDepthTimestampValidBits = queueFamilies[*indices.graphicsFamily].timestampValidBits;
	if (!deviceProperties.limits.timestampComputeAndGraphics
		|| !graphicsQueueHasTimestampBits
		|| gpuTimestampPeriodNanoseconds <= 0.0f) {
		for (auto& frameSamples : completedGpuTimestampSamplesByFrame) {
			for (GpuTimestampViewSample& sample : frameSamples) {
				sample = {};
				sample.reason = "The selected Vulkan graphics queue does not expose timestamp queries.";
			}
		}
		TERRAENGINE_LOG(LogLevel::Warning, "Vulkan GPU profiler timestamps are unavailable on the selected graphics queue.");
		return true;
	}

	const std::uint32_t queryCount = static_cast<std::uint32_t>(MAX_FRAMES_IN_FLIGHT)
		* kTotalGpuTimestampQueriesPerFrame;
	VkQueryPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
	poolInfo.queryCount = queryCount;
	if (vkCreateQueryPool(device, &poolInfo, nullptr, &gpuTimestampQueryPool) != VK_SUCCESS) {
		for (auto& frameSamples : completedGpuTimestampSamplesByFrame) {
			for (GpuTimestampViewSample& sample : frameSamples) {
				sample = {};
				sample.reason = "Vulkan failed to create the GPU timestamp query pool.";
			}
		}
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to create Vulkan GPU timestamp query pool; profiler GPU rows will stay unavailable.");
		gpuTimestampQueryPool = VK_NULL_HANDLE;
		return true;
	}

	// A new query pool starts with uninitialized queries. Reset every slot once
	// through a command buffer so startup does not require the optional
	// hostQueryReset feature; the per-view reset below still handles reuse.
	VkCommandBuffer resetCommandBuffer = BeginSingleTimeCommands();
	if (resetCommandBuffer == VK_NULL_HANDLE)
	{
		vkDestroyQueryPool(device, gpuTimestampQueryPool, nullptr);
		gpuTimestampQueryPool = VK_NULL_HANDLE;
		return true;
	}
	vkCmdResetQueryPool(resetCommandBuffer, gpuTimestampQueryPool, 0u, queryCount);
	if (!EndSingleTimeCommands(resetCommandBuffer, "GPU timestamp query reset"))
	{
		vkDestroyQueryPool(device, gpuTimestampQueryPool, nullptr);
		gpuTimestampQueryPool = VK_NULL_HANDLE;
		return true;
	}
	gpuTimestampQueriesSupported = true;
	// Query-pool generation changes whenever Vulkan recreates the underlying
	// object. Completed measurements carry this value so consumers can reject
	// stale readbacks instead of joining unrelated device lifetimes.
	++gpuTimestampQueryPoolGeneration;
	if(gpuTimestampQueryPoolGeneration == 0u) {
		gpuTimestampQueryPoolGeneration = 1u;
	}
	return true;
}


void VulkanContext::Impl::DestroyGpuTimestampQueries() {
	DestroyPostProcessTimestampQueries();
	if (device != VK_NULL_HANDLE && gpuTimestampQueryPool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(device, gpuTimestampQueryPool, nullptr);
	}
	gpuTimestampQueryPool = VK_NULL_HANDLE;
	gpuTimestampQueriesSupported = false;
	gpuTimestampPeriodNanoseconds = 0.0f;
	for (auto& frameWrites : pendingGpuTimestampWritesByFrame) {
		frameWrites.fill(false);
	}
	for(auto& frameSamples : virtualGeometryGpuTimestampSamplesByFrame) {
		for(auto& viewSamples : frameSamples) {
			for(VirtualGeometryGpuTimestampStageSample& sample : viewSamples) {
				sample = {};
				sample.reason = "Vulkan timestamp queries are unavailable because the query pool was destroyed.";
			}
		}
	}
}


void VulkanContext::Impl::MarkGpuTimestampBegin(VkCommandBuffer cmd, uint32_t frameIndex, VulkanContext::ViewType view, std::uint64_t frameId) {
	if (!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| cmd == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	const uint32_t viewIndex = (view == VulkanContext::ViewType::Game) ? 0u : 1u;
	const uint32_t firstQuery = frameIndex * kTotalGpuTimestampQueriesPerFrame + viewIndex * kGpuTimestampQueriesPerView;
	// Reset immediately before writing so reused frame-slot queries never carry
	// an old availability bit into the next profiler sample.
	vkCmdResetQueryPool(cmd, gpuTimestampQueryPool, firstQuery, kGpuTimestampQueriesPerView);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpuTimestampQueryPool, firstQuery);
	pendingGpuTimestampWritesByFrame[frameIndex][viewIndex] = true;
	pendingGpuTimestampFrameIdsByFrame[frameIndex][viewIndex] = frameId;
	completedGpuTimestampSamplesByFrame[frameIndex][viewIndex] = {
		false,
		0.0f,
		frameId,
		0u,
		"GPU timestamp query is pending until this frame slot completes."
	};
}


void VulkanContext::Impl::MarkGpuTimestampEnd(VkCommandBuffer cmd, uint32_t frameIndex, VulkanContext::ViewType view) {
	if (!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| cmd == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	const uint32_t viewIndex = (view == VulkanContext::ViewType::Game) ? 0u : 1u;
	const uint32_t firstQuery = frameIndex * kTotalGpuTimestampQueriesPerFrame + viewIndex * kGpuTimestampQueriesPerView;
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpuTimestampQueryPool, firstQuery + 1u);
}

// VulkanContext::Impl::BeginVirtualGeometryTimestampFrame establishes explicit
// unavailable rows for every stage. Later executors must replace a row only when
// they own a live Vulkan command; CPU-prepared packets cannot opt in here.
void VulkanContext::Impl::BeginVirtualGeometryTimestampFrame(
	uint32_t frameIndex,
	VulkanContext::ViewType view,
	std::uint64_t frameNumber,
	std::uint32_t viewId,
	std::uint64_t rendererGeneration)
{
	if(frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	const std::uint32_t viewIndex = view == VulkanContext::ViewType::Game ? 0u : 1u;
	const auto markers = TerraEngine::Renderer::Vulkan::CreateVulkanVirtualGeometryStageMarkers();
	for(std::size_t index = 0u; index < markers.size(); ++index) {
		const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryStageMarker& marker = markers[index];
		VirtualGeometryGpuTimestampStageSample& sample =
			virtualGeometryGpuTimestampSamplesByFrame[frameIndex][viewIndex][index];
		sample = {};
		sample.stage = marker.stage;
		sample.route = marker.route;
		sample.availability = marker.availability;
		sample.frameNumber = frameNumber;
		sample.viewId = viewId;
		sample.rendererGeneration = rendererGeneration;
		sample.reason = marker.reason;
	}
}

// VulkanContext::Impl::MarkVirtualGeometryTimestampBegin reserves a dedicated
// pair of queries for a stage only when an executor is ready to record that
// stage's live command. This keeps CPU packet upload and CPU-expanded draws
// truthful while allowing the begin timestamp to surround the dispatch.
void VulkanContext::Impl::MarkVirtualGeometryTimestampBegin(
	VkCommandBuffer cmd,
	uint32_t frameIndex,
	VulkanContext::ViewType view,
	TerraEngine::Renderer::VirtualGeometryBackendStage stage,
	TerraEngine::Renderer::VirtualGeometryBackendRoute route,
	std::uint64_t geometryGeneration,
	std::uint64_t residencyGeneration,
	std::uint64_t commandGeneration)
{
	const std::uint32_t stageIndex = GetVirtualGeometryTimestampStageIndex(stage);
	if(!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| cmd == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT
		|| stageIndex >= TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount) {
		return;
	}

	const std::uint32_t viewIndex = view == VulkanContext::ViewType::Game ? 0u : 1u;
	VirtualGeometryGpuTimestampStageSample& sample =
		virtualGeometryGpuTimestampSamplesByFrame[frameIndex][viewIndex][stageIndex];
	if(sample.frameNumber == 0u
		|| sample.rendererGeneration == 0u
		|| geometryGeneration == 0u
		|| commandGeneration == 0u) {
		return;
	}

	const std::uint32_t firstQuery =
		frameIndex * kTotalGpuTimestampQueriesPerFrame
		+ kGpuTimestampQueriesPerFrame
		+ viewIndex * static_cast<std::uint32_t>(TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount)
			* kVirtualGeometryTimestampQueriesPerStage
		+ stageIndex * kVirtualGeometryTimestampQueriesPerStage;
	// Reset on reuse so a completed value from an earlier frame slot cannot be
	// mistaken for this command's delayed result.
	vkCmdResetQueryPool(cmd, gpuTimestampQueryPool, firstQuery, kVirtualGeometryTimestampQueriesPerStage);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpuTimestampQueryPool, firstQuery);
	sample.route = route;
	sample.geometryGeneration = geometryGeneration;
	sample.residencyGeneration = residencyGeneration;
	sample.commandGeneration = commandGeneration;
	sample.availability = TerraEngine::Renderer::BackendMeasurementAvailability::Pending;
	sample.firstQuery = firstQuery;
	sample.timestampWritesRecorded = false;
	sample.reason = "The live Vulkan virtual-geometry command is waiting for its end timestamp and delayed readback.";
}

// VulkanContext::Impl::MarkVirtualGeometryTimestampEnd closes the dedicated
// stage interval. Query results are read only after the owning frame-slot fence.
void VulkanContext::Impl::MarkVirtualGeometryTimestampEnd(
	VkCommandBuffer cmd,
	uint32_t frameIndex,
	VulkanContext::ViewType view,
	TerraEngine::Renderer::VirtualGeometryBackendStage stage)
{
	const std::uint32_t stageIndex = GetVirtualGeometryTimestampStageIndex(stage);
	if(!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| cmd == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT
		|| stageIndex >= TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount) {
		return;
	}

	const std::uint32_t viewIndex = view == VulkanContext::ViewType::Game ? 0u : 1u;
	VirtualGeometryGpuTimestampStageSample& sample =
		virtualGeometryGpuTimestampSamplesByFrame[frameIndex][viewIndex][stageIndex];
	if(sample.availability != TerraEngine::Renderer::BackendMeasurementAvailability::Pending) {
		return;
	}
	vkCmdWriteTimestamp(
		cmd,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		gpuTimestampQueryPool,
		sample.firstQuery + 1u);
	sample.timestampWritesRecorded = true;
	sample.reason = "The live Vulkan virtual-geometry timestamp interval is pending delayed readback.";
}


void VulkanContext::Impl::ResolveCompletedGpuTimestamps(uint32_t frameIndex) {
	if (!gpuTimestampQueriesSupported
		|| gpuTimestampQueryPool == VK_NULL_HANDLE
		|| frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	struct TimestampQueryResult
	{
		std::uint64_t value = 0u;
		std::uint64_t available = 0u;
	};

	for (uint32_t viewIndex = 0u; viewIndex < kStatsViewCount; ++viewIndex) {
		GpuTimestampViewSample& sample = completedGpuTimestampSamplesByFrame[frameIndex][viewIndex];
		if (!pendingGpuTimestampWritesByFrame[frameIndex][viewIndex]) {
			sample = {};
			sample.reason = "No GPU timestamp query was written for this view in the completed frame slot.";
			continue;
		}

		std::array<TimestampQueryResult, kGpuTimestampQueriesPerView> queryResults{};
		const uint32_t firstQuery = frameIndex * kTotalGpuTimestampQueriesPerFrame + viewIndex * kGpuTimestampQueriesPerView;
		const VkResult result = vkGetQueryPoolResults(
			device,
			gpuTimestampQueryPool,
			firstQuery,
			kGpuTimestampQueriesPerView,
			sizeof(TimestampQueryResult) * queryResults.size(),
			queryResults.data(),
			sizeof(TimestampQueryResult),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

		pendingGpuTimestampWritesByFrame[frameIndex][viewIndex] = false;
		if (result != VK_SUCCESS
			|| queryResults[0].available == 0u
			|| queryResults[1].available == 0u
			|| queryResults[1].value < queryResults[0].value) {
			sample = {};
			sample.frameId = pendingGpuTimestampFrameIdsByFrame[frameIndex][viewIndex];
			sample.reason = "GPU timestamp query result was not available after the frame-slot fence.";
			continue;
		}

		const std::uint64_t tickDelta = queryResults[1].value - queryResults[0].value;
		const double milliseconds = static_cast<double>(tickDelta) * static_cast<double>(gpuTimestampPeriodNanoseconds) / 1000000.0;
		sample.valid = true;
		sample.milliseconds = static_cast<float>(std::max(0.0, milliseconds));
		sample.frameId = pendingGpuTimestampFrameIdsByFrame[frameIndex][viewIndex];
		sample.latencyFrames = 1u;
		sample.reason.clear();
	}
}

// VulkanContext::Impl::ResolveCompletedVirtualGeometryTimestamps publishes only
// after the frame-slot fence. Unavailable rows keep no numeric duration, while
// measured rows retain the pool generation and raw ticks needed for validation.
void VulkanContext::Impl::ResolveCompletedVirtualGeometryTimestamps(uint32_t frameIndex)
{
	if(frameIndex >= MAX_FRAMES_IN_FLIGHT) {
		return;
	}

	struct TimestampQueryResult
	{
		std::uint64_t value = 0u;
		std::uint64_t available = 0u;
	};

	for(uint32_t viewIndex = 0u; viewIndex < kStatsViewCount; ++viewIndex) {
		for(VirtualGeometryGpuTimestampStageSample& sample :
			virtualGeometryGpuTimestampSamplesByFrame[frameIndex][viewIndex]) {
			if(sample.published || sample.frameNumber == 0u) {
				continue;
			}

			TerraEngine::Renderer::VirtualGeometryBackendMeasurement measurement{};
			measurement.stage = sample.stage;
			measurement.frameNumber = sample.frameNumber;
			measurement.viewId = sample.viewId;
			measurement.rendererGeneration = sample.rendererGeneration;
			measurement.firstQuery = sample.firstQuery;
			measurement.queryCount = sample.timestampWritesRecorded
				? kVirtualGeometryTimestampQueriesPerStage
				: 0u;
			measurement.availability = sample.availability;
			measurement.reason = sample.reason;

			if(sample.availability == TerraEngine::Renderer::BackendMeasurementAvailability::Pending) {
				std::array<TimestampQueryResult, kVirtualGeometryTimestampQueriesPerStage> queryResults{};
				const VkResult result = vkGetQueryPoolResults(
					device,
					gpuTimestampQueryPool,
					sample.firstQuery,
					kVirtualGeometryTimestampQueriesPerStage,
					sizeof(TimestampQueryResult) * queryResults.size(),
					queryResults.data(),
					sizeof(TimestampQueryResult),
					VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

				if(!sample.timestampWritesRecorded
					|| result != VK_SUCCESS
					|| queryResults[0].available == 0u
					|| queryResults[1].available == 0u
					|| queryResults[1].value < queryResults[0].value) {
					measurement.availability = TerraEngine::Renderer::BackendMeasurementAvailability::Invalid;
					measurement.queryCount = 0u;
					measurement.reason = "The completed Vulkan virtual-geometry timestamp interval was unavailable or invalid.";
				}
				else {
					const std::uint64_t rawTicks = queryResults[1].value - queryResults[0].value;
					measurement.queryPoolGeneration = gpuTimestampQueryPoolGeneration;
					measurement.rawTicks = rawTicks;
					measurement.durationMilliseconds =
						static_cast<double>(rawTicks) * static_cast<double>(gpuTimestampPeriodNanoseconds) / 1000000.0;
					measurement.availability = TerraEngine::Renderer::BackendMeasurementAvailability::Measured;
					measurement.reason.clear();

					// A receipt is stronger than timing alone: the exact live command
					// recorded both marker boundaries, submitted with this frame slot,
					// and became visible only after the slot fence completed. The
					// command serial acts as the fence identity across handle reuse.
					TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt receipt{};
					receipt.stage = sample.stage;
					receipt.route = sample.route;
					receipt.frameNumber = sample.frameNumber;
					receipt.viewId = sample.viewId;
					receipt.rendererGeneration = sample.rendererGeneration;
					receipt.geometryGeneration = sample.geometryGeneration;
					receipt.residencyGeneration = sample.residencyGeneration;
					receipt.commandGeneration = sample.commandGeneration;
					receipt.completionFence = sample.frameNumber;
					receipt.commandRecorded = true;
					receipt.commandSubmitted = true;
					receipt.completionObserved = true;
					receipt.generationMatched = true;
					receipt.evidenceId = "vulkan.timestamp."
						+ std::to_string(gpuTimestampQueryPoolGeneration)
						+ "."
						+ std::to_string(sample.firstQuery)
						+ "."
						+ std::to_string(sample.frameNumber);
					if(receipt.IsExecuted()) {
						const std::uint32_t stageIndex = GetVirtualGeometryTimestampStageIndex(receipt.stage);
						if(stageIndex < completedVirtualGeometryStageEvidence.size()) {
							completedVirtualGeometryStageEvidence[stageIndex] = true;
						}
						if(receipt.stage == TerraEngine::Renderer::VirtualGeometryBackendStage::IndirectGeneration
							&& receipt.commandGeneration == latestVirtualGeometryExecutorPlan.commandGeneration
							&& latestVirtualGeometryExecutorPlan.boundedPacketCount != 0u) {
							// A non-empty indirect receipt is the earliest point at which
							// this frame's GPU-authored commands may be advertised.
							latestVirtualGeometryExecutorPlan.generatedSubmissionExecutionAvailable = true;
						}
						completedVirtualGeometryExecutionReceipts.push_back(std::move(receipt));
					}
				}
			}

			completedVirtualGeometryMeasurements.push_back(std::move(measurement));
			sample.published = true;
		}
	}
}


void VulkanContext::Impl::RecordPendingStats(uint32_t statsSlotIndex, const RenderStatsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingStatsBySlot[statsSlotIndex] = snapshot;
}


void VulkanContext::Impl::RecordPendingBudgetDiagnostics(uint32_t statsSlotIndex, const FrameBudgetDiagnosticsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingBudgetDiagnosticsBySlot[statsSlotIndex] = snapshot;
}


void VulkanContext::Impl::RecordPendingLightAnalysisCandidates(uint32_t statsSlotIndex, const std::vector<FrameLightCandidate>& candidates) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingLightAnalysisCandidatesBySlot[statsSlotIndex] = candidates;
}


void VulkanContext::Impl::PublishCompletedFrameStats(uint32_t frameIndex) {
	// Called only after the frame fence (and any stranded editor drain) proves
	// real completion; publishing counters must not manufacture shadow completion.
	CompleteShadowWrites(frameIndex);
	ResolveCompletedGpuTimestamps(frameIndex);
	ResolveCompletedVirtualGeometryTimestamps(frameIndex);

	TerraEngine::Renderer::BackendResourceResidencySnapshot backendResidency{};
	backendResidency.frameId = frameIndex;
	backendResidency.records.reserve(meshCache.size() + textureCache.size());
	for (const auto& [meshId, mesh] : meshCache)
	{
		const VkDeviceSize gpuBytes = TotalMeshGpuBytes(mesh);
		if (gpuBytes == 0u)
		{
			continue;
		}

		TerraEngine::Renderer::BackendResourceResidencyRecord record{};
		record.assetId = meshId;
		record.kind = TerraEngine::Renderer::BackendResourceResidencyKind::Mesh;
		record.gpuBytes = static_cast<std::uint64_t>(gpuBytes);
		record.exactGpuBytes = static_cast<std::uint64_t>(gpuBytes);
		record.allocationId = static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId));
		record.byteStatus = "exact";
		record.retirementState = "published";
		record.subresourceCount = 8u;
		record.detail = "vulkan mesh cache";
		backendResidency.records.push_back(std::move(record));
	}
	if (frameIndex < virtualGeometryGpuPacketResourcesByFrame.size())
	{
		for (const VirtualGeometryGpuPacketFrameResources& resources : virtualGeometryGpuPacketResourcesByFrame[frameIndex])
		{
			if (!resources.live || resources.pagePayloadBytesByMesh.empty())
			{
				continue;
			}
			for (const auto& [meshId, gpuBytes] : resources.pagePayloadBytesByMesh)
			{
				if (meshId == TerraEngine::EMPTY_UUID || gpuBytes == 0u)
				{
					continue;
				}

				TerraEngine::Renderer::BackendResourceResidencyRecord record{};
				record.assetId = meshId;
				record.kind = TerraEngine::Renderer::BackendResourceResidencyKind::Mesh;
				record.gpuBytes = gpuBytes;
				record.exactGpuBytes = gpuBytes;
				record.allocationId = static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId))
					^ (resources.resourceSerial + 0x9e3779b97f4a7c15ull);
				record.byteStatus = "exact";
				record.retirementState = "published";
				record.subresourceCount = 1u;
				// Page-local VG geometry is packed into a per-view physical
				// payload buffer instead of the owner mesh cache. Publishing a
				// second asset-owned record keeps Resource Residency exact while
				// still allowing the renderer to batch those page bytes together.
				record.detail = "vulkan virtual geometry page-local payload";
				backendResidency.records.push_back(std::move(record));
			}
		}
	}
	for (const auto& [textureId, texture] : textureCache)
	{
		if (texture.size == 0u)
		{
			continue;
		}

		TerraEngine::Renderer::BackendResourceResidencyRecord record{};
		record.assetId = textureId;
		record.kind = TerraEngine::Renderer::BackendResourceResidencyKind::Texture;
		record.gpuBytes = static_cast<std::uint64_t>(texture.size);
		record.exactGpuBytes = static_cast<std::uint64_t>(texture.size);
		record.allocationId = static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(textureId));
		record.byteStatus = "exact";
		record.retirementState = "published";
		record.subresourceCount = texture.residentMipCount;
		record.detail = "vulkan texture cache";
		backendResidency.records.push_back(std::move(record));
	}
	TerraEngine::Renderer::PublishBackendResourceResidencySnapshot(std::move(backendResidency));

	const uint32_t gameSlot = GetStatsSlotIndex(frameIndex, VulkanContext::ViewType::Game);
	const uint32_t sceneSlot = GetStatsSlotIndex(frameIndex, VulkanContext::ViewType::Scene);
	const auto hasReadableCompletedHiZ = [](const VulkanHiZ& hiZ, const RenderStatsSnapshot& stats) {
		const bool sizeMatchesStats =
			stats.renderSize.x == 0u
			|| stats.renderSize.y == 0u
			|| (hiZ.width == stats.renderSize.x && hiZ.height == stats.renderSize.y);
		return hiZ.image != VK_NULL_HANDLE
			&& hiZ.fullView != VK_NULL_HANDLE
			&& hiZ.sampler != VK_NULL_HANDLE
			&& hiZ.mipLevels > 0u
			&& hiZ.layoutInitialized
			&& hiZ.history.valid
			&& sizeMatchesStats;
	};

	RenderStatsSnapshot gameSnapshot = pendingStatsBySlot[gameSlot];
	const GpuTimestampViewSample gameGpuTimestamp = completedGpuTimestampSamplesByFrame[std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1))][0u];
	const MeshletCullingCountersGPU gameCounters = ReadMeshletCounters(gameSlot);

	if (gameSnapshot.valid) {
		gameSnapshot.gpuTimestampValid = gameGpuTimestamp.valid;
		gameSnapshot.gpuTimestampMs = gameGpuTimestamp.milliseconds;
		gameSnapshot.gpuTimestampFrameId = gameGpuTimestamp.frameId;
		gameSnapshot.gpuTimestampLatencyFrames = gameGpuTimestamp.latencyFrames;
		gameSnapshot.gpuTimestampReason = gameGpuTimestamp.reason;
		const bool useGpuCounters = (gameCounters.meshletsTotal > 0u) || (gameSnapshot.meshletsTotal == 0u);
		if (useGpuCounters) {
			gameSnapshot.meshletsTotal = gameCounters.meshletsTotal;
			gameSnapshot.meshletsDispatched = gameCounters.meshletsDispatched;
			gameSnapshot.meshletsCulled = gameCounters.meshletsCulled;
			gameSnapshot.meshletsCulledHiZ = gameCounters.meshletsCulledHiZ;
			gameSnapshot.hiZTests = gameCounters.hiZTests;
			gameSnapshot.hiZProjectionFailed = gameCounters.hiZProjectionFailed;
			gameSnapshot.hiZFullRectCulled = gameCounters.hiZFullRectCulled;
			gameSnapshot.hiZRectCoverageCulled = gameCounters.hiZRectCoverageCulled;
			gameSnapshot.hiZCenterCulled = gameCounters.hiZCenterCulled;
			gameSnapshot.hiZVertexCoverageCulled = gameCounters.hiZVertexCoverageCulled;
			gameSnapshot.hiZNoOcclusionProof = gameCounters.hiZNoOcclusionProof;
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::GPUReadback;
			gameSnapshot.meshletCounterFrameId = frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = 0u;
			gameSnapshot.meshletCounterReason.clear();
		} else if (gameSnapshot.taskCullingActive) {
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::DelayedGPUReadback;
			gameSnapshot.meshletCounterFrameId = frameIndex > 0u ? frameIndex - 1u : frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = frameIndex > 0u ? 1u : 0u;
			gameSnapshot.meshletCounterReason = "GPU meshlet counter readback is delayed; showing prepared counts.";
		} else {
			gameSnapshot.meshletCounterSource = VirtualGeometryCounterSource::Prepared;
			gameSnapshot.meshletCounterFrameId = frameIndex;
			gameSnapshot.meshletCounterLatencyFrames = 0u;
			gameSnapshot.meshletCounterReason = "Task culling is inactive; counts are prepared CPU estimates.";
		}
		gameSnapshot.hiZActive = gameSnapshot.hiZActive && hasReadableCompletedHiZ(HiZForFrame(kHiZGameViewIndex, frameIndex), gameSnapshot);
		gameSnapshot.vrsActive = gameSnapshot.vrsActive && UsesVrsAttachment();
	}

	RenderStatsSnapshot sceneSnapshot = pendingStatsBySlot[sceneSlot];
	const GpuTimestampViewSample sceneGpuTimestamp = completedGpuTimestampSamplesByFrame[std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1))][1u];
	const MeshletCullingCountersGPU sceneCounters = ReadMeshletCounters(sceneSlot);

	if (sceneSnapshot.valid) {
		sceneSnapshot.gpuTimestampValid = sceneGpuTimestamp.valid;
		sceneSnapshot.gpuTimestampMs = sceneGpuTimestamp.milliseconds;
		sceneSnapshot.gpuTimestampFrameId = sceneGpuTimestamp.frameId;
		sceneSnapshot.gpuTimestampLatencyFrames = sceneGpuTimestamp.latencyFrames;
		sceneSnapshot.gpuTimestampReason = sceneGpuTimestamp.reason;
		const bool useGpuCounters = (sceneCounters.meshletsTotal > 0u) || (sceneSnapshot.meshletsTotal == 0u);
		if (useGpuCounters) {
			sceneSnapshot.meshletsTotal = sceneCounters.meshletsTotal;
			sceneSnapshot.meshletsDispatched = sceneCounters.meshletsDispatched;
			sceneSnapshot.meshletsCulled = sceneCounters.meshletsCulled;
			sceneSnapshot.meshletsCulledHiZ = sceneCounters.meshletsCulledHiZ;
			sceneSnapshot.hiZTests = sceneCounters.hiZTests;
			sceneSnapshot.hiZProjectionFailed = sceneCounters.hiZProjectionFailed;
			sceneSnapshot.hiZFullRectCulled = sceneCounters.hiZFullRectCulled;
			sceneSnapshot.hiZRectCoverageCulled = sceneCounters.hiZRectCoverageCulled;
			sceneSnapshot.hiZCenterCulled = sceneCounters.hiZCenterCulled;
			sceneSnapshot.hiZVertexCoverageCulled = sceneCounters.hiZVertexCoverageCulled;
			sceneSnapshot.hiZNoOcclusionProof = sceneCounters.hiZNoOcclusionProof;
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::GPUReadback;
			sceneSnapshot.meshletCounterFrameId = frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = 0u;
			sceneSnapshot.meshletCounterReason.clear();
		} else if (sceneSnapshot.taskCullingActive) {
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::DelayedGPUReadback;
			sceneSnapshot.meshletCounterFrameId = frameIndex > 0u ? frameIndex - 1u : frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = frameIndex > 0u ? 1u : 0u;
			sceneSnapshot.meshletCounterReason = "GPU meshlet counter readback is delayed; showing prepared counts.";
		} else {
			sceneSnapshot.meshletCounterSource = VirtualGeometryCounterSource::Prepared;
			sceneSnapshot.meshletCounterFrameId = frameIndex;
			sceneSnapshot.meshletCounterLatencyFrames = 0u;
			sceneSnapshot.meshletCounterReason = "Task culling is inactive; counts are prepared CPU estimates.";
		}
		sceneSnapshot.hiZActive = sceneSnapshot.hiZActive && hasReadableCompletedHiZ(HiZForFrame(kHiZSceneViewIndex, frameIndex), sceneSnapshot);
		sceneSnapshot.vrsActive = sceneSnapshot.vrsActive && UsesVrsAttachment();
	}

	const FrameBudgetDiagnosticsSnapshot gameBudgetDiagnostics = pendingBudgetDiagnosticsBySlot[gameSlot];
	const FrameBudgetDiagnosticsSnapshot sceneBudgetDiagnostics = pendingBudgetDiagnosticsBySlot[sceneSlot];
	const auto buildLightingDiagnostics = [this](const FrameBudgetDiagnosticsSnapshot& budgetDiagnostics) {
		LightingDiagnosticsSnapshot snapshot{};
		snapshot.viewType = budgetDiagnostics.viewType;
		snapshot.valid = budgetDiagnostics.valid;
		snapshot.indirectLightingActive = true;
		const bool gtaoAvailable = ambientOcclusionBlurredByFrame[0].image != VK_NULL_HANDLE
			&& ambientOcclusionBlurredByFrame[0].view != VK_NULL_HANDLE
			&& gtaoComputePipeline != VK_NULL_HANDLE
			&& gtaoBlurComputePipeline != VK_NULL_HANDLE;
		snapshot.ambientOcclusionActive = gtaoAvailable;

		LightingDiagnosticEntry shadowEntry{};
		shadowEntry.category = LightingDiagnosticCategory::Shadow;
		shadowEntry.status = budgetDiagnostics.shadowReductionOccurred
			? LightingDiagnosticStatus::Degraded
			: LightingDiagnosticStatus::Active;
		shadowEntry.message = budgetDiagnostics.shadowReductionOccurred
			? "Some shadow-capable retained lights did not receive shadow resources this frame."
			: "Shadow-capable retained lights are within the active shadow budget.";
		shadowEntry.recommendedAction = budgetDiagnostics.shadowReductionOccurred
			? "Increase the shadow light budget, lower shadow demand, or raise important lights' priority."
			: "";
		snapshot.entries.push_back(std::move(shadowEntry));

		LightingDiagnosticEntry shadowMemoryEntry{};
		shadowMemoryEntry.category = LightingDiagnosticCategory::Shadow;
		shadowMemoryEntry.status = budgetDiagnostics.allocationFailureCount > 0u
			? LightingDiagnosticStatus::Unavailable
			: LightingDiagnosticStatus::Active;
		shadowMemoryEntry.message =
			"Shadow storage allocated "
			+ std::to_string(budgetDiagnostics.allocatedRegionCount)
			+ " active regions for "
			+ std::to_string(budgetDiagnostics.activeShadowCount)
			+ " shadowed lights.";
		shadowMemoryEntry.recommendedAction = budgetDiagnostics.allocationFailureCount > 0u
			? "Reduce shadow demand or inspect Vulkan allocation failures."
			: "";
		snapshot.entries.push_back(std::move(shadowMemoryEntry));

		const auto shadowKindName = [](ShadowResourceKind kind) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (kind)
			{
			case ShadowResourceKind::DirectionalCascade:
				return "directional cascade";
			case ShadowResourceKind::PointCube:
				return "point cubemap";
			case ShadowResourceKind::SpotAtlas:
				return "spot atlas";
			case ShadowResourceKind::AreaAtlas:
				return "area atlas";
			default:
				return "unsupported shadow resource";
			}
		};


		for (const ShadowResourceAssignmentDiagnostic& assignment : budgetDiagnostics.shadowAssignments)
		{
			LightingDiagnosticEntry assignmentEntry{};
			assignmentEntry.category = LightingDiagnosticCategory::Shadow;
			assignmentEntry.status = assignment.assigned
				? LightingDiagnosticStatus::Active
				: LightingDiagnosticStatus::Degraded;
			assignmentEntry.lightId = assignment.lightId;
			assignmentEntry.message = assignment.assigned
				? std::string("Light received ") + shadowKindName(assignment.resourceKind) + " slot " + std::to_string(assignment.resourceSlot) + "."
				: std::string("Light requested shadows but did not receive a ") + shadowKindName(assignment.resourceKind) + " slot this frame.";

			if (assignment.reusedPreviousOwner)
			{
				assignmentEntry.message += " Reused previous assignment for stability.";
			}

			if (assignment.replacedPreviousOwner)
			{
				assignmentEntry.message += " Replaced the previous owner of this slot.";
			}
			assignmentEntry.recommendedAction = assignment.assigned
				? ""
				: "Increase maxShadowLights or raise this light's authored priority.";
			snapshot.entries.push_back(std::move(assignmentEntry));
		}

		LightingDiagnosticEntry indirectEntry{};
		indirectEntry.category = LightingDiagnosticCategory::IndirectLighting;
		indirectEntry.status = LightingDiagnosticStatus::Active;
		indirectEntry.message = "Indirect lighting is evaluated separately from hard direct shadow visibility.";
		snapshot.entries.push_back(std::move(indirectEntry));

		LightingDiagnosticEntry aoEntry{};
		aoEntry.category = LightingDiagnosticCategory::AmbientOcclusion;
		aoEntry.status = gtaoAvailable
			? LightingDiagnosticStatus::Active
			: LightingDiagnosticStatus::Fallback;
		aoEntry.message = gtaoAvailable
			? "GTAO contact darkening is generated before material shading and applied to indirect lighting."
			: "Contact darkening uses material occlusion until the GTAO compute pass is available.";
		aoEntry.recommendedAction = gtaoAvailable
			? ""
			: "Check GTAO shader packaging and descriptor creation if screen-space contact darkening should be active.";
		snapshot.entries.push_back(std::move(aoEntry));

		LightingDiagnosticEntry materialEntry{};
		materialEntry.category = LightingDiagnosticCategory::MaterialShadow;
		materialEntry.status = LightingDiagnosticStatus::Fallback;
		materialEntry.message = "Opaque and alpha-cutout materials cast shadows; blended transparent materials currently do not cast shadows.";
		materialEntry.recommendedAction = "Use Mask alpha mode for cutout objects that must cast silhouette shadows.";
		snapshot.entries.push_back(std::move(materialEntry));

		LightingDiagnosticEntry budgetEntry{};
		budgetEntry.category = LightingDiagnosticCategory::Budget;
		budgetEntry.status = budgetDiagnostics.budgetReductionOccurred
			? LightingDiagnosticStatus::Degraded
			: LightingDiagnosticStatus::Active;
		budgetEntry.message = budgetDiagnostics.budgetReductionOccurred
			? "The scene exceeded one or more active lighting budgets."
			: "The scene is within active lighting budgets.";
		budgetEntry.recommendedAction = budgetDiagnostics.budgetReductionOccurred
			? "Use quality settings or light priorities to control which lights are retained."
			: "";
		snapshot.entries.push_back(std::move(budgetEntry));
		return snapshot;
	};
	const LightingDiagnosticsSnapshot gameLightingDiagnostics = buildLightingDiagnostics(gameBudgetDiagnostics);
	const LightingDiagnosticsSnapshot sceneLightingDiagnostics = buildLightingDiagnostics(sceneBudgetDiagnostics);

	GpuLightAnalysisViewSnapshot gameLightAnalysis{};
	GpuLightAnalysisViewSnapshot sceneLightAnalysis{};

	if (frameIndex < lightAnalysisSSBOs.size() && lightAnalysisSSBOs[frameIndex].buffer != VK_NULL_HANDLE) {
		const VulkanBuffer& analysisBuffer = lightAnalysisSSBOs[frameIndex];
		void* mappedAnalysis = nullptr;

		if (vmaMapMemory(allocator, analysisBuffer.allocation, &mappedAnalysis) == VK_SUCCESS) {
			vmaInvalidateAllocation(allocator, analysisBuffer.allocation, 0, analysisBuffer.size);
			const auto* analysisRecords = static_cast<const GPULightAnalysisRecord*>(mappedAnalysis);
			const auto readSnapshot = [&](uint32_t statsSlotIndex) {
				GpuLightAnalysisViewSnapshot snapshot{};

				if (statsSlotIndex >= pendingLightAnalysisCandidatesBySlot.size()) {
					return snapshot;
				}

				const std::vector<FrameLightCandidate>& candidates = pendingLightAnalysisCandidatesBySlot[statsSlotIndex];

				if (candidates.empty()) {
					snapshot.valid = true;
					return snapshot;
				}

				const std::size_t clampedCount = std::min<std::size_t>(candidates.size(), MAX_GLOBAL_LIGHTS);
				const std::size_t baseIndex = static_cast<std::size_t>(statsSlotIndex) * static_cast<std::size_t>(MAX_GLOBAL_LIGHTS);
				snapshot.recordsByLightId.reserve(clampedCount);

				for (std::size_t lightIndex = 0; lightIndex < clampedCount; ++lightIndex) {
					const GPULightAnalysisRecord& record = analysisRecords[baseIndex + lightIndex];
					snapshot.recordsByLightId.emplace(candidates[lightIndex].lightId, record);

					if ((record.flags & kGpuLightAnalysisFlagVisible) != 0u) {
						snapshot.visibleCandidateCount += 1u;
					}
				}
				snapshot.valid = true;
				return snapshot;
			};

			gameLightAnalysis = readSnapshot(gameSlot);
			sceneLightAnalysis = readSnapshot(sceneSlot);
			vmaUnmapMemory(allocator, analysisBuffer.allocation);
		}
	}

	ClusterValidationSnapshot clusterSnapshot{};

	if (clusterValidationCaptureEnabled) {
		clusterSnapshot = ReadCompletedClusterValidationSnapshot(frameIndex);
	}

	{
		std::lock_guard<std::mutex> lock(statsMutex);
		// A completed frame slot can legitimately omit one logical view (for
		// example while a custom capture is the only submitted surface). Keep the
		// last completed valid snapshot for that view until lifecycle reset instead
		// of replacing it with an empty slot and making diagnostics disappear.
		if(gameSnapshot.valid) {
			latestStatsByView[0] = gameSnapshot;
		}
		if(sceneSnapshot.valid) {
			latestStatsByView[1] = sceneSnapshot;
		}
		latestBudgetDiagnosticsByView[0] = gameBudgetDiagnostics;
		latestBudgetDiagnosticsByView[1] = sceneBudgetDiagnostics;
		latestLightingDiagnosticsByView[0] = gameLightingDiagnostics;
		latestLightingDiagnosticsByView[1] = sceneLightingDiagnostics;
		latestGpuLightAnalysisByView[0] = std::move(gameLightAnalysis);
		latestGpuLightAnalysisByView[1] = std::move(sceneLightAnalysis);

		if (clusterSnapshot.valid) {
			previousClusterValidationSnapshot = clusterSnapshot;
			previousClusterValidationSnapshotValid = true;
		}
	}


	if (clusterSnapshot.valid) {
		const FrameBudgetDiagnosticsSnapshot& captureDiagnostics = (clusterSnapshot.viewType == RenderViewType::Scene)
			? sceneBudgetDiagnostics
			: gameBudgetDiagnostics;
		WriteClusterValidationCapture(clusterSnapshot, captureDiagnostics);
	}
}


RenderStatsSnapshot VulkanContext::Impl::GetLatestStats(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestStatsByView[index];
}


FrameBudgetDiagnosticsSnapshot VulkanContext::Impl::GetLatestBudgetDiagnostics(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestBudgetDiagnosticsByView[index];
}


LightingDiagnosticsSnapshot VulkanContext::Impl::GetLatestLightingDiagnostics(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestLightingDiagnosticsByView[index];
}


VulkanContext::Impl::GpuLightAnalysisViewSnapshot VulkanContext::Impl::GetLatestGpuLightAnalysis(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestGpuLightAnalysisByView[index];
}


void VulkanContext::Impl::RecordPendingClusterValidationSnapshot(uint32_t frameIndex, const ClusterValidationSnapshot& snapshot) {
	if (frameIndex >= pendingClusterValidationByFrame.size()) {
		return;
	}
	pendingClusterValidationByFrame[frameIndex] = snapshot;
}


ClusterValidationSnapshot VulkanContext::Impl::ReadCompletedClusterValidationSnapshot(uint32_t frameIndex) const {
	if (!clusterValidationCaptureEnabled) {
		return {};
	}


	if (frameIndex >= pendingClusterValidationByFrame.size()) {
		return {};
	}

	ClusterValidationSnapshot snapshot = pendingClusterValidationByFrame[frameIndex];

	if (!snapshot.valid || !snapshot.buildMetadata.clusteredLightingAvailable) {
		return snapshot;
	}

	if (frameIndex >= clusterGridSSBOs.size() || frameIndex >= lightIndexSSBOs.size()) {
		snapshot.valid = false;
		return snapshot;
	}

	const VulkanBuffer& clusterBuffer = clusterGridSSBOs[frameIndex];
	const VulkanBuffer& lightIndexBuffer = lightIndexSSBOs[frameIndex];

	if (clusterBuffer.buffer == VK_NULL_HANDLE || lightIndexBuffer.buffer == VK_NULL_HANDLE) {
		snapshot.valid = false;
		return snapshot;
	}

	const std::uint32_t clusterCount = snapshot.buildMetadata.clusterCount;
	snapshot.ranges.clear();
	snapshot.ranges.reserve(clusterCount);

	void* mappedClusters = nullptr;

	if (vmaMapMemory(allocator, clusterBuffer.allocation, &mappedClusters) != VK_SUCCESS) {
		snapshot.valid = false;
		return snapshot;
	}

	const VkDeviceSize clusterReadOffset = static_cast<VkDeviceSize>(snapshot.buildMetadata.clusterDataOffset) * sizeof(ClusterData);
	const VkDeviceSize clusterReadSize = static_cast<VkDeviceSize>(sizeof(ClusterData) * clusterCount);
	vmaInvalidateAllocation(allocator, clusterBuffer.allocation, clusterReadOffset, clusterReadSize);

	const ClusterData* clusterGridData = static_cast<const ClusterData*>(mappedClusters) + snapshot.buildMetadata.clusterDataOffset;
	std::uint64_t rangeHash = kClusterRangeHashOffset;
	std::uint32_t usedIndexCount = 0u;
	std::uint32_t actualAssignedLightCount = 0u;
	std::uint32_t emptyClusterCount = 0u;
	std::uint32_t maxClusterLightCount = 0u;
	std::uint32_t outOfBoundsRangeCount = 0u;


	for (std::uint32_t clusterIndex = 0u; clusterIndex < clusterCount; ++clusterIndex) {
		const ClusterData& cluster = clusterGridData[clusterIndex];
		const std::uint64_t rangeEnd = static_cast<std::uint64_t>(cluster.offset) + static_cast<std::uint64_t>(cluster.count);
		const bool outOfBounds = rangeEnd > snapshot.buildMetadata.lightIndexCapacity
			|| rangeEnd > static_cast<std::uint64_t>(MAX_LIGHT_INDEX_ENTRIES);

		ClusterLightRangeSnapshot rangeSnapshot{};
		rangeSnapshot.clusterIndex = clusterIndex;
		rangeSnapshot.offset = cluster.offset;
		rangeSnapshot.count = cluster.count;
		rangeSnapshot.outOfBounds = outOfBounds;
		snapshot.ranges.push_back(rangeSnapshot);


		if (cluster.count == 0u) {
			emptyClusterCount += 1u;
		}
		maxClusterLightCount = std::max(maxClusterLightCount, cluster.count);
		actualAssignedLightCount += cluster.count;

		if (rangeEnd > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
			usedIndexCount = std::numeric_limits<std::uint32_t>::max();
		}
		else {
			usedIndexCount = std::max(usedIndexCount, static_cast<std::uint32_t>(rangeEnd));
		}

		if (outOfBounds) {
			outOfBoundsRangeCount += 1u;
		}

		HashClusterRangeValue(rangeHash, clusterIndex);
		HashClusterRangeValue(rangeHash, cluster.offset);
		HashClusterRangeValue(rangeHash, cluster.count);
		HashClusterRangeValue(rangeHash, outOfBounds ? 1u : 0u);
	}

	vmaUnmapMemory(allocator, clusterBuffer.allocation);

	std::uint32_t invalidLightIndexCount = 0u;

	if (usedIndexCount > 0u) {
		void* mappedLightIndices = nullptr;
		if (vmaMapMemory(allocator, lightIndexBuffer.allocation, &mappedLightIndices) == VK_SUCCESS) {
			const VkDeviceSize lightIndexReadOffset = static_cast<VkDeviceSize>(snapshot.buildMetadata.lightIndexOffset) * sizeof(std::uint32_t);
			const VkDeviceSize lightIndexReadSize = static_cast<VkDeviceSize>(sizeof(std::uint32_t) * usedIndexCount);
			vmaInvalidateAllocation(allocator, lightIndexBuffer.allocation, lightIndexReadOffset, lightIndexReadSize);

			const std::uint32_t* lightIndexData = static_cast<const std::uint32_t*>(mappedLightIndices) + snapshot.buildMetadata.lightIndexOffset;

			for (std::uint32_t entryIndex = 0u; entryIndex < usedIndexCount; ++entryIndex) {
				const std::uint32_t lightIndex = lightIndexData[entryIndex];
				if (lightIndex == 0xFFFFFFFFu || lightIndex >= snapshot.buildMetadata.activeLightCount) {
					invalidLightIndexCount += 1u;
				}
			}

			vmaUnmapMemory(allocator, lightIndexBuffer.allocation);
		}
	}

	std::uint32_t determinismMismatchCount = 0u;

	if (previousClusterValidationSnapshotValid
		&& previousClusterValidationSnapshot.viewType == snapshot.viewType
		&& previousClusterValidationSnapshot.ranges.size() == snapshot.ranges.size()
		&& previousClusterValidationSnapshot.buildMetadata.activeLightCount == snapshot.buildMetadata.activeLightCount) {

		for (size_t rangeIndex = 0; rangeIndex < snapshot.ranges.size(); ++rangeIndex) {
			const ClusterLightRangeSnapshot& currentRange = snapshot.ranges[rangeIndex];
			const ClusterLightRangeSnapshot& previousRange = previousClusterValidationSnapshot.ranges[rangeIndex];

			if (currentRange.offset != previousRange.offset
				|| currentRange.count != previousRange.count
				|| currentRange.outOfBounds != previousRange.outOfBounds) {
				determinismMismatchCount += 1u;
			}
		}
	}

	snapshot.buildMetadata.usedIndexCount = usedIndexCount;
	snapshot.buildMetadata.actualAssignedLightCount = actualAssignedLightCount;
	snapshot.buildMetadata.emptyClusterCount = emptyClusterCount;
	snapshot.buildMetadata.maxClusterLightCount = maxClusterLightCount;
	snapshot.buildMetadata.outOfBoundsRangeCount = outOfBoundsRangeCount;
	snapshot.buildMetadata.invalidLightIndexCount = invalidLightIndexCount;
	snapshot.buildMetadata.overflowClusterCount = outOfBoundsRangeCount;
	snapshot.buildMetadata.determinismMismatchCount = determinismMismatchCount;
	snapshot.buildMetadata.rangeHash = rangeHash;
	snapshot.buildMetadata.overflowDetected = (outOfBoundsRangeCount > 0u) || (invalidLightIndexCount > 0u);
	return snapshot;
}


void VulkanContext::Impl::WriteClusterValidationCapture(const ClusterValidationSnapshot& snapshot, const FrameBudgetDiagnosticsSnapshot& diagnostics) const {
	if (!clusterValidationCaptureEnabled || !snapshot.valid) {
		return;
	}

	try {
		namespace fs = std::filesystem;
		const fs::path outputPath = clusterValidationCapturePath;

		if (outputPath.has_parent_path()) {
			fs::create_directories(outputPath.parent_path());
		}

		const double emptyClusterPercent = snapshot.buildMetadata.clusterCount > 0u
			? (static_cast<double>(snapshot.buildMetadata.emptyClusterCount) * 100.0 / static_cast<double>(snapshot.buildMetadata.clusterCount))
			: 0.0;
		const double logicalIndexOverheadPercent = snapshot.buildMetadata.actualAssignedLightCount > 0u
			? (static_cast<double>(snapshot.buildMetadata.usedIndexCount) * 100.0 / static_cast<double>(snapshot.buildMetadata.actualAssignedLightCount))
			: 0.0;

		nlohmann::json capture;
		capture["view_type"] = RenderViewTypeToString(snapshot.viewType);
		capture["valid"] = snapshot.valid;
		capture["clustered_lighting_available"] = snapshot.buildMetadata.clusteredLightingAvailable;
		capture["overflow_detected"] = snapshot.buildMetadata.overflowDetected;
		capture["active_preset_name"] = QualityPresetToString(diagnostics.activePreset);
		capture["reduction_category"] = LightingReductionCategoryToString(diagnostics.reductionCategory);
		capture["gpu_assist_active"] = diagnostics.gpuAssistActive;
		capture["custom_override_active"] = diagnostics.customOverrideActive;
		capture["uploaded_light_budget"] = diagnostics.uploadedLightBudget;
		capture["retained_light_budget"] = diagnostics.retainedLightBudget;
		capture["shadow_light_budget"] = diagnostics.shadowLightBudget;
		capture["special_light_budget"] = diagnostics.specialLightBudget;
		capture["candidate_light_count"] = diagnostics.candidateLightCount;
		capture["uploaded_light_count"] = diagnostics.uploadedLightCount;
		capture["retained_light_count"] = diagnostics.retainedLightCount;
		capture["special_light_count"] = diagnostics.specialLightCount;
		capture["shadow_eligible_light_count"] = diagnostics.shadowEligibleLightCount;
		capture["shadowed_light_count"] = diagnostics.shadowedLightCount;
		capture["gpu_visible_candidate_count"] = diagnostics.gpuVisibleCandidateCount;
		capture["retained_directional_light_count"] = diagnostics.retainedDirectionalLightCount;
		capture["retained_authored_priority_light_count"] = diagnostics.retainedAuthoredPriorityLightCount;
		capture["budget_reduction_occurred"] = diagnostics.budgetReductionOccurred;
		capture["uploaded_reduction_occurred"] = diagnostics.uploadedReductionOccurred;
		capture["special_reduction_occurred"] = diagnostics.specialReductionOccurred;
		capture["shadow_reduction_occurred"] = diagnostics.shadowReductionOccurred;
		capture["retained_light_churn_count"] = diagnostics.retainedLightChurnCount;
		capture["retained_light_replacement_count"] = diagnostics.retainedLightReplacementCount;
		capture["stability_bias_applied_count"] = diagnostics.stabilityBiasAppliedCount;
		capture["shadow_reuse_count"] = diagnostics.shadowReuseCount;
		capture["shadow_replacement_count"] = diagnostics.shadowReplacementCount;
		capture["configured_shadow_capacity"] = diagnostics.configuredShadowCapacity;
		capture["active_shadow_count"] = diagnostics.activeShadowCount;
		capture["allocated_shadow_region_count"] = diagnostics.allocatedRegionCount;
		capture["allocated_shadow_2d_region_count"] = diagnostics.allocated2DRegionCount;
		capture["allocated_shadow_point_region_count"] = diagnostics.allocatedPointRegionCount;
		capture["allocated_shadow_directional_region_count"] = diagnostics.allocatedDirectionalRegionCount;
		capture["allocated_shadow_spot_region_count"] = diagnostics.allocatedSpotRegionCount;
		capture["allocated_shadow_area_region_count"] = diagnostics.allocatedAreaRegionCount;
		capture["estimated_shadow_memory_bytes"] = diagnostics.estimatedShadowMemoryBytes;
		capture["shadow_cache_reuse_count"] = diagnostics.cacheReuseCount;
		capture["shadow_cache_refresh_count"] = diagnostics.cacheRefreshCount;
		capture["shadow_cache_invalidation_count"] = diagnostics.cacheInvalidationCount;
		capture["degraded_shadow_count"] = diagnostics.degradedShadowCount;
		capture["shadow_allocation_failure_count"] = diagnostics.allocationFailureCount;
		capture["camera_visible_shadow_caster_count"] = diagnostics.cameraVisibleShadowCasterCount;
		capture["light_visible_shadow_caster_count"] = diagnostics.lightVisibleShadowCasterCount;
		capture["shadow_draw_submission_count"] = diagnostics.shadowDrawSubmissionCount;
		// Native recording receipts supply these values; candidate groups never
		// inflate diagnostics when upload or indirect command publication fails.
		capture["shadow_grouped_submission_count"] = diagnostics.shadowGroupedSubmissionCount;
		capture["shadow_singleton_submission_count"] = diagnostics.shadowSingletonSubmissionCount;
		capture["shadow_grouped_caster_segment_count"] = diagnostics.shadowGroupedCasterSegmentCount;
		capture["shadow_pass_recorded"] = diagnostics.shadowPassRecorded;
		capture["shadow_assignments"] = nlohmann::json::array();

		for (const ShadowResourceAssignmentDiagnostic& assignment : diagnostics.shadowAssignments) {
			nlohmann::json assignmentJson;
			assignmentJson["light_id"] = TerraEngine::ToHexString(assignment.lightId);
			assignmentJson["resource_kind"] = ShadowResourceKindToString(assignment.resourceKind);
			assignmentJson["resource_slot"] = assignment.resourceSlot;
			assignmentJson["storage_region_id"] = assignment.storageRegionId;
			assignmentJson["atlas_id"] = assignment.atlasId;
			assignmentJson["region_x"] = assignment.regionX;
			assignmentJson["region_y"] = assignment.regionY;
			assignmentJson["region_width"] = assignment.regionWidth;
			assignmentJson["region_height"] = assignment.regionHeight;
			assignmentJson["estimated_bytes"] = assignment.estimatedBytes;
			assignmentJson["frame_serial"] = assignment.frameSerial;
			assignmentJson["frames_since_last_seen"] = assignment.framesSinceLastSeen;
			assignmentJson["retention_score"] = assignment.retentionScore;
			assignmentJson["quality_tier"] = ShadowQualityTierToString(assignment.qualityTier);
			assignmentJson["degradation_reason"] = ShadowDegradationReasonToString(assignment.degradationReason);
			assignmentJson["refresh_reason"] = ShadowRefreshReasonToString(assignment.refreshReason);
			assignmentJson["assigned"] = assignment.assigned;
			assignmentJson["reused_previous_owner"] = assignment.reusedPreviousOwner;
			assignmentJson["replaced_previous_owner"] = assignment.replacedPreviousOwner;
			assignmentJson["shadowed_previous_frame"] = assignment.shadowedPreviousFrame;
			assignmentJson["reused_cached_result"] = assignment.reusedCachedResult;
			assignmentJson["refreshed_this_frame"] = assignment.refreshedThisFrame;
			assignmentJson["invalidated_this_frame"] = assignment.invalidatedThisFrame;
			capture["shadow_assignments"].push_back(std::move(assignmentJson));
		}
		capture["cluster_count"] = snapshot.buildMetadata.clusterCount;
		capture["active_light_count"] = snapshot.buildMetadata.activeLightCount;
		capture["light_index_capacity"] = snapshot.buildMetadata.lightIndexCapacity;
		capture["used_index_count"] = snapshot.buildMetadata.usedIndexCount;
		capture["actual_assigned_light_count"] = snapshot.buildMetadata.actualAssignedLightCount;
		capture["logical_index_overhead_percent"] = logicalIndexOverheadPercent;
		capture["empty_cluster_count"] = snapshot.buildMetadata.emptyClusterCount;
		capture["empty_cluster_percent"] = emptyClusterPercent;
		capture["max_cluster_light_count"] = snapshot.buildMetadata.maxClusterLightCount;
		capture["retained_eligible_light_count"] = snapshot.buildMetadata.maxClusterLightCount;
		capture["out_of_bounds_range_count"] = snapshot.buildMetadata.outOfBoundsRangeCount;
		capture["invalid_light_index_count"] = snapshot.buildMetadata.invalidLightIndexCount;
		capture["overflow_cluster_count"] = snapshot.buildMetadata.overflowClusterCount;
		capture["determinism_mismatch_count"] = snapshot.buildMetadata.determinismMismatchCount;
		capture["range_hash"] = snapshot.buildMetadata.rangeHash;
		capture["range_hash_hex"] = BuildHashHex(snapshot.buildMetadata.rangeHash);
		capture["ranges"] = nlohmann::json::array();


		for (const ClusterLightRangeSnapshot& range : snapshot.ranges) {
			nlohmann::json rangeJson;
			rangeJson["cluster_index"] = range.clusterIndex;
			rangeJson["offset"] = range.offset;
			rangeJson["count"] = range.count;
			rangeJson["out_of_bounds"] = range.outOfBounds;
			capture["ranges"].push_back(std::move(rangeJson));
		}

		std::ofstream outputFile(outputPath);
		outputFile << capture.dump(2);
	}
	catch (const std::exception& exception) {
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to write cluster validation capture: " << exception.what());
	}
}
