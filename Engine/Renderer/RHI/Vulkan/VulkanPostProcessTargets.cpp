// VulkanPostProcessTargets.cpp - Transactional finished-image allocation.
#include "VulkanContextImpl.h"
#include "VulkanPostProcessPasses.h"
#include "VulkanPostProcessScaledCamera.h"
#include "VulkanPostProcessScreenSpaceRaster.h"
#include "VulkanPostProcessCustomImages.h"
#include "VulkanPostProcessTemporalFrameState.h"

namespace TerraEngine::Renderer::Vulkan
{
	PostProcessTargetBundle::~PostProcessTargetBundle()
	{
		passes.reset();
		// Descriptor sets borrow image views. Release the pool before any images,
		// including when creation throws halfway through a temporary candidate.
		if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
		if (sampler) vkDestroySampler(device, sampler, nullptr);
		if(customImages)
		{
			customImages.reset();
			return;
		}
		auto destroyImage = [&](PostProcessTargetImage& image) {
			if (image.view) vkDestroyImageView(device, image.view, nullptr);
			if (image.image && image.allocation) vmaDestroyImage(allocator, image.image, image.allocation);
			image = {};
		};
		destroyImage(sceneColor);
		for (auto& image : processing) destroyImage(image);
	}

	bool PostProcessTargetBundle::RetainCustomImageSet(std::string& reason)
	{
		if(customImages) return true;
		// A failed adoption releases nothing. Once the real image set accepts
		// ownership, the copied handles above are only metadata for existing
		// built-in passes and the outer descriptor pool.
		const std::array images{sceneColor, processing[0], processing[1]};
		customImages = AdoptCustomPostProcessImageSet(device, allocator,
			std::span<const PostProcessTargetImage>(images.data(), processingImageCount + 1u), reason);
		return bool(customImages);
	}
}

namespace
{
	using namespace TerraEngine::Renderer::Vulkan;
	constexpr VkImageUsageFlags kPostProcessUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
		| VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
		| VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	bool SameExtent(VkExtent2D first, VkExtent2D second) noexcept
	{
		return first.width == second.width && first.height == second.height;
	}

	bool CreatePostProcessImage(PostProcessTargetBundle& owner, PostProcessTargetImage& image,
		VkExtent2D extent, VkFormat format, PostProcessTargetFailure failure)
	{
		image.extent = extent;
		image.format = format;
		image.usage = kPostProcessUsage;
		VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format;
		info.extent = { extent.width, extent.height, 1 };
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = image.usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VmaAllocationCreateInfo allocation{};
		allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		if (failure == PostProcessTargetFailure::ImageAllocation
			|| vmaCreateImage(owner.allocator, &info, &allocation, &image.image, &image.allocation, nullptr) != VK_SUCCESS)
			return false;
		vmaSetAllocationName(owner.allocator, image.allocation, "Post-processing view target");
		VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		view.image = image.image;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = format;
		view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		return failure != PostProcessTargetFailure::ImageView
			&& vkCreateImageView(owner.device, &view, nullptr, &image.view) == VK_SUCCESS;
	}

