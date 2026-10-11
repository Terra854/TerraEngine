// VulkanPostProcessing.cpp — Depth history, Hi-Z, ambient occlusion, and shading-rate generation
//
// This file owns one focused VulkanContext responsibility. State remains in VulkanContext::Impl,
// so the split changes compilation boundaries without changing resource lifetime or synchronization.

#include "VulkanContextSource.h"

// VulkanContext::Impl::PrepareDepthForCompute is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::PrepareDepthForCompute(VkCommandBuffer cmd, VkImage sourceDepthImage, VkImageView depthView, bool updateGlobalDepthDescriptor) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (cmd == VK_NULL_HANDLE
		|| sourceDepthImage == VK_NULL_HANDLE
		|| currentFrame >= globalDescriptorSets.size()
		|| depthView == VK_NULL_HANDLE) {
		return false;
	}
	// Admit the persistent descriptor before recording any layout transition.
	// A failed existing drain leaves this image available to the draw fallback.
	if (updateGlobalDepthDescriptor && (globalDescriptorSets[currentFrame] == VK_NULL_HANDLE
		|| !WaitForDescriptorMutationIdle())) return false;

	// Depth-dependent compute passes sample the depth image through a descriptor
	// after graphics has populated it. Vulkan may perform depth writes during
	// either early or late fragment tests, so the barrier must wait on both
	// stages; waiting only on late tests lets Hi-Z read the clear value on
	// drivers that keep simple opaque depth writes in the early path.
	VkImageMemoryBarrier depthBarrier{};
	depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.image = sourceDepthImage;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.subresourceRange.baseMipLevel = 0u;
	depthBarrier.subresourceRange.levelCount = 1u;
	depthBarrier.subresourceRange.baseArrayLayer = 0u;
	depthBarrier.subresourceRange.layerCount = 1u;
	depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &depthBarrier);

	if (!updateGlobalDepthDescriptor) {
		return true;
	}

	// Binding 12 lives in the persistent global descriptor set. Even though the
	// binding is update-after-bind, exported builds can still have a submitted
	// runtime command buffer tracked for this frame slot, so drain that work
	// before changing which depth view clustered lighting samples.
	// The existing drain was checked before the barrier; no additional wait is
	// introduced here and no failed admission can leave a borrowed image layout.

	VkDescriptorImageInfo sampledDepthInfo{};
	sampledDepthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	sampledDepthInfo.imageView = depthView;
	sampledDepthInfo.sampler = VK_NULL_HANDLE;

	VkWriteDescriptorSet depthWrite{};
	depthWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	depthWrite.dstSet = globalDescriptorSets[currentFrame];
	depthWrite.dstBinding = 12u;
	depthWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	depthWrite.descriptorCount = 1u;
	depthWrite.pImageInfo = &sampledDepthInfo;
	vkUpdateDescriptorSets(device, 1u, &depthWrite, 0u, nullptr);
	return true;
}

// VulkanContext::Impl::RestoreDepthForAttachment is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RestoreDepthForAttachment(VkCommandBuffer cmd, VkImage sourceDepthImage) {
	if (cmd == VK_NULL_HANDLE || sourceDepthImage == VK_NULL_HANDLE) {
		return;
	}

	// Pre-scene compute passes borrow the previous frame depth as a sampled
	// image. The main scene render pass immediately after them owns the same
	// image as a writable depth attachment, so restore the layout explicitly
	// before Vulkan's render-pass load/clear operation sees the attachment.
	VkImageMemoryBarrier depthBarrier{};
	depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.image = sourceDepthImage;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.subresourceRange.baseMipLevel = 0u;
	depthBarrier.subresourceRange.levelCount = 1u;
	depthBarrier.subresourceRange.baseArrayLayer = 0u;
	depthBarrier.subresourceRange.layerCount = 1u;
	depthBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &depthBarrier);
}

// VulkanContext::Impl::StoreAmbientOcclusionDepthHistory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::StoreAmbientOcclusionDepthHistory(VkCommandBuffer cmd, VkImage sourceDepthImage, uint32_t viewIndex, const glm::mat4& viewProjection) {
	if (cmd == VK_NULL_HANDLE
		|| sourceDepthImage == VK_NULL_HANDLE
		|| viewIndex >= depthHistoryByView.size()
		|| currentFrame >= MAX_FRAMES_IN_FLIGHT
		|| ambientOcclusionWidth == 0u
		|| ambientOcclusionHeight == 0u) {
		return;
	}

	ViewDepthHistoryState& viewHistory = depthHistoryByView[viewIndex];
	VulkanImage& historyDepth = viewHistory.ambientOcclusionDepthByFrame[currentFrame];
	if (historyDepth.image == VK_NULL_HANDLE || historyDepth.view == VK_NULL_HANDLE) {
		viewHistory.ambientOcclusionDepthValidByFrame[currentFrame] = false;
		return;
	}

	auto TransitionDepthImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = 1u;
		barrier.subresourceRange.baseArrayLayer = 0u;
		barrier.subresourceRange.layerCount = 1u;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	};

	// The scene render pass writes the active view depth attachment. Copy that
	// completed depth into an AO-owned history image so next frame's GTAO pass
	// never samples an attachment that is being cleared or rewritten.
	TransitionDepthImage(
		sourceDepthImage,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);
	TransitionDepthImage(
		historyDepth.image,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageCopy copyRegion{};
	copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	copyRegion.srcSubresource.mipLevel = 0u;
	copyRegion.srcSubresource.baseArrayLayer = 0u;
	copyRegion.srcSubresource.layerCount = 1u;
	copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	copyRegion.dstSubresource.mipLevel = 0u;
	copyRegion.dstSubresource.baseArrayLayer = 0u;
	copyRegion.dstSubresource.layerCount = 1u;
	copyRegion.extent = {
		std::max(ambientOcclusionWidth, 1u),
		std::max(ambientOcclusionHeight, 1u),
		1u
	};
	vkCmdCopyImage(
		cmd,
		sourceDepthImage,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		historyDepth.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1u,
		&copyRegion);

	TransitionDepthImage(
		historyDepth.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	TransitionDepthImage(
		sourceDepthImage,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

	viewHistory.ambientOcclusionDepthViewProjectionByFrame[currentFrame] = viewProjection;
	viewHistory.ambientOcclusionDepthValidByFrame[currentFrame] = true;
}

// VulkanContext::Impl::GenerateHiZPyramid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::GenerateHiZPyramid(VulkanHiZ& hiZ, VkCommandBuffer cmd, VkImage sourceDepthImage, VkImageView depthView, VkExtent2D sourceDepthExtent, uint32_t snapshotSlotIndex, const VulkanHiZHistoryMetadata& completedHistory) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	// Viewport dimensions can change between RenderFrame discovering the desired
	// size and BeginFrame reaching a fence-safe resource recreation boundary.
	// Never dispatch the old pyramid over the new depth image: Texture2D.Load is
	// not guaranteed to make out-of-range coordinates harmless on every driver.
	// Skipping this one generation keeps all geometry visible, and BeginFrame
	// recreates both ping-pong images at the requested extent before retrying.
	if (sourceDepthExtent.width == 0u
		|| sourceDepthExtent.height == 0u
		|| hiZ.width != sourceDepthExtent.width
		|| hiZ.height != sourceDepthExtent.height) {
		hiZ.latestInvalidReason = VulkanHiZInvalidReason::Resize;
		hiZ.latestOcclusionAllowed = false;
		hiZ.latestValidation = {};
		hiZ.latestValidation.width = sourceDepthExtent.width;
		hiZ.latestValidation.height = sourceDepthExtent.height;
		hiZ.latestValidation.pyramidReady = false;
		hiZ.latestValidation.failOpen = true;
		hiZ.latestValidation.invalidReason = VulkanHiZInvalidReason::Resize;
		return false;
	}

	if (cmd == VK_NULL_HANDLE
		|| sourceDepthImage == VK_NULL_HANDLE
		|| depthView == VK_NULL_HANDLE
		|| hiZ.image == VK_NULL_HANDLE
		|| hiZ.fullView == VK_NULL_HANDLE
		|| hiZ.mipLevels == 0u
		|| hiZ.width == 0u
		|| hiZ.height == 0u
		|| hiZ.mipViews.size() < hiZ.mipLevels
		|| hiZComputePipeline == VK_NULL_HANDLE
		|| hiZPipelineLayout == VK_NULL_HANDLE
		|| hiZDescriptorSetLayout == VK_NULL_HANDLE) {
		hiZ.latestInvalidReason = VulkanHiZInvalidReason::MissingPyramid;
		hiZ.latestOcclusionAllowed = false;
		hiZ.latestValidation = {};
		hiZ.latestValidation.width = hiZ.width;
		hiZ.latestValidation.height = hiZ.height;
		hiZ.latestValidation.pyramidReady = false;
		hiZ.latestValidation.invalidReason = VulkanHiZInvalidReason::MissingPyramid;
		return false;
	}

	for (uint32_t mipIndex = 0u; mipIndex < hiZ.mipLevels; ++mipIndex) {
		// Every dispatch binds a descriptor set that points at one concrete mip.
		// Missing views mean swapchain recreation is incomplete, so skip safely.
		if (hiZ.mipViews[mipIndex] == VK_NULL_HANDLE) {
			hiZ.latestInvalidReason = VulkanHiZInvalidReason::MissingPyramid;
			hiZ.latestOcclusionAllowed = false;
			hiZ.latestValidation = {};
			hiZ.latestValidation.width = hiZ.width;
			hiZ.latestValidation.height = hiZ.height;
			hiZ.latestValidation.pyramidReady = false;
			hiZ.latestValidation.invalidReason = VulkanHiZInvalidReason::MissingPyramid;
			return false;
		}
	}

	const uint32_t clampedSnapshotSlot = std::min(snapshotSlotIndex, MAX_RENDER_SNAPSHOTS_PER_FRAME - 1u);
	if (hiZ.mipLevels > MAX_HIZ_DESCRIPTOR_MIP_LEVELS) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping Hi-Z generation because the pyramid exceeds descriptor capacity.");
		return false;
	}
	// Allocate a viewport-independent descriptor range. Scene and Game views may
	// have different mip counts while their command buffers wait for one batched
	// submit, so sizing this pool from the active view would destroy descriptor
	// sets already bound by the earlier view.
	const std::size_t descriptorSetCount =
		static_cast<std::size_t>(MAX_FRAMES_IN_FLIGHT)
		* static_cast<std::size_t>(MAX_RENDER_SNAPSHOTS_PER_FRAME)
		* static_cast<std::size_t>(MAX_HIZ_DESCRIPTOR_MIP_LEVELS);
	if (hiZDescriptorPool == VK_NULL_HANDLE || hiZDescriptorSets.size() != descriptorSetCount) {
		if (hiZDescriptorPool != VK_NULL_HANDLE) {
			// This path is normally hit only after a resize. Wait before
			// destroying the old per-frame sets so no pending frame can still
			// reference them.
			const VkResult idleResult = WaitDeviceIdle();
			if (MarkDeviceLost(idleResult, "Hi-Z descriptor pool resize device idle")) {
				return false;
			}
			if (idleResult != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Skipping Hi-Z descriptor pool resize because the device did not become idle: " << idleResult);
				return false;
			}
			vkDestroyDescriptorPool(device, hiZDescriptorPool, nullptr);
			hiZDescriptorPool = VK_NULL_HANDLE;
			hiZDescriptorSets.clear();
		}

		std::array<VkDescriptorPoolSize, 2> poolSizes{};
		poolSizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		poolSizes[0].descriptorCount = static_cast<uint32_t>(descriptorSetCount);
		poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		poolSizes[1].descriptorCount = static_cast<uint32_t>(descriptorSetCount);

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.maxSets = static_cast<uint32_t>(descriptorSetCount);
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.data();
		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &hiZDescriptorPool) != VK_SUCCESS) {
			hiZDescriptorPool = VK_NULL_HANDLE;
			hiZDescriptorSets.clear();
			return false;
		}

		std::vector<VkDescriptorSetLayout> layouts(descriptorSetCount, hiZDescriptorSetLayout);
		hiZDescriptorSets.resize(descriptorSetCount, VK_NULL_HANDLE);
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = hiZDescriptorPool;
		allocInfo.descriptorSetCount = static_cast<uint32_t>(hiZDescriptorSets.size());
		allocInfo.pSetLayouts = layouts.data();
		if (vkAllocateDescriptorSets(device, &allocInfo, hiZDescriptorSets.data()) != VK_SUCCESS) {
			vkDestroyDescriptorPool(device, hiZDescriptorPool, nullptr);
			hiZDescriptorPool = VK_NULL_HANDLE;
			hiZDescriptorSets.clear();
			return false;
		}
	}

	// Hi-Z can be recorded by multiple render snapshots in one frame, such as
	// scene view and game view. Each snapshot owns a descriptor range so a later
	// view never rewrites descriptors already bound into an earlier command
	// buffer that has not reached vkQueueSubmit yet.
	const std::size_t frameDescriptorBase =
		(static_cast<std::size_t>(currentFrame) * static_cast<std::size_t>(MAX_RENDER_SNAPSHOTS_PER_FRAME)
			+ static_cast<std::size_t>(clampedSnapshotSlot))
		* static_cast<std::size_t>(MAX_HIZ_DESCRIPTOR_MIP_LEVELS);
	if (frameDescriptorBase + hiZ.mipLevels > hiZDescriptorSets.size()) {
		return false;
	}

	// Touch the active view depth image only after every Hi-Z resource is known
	// to be valid. Any early return before this point leaves depth in the
	// render-pass layout, while a complete Hi-Z record restores it before the
	// next frame.
	if (!PrepareDepthForCompute(cmd, sourceDepthImage, depthView, false)) {
		return false;
	}

	auto TransitionHiZMip = [&](uint32_t mipIndex,
		VkImageLayout oldLayout,
		VkImageLayout newLayout,
		VkAccessFlags srcAccess,
		VkAccessFlags dstAccess,
		VkPipelineStageFlags srcStage,
		VkPipelineStageFlags dstStage) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = hiZ.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = mipIndex;
		barrier.subresourceRange.levelCount = 1u;
		barrier.subresourceRange.baseArrayLayer = 0u;
		barrier.subresourceRange.layerCount = 1u;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	};

	auto WriteHiZDescriptorSet = [&](uint32_t mipIndex,
		VkImageView inputView,
		VkImageLayout inputLayout,
		VkImageView outputView) -> VkDescriptorSet {
		VkDescriptorSet descriptorSet = hiZDescriptorSets[frameDescriptorBase + mipIndex];

		VkDescriptorImageInfo inputInfo{};
		inputInfo.imageLayout = inputLayout;
		inputInfo.imageView = inputView;
		inputInfo.sampler = VK_NULL_HANDLE;

		VkDescriptorImageInfo outputInfo{};
		outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		outputInfo.imageView = outputView;
		outputInfo.sampler = VK_NULL_HANDLE;

		std::array<VkWriteDescriptorSet, 2> writes{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = descriptorSet;
		writes[0].dstBinding = 0u;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].descriptorCount = 1u;
		writes[0].pImageInfo = &inputInfo;
		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = descriptorSet;
		writes[1].dstBinding = 1u;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[1].descriptorCount = 1u;
		writes[1].pImageInfo = &outputInfo;
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0u, nullptr);
		return descriptorSet;
	};

	// struct HiZPushConstants mirrors HiZGenerate.comp.hlsl: source dimensions
	// are used for reads, and output dimensions reject inactive workgroup lanes.
	struct HiZPushConstants
	{
		uint32_t inputWidth = 1u;
		uint32_t inputHeight = 1u;
		uint32_t outputWidth = 1u;
		uint32_t outputHeight = 1u;
		uint32_t occlusionState = 0u;
		uint32_t invalidReason = static_cast<uint32_t>(VulkanHiZInvalidReason::FirstFrame);
		uint32_t reserved0 = 0u;
		uint32_t reserved1 = 0u;
	};

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZComputePipeline);

	// First use initializes the allocation inside the normal frame submission.
	// Later writes preserve the submitted shader-read layout. A second snapshot
	// recorded into the same queue batch also observes the first recorded write.
	const bool hiZHasPriorRecordedLayout = hiZ.layoutInitialized || hiZ.layoutTransitionRecorded;
	const VkImageLayout hiZOldLayout = hiZHasPriorRecordedLayout
		? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		: VK_IMAGE_LAYOUT_UNDEFINED;
	const VkAccessFlags hiZSourceAccess = hiZHasPriorRecordedLayout ? VK_ACCESS_SHADER_READ_BIT : 0u;
	const VkPipelineStageFlags hiZSourceStage = hiZHasPriorRecordedLayout
		? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
		: VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

	TransitionHiZMip(
		0u,
		hiZOldLayout,
		VK_IMAGE_LAYOUT_GENERAL,
		hiZSourceAccess,
		VK_ACCESS_SHADER_WRITE_BIT,
		hiZSourceStage,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	VkDescriptorSet levelZeroSet = WriteHiZDescriptorSet(0u, depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, hiZ.mipViews[0]);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZPipelineLayout, 0u, 1u, &levelZeroSet, 0u, nullptr);
	HiZPushConstants levelZeroPush{};
	levelZeroPush.inputWidth = hiZ.width;
	levelZeroPush.inputHeight = hiZ.height;
	levelZeroPush.outputWidth = hiZ.width;
	levelZeroPush.outputHeight = hiZ.height;
	levelZeroPush.occlusionState = hiZ.latestOcclusionAllowed ? 1u : 0u;
	levelZeroPush.invalidReason = static_cast<uint32_t>(hiZ.latestInvalidReason);
	vkCmdPushConstants(cmd, hiZPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(levelZeroPush), &levelZeroPush);
	vkCmdDispatch(cmd, (hiZ.width + 7u) / 8u, (hiZ.height + 7u) / 8u, 1u);

	TransitionHiZMip(
		0u,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	for (uint32_t mipIndex = 1u; mipIndex < hiZ.mipLevels; ++mipIndex) {
		const uint32_t sourceWidth = std::max(1u, hiZ.width >> (mipIndex - 1u));
		const uint32_t sourceHeight = std::max(1u, hiZ.height >> (mipIndex - 1u));
		const uint32_t outputWidth = std::max(1u, hiZ.width >> mipIndex);
		const uint32_t outputHeight = std::max(1u, hiZ.height >> mipIndex);

		TransitionHiZMip(
			mipIndex,
			hiZOldLayout,
			VK_IMAGE_LAYOUT_GENERAL,
			hiZSourceAccess,
			VK_ACCESS_SHADER_WRITE_BIT,
			hiZSourceStage,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

		VkDescriptorSet mipSet = WriteHiZDescriptorSet(
			mipIndex,
			hiZ.mipViews[mipIndex - 1u],
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			hiZ.mipViews[mipIndex]);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZPipelineLayout, 0u, 1u, &mipSet, 0u, nullptr);

		HiZPushConstants mipPush{};
		mipPush.inputWidth = sourceWidth;
		mipPush.inputHeight = sourceHeight;
		mipPush.outputWidth = outputWidth;
		mipPush.outputHeight = outputHeight;
		mipPush.occlusionState = hiZ.latestOcclusionAllowed ? 1u : 0u;
		mipPush.invalidReason = static_cast<uint32_t>(hiZ.latestInvalidReason);
		vkCmdPushConstants(cmd, hiZPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(mipPush), &mipPush);
		vkCmdDispatch(cmd, (outputWidth + 7u) / 8u, (outputHeight + 7u) / 8u, 1u);

		TransitionHiZMip(
			mipIndex,
			VK_IMAGE_LAYOUT_GENERAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_SHADER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	// Restore the active view depth image to the layout expected at the start
	// of the next frame. Keeping this invariant avoids layout mismatches while
	// the independent Hi-Z pyramid remains readable.
	VkImageMemoryBarrier restoreDepthBarrier{};
	restoreDepthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	restoreDepthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	restoreDepthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	restoreDepthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	restoreDepthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	restoreDepthBarrier.image = sourceDepthImage;
	restoreDepthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	restoreDepthBarrier.subresourceRange.baseMipLevel = 0u;
	restoreDepthBarrier.subresourceRange.levelCount = 1u;
	restoreDepthBarrier.subresourceRange.baseArrayLayer = 0u;
	restoreDepthBarrier.subresourceRange.layerCount = 1u;
	restoreDepthBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	restoreDepthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(
		cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		0,
		0, nullptr,
		0, nullptr,
		1, &restoreDepthBarrier);

	hiZ.history = completedHistory;
	hiZ.history.valid = true;
	hiZ.history.warmupFrames = std::max(hiZ.history.warmupFrames, 1u);
	hiZ.history.invalidReason = VulkanHiZInvalidReason::None;
	// This is only a recording promise. EndFrame promotes it to submitted state;
	// resize cancellation clears it so the next command uses UNDEFINED again.
	hiZ.layoutTransitionRecorded = true;
	return true;
}

// VulkanContext::Impl::GenerateAmbientOcclusion is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::GenerateAmbientOcclusion(VkCommandBuffer cmd, const FrameCullingDataGPU& frameData, uint32_t snapshotSlotIndex, VkImageView sampledDepthView) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	const bool aoEnabled = frameData.renderSettings2.y > 0.001f && frameData.renderSettings2.z > 0.5f;
	if (!aoEnabled
		|| cmd == VK_NULL_HANDLE
		|| sampledDepthView == VK_NULL_HANDLE
		|| aoPipelineLayout == VK_NULL_HANDLE
		|| aoDescriptorSetLayout == VK_NULL_HANDLE
		|| currentFrame >= globalDescriptorSets.size()
		|| globalDescriptorSets[currentFrame] == VK_NULL_HANDLE
		|| gtaoComputePipeline == VK_NULL_HANDLE
		|| gtaoBlurComputePipeline == VK_NULL_HANDLE
		|| currentFrame >= ambientOcclusionRawByFrame.size()
		|| ambientOcclusionRawByFrame[currentFrame].image == VK_NULL_HANDLE
		|| ambientOcclusionBlurredByFrame[currentFrame].image == VK_NULL_HANDLE
		|| ambientOcclusionHistoryByFrame[currentFrame].image == VK_NULL_HANDLE
		|| ambientOcclusionRawByFrame[currentFrame].view == VK_NULL_HANDLE
		|| ambientOcclusionBlurredByFrame[currentFrame].view == VK_NULL_HANDLE
		|| ambientOcclusionHistoryByFrame[currentFrame].view == VK_NULL_HANDLE) {
		return false;
	}

	VulkanImage& ambientOcclusionRaw = ambientOcclusionRawByFrame[currentFrame];
	VulkanImage& ambientOcclusionBlurred = ambientOcclusionBlurredByFrame[currentFrame];
	VulkanImage& ambientOcclusionHistory = ambientOcclusionHistoryByFrame[currentFrame];
	bool& ambientOcclusionHistoryValid = ambientOcclusionHistoryValidByFrame[currentFrame];
	const uint32_t width = std::max(ambientOcclusionWidth, 1u);
	const uint32_t height = std::max(ambientOcclusionHeight, 1u);
	const uint32_t clampedSnapshotSlot = std::min(snapshotSlotIndex, MAX_RENDER_SNAPSHOTS_PER_FRAME - 1u);
	// GTAO and blur run back-to-back inside one command buffer. Give each pass
	// its own descriptor set so the blur descriptor update never mutates the
	// GTAO set after that set has already been bound for dispatch.
	constexpr uint32_t kAoDescriptorSetsPerSnapshot = 2u;
	const std::size_t descriptorSetCount =
		static_cast<std::size_t>(MAX_FRAMES_IN_FLIGHT)
		* static_cast<std::size_t>(MAX_RENDER_SNAPSHOTS_PER_FRAME)
		* static_cast<std::size_t>(kAoDescriptorSetsPerSnapshot);
	if (aoDescriptorPool == VK_NULL_HANDLE || aoDescriptorSets.size() != descriptorSetCount) {
		if (aoDescriptorPool != VK_NULL_HANDLE) {
			// AO descriptors are recorded into primary command buffers. Wait
			// before recreating the pool during resize so no pending frame can
			// still reference sets from the old pool.
			const VkResult idleResult = WaitDeviceIdle();
			if (MarkDeviceLost(idleResult, "ambient occlusion descriptor pool resize device idle")) {
				return false;
			}
			if (idleResult != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Skipping ambient occlusion descriptor pool resize because the device did not become idle: " << idleResult);
				return false;
			}
			vkDestroyDescriptorPool(device, aoDescriptorPool, nullptr);
			aoDescriptorPool = VK_NULL_HANDLE;
			aoDescriptorSets.clear();
		}

		std::array<VkDescriptorPoolSize, 2> poolSizes{};
		poolSizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		poolSizes[0].descriptorCount = static_cast<uint32_t>(descriptorSetCount * 3u);
		poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		poolSizes[1].descriptorCount = static_cast<uint32_t>(descriptorSetCount * 2u);

		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.maxSets = static_cast<uint32_t>(descriptorSetCount);
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.data();
		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &aoDescriptorPool) != VK_SUCCESS) {
			aoDescriptorPool = VK_NULL_HANDLE;
			aoDescriptorSets.clear();
			return false;
		}

		std::vector<VkDescriptorSetLayout> layouts(descriptorSetCount, aoDescriptorSetLayout);
		aoDescriptorSets.resize(descriptorSetCount, VK_NULL_HANDLE);
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = aoDescriptorPool;
		allocInfo.descriptorSetCount = static_cast<uint32_t>(aoDescriptorSets.size());
		allocInfo.pSetLayouts = layouts.data();
		if (vkAllocateDescriptorSets(device, &allocInfo, aoDescriptorSets.data()) != VK_SUCCESS) {
			vkDestroyDescriptorPool(device, aoDescriptorPool, nullptr);
			aoDescriptorPool = VK_NULL_HANDLE;
			aoDescriptorSets.clear();
			return false;
		}
	}

	const std::size_t descriptorIndex =
		(static_cast<std::size_t>(currentFrame) * static_cast<std::size_t>(MAX_RENDER_SNAPSHOTS_PER_FRAME)
			+ static_cast<std::size_t>(clampedSnapshotSlot))
		* static_cast<std::size_t>(kAoDescriptorSetsPerSnapshot);
	if (descriptorIndex + 1u >= aoDescriptorSets.size()
		|| aoDescriptorSets[descriptorIndex] == VK_NULL_HANDLE
		|| aoDescriptorSets[descriptorIndex + 1u] == VK_NULL_HANDLE) {
		return false;
	}
	VkDescriptorSet gtaoDescriptorSet = aoDescriptorSets[descriptorIndex];
	VkDescriptorSet blurDescriptorSet = aoDescriptorSets[descriptorIndex + 1u];

	auto TransitionAoImage = [&](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = 1u;
		barrier.subresourceRange.baseArrayLayer = 0u;
		barrier.subresourceRange.layerCount = 1u;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	};

	// The AO targets persist across frames in shader-read layout. Each dispatch
	// temporarily moves one target to GENERAL, writes it, then makes it readable
	// before the next compute pass or material shading samples it.
	TransitionAoImage(
		ambientOcclusionRaw.image,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	VkDescriptorImageInfo depthInfo{};
	depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	depthInfo.imageView = sampledDepthView;
	depthInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo rawOutputInfo{};
	rawOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	rawOutputInfo.imageView = ambientOcclusionRaw.view;
	rawOutputInfo.sampler = VK_NULL_HANDLE;

	std::array<VkWriteDescriptorSet, 2> gtaoWrites{};
	gtaoWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	gtaoWrites[0].dstSet = gtaoDescriptorSet;
	gtaoWrites[0].dstBinding = 12;
	gtaoWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	gtaoWrites[0].descriptorCount = 1;
	gtaoWrites[0].pImageInfo = &depthInfo;

	gtaoWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	gtaoWrites[1].dstSet = gtaoDescriptorSet;
	gtaoWrites[1].dstBinding = 23;
	gtaoWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	gtaoWrites[1].descriptorCount = 1;
	gtaoWrites[1].pImageInfo = &rawOutputInfo;
	vkUpdateDescriptorSets(device, static_cast<uint32_t>(gtaoWrites.size()), gtaoWrites.data(), 0, nullptr);

	// struct GTAOPushConstants groups related state and operations so callers have one documented contract.
	struct GTAOPushConstants
	{
		glm::uvec2 viewportSize = glm::uvec2(1u);
		float radiusPixels = 1.0f;
		float strength = 1.0f;
	};

	const float quality = std::clamp(frameData.renderSettings2.z, 1.0f, 3.0f);
	GTAOPushConstants gtaoPush{};
	gtaoPush.viewportSize = glm::uvec2(width, height);
	gtaoPush.radiusPixels = 1.0f + quality * 1.5f;
	gtaoPush.strength = std::clamp(frameData.renderSettings2.y, 0.0f, 4.0f);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoPipelineLayout, 0, 1, &gtaoDescriptorSet, 0, nullptr);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoComputePipeline);
	vkCmdPushConstants(cmd, aoPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gtaoPush), &gtaoPush);
	vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1u);

	TransitionAoImage(
		ambientOcclusionRaw.image,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	TransitionAoImage(
		ambientOcclusionBlurred.image,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	VkDescriptorImageInfo rawInputInfo{};
	rawInputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	rawInputInfo.imageView = ambientOcclusionRaw.view;
	rawInputInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo blurredOutputInfo{};
	blurredOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	blurredOutputInfo.imageView = ambientOcclusionBlurred.view;
	blurredOutputInfo.sampler = VK_NULL_HANDLE;

	VkDescriptorImageInfo historyInputInfo{};
	historyInputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	historyInputInfo.imageView = ambientOcclusionHistory.view;
	historyInputInfo.sampler = VK_NULL_HANDLE;

	// The blur shader also samples depth for bilateral edge preservation. The
	// descriptor set is separate from GTAO, so it needs its own binding 12 write
	// even though both passes sample the same depth-history image.
	std::array<VkWriteDescriptorSet, 4> blurWrites{};
	blurWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[0].dstSet = blurDescriptorSet;
	blurWrites[0].dstBinding = 12;
	blurWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	blurWrites[0].descriptorCount = 1;
	blurWrites[0].pImageInfo = &depthInfo;

	blurWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[1].dstSet = blurDescriptorSet;
	blurWrites[1].dstBinding = 22;
	blurWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	blurWrites[1].descriptorCount = 1;
	blurWrites[1].pImageInfo = &rawInputInfo;

	blurWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[2].dstSet = blurDescriptorSet;
	blurWrites[2].dstBinding = 24;
	blurWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	blurWrites[2].descriptorCount = 1;
	blurWrites[2].pImageInfo = &blurredOutputInfo;

	blurWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	blurWrites[3].dstSet = blurDescriptorSet;
	blurWrites[3].dstBinding = 21;
	blurWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	blurWrites[3].descriptorCount = 1;
	blurWrites[3].pImageInfo = &historyInputInfo;
	vkUpdateDescriptorSets(device, static_cast<uint32_t>(blurWrites.size()), blurWrites.data(), 0, nullptr);

	// struct GTAOBlurPushConstants groups related state and operations so callers have one documented contract.
	struct GTAOBlurPushConstants
	{
		glm::uvec2 viewportSize = glm::uvec2(1u);
		float depthSigma = 0.0025f;
		float historyBlend = 0.0f;
	};

	GTAOBlurPushConstants blurPush{};
	blurPush.viewportSize = glm::uvec2(width, height);
	blurPush.depthSigma = 0.0025f / quality;
	// Keep temporal AO blending disabled until the resolved AO history is also
	// split per view/camera. The depth input is now owned, but reusing one AO
	// history image across multiple views can still smear between cameras.
	blurPush.historyBlend = 0.0f;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gtaoBlurComputePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoPipelineLayout, 0, 1, &blurDescriptorSet, 0, nullptr);
	vkCmdPushConstants(cmd, aoPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(blurPush), &blurPush);
	vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1u);

	TransitionAoImage(
		ambientOcclusionBlurred.image,
		VK_IMAGE_LAYOUT_GENERAL,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);
	TransitionAoImage(
		ambientOcclusionHistory.image,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_ACCESS_SHADER_READ_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageCopy copyRegion{};
	copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.srcSubresource.mipLevel = 0u;
	copyRegion.srcSubresource.baseArrayLayer = 0u;
	copyRegion.srcSubresource.layerCount = 1u;
	copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.dstSubresource.mipLevel = 0u;
	copyRegion.dstSubresource.baseArrayLayer = 0u;
	copyRegion.dstSubresource.layerCount = 1u;
	copyRegion.extent = { width, height, 1u };
	// Keep the AO material descriptor stable for the lifetime of the frame.
	// The blur pass writes the frame's final AO image, then this copy seeds the
	// next frame's history image without swapping Vulkan handles behind any
	// already-recorded descriptor sets.
	vkCmdCopyImage(
		cmd,
		ambientOcclusionBlurred.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		ambientOcclusionHistory.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1u,
		&copyRegion);

	TransitionAoImage(
		ambientOcclusionBlurred.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_READ_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	TransitionAoImage(
		ambientOcclusionHistory.image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ambientOcclusionHistoryValid = true;
	return true;
}

// VulkanContext::Impl::GenerateVRSImage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::GenerateVRSImage(VkCommandBuffer cmd, VkImageView colorView) {
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (cmd == VK_NULL_HANDLE
		|| lightingComputePipelineLayout == VK_NULL_HANDLE
		|| currentFrame >= globalDescriptorSets.size()
		|| globalDescriptorSets[currentFrame] == VK_NULL_HANDLE
		|| vrsComputePipeline == VK_NULL_HANDLE
		|| vrs.image == VK_NULL_HANDLE
		|| vrs.view == VK_NULL_HANDLE
		|| colorView == VK_NULL_HANDLE
		|| vrs.width == 0u
		|| vrs.height == 0u
		|| vrs.texelSize.width == 0u
		|| vrs.texelSize.height == 0u) {
		return;
	}

	// Transition VRS to GENERAL
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.image = vrs.image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	// Update descriptor set with current color view and VRS image
	VkDescriptorImageInfo colorInfo{};
	colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	colorInfo.imageView = colorView;
	colorInfo.sampler = CurrentHiZ(kHiZGameViewIndex).sampler;

	VkDescriptorImageInfo vrsOutputInfo{};
	vrsOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	vrsOutputInfo.imageView = vrs.view;

	std::array<VkWriteDescriptorSet, 2> vrsWrites{};
	vrsWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	vrsWrites[0].dstSet = globalDescriptorSets[currentFrame];
	vrsWrites[0].dstBinding = 14;
	vrsWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	vrsWrites[0].descriptorCount = 1;
	vrsWrites[0].pImageInfo = &vrsOutputInfo;

	vrsWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	vrsWrites[1].dstSet = globalDescriptorSets[currentFrame];
	vrsWrites[1].dstBinding = 15;
	vrsWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	vrsWrites[1].descriptorCount = 1;
	vrsWrites[1].pImageInfo = &colorInfo;

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(vrsWrites.size()), vrsWrites.data(), 0, nullptr);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vrsComputePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightingComputePipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

	struct {
		uint32_t width;
		uint32_t height;
		uint32_t texelSize;
	} push;
	push.width = swapchainExtent.width;
	push.height = swapchainExtent.height;
	push.texelSize = vrs.texelSize.width;
	vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

	vkCmdDispatch(cmd, (vrs.width + 7) / 8, (vrs.height + 7) / 8, 1);

	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