	bool CreatePostProcessDescriptors(PostProcessTargetBundle& owner, PostProcessTargetFailure failure)
	{
		VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		sampler.magFilter = VK_FILTER_LINEAR;
		sampler.minFilter = VK_FILTER_LINEAR;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.maxLod = 0.0f;
		if (vkCreateSampler(owner.device, &sampler, nullptr, &owner.sampler) != VK_SUCCESS) return false;
		const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
			{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
		}};
		VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
		layout.pBindings = bindings.data();
		if (vkCreateDescriptorSetLayout(owner.device, &layout, nullptr, &owner.descriptorLayout) != VK_SUCCESS) return false;
		const std::uint32_t imageCount = 1u + owner.processingImageCount;
		const std::array<VkDescriptorPoolSize, 2> sizes{{
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageCount },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imageCount }
		}};
		VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		pool.maxSets = imageCount;
		pool.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
		pool.pPoolSizes = sizes.data();
		if (vkCreateDescriptorPool(owner.device, &pool, nullptr, &owner.descriptorPool) != VK_SUCCESS) return false;
		const std::array<VkDescriptorSetLayout, 3> layouts{ owner.descriptorLayout, owner.descriptorLayout, owner.descriptorLayout };
		VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		allocate.descriptorPool = owner.descriptorPool;
		allocate.descriptorSetCount = imageCount;
		allocate.pSetLayouts = layouts.data();
		std::array<VkDescriptorSet, 3> sets{};
		if (failure == PostProcessTargetFailure::DescriptorAllocation
			|| vkAllocateDescriptorSets(owner.device, &allocate, sets.data()) != VK_SUCCESS) return false;
		const std::array<PostProcessTargetImage*, 3> images{ &owner.sceneColor, &owner.processing[0], &owner.processing[1] };
		for (std::size_t index = 0; index < imageCount; ++index)
		{
			auto& image = *images[index];
			image.descriptor = sets[index];
			const std::array<VkDescriptorImageInfo, 2> values{{
				{ owner.sampler, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
				{ VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_GENERAL }
			}};
			std::array<VkWriteDescriptorSet, 2> writes{};
			for (std::uint32_t binding = 0; binding < writes.size(); ++binding)
			{
				writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[binding].dstSet = image.descriptor;
				writes[binding].dstBinding = binding;
				writes[binding].descriptorCount = 1;
				writes[binding].descriptorType = bindings[binding].descriptorType;
				writes[binding].pImageInfo = &values[binding];
			}
			// These are new private sets, never a descriptor mutation of in-flight
			// work. The actual image layout becomes valid when the recorder uses it.
			vkUpdateDescriptorSets(owner.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
		}
		return true;
	}
}

void VulkanContext::RetireInactiveCameraPostProcessTargets(
	std::span<const TerraEngine::Renderer::PublishedGraphicsCameraView> activeViews)
{
	if (!impl) return;
	std::lock_guard<std::recursive_mutex> lock(impl->deviceStateMutex);
	const auto active = [&](const auto& key) {
		// Generic direct-native views intentionally retain their original lifetime.
		if(!key.viewLifetimeGeneration) return true;
		return std::any_of(activeViews.begin(), activeViews.end(), [&](const auto& entry) {
			if (!entry.target.camera || !entry.target.view) return false;
			const auto& view = *entry.target.view;
			return TerraEngine::Renderer::Vulkan::MatchesPostProcessCamera(key, *entry.target.camera)
				&& key.viewLifetimeGeneration == view.lifetimeGeneration
				&& key.viewKind == (static_cast<std::uint32_t>(view.outputKind) << 8u | static_cast<std::uint32_t>(view.viewType))
				&& key.viewId == view.outputId && key.rendererGeneration == impl->renderSessionGeneration;
		});
	};
	for (auto iterator = impl->postProcessTargets.begin(); iterator != impl->postProcessTargets.end();) {
		const auto& key = iterator->first;
		if (active(key)) { ++iterator; continue; }
		try {
			// Retiring removes new lookup admission only. Existing readbacks retain
			// their images until their actual copy fence completes in the old gate.
			impl->retiredPostProcessTargets.emplace_back();
		} catch (const std::bad_alloc&) { ++iterator; continue; }
		impl->retiredPostProcessTargets.back() = std::move(iterator->second);
		iterator = impl->postProcessTargets.erase(iterator);
	}
	std::erase_if(impl->preparedPostProcessTargets, [&](const auto& entry) { return !active(entry.first); });
	impl->DrainRetiredPostProcessTargets();
}

void VulkanContext::DiscardPostProcessCandidates()
{
	if(!impl) return;
	std::lock_guard lock(impl->deviceStateMutex);
	// These resource sets have never been given to a frame recorder.
	impl->preparedPostProcessTargets.clear();
}

VulkanContext::Impl::PostProcessTargetBundle* VulkanContext::Impl::FindPostProcessTargets(
	const PostProcessTargetKey& key) noexcept
{
	const auto found = postProcessTargets.find(key);
	return found == postProcessTargets.end() ? nullptr : found->second.get();
}

VulkanContext::Impl::PostProcessTargetBundle* VulkanContext::Impl::EnsurePostProcessTargets(
	const PostProcessTargetRequest& request, std::string& reason,
	const std::function<bool(PostProcessTargetBundle&, std::string&)>& preparePasses)
{
	using namespace TerraEngine::Renderer::Vulkan;
	reason.clear();
	// Off never probes formats, acquires Vulkan owners, or changes raw view state.
	if (!request.enabled) return nullptr;
	std::lock_guard<std::recursive_mutex> lock(deviceStateMutex);
	if (!initialized || deviceLost || !device || !allocator || !physicalDevice
		|| request.key.rendererGeneration != renderSessionGeneration)
	{
		reason = "Post-processing target device or renderer generation is unavailable.";
		return nullptr;
	}
	if (request.processingImageCount < 1u || request.processingImageCount > 2u) {
		reason = "The processing chain requests an unsupported intermediate count.";
		return nullptr;
	}
	const auto compatible = [&](const auto* target) {
		return target && SameExtent(target->internalExtent, request.internalExtent)
			&& SameExtent(target->outputExtent, request.outputExtent)
			&& target->processingImageCount == request.processingImageCount
			&& target->passCompatibilityId == request.passCompatibilityId
			&& (!preparePasses || target->passes);
	};
	if (auto* current = FindPostProcessTargets(request.key); compatible(current)) return current;
	// Allocate retirement bookkeeping before touching the accepted owner.
	const auto publish = [&](std::unique_ptr<PostProcessTargetBundle>& source) {
		auto entry = postProcessTargets.try_emplace(request.key).first;
		if(entry->second)
		{
			retiredPostProcessTargets.emplace_back();
			retiredPostProcessTargets.back() = std::move(entry->second);
		}
		entry->second = std::move(source);
		return entry->second.get();
	};
	if(auto staged = preparedPostProcessTargets.find(request.key);
		staged != preparedPostProcessTargets.end() && compatible(staged->second.get()))
	{
		if(request.prepareOnly) return staged->second.get();
		try
		{
			auto* result = publish(staged->second);
			preparedPostProcessTargets.erase(staged);
			return result;
		}
		catch(const std::bad_alloc&)
		{
			reason = "Could not reserve retirement bookkeeping for the prepared color target.";
			return nullptr;
		}
	}
	PostProcessTargetFailure failure = PostProcessTargetFailure::None;
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	failure = std::exchange(postProcessTargetFailure, PostProcessTargetFailure::None);
#endif
	VkFormat selected = VK_FORMAT_UNDEFINED;
	for (VkFormat format : { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT })
	{
		PostProcessFormatSupport support;
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
		support.optimalFeatures = properties.optimalTilingFeatures;
		support.imageQueryResult = vkGetPhysicalDeviceImageFormatProperties(physicalDevice, format,
			VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, kPostProcessUsage, 0, &support.imageProperties);
		if (failure != PostProcessTargetFailure::FormatQualification
			&& QualifiesPostProcessFormat(support, request.internalExtent, kPostProcessUsage)
			&& QualifiesPostProcessFormat(support, request.outputExtent, kPostProcessUsage))
		{
			selected = format;
			break;
		}
	}
	if (selected == VK_FORMAT_UNDEFINED)
	{
		reason = "No linear HDR sampled/storage/copy color format supports the requested extents.";
		return nullptr;
	}
	try
	{
		auto candidate = std::make_unique<PostProcessTargetBundle>();
		candidate->device = device;
		candidate->allocator = allocator;
		candidate->key = request.key;
		candidate->internalExtent = request.internalExtent;
		candidate->outputExtent = request.outputExtent;
		candidate->processingImageCount = request.processingImageCount;
		candidate->passCompatibilityId = request.passCompatibilityId;
		if (nextPostProcessTargetGeneration == std::numeric_limits<std::uint64_t>::max()) {
			reason = "Post-processing target generation space is exhausted.";
			return nullptr;
		}
		candidate->generation = nextPostProcessTargetGeneration;
		bool imagesReady = CreatePostProcessImage(*candidate, candidate->sceneColor, request.internalExtent, selected, failure);
		for (std::uint32_t index = 0; imagesReady && index < candidate->processingImageCount; ++index) {
			imagesReady = CreatePostProcessImage(*candidate, candidate->processing[index],
				request.outputExtent, selected, PostProcessTargetFailure::None);
		}
		if (!imagesReady || !CreatePostProcessDescriptors(*candidate, failure))
		{
			reason = "Post-processing target image, view, sampler, or descriptor allocation failed.";
			return nullptr;
		}
		candidate->resourcesReady = true;
		// Pipeline/framebuffer preparation participates in the same candidate
		// transaction. No previously presented owner changes on callback failure.
		if (preparePasses && (!preparePasses(*candidate, reason) || !candidate->passes)) {
			if (reason.empty()) reason = "Post-processing pass preparation produced no compatible owner.";
			return nullptr;
		}
		++nextPostProcessTargetGeneration;
		if(request.prepareOnly)
		{
			// A whole batch may prepare several views. The current images stay
			// visible until the settings controller accepts every candidate.
			auto staged = preparedPostProcessTargets.try_emplace(request.key).first;
			staged->second = std::move(candidate);
			return staged->second.get();
		}
		return publish(candidate);
	}
	catch (const std::exception&)
	{
		reason = "Post-processing target candidate preparation failed before publication.";
		return nullptr;
	}
}

bool VulkanContext::Impl::ReleasePostProcessTargets(const PostProcessTargetKey& key, std::string& reason)
{
	std::lock_guard<std::recursive_mutex> lock(deviceStateMutex);
	reason.clear();
	const auto entry = postProcessTargets.find(key);
	if (entry == postProcessTargets.end()) return true;
	try {
		retiredPostProcessTargets.emplace_back();
	} catch (const std::bad_alloc&) {
		reason = "Post-processing target retirement bookkeeping is unavailable.";
		return false;
	}
	retiredPostProcessTargets.back() = std::move(entry->second);
	postProcessTargets.erase(entry);
	DrainRetiredPostProcessTargets();
	return true;
}

bool VulkanContext::Impl::HasRetiredOffscreenPostProcessBorrowers(VkImage image)
{
	if(!image) return false;
	const auto borrows = [image](const auto& target) {
		return target && target->borrowedOffscreenImage == image;
	};
	// A staged candidate has no commands and may release its framebuffer
	// immediately. Submitted owners remain bound to their normal fence gates.
	std::erase_if(preparedPostProcessTargets, [&](const auto& entry) { return borrows(entry.second); });
	for(auto entry = postProcessTargets.begin(); entry != postProcessTargets.end();)
	{
		if(!borrows(entry->second)) { ++entry; continue; }
		try { retiredPostProcessTargets.emplace_back(); }
		catch(const std::bad_alloc&) { return true; }
		retiredPostProcessTargets.back() = std::move(entry->second);
		entry = postProcessTargets.erase(entry);
	}
	DrainRetiredPostProcessTargets();
	return std::any_of(retiredPostProcessTargets.begin(), retiredPostProcessTargets.end(), borrows);
}

void VulkanContext::Impl::RecordPostProcessTargetSubmission(const PostProcessTargetKey& key,
	std::uint64_t generation, std::uint32_t frameSlot, VkFence fence) noexcept
{
	// The caller supplies only a fence armed by this successful submission.
	// Unfenced offscreen work must retain pending pass uses until a later queue
	// completion submission actually arms the fence that covers that work.
	if (frameSlot >= postProcessFrameCompletion.size() || !fence) return;
	auto& completion = postProcessFrameCompletion[frameSlot];
	const auto stamp = ++completion.submitted;
	completion.fence = fence;
	const auto mark = [&](PostProcessTargetBundle& target) {
		if (target.key != key || target.generation != generation) return;
		target.lastUseByFrame[frameSlot] = stamp;
		// This epoch belongs to a real covering fence, not a recycled signaled
		// frame slot. It also settles earlier accepted unfenced Scene commands.
		if(target.passes && target.passes->temporalFrameState)
			target.passes->temporalFrameState->AttachCompletion(frameSlot, stamp);
	};
	if (auto* target = FindPostProcessTargets(key)) mark(*target);
	for (auto& target : retiredPostProcessTargets) mark(*target);
}

bool VulkanContext::Impl::RecordPostProcessSourceOverwrite(VkCommandBuffer command, VkImage image) noexcept
{
	// Disabled scenes keep their original route and incur no allocation or pass
	// bookkeeping until a previously active image publication needs invalidation.
	if (postProcessTargets.empty() && retiredPostProcessTargets.empty()) return true;
	if (!command || !image) return false;
	for (const auto& overwrite : pendingPostProcessSourceOverwrites)
		if (overwrite.command == command && overwrite.image == image) return true;
	try { pendingPostProcessSourceOverwrites.push_back({command, image}); return true; }
	catch (...) { return false; }
}

bool VulkanContext::Impl::RecordSceneViewContent(VkCommandBuffer command, const OffscreenResources& target,
	const TerraEngine::Renderer::Vulkan::SceneViewContentStamp& stamp) noexcept
{
	if(!command || !target.view || !target.generation) return false;
	for(auto& pending : pendingSceneViewContent)
	{
		if(pending.command == command && pending.imageView == target.view && pending.targetGeneration == target.generation)
		{
			pending.stamp = stamp; pending.stamp.targetGeneration = target.generation; pending.stamp.imageContentSerial = 0;
			return true;
		}
	}
	try
	{
		PendingSceneViewContent pending{command, target.view, target.generation, stamp};
		pending.stamp.targetGeneration = target.generation; pending.stamp.imageContentSerial = 0;
		pendingSceneViewContent.push_back(pending); return true;
	}
	catch(...) { return false; } // Failure precedes queue admission; never invent content.
}

void VulkanContext::Impl::MarkSceneViewContentCommandsSubmitted(std::span<const VkCommandBuffer> commands) noexcept
{
	// Preserve actual queue writer order, including multiple Scene writes in one
	// submit. Optional POD publication and scalar counters allocate nothing here.
	const auto find = [&](VkImageView view, std::uint64_t generation) -> OffscreenResources* {
		if(sceneView.view == view && sceneView.generation == generation) return &sceneView;
		for(auto& retired : retiredOffscreenResources)
			if(retired.resources.view == view && retired.resources.generation == generation) return &retired.resources;
		return nullptr;
	};
	for(const auto command : commands)
	{
		for(const auto& pending : pendingSceneViewContent)
		{
			if(pending.command != command) continue;
			auto* target = find(pending.imageView, pending.targetGeneration);
			if(!target) continue;
			// An accepted unqualified overwrite invalidates old proof. In particular,
			// a resized first frame cannot keep masquerading as its preceding view.
			if(!pending.stamp.viewLifetimeGeneration || !pending.stamp.frameId
				|| sceneViewContentSerial == (std::numeric_limits<std::uint64_t>::max)())
			{ target->sceneContentStamp.reset(); continue; }
			auto accepted = pending.stamp; accepted.imageContentSerial = ++sceneViewContentSerial;
			target->sceneContentStamp = accepted;
		}
	}
	std::erase_if(pendingSceneViewContent, [&](const auto& pending) {
		return std::find(commands.begin(), commands.end(), pending.command) != commands.end();
	});
}

void VulkanContext::Impl::DiscardSceneViewContentCommands(std::span<const VkCommandBuffer> commands) noexcept
{
	std::erase_if(pendingSceneViewContent, [&](const auto& pending) {
		return std::find(commands.begin(), commands.end(), pending.command) != commands.end();
	});
}

std::optional<TerraEngine::Renderer::Vulkan::SceneViewContentStamp>
VulkanContext::GetSceneViewContentStamp(void* imageView, std::uint64_t targetGeneration) const
{
	if(!impl || !imageView || !targetGeneration) return {};
	// Existing recursive protection supports the editor's already-protected
	// prepare callback. There is no queue lock or filesystem lookup in this read.
	std::lock_guard<std::recursive_mutex> lock(impl->deviceStateMutex);
	if(!impl->initialized || impl->deviceLost || !impl->editorMode) return {};
	const auto matches = [&](const Impl::OffscreenResources& resource) {
		return reinterpret_cast<void*>(resource.view) == imageView && resource.generation == targetGeneration;
	};
	if(matches(impl->sceneView)) return impl->sceneView.sceneContentStamp;
	for(const auto& retired : impl->retiredOffscreenResources)
		if(matches(retired.resources)) return retired.resources.sceneContentStamp;
	return {};
}

void VulkanContext::Impl::MarkPostProcessCommandsSubmitted(std::span<const VkCommandBuffer> commands,
	std::uint32_t frameSlot, VkFence fence) noexcept
{
	MarkPostProcessTimestampCommandsSubmitted(commands, frameSlot, fence);
	if (frameSlot >= postProcessFrameCompletion.size()) return;
	MarkSceneViewContentCommandsSubmitted(commands);
	const auto contains = [&](VkCommandBuffer command) {
		return std::find(commands.begin(), commands.end(), command) != commands.end();
	};
	// Queue order identifies the final writer when several cameras share one
	// physical surface in this frame. Older private images remain fence-owned,
	// but cannot masquerade as the surface's current accepted capture stages.
	const auto eachTarget = [&](const auto& callback) {
		for (auto& [key, target] : postProcessTargets) if (target) callback(*target);
		for (auto& target : retiredPostProcessTargets) if (target) callback(*target);
	};
	for (const auto command : commands) {
		eachTarget([&](PostProcessTargetBundle& target) {
			if(!target.passes || !target.passes->temporalFrameState) return;
			// Accept in actual submitted command order. Temporal output need not
			// be the final composited output, so its proof is the recorded target
			// borrower rather than the latest finished-image publication.
			const bool recordedBorrower=std::any_of(target.pendingUses.begin(),target.pendingUses.end(),
				[&](const auto& use) { return use.command==command && use.frameSlot==frameSlot; });
			if(recordedBorrower) target.passes->temporalFrameState->Accept(command);
		});
		for (const auto& overwrite : pendingPostProcessSourceOverwrites) {
			if (overwrite.command != command) continue;
			eachTarget([&](PostProcessTargetBundle& previous) {
				if (!previous.passes) return;
				auto& output = previous.passes->output;
				if (output.sourceImage == overwrite.image && output.submissionAccepted) {
					output.submissionAccepted = false;
					output.command = VK_NULL_HANDLE;
				}
			});
		}
		eachTarget([&](PostProcessTargetBundle& current) {
			if (!current.passes) return;
			auto& output = current.passes->output;
			if (output.command != command || output.submissionAccepted || !output.sourceImage) return;
			eachTarget([&](PostProcessTargetBundle& previous) {
				if (&previous == &current || !previous.passes) return;
				auto& old = previous.passes->output;
				if (old.submissionAccepted && old.sourceImage == output.sourceImage) {
					old.submissionAccepted = false;
					old.command = VK_NULL_HANDLE;
				}
			});
			output.submissionAccepted = true;
			if(current.passes->scaledCamera) current.passes->scaledCamera->initialized = true;
		});
	}
	const auto publish = [&](PostProcessTargetBundle& target) {
		bool borrowed = false;
		for (auto& use : target.pendingUses) {
			if (use.frameSlot != frameSlot) continue;
			if (contains(use.command)) use.submittedUnfenced = true;
			borrowed |= use.submittedUnfenced;
		}
		if (!fence || !borrowed) return;
		// A new real fence-bearing queue submission covers both its own commands
		// and the earlier unfenced scene prefix on the same graphics queue.
		RecordPostProcessTargetSubmission(target.key, target.generation, frameSlot, fence);
		std::erase_if(target.pendingUses, [frameSlot](const auto& use) {
			return use.frameSlot == frameSlot && use.submittedUnfenced;
		});
	};
	for (auto& [key, target] : postProcessTargets) if (target) publish(*target);
	for (auto& target : retiredPostProcessTargets) publish(*target);
	std::erase_if(pendingPostProcessSourceOverwrites, [&](const auto& overwrite) { return contains(overwrite.command); });
}

void VulkanContext::Impl::DiscardPostProcessCommands(std::span<const VkCommandBuffer> commands) noexcept
{
	DiscardPostProcessTimestampCommands(commands);
	DiscardSceneViewContentCommands(commands);
	const auto contains = [&](VkCommandBuffer command) {
		return std::find(commands.begin(), commands.end(), command) != commands.end();
	};
	const auto discard = [&](PostProcessTargetBundle& target) {
		// A failed/abandoned recording never becomes temporal history. The state
		// owner deliberately preserves accepted unfenced work until real cover.
		if(target.passes && target.passes->temporalFrameState)
			for(const auto command:commands) target.passes->temporalFrameState->Discard(command);
		if (target.passes && !target.passes->output.submissionAccepted
			&& target.passes->output.command && contains(target.passes->output.command))
			target.passes->output = {};
		std::erase_if(target.pendingUses, [&](const auto& use) {
			return !use.submittedUnfenced && contains(use.command);
		});
	};
	for (auto& [key, target] : postProcessTargets) if (target) discard(*target);
	for (auto& target : retiredPostProcessTargets) discard(*target);
	std::erase_if(pendingPostProcessSourceOverwrites, [&](const auto& overwrite) { return contains(overwrite.command); });
}

void VulkanContext::Impl::CompletePostProcessDeviceIdle() noexcept
{
	CompletePostProcessTimestampDeviceIdle();
	// Queue idle proves completion only for submitted work. A recorded command
	// can still be queued afterward and must retain its images until discarded.
	for (auto& completion : postProcessFrameCompletion) completion.completed = completion.submitted;
	const auto complete=[&](PostProcessTargetBundle& target) {
		if(target.passes && target.passes->temporalFrameState)
			target.passes->temporalFrameState->CompleteDeviceIdle();
		CompletePostProcessTargetSubmittedUses(target);
	};
	for (auto& [key, target] : postProcessTargets) if (target) complete(*target);
	for (auto& target : retiredPostProcessTargets) if (target) complete(*target);
}

bool VulkanContext::Impl::ObservePostProcessFrameCompletion(std::uint32_t frameSlot, VkFence fence) noexcept
{
	if (frameSlot >= postProcessFrameCompletion.size() || !device || deviceLost || !fence) return false;
	auto& completion = postProcessFrameCompletion[frameSlot];
	if (completion.submitted == completion.completed) return true;
	if (completion.fence != fence || vkGetFenceStatus(device, fence) != VK_SUCCESS) return false;
	CompletePostProcessTimestampFrame(frameSlot, fence);
	// Only the exact successfully observed fence authorizes temporal history
	// slot reuse. Retired targets can still own prior accepted GPU borrowers.
	const auto complete=[&](PostProcessTargetBundle& target) {
		if(target.passes && target.passes->temporalFrameState)
			target.passes->temporalFrameState->Complete(frameSlot, completion.submitted);
	};
	for(auto& [key,target]:postProcessTargets) if(target) complete(*target);
	for(auto& target:retiredPostProcessTargets) if(target) complete(*target);
	completion.completed = completion.submitted;
	return true;
}

void VulkanContext::Impl::MarkPostProcessEditorDescriptorBorrowed(
	const PostProcessTargetKey& key, std::uint64_t generation) noexcept
{
	if (auto* target = FindPostProcessTargets(key); target && target->generation == generation)
		target->editorDescriptorAcknowledged = false;
}

void VulkanContext::Impl::AcknowledgePostProcessEditorDescriptorRetirement(
	const PostProcessTargetKey& key, std::uint64_t generation) noexcept
{
	const auto acknowledge = [&](PostProcessTargetBundle& target) {
		if (target.key == key && target.generation == generation) target.editorDescriptorAcknowledged = true;
	};
	if (auto* target = FindPostProcessTargets(key)) acknowledge(*target);
	for (auto& target : retiredPostProcessTargets) acknowledge(*target);
}

void VulkanContext::Impl::RecordPostProcessEditorSubmission(std::uint32_t frameSlot, VkFence fence) noexcept
{
	// A retired UI descriptor can still occur in submitted draw data until its
	// explicit acknowledgement. Mark these borrowers as well as current outputs.
	for (auto& [key, target] : postProcessTargets)
		if (target && !target->editorDescriptorAcknowledged)
			RecordPostProcessTargetSubmission(key, target->generation, frameSlot, fence);
	for (auto& target : retiredPostProcessTargets)
		if (!target->editorDescriptorAcknowledged)
			RecordPostProcessTargetSubmission(target->key, target->generation, frameSlot, fence);
}

void VulkanContext::Impl::DrainRetiredPostProcessTargets()
{
	for (auto iterator = retiredPostProcessTargets.begin(); iterator != retiredPostProcessTargets.end();)
	{
		const auto& target = **iterator;
		bool readbacksComplete = !HasInFlightReadbackForImage(target.sceneColor.image);
		for (const auto& image : target.processing) readbacksComplete &= !HasInFlightReadbackForImage(image.image);
		const bool complete = TerraEngine::Renderer::Vulkan::CanRetirePostProcessTarget(target,
			postProcessFrameCompletion, readbacksComplete);
		if (complete) iterator = retiredPostProcessTargets.erase(iterator);
		else ++iterator;
	}
}

void VulkanContext::Impl::RetirePostProcessScreenRaster(void* owner,
	const std::shared_ptr<TerraEngine::Renderer::Vulkan::ScreenSpaceRasterResources>& raster) noexcept
{
	if(!owner || !raster) return;
	// The owning bundle calls this only after retiring accepted borrowers or
	// while discarding a candidate that was never submitted.
	auto& backend = *static_cast<Impl*>(owner);
	backend.RetireSkyBackgroundPipeline(raster->renderPass);
	try
	{
		std::string reason;
		backend.RemoveIndexedScreenSpacePass(raster, reason);
	}
	catch(const std::exception&) {}
}

void VulkanContext::Impl::DestroyPostProcessTargets() noexcept
{
	// Only shutdown with actual idle/device-loss proof may bypass individual
	// frame, readback, and UI acknowledgement gates. Cleanup establishes it first.
	retiredPostProcessTargets.clear();
	postProcessTargets.clear();
	preparedPostProcessTargets.clear();
	pendingPostProcessSourceOverwrites.clear();
	pendingSceneViewContent.clear();
	postProcessFrameCompletion = {};
}
