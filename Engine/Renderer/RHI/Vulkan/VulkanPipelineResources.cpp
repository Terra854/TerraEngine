// VulkanPipelineResources.cpp — Reflected Vulkan pipeline layouts and pipeline cache entries
//
// This file owns one focused Vulkan resource responsibility. Allocations and retirement
// still flow through VulkanContext::Impl so frame-fence lifetime rules remain unchanged.

#include "VulkanResourceSource.h"
#include "VulkanPostProcessScreenSpaceRaster.h"


bool VulkanContext::Impl::BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants, VkShaderStageFlags fallbackGeometryStages) {
	if (globalDescriptorSetLayout == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Global descriptor set layout is not initialized; cannot build reflected pipeline layouts.");
		return false;
	}


	struct AggregatedBinding {
		DescriptorBinding binding{};
		VkShaderStageFlags stages = 0;
		bool initialized = false;
	};

	std::map<uint32_t, std::map<uint32_t, AggregatedBinding>> setBindings;
	std::map<std::pair<uint32_t, uint32_t>, VkShaderStageFlags> pushConstantMerge;
	supportsDefaultPushConstants = false;


	for (const auto& stage : collection.stages) {
		const Shader* shader = stage.shader.get();
		if (!shader) {
			TERRAENGINE_LOG(LogLevel::Error, "Shader stage missing for collection; cannot build pipeline layouts");
			return false;
		}


		for (const DescriptorSetLayoutDesc& setDesc : shader->descriptorSets) {
			for (uint32_t idx = 0; idx < setDesc.bindingCount; ++idx) {
				const uint32_t bindingIndex = setDesc.firstBinding + idx;

				if (bindingIndex >= shader->descriptorBindings.size()) continue;
			const DescriptorBinding& srcBinding = shader->descriptorBindings[bindingIndex];
			auto& aggregated = setBindings[setDesc.setIndex][srcBinding.binding];

			if (!aggregated.initialized) {
				aggregated.binding = srcBinding;
				if (aggregated.binding.arrayCount == 0) {
					aggregated.binding.arrayCount = 1; // default to 1 so descriptors get valid writes
				}
				aggregated.initialized = true;
			} else {

				if (aggregated.binding.type != srcBinding.type || aggregated.binding.arrayCount != srcBinding.arrayCount) {
					TERRAENGINE_LOG(LogLevel::Warning, "Descriptor binding mismatch across stages for set " << setDesc.setIndex << ", binding " << srcBinding.binding);
				}
				}
				StageVisibilityMask visibility = srcBinding.visibility;

				if (visibility == 0) {
					visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
				}
				aggregated.stages |= StageVisibilityToVulkan(visibility);
			}
		}


		for (const PushConstantRange& pc : shader->pushConstants) {
			StageVisibilityMask visibility = pc.visibility;
			if (visibility == 0) {
				visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
			}
			const VkShaderStageFlags vkStages = StageVisibilityToVulkan(visibility);
			std::pair<uint32_t, uint32_t> key = {pc.offsetBytes, pc.sizeBytes};
			pushConstantMerge[key] |= vkStages;

			if (pc.offsetBytes == 0
				&& pc.sizeBytes >= sizeof(PushConstants)
				&& (vkStages & (VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_VERTEX_BIT))) {
				supportsDefaultPushConstants = true;
			}
		}
	}


	for (const auto& [key, stages] : pushConstantMerge) {
		VkPushConstantRange range{};
		range.offset = key.first;
		range.size = key.second;
		range.stageFlags = stages;
		pushConstants.push_back(range);
	}


	if (pushConstants.empty()) {
		VkPushConstantRange fallback{};
		fallback.offset = 0;
		fallback.size = sizeof(PushConstants);
		fallback.stageFlags = fallbackGeometryStages | VK_SHADER_STAGE_FRAGMENT_BIT;
		pushConstants.push_back(fallback);
		supportsDefaultPushConstants = true;
	}

	// Set 0 is always the Global Bindless Heap
	setLayouts.push_back(globalDescriptorSetLayout);
	bindingsPerSet.push_back({});


	for (const auto& [setIndex, bindingMap] : setBindings) {
		if (setIndex == 0) continue; // Skip Set 0, handled globally

		// Handle gaps if sets are not contiguous (e.g. Set 0 and Set 2 but no Set 1)
		// This simple implementation assumes sequential or just appends.
		// Real robustness would require resizing setLayouts to setIndex+1.
		// For now, we assume materials use Set 1.
		while (setLayouts.size() <= setIndex) {
			setLayouts.push_back(VK_NULL_HANDLE); // Placeholder for gaps?
			bindingsPerSet.push_back({});
		}

		// If we filled a gap with NULL, we need to create a real layout now?
		// Actually, standard behavior: just append.
		// If setIndex is 1, and size is 1 (Set 0), we push back. Correct.

		std::vector<VkDescriptorSetLayoutBinding> vkBindings;
		std::vector<DescriptorBinding> storedBindings;
		vkBindings.reserve(bindingMap.size());
		storedBindings.reserve(bindingMap.size());


		for (const auto& [bindingNumber, aggregated] : bindingMap) {
			VkDescriptorSetLayoutBinding vkBinding{};
			vkBinding.binding = bindingNumber;
			vkBinding.descriptorCount = aggregated.binding.arrayCount;
			vkBinding.descriptorType = MapDescriptorType(static_cast<DescriptorType>(aggregated.binding.type));
			vkBinding.stageFlags = aggregated.stages;
			vkBindings.push_back(vkBinding);
			storedBindings.push_back(aggregated.binding);
		}

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = static_cast<uint32_t>(vkBindings.size());
		layoutInfo.pBindings = vkBindings.data();

		VkDescriptorSetLayout layout = VK_NULL_HANDLE;

		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create descriptor set layout from reflection");
			for (size_t i = 1; i < setLayouts.size(); ++i) { // Start from 1 to avoid destroying global layout

				if (setLayouts[i] != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayouts[i], nullptr);
			}
			return false;
		}


		if (setLayouts.size() <= setIndex) {
			setLayouts.resize(setIndex + 1, VK_NULL_HANDLE);
			bindingsPerSet.resize(setIndex + 1);
		}
		setLayouts[setIndex] = layout;
		bindingsPerSet[setIndex] = std::move(storedBindings);
	}

	return true;
}

namespace
{
	std::uint32_t ScreenRasterMask(const TerraEngine::Renderer::Vulkan::ScreenSpaceRasterResources& raster) noexcept
	{
		return (raster.demand.reflections ? 1u : 0u) | (raster.demand.indirectLighting ? 2u : 0u) | (raster.demand.ambientOcclusion ? 4u : 0u);
	}
	bool ValidScreenFragment(const std::vector<std::uint32_t>& words, bool omitLegacyAo)
	{
		if(words.size() < 5 || words[0] != 0x07230203u) return false;
		std::map<std::uint32_t, std::uint32_t> locations, variables, storage, pointerTypes, vectorTypes, scalarWidths, integerSigned, bindings, sets;
		std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> offsets;
		std::uint32_t push = 0; bool entry = false;
		for(std::size_t i = 5; i < words.size();)
		{
			const auto count = words[i] >> 16, op = words[i] & 65535u;
			if(!count || count > words.size() - i) return false;
			if(op == 15 && count >= 5 && words[i + 1] == 4)
			{
				const auto* name = reinterpret_cast<const char*>(words.data() + i + 3);
				entry = std::memchr(name, 0, (count - 3) * 4) && std::strcmp(name, "main") == 0;
			}
			if(op == 71 && count == 4)
			{
				if(words[i + 2] == 30) locations[words[i + 1]] = words[i + 3];
				if(words[i + 2] == 33) bindings[words[i + 1]] = words[i + 3];
				if(words[i + 2] == 34) sets[words[i + 1]] = words[i + 3];
			}
			if(op == 72 && count == 5 && words[i + 3] == 35) offsets[words[i + 1]][words[i + 2]] = words[i + 4];
			if(op == 59 && count >= 4) { variables[words[i + 2]] = words[i + 1]; storage[words[i + 2]] = words[i + 3]; if(words[i + 3] == 9) push = words[i + 1]; }
			if(op == 32 && count == 4) pointerTypes[words[i + 1]] = words[i + 3];
			if(op == 23 && count == 4) { if(words[i + 3] != 4) vectorTypes[words[i + 1]] = 0; else vectorTypes[words[i + 1]] = words[i + 2]; }
			if(op == 22 && count == 3) { scalarWidths[words[i + 1]] = words[i + 2]; integerSigned[words[i + 1]] = 2; }
			if(op == 21 && count == 4) { scalarWidths[words[i + 1]] = words[i + 2]; integerSigned[words[i + 1]] = words[i + 3]; }
			i += count;
		}
		std::array<bool, 7> seen{};
		for(const auto& [id, location] : locations)
		{
			if(!storage.contains(id) || storage.at(id) != 3) continue;
			if(location >= seen.size() || seen[location] || !variables.contains(id) || !pointerTypes.contains(variables.at(id))) return false;
			const auto type = pointerTypes.at(variables.at(id));
			if(!vectorTypes.contains(type)) return false;
			const auto scalar = vectorTypes.at(type);
			if(!scalarWidths.contains(scalar) || scalarWidths.at(scalar) != 32 || integerSigned.at(scalar) != (location == 1 ? 0u : 2u)) return false;
			seen[location] = true;
		}
		if(!entry || std::find(seen.begin(), seen.end(), false) != seen.end() || !pointerTypes.contains(push)) return false;
		const auto layout = offsets.find(pointerTypes.at(push)); if(layout == offsets.end() || layout->second.size() != 30) return false;
		std::size_t member = 0;
		for(const auto offset : {0u, 64u}) if(layout->second.at(static_cast<std::uint32_t>(member++)) != offset) return false;
		for(std::uint32_t offset = 128; offset < 236; offset += 4) if(layout->second.at(static_cast<std::uint32_t>(member++)) != offset) return false;
		if(layout->second.at(static_cast<std::uint32_t>(member)) != 240) return false;
		bool legacyAoRead = false;
		for(const auto& [id, binding] : bindings) if(binding == 19 && sets.contains(id) && sets.at(id) == 0) legacyAoRead = true;
		return legacyAoRead != omitLegacyAo;
	}

	std::optional<PipelineKey> BuildOwnedPipelineKey(const ShaderCollection& collection,
		TerraEngine::UUID shaderCollectionId, uint32_t materialFlags,
		bool usesConventionalVisibilityIdentity, const VulkanRasterPipelineConfiguration& raster)
	{
		// Stage order and accepted publication identities are exact equality
		// inputs. A bytecode-length digest cannot prove identical module bytes.
		PipelineKey key{};
		key.shaderCollectionId = shaderCollectionId;
		key.materialFlags = materialFlags;
		key.rasterRoute = raster.route;
		key.rasterSamples = static_cast<std::uint32_t>(raster.samples);
		key.screenSpaceMask = raster.screenSpaceMask;
		key.screenSpaceFragmentGeneration = raster.screenSpaceFragmentGeneration;
		if(raster.screenSpaceMask)
		{
			const auto owner = raster.screenSpaceRaster.lock();
			if(raster.route != VulkanRasterRoute::IndexedCorners || !owner || owner->renderPass != raster.qualifiedPass
				|| ScreenRasterMask(*owner) != raster.screenSpaceMask || !raster.screenSpaceFragment || !raster.screenSpaceFragmentGeneration) return std::nullopt;
		}
		else if(raster.screenSpaceFragment || raster.screenSpaceFragmentGeneration) return std::nullopt;
		for (std::size_t i = 0; i < raster.attachmentFormats.size(); ++i) key.rasterAttachmentFormats[i] = static_cast<std::uint32_t>(raster.attachmentFormats[i]);
		key.collectionGeneration = collection.collectionGeneration;
		key.geometryAbiVersion = collection.geometryAbiVersion;
		key.geometryPolicy = static_cast<std::uint32_t>(collection.geometryPolicy);
		key.usesConventionalVisibilityIdentity = usesConventionalVisibilityIdentity;
		key.stagePublications.reserve(collection.stages.size());
		for (const auto& binding : collection.stages)
		{
			if (!binding.shader || binding.publicationGeneration == 0U
				|| binding.shader->publicationGeneration != binding.publicationGeneration)
				return std::nullopt;
			key.stagePublications.push_back({binding.stage, binding.shaderId, binding.publicationGeneration});
		}
		return key;
	}
}

bool VulkanContext::Impl::RegisterIndexedSingleSamplePass(VkRenderPass pass)
{
	// This only accepts the two actual single-sample camera passes created by
	// this backend. The editor presenter itself has no typed identity target.
	if (!UsesConventionalVisibilityAttachments(pass) || UsesVrsAttachment()) return false;
	const VkFormat color = pass == offscreenRenderPass ? offscreenColorFormat : swapchainImageFormat;
	const VkFormat depth = pass == offscreenRenderPass ? FindDepthFormat() : depthFormat;
	if (color == VK_FORMAT_UNDEFINED || depth == VK_FORMAT_UNDEFINED) return false;
	try
	{
		const auto& shape = indexedRasterPasses.try_emplace(pass, IndexedRasterPassCompatibility{
			VK_SAMPLE_COUNT_1_BIT, color, VK_FORMAT_R32G32B32A32_UINT, depth, false}).first->second;
		return shape.samples == VK_SAMPLE_COUNT_1_BIT && shape.colorFormat == color
			&& shape.identityFormat == VK_FORMAT_R32G32B32A32_UINT && shape.depthFormat == depth && !shape.usesVrsAttachment;
	}
	catch (const std::bad_alloc&) { return false; }
}

bool VulkanContext::Impl::RegisterIndexedScreenSpacePass(
	std::shared_ptr<const TerraEngine::Renderer::Vulkan::ScreenSpaceRasterResources> raster, std::string& reason)
{
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex); reason.clear();
	try
	{
		if(!initialized || deviceLost || !raster || !raster->renderPass || !raster->framebuffer || raster->device != device
			|| UsesVrsAttachment() || !raster->owner.sceneTargetGeneration || !raster->owner.view.lifetimeGeneration
			|| !raster->extent.width || !raster->extent.height
			|| raster->owner.lifecycle.rendererSessionGeneration != renderSessionGeneration
			|| raster->owner.lifecycle.backendResourceGeneration != backendResourceGeneration)
		{ reason = "Screen raster registration requires a live exact renderer/target owner and VRS off."; return false; }
		const auto mask = ScreenRasterMask(*raster);
		if(!mask || raster->colors[1].format != VK_FORMAT_R32G32B32A32_UINT
			|| (raster->colors[0].format != VK_FORMAT_R16G16B16A16_SFLOAT && raster->colors[0].format != VK_FORMAT_R32G32B32A32_SFLOAT)
			|| (raster->depth.format != VK_FORMAT_D32_SFLOAT && raster->depth.format != VK_FORMAT_D16_UNORM))
		{ reason = "Screen raster registration has no eligible HDR/identity/depth demand shape."; return false; }
		std::uint32_t physical = 0;
		for(std::size_t logical = 0; logical < 7; ++logical)
		{
			const bool needed = logical < 3 || ((logical == 3 || logical == 5) ? (mask & 5u) != 0 : (mask & 6u) != 0);
			const auto& image = raster->colors[logical]; const auto& reference = raster->colorReferences[logical];
			if(!needed)
			{
				if(image.image || image.view || reference.attachment != VK_ATTACHMENT_UNUSED)
				{ reason = "Unused screen raster output must have no attachment allocation or writes."; return false; }
				continue;
			}
			const auto format = logical == 1 ? VK_FORMAT_R32G32B32A32_UINT : logical == 2 ? VK_FORMAT_R16G16B16A16_SFLOAT : raster->colors[0].format;
			if(!image.image || !image.view || image.format != format || image.owner != raster->owner
				|| image.samples != VK_SAMPLE_COUNT_1_BIT || image.mipLevels != 1
				|| !(image.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
				|| image.extent.width != raster->extent.width || image.extent.height != raster->extent.height
				|| reference.attachment != physical++ || reference.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
			{ reason = "Screen raster logical output differs from its exact qualified seven-location shape."; return false; }
		}
		if(!raster->depth.image || !raster->depth.view || raster->depth.owner != raster->owner
			|| raster->depth.samples != VK_SAMPLE_COUNT_1_BIT || raster->depth.mipLevels != 1
			|| raster->depth.extent.width != raster->extent.width || raster->depth.extent.height != raster->extent.height
			|| !(raster->depth.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
			|| raster->depthAttachmentIndex != physical || raster->attachmentCount != physical + 1)
		{ reason = "Screen raster depth is not the exact registered single-sample attachment."; return false; }
		const auto existing = indexedRasterPasses.find(raster->renderPass);
		if(existing != indexedRasterPasses.end())
		{
			if(existing->second.screenSpaceRaster.lock() == raster && existing->second.screenSpaceMask == mask
				&& existing->second.screenSpaceFragment && existing->second.screenSpaceFragmentGeneration) return true;
			reason = "An existing pass registration must retire explicitly before handle or owner reuse."; return false;
		}
		if(const auto stale = pipelineCache.find(raster->renderPass); stale != pipelineCache.end() && !stale->second.empty())
		{ reason = "An unowned pipeline cache must retire before screen render-pass handle reuse."; return false; }
		const auto file = mask & 4u ? "Shaders/DefaultScreenSpaceAo.frag.spv" : "Shaders/DefaultScreenSpace.frag.spv";
		auto& resources = ResourceManager::Instance(); const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(file));
		if(bytes.empty() || bytes.size() % 4) { reason = "The installed screen-space fragment variant is absent or malformed."; return false; }
		std::vector<std::uint32_t> words(bytes.size() / 4); std::memcpy(words.data(), bytes.data(), bytes.size());
		if(!ValidScreenFragment(words, (mask & 4u) != 0)) { reason = "Screen-space fragment output/push/AO ownership ABI is invalid."; return false; }
		static std::atomic<std::uint64_t> generations{0}; auto previous = generations.load(std::memory_order_relaxed);
		while(previous != std::numeric_limits<std::uint64_t>::max() && !generations.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) {}
		if(previous == std::numeric_limits<std::uint64_t>::max()) { reason = "Screen fragment generation space is exhausted."; return false; }
		IndexedRasterPassCompatibility compatibility{VK_SAMPLE_COUNT_1_BIT, raster->colors[0].format, raster->colors[1].format, raster->depth.format, false};
		compatibility.screenSpaceMask = mask; compatibility.screenSpaceFragmentGeneration = previous + 1;
		compatibility.screenSpaceFragment = std::make_shared<const std::vector<std::uint32_t>>(std::move(words));
		compatibility.screenSpaceRaster = raster;
		indexedRasterPasses.emplace(raster->renderPass, std::move(compatibility));
		return true;
	}
	catch(const std::exception& error) { reason = error.what(); return false; }
}

bool VulkanContext::Impl::RemoveIndexedScreenSpacePass(
	const std::shared_ptr<const TerraEngine::Renderer::Vulkan::ScreenSpaceRasterResources>& raster, std::string& reason)
{
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex); reason.clear();
	if(!raster) { reason = "Screen pass retirement requires its retained completed owner."; return false; }
	const auto registration = indexedRasterPasses.find(raster->renderPass);
	if(registration == indexedRasterPasses.end()) return true;
	if(registration->second.screenSpaceRaster.lock() != raster || !registration->second.screenSpaceMask)
	{ reason = "Screen pass retirement owner does not match the current registration."; return false; }
	try
	{
		// Parent calls only after this target's ACTUAL GPU completion. Collect all
		// fallible bookkeeping before freeing any per-mesh descriptor or pipeline.
		std::set<VkDescriptorSetLayout> layouts;
		const auto cache = pipelineCache.find(raster->renderPass);
		if(cache != pipelineCache.end()) for(const auto& [key, pipeline] : cache->second)
		{
			if(pipeline.screenSpaceMask != registration->second.screenSpaceMask
				|| pipeline.screenSpaceFragmentGeneration != registration->second.screenSpaceFragmentGeneration)
			{ reason = "A different pipeline owner still occupies the retiring screen pass cache."; return false; }
			for(std::size_t i = 1; i < pipeline.setLayouts.size(); ++i) if(pipeline.setLayouts[i]) layouts.insert(pipeline.setLayouts[i]);
		}
		std::vector<VkDescriptorSet> sets;
		for(const auto& [id, mesh] : meshCache) for(const auto& [layout, set] : mesh.descriptorSetsByLayout)
			if(layouts.contains(layout) && set) sets.push_back(set);
		if(sets.size() > std::numeric_limits<std::uint32_t>::max()) { reason = "Completed screen descriptor retirement count overflowed."; return false; }
		if(!sets.empty() && vkFreeDescriptorSets(device, descriptorPool, static_cast<std::uint32_t>(sets.size()), sets.data()) != VK_SUCCESS)
		{ reason = "Completed screen pass descriptor retirement failed."; return false; }
		for(auto& [id, mesh] : meshCache) std::erase_if(mesh.descriptorSetsByLayout, [&](const auto& entry) { return layouts.contains(entry.first); });
		if(cache != pipelineCache.end())
		{
			for(auto& [key, pipeline] : cache->second)
			{
				if(pipeline.pipeline) vkDestroyPipeline(device, pipeline.pipeline, nullptr);
				if(pipeline.layout) vkDestroyPipelineLayout(device, pipeline.layout, nullptr);
			}
			for(auto layout : layouts) vkDestroyDescriptorSetLayout(device, layout, nullptr);
			pipelineCache.erase(cache);
		}
		indexedRasterPasses.erase(registration); return true;
	}
	catch(const std::exception& error) { reason = error.what(); return false; }
}

std::optional<VulkanRasterPipelineConfiguration> VulkanContext::Impl::PrepareDefaultIndexedPipelineConfiguration(
	VkRenderPass pass, VkSampleCountFlagBits samples, std::string& reason)
{
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	if (deviceLost || !initialized) { reason = "The renderer is not ready for indexed preparation."; return std::nullopt; }
	if (samples != VK_SAMPLE_COUNT_1_BIT && samples != VK_SAMPLE_COUNT_4_BIT)
	{ reason = "Indexed reference variants support only one or four samples."; return std::nullopt; }
	const auto compatibility = indexedRasterPasses.find(pass);
	if (compatibility == indexedRasterPasses.end() || compatibility->second.samples != samples
		|| compatibility->second.colorFormat == VK_FORMAT_UNDEFINED || compatibility->second.depthFormat == VK_FORMAT_UNDEFINED
		|| compatibility->second.identityFormat != VK_FORMAT_R32G32B32A32_UINT || compatibility->second.usesVrsAttachment)
	{ reason = "No actual compatible indexed raster pass is registered."; return std::nullopt; }
	auto& resources = ResourceManager::Instance();
	const auto source = resources.AcquireShaderCollectionSnapshot(resources.GetDefaultShaderCollectionID());
	if (!source || !source->reflectionComplete || source->collectionGeneration == 0
		|| source->geometryAbiVersion != 1 || source->usesAnimationBindings || source->usesReservedBindings)
	{ reason = "The exact engine Default surface collection is unavailable."; return std::nullopt; }
	const auto vertexId = resources.LoadShader(resources.ResolveEngineAssetPath("Shaders/DefaultIndexed.vert.tspf"));
	const auto vertex = resources.AcquireShaderSnapshot(vertexId);
	if (!vertex || vertex->publicationGeneration == 0 || vertex->bytecode.empty()
		|| static_cast<ShaderStage>(vertex->reflection.source.stage) != ShaderStage::Vertex)
	{ reason = "The installed reflected DefaultIndexed vertex stage is unavailable."; return std::nullopt; }
	auto collection = std::make_shared<ShaderCollection>(*source);
	collection->stages.clear();
	collection->stages.push_back({ShaderStage::Vertex, vertexId, vertex, vertex->publicationGeneration});
	for (const auto& stage : source->stages)
		if (stage.stage == ShaderStage::Fragment) collection->stages.push_back(stage);
	if (collection->stages.size() != 2)
	{ reason = "The Default indexed variant requires one exact fragment surface stage."; return std::nullopt; }
	reason.clear();
	VulkanRasterPipelineConfiguration configuration;
	configuration.route = VulkanRasterRoute::IndexedCorners;
	configuration.samples = samples;
	configuration.preparedCollection = std::move(collection);
	configuration.qualifiedPass = pass;
	configuration.attachmentFormats = {compatibility->second.colorFormat, compatibility->second.identityFormat, compatibility->second.depthFormat};
	configuration.screenSpaceMask = compatibility->second.screenSpaceMask;
	configuration.screenSpaceFragmentGeneration = compatibility->second.screenSpaceFragmentGeneration;
	configuration.screenSpaceFragment = compatibility->second.screenSpaceFragment;
	configuration.screenSpaceRaster = compatibility->second.screenSpaceRaster;
	return configuration;
}

std::optional<PipelineKey> VulkanContext::Impl::BuildPipelineKey(
	TerraEngine::UUID shaderCollectionId, uint32_t materialFlags,
	bool usesConventionalVisibilityIdentity, const VulkanRasterPipelineConfiguration& raster) const
{
	const auto collection = raster.preparedCollection ? raster.preparedCollection : ResourceManager::Instance().AcquireShaderCollectionSnapshot(shaderCollectionId);
	return collection ? BuildOwnedPipelineKey(*collection, shaderCollectionId,
		materialFlags, usesConventionalVisibilityIdentity, raster) : std::nullopt;
}


void VulkanContext::Impl::EnsurePipeline(
	TerraEngine::UUID shaderCollectionId,
	VkRenderPass renderPass,
	uint32_t materialFlags,
	bool usesConventionalVisibilityIdentity, const VulkanRasterPipelineConfiguration& raster) {

	const bool indexed = raster.route == VulkanRasterRoute::IndexedCorners;
	const bool screenSpaceMrt = indexed && raster.screenSpaceMask != 0;
	const auto screenRasterOwner = screenSpaceMrt ? raster.screenSpaceRaster.lock() : nullptr;
	if(screenSpaceMrt && !screenRasterOwner) return;
	if ((raster.samples != VK_SAMPLE_COUNT_1_BIT && raster.samples != VK_SAMPLE_COUNT_4_BIT)
		|| (!indexed && raster.samples != VK_SAMPLE_COUNT_1_BIT)
		|| (indexed && (raster.qualifiedPass != renderPass || !raster.preparedCollection
			|| shaderCollectionId != ResourceManager::Instance().GetDefaultShaderCollectionID()))) return;
	if (indexed)
	{
		const auto compatibility = indexedRasterPasses.find(renderPass);
		if (compatibility == indexedRasterPasses.end() || compatibility->second.samples != raster.samples
			|| compatibility->second.colorFormat == VK_FORMAT_UNDEFINED
			|| compatibility->second.depthFormat == VK_FORMAT_UNDEFINED
			|| compatibility->second.identityFormat != VK_FORMAT_R32G32B32A32_UINT
			|| compatibility->second.usesVrsAttachment
			|| compatibility->second.screenSpaceMask != raster.screenSpaceMask
			|| compatibility->second.screenSpaceFragmentGeneration != raster.screenSpaceFragmentGeneration
			|| compatibility->second.screenSpaceFragment != raster.screenSpaceFragment
			|| (screenSpaceMrt && (raster.samples != VK_SAMPLE_COUNT_1_BIT
				|| compatibility->second.screenSpaceRaster.lock() != raster.screenSpaceRaster.lock()
				|| raster.screenSpaceRaster.expired()))
			|| raster.attachmentFormats != std::array<VkFormat, 3>{compatibility->second.colorFormat,
				compatibility->second.identityFormat, compatibility->second.depthFormat}) return;
	}
	if (renderPass == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Cannot ensure pipeline: render pass handle is null.");
		return;
	}
	const auto collection = raster.preparedCollection ? raster.preparedCollection : ResourceManager::Instance().AcquireShaderCollectionSnapshot(shaderCollectionId);
	if(collection == nullptr || !collection->reflectionComplete
		|| collection->usesReservedBindings
		|| collection->usesAnimationBindings
		|| collection->geometryAbiVersion != 1u
		|| collection->collectionGeneration == 0u)
	{
		// Include every admission field in the diagnostic. A missing collection
		// and an invalid reflected contract require different repairs, and a
		// generic message made live Vulkan failures unnecessarily ambiguous.
		if(collection == nullptr)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Failed to build graphics pipeline: shader collection "
				<< ToHexString(shaderCollectionId) << " is unavailable.");
		}
		else
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Failed to build graphics pipeline: shader geometry contract is incomplete or incompatible"
				<< " (collection=" << ToHexString(shaderCollectionId)
				<< ", reflectionComplete=" << collection->reflectionComplete
				<< ", usesReservedBindings=" << collection->usesReservedBindings
				<< ", usesAnimationBindings=" << collection->usesAnimationBindings
				<< ", geometryAbiVersion=" << collection->geometryAbiVersion
				<< ", collectionGeneration=" << collection->collectionGeneration
				<< ").");
		}
		return;
	}
	const std::optional<PipelineKey> key = BuildOwnedPipelineKey(*collection,
		shaderCollectionId, materialFlags, usesConventionalVisibilityIdentity, raster);
	if(!key.has_value())
	{
		TERRAENGINE_LOG(LogLevel::Error,
			"Failed to build graphics pipeline: shader module generation is missing.");
		return;
	}

	auto passIt = pipelineCache.find(renderPass);

	if (passIt != pipelineCache.end()) {
		auto cachedPipelineIt = passIt->second.find(*key);
		if (cachedPipelineIt != passIt->second.end()) {
			VulkanPipeline& cached = cachedPipelineIt->second;

			if (cached.pipeline != VK_NULL_HANDLE && cached.layout != VK_NULL_HANDLE) {
				return;
			}

			// A stale/invalid cache entry can appear when creation failed previously.
			// Remove it now so the next path can attempt a clean rebuild.
			if (device != VK_NULL_HANDLE) {
				if (cached.pipeline != VK_NULL_HANDLE) {
					vkDestroyPipeline(device, cached.pipeline, nullptr);
				}

				if (cached.layout != VK_NULL_HANDLE) {
					vkDestroyPipelineLayout(device, cached.layout, nullptr);
				}

				for (size_t layoutIndex = 1; layoutIndex < cached.setLayouts.size(); ++layoutIndex) {

					if (cached.setLayouts[layoutIndex] != VK_NULL_HANDLE) {
						vkDestroyDescriptorSetLayout(device, cached.setLayouts[layoutIndex], nullptr);
					}
				}
			}
			passIt->second.erase(cachedPipelineIt);
		}
	}

	// Cache hits return above. This marker therefore reports only an actual
	// graphics-pipeline build or rebuild, which is the expensive driver path a
	// broad resource-preparation sample cannot distinguish.
	TERRAENGINE_PROFILE_SCOPE("Vulkan: Build Graphics Pipeline", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);

	const bool shadowPipeline = (renderPass == shadowRenderPass);
	const bool conventionalVisibilityPass = screenSpaceMrt || UsesConventionalVisibilityAttachments(renderPass);
	const bool defaultConventionalIdentityVariant = usesConventionalVisibilityIdentity
		&& conventionalVisibilityPass
		&& shaderCollectionId == ResourceManager::Instance().GetDefaultShaderCollectionID();
	const bool defaultTaskConventionalIdentityVariant = usesConventionalVisibilityIdentity
		&& conventionalVisibilityPass
		&& shaderCollectionId == ResourceManager::Instance().GetDefaultTaskShaderCollectionID();
	const bool useConventionalIdentityVariant = defaultConventionalIdentityVariant
		|| defaultTaskConventionalIdentityVariant;
	if (usesConventionalVisibilityIdentity && !useConventionalIdentityVariant) {
		// Authored surface shaders retain their target0-only ABI. The second
		// attachment remains clear for those draws instead of loading an arbitrary
		// SPIR-V fragment module that lacks their reflected descriptor contract.
		TERRAENGINE_LOG(LogLevel::Debug,
			"Conventional visibility identity is unavailable for a non-default surface collection; using target0-only shader ABI.");
	}
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
	bool hasFragmentStage = false;
	bool hasVertexStage = false;
	bool hasMeshStage = false;
	bool hasTaskStage = false;
	bool identityVariantLoadFailed = false;

	for (auto& stage : collection->stages) {
		const Shader* shader = stage.shader.get();
		if (shader) {
			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			std::vector<std::uint32_t> conventionalIdentityBytecode;
			const bool replaceFragmentWithIdentity = useConventionalIdentityVariant
				&& stage.stage == ShaderStage::Fragment && !screenSpaceMrt;
			if (screenSpaceMrt && stage.stage == ShaderStage::Fragment) {
				// Registration owns these exact immutable installed bytes. Never
				// reread a file or substitute an unrelated ordinary identity shader.
				createInfo.codeSize = raster.screenSpaceFragment->size() * sizeof(std::uint32_t);
				createInfo.pCode = raster.screenSpaceFragment->data();
			}
			else if (replaceFragmentWithIdentity) {
				// The artifact is intentionally raw Vulkan SPIR-V rather than a
				// cross-backend .tspf resource. Read it through ResourceManager so an
				// editor session uses the installed tree while an exported game reads
				// the same logical path from its mounted TEPAK archive.
				const std::filesystem::path variantPath = ResourceManager::Instance().GetEngineRoot()
					/ "Shaders" / "Default.ConventionalVisibilityIdentity.frag.spv";
				const std::vector<std::uint8_t> variantBytes =
					ResourceManager::Instance().ReadFileBytes(variantPath);
				if (variantBytes.empty()) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Failed to load Vulkan conventional visibility identity shader: " << variantPath);
					identityVariantLoadFailed = true;
					continue;
				}
				if ((variantBytes.size() % sizeof(std::uint32_t)) != 0U) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Vulkan conventional visibility identity shader has an invalid SPIR-V byte count: " << variantPath);
					identityVariantLoadFailed = true;
					continue;
				}
				conventionalIdentityBytecode.resize(variantBytes.size() / sizeof(std::uint32_t));
				std::memcpy(conventionalIdentityBytecode.data(), variantBytes.data(), variantBytes.size());
				createInfo.codeSize = variantBytes.size();
				createInfo.pCode = conventionalIdentityBytecode.data();
			} else {
				createInfo.codeSize = shader->bytecode.size();
				createInfo.pCode = reinterpret_cast<const uint32_t*>(shader->bytecode.data());
			}

			VkShaderModule shaderModule;

			if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module");
				continue;
			}

			VkPipelineShaderStageCreateInfo stageInfo{};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

			if (stage.stage == ShaderStage::Fragment) {
				stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
				hasFragmentStage = true;
			}
			else if (stage.stage == ShaderStage::Vertex && indexed) {
				stageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
				hasVertexStage = true;
			}
			else if (stage.stage == ShaderStage::Mesh && !indexed) {

				if (!meshShaderSupported) {
					TERRAENGINE_LOG(LogLevel::Error, "Cannot build mesh pipeline because mesh shader support is unavailable on this device.");
					vkDestroyShaderModule(device, shaderModule, nullptr);
					continue;
				}
				stageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
				hasMeshStage = true;
			}
			else if (stage.stage == ShaderStage::Task && !indexed) {

				if (!taskShaderSupported) {
					TERRAENGINE_LOG(LogLevel::Error, "Cannot build task shader stage because task shader support is unavailable on this device.");
					vkDestroyShaderModule(device, shaderModule, nullptr);
					continue;
				}
				stageInfo.stage = VK_SHADER_STAGE_TASK_BIT_EXT;
				hasTaskStage = true;
			}
			else {
				TERRAENGINE_LOG(LogLevel::Warning, "Skipping unsupported shader stage for graphics pipeline build.");
				vkDestroyShaderModule(device, shaderModule, nullptr);
				continue;
			}

			stageInfo.module = shaderModule;
			stageInfo.pName = shader->stringTable.data() + shader->binary.entryPointOffset;
			shaderStages.push_back(stageInfo);
		}
	}
	if (identityVariantLoadFailed) {
		// Do not silently build a mesh-only pipeline after a required fragment
		// variant failed to load; the next frame may recover after installation.
		for (const VkPipelineShaderStageCreateInfo& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}


	if (shaderStages.empty()) {
		TERRAENGINE_LOG(LogLevel::Error, "No shader stages available to build pipeline");
		return;
	}


	if ((!indexed && !hasMeshStage) || (indexed && (!hasVertexStage || !hasFragmentStage || hasMeshStage || hasTaskStage || shaderStages.size() != 2U))) {
		TERRAENGINE_LOG(LogLevel::Error, "Graphics pipeline must include a mesh stage in this renderer.");
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}


	if (hasTaskStage && !hasMeshStage) {
		TERRAENGINE_LOG(LogLevel::Error, "Shader collection includes a task stage without a mesh stage.");
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	std::vector<VkDescriptorSetLayout> setLayouts;
	std::vector<std::vector<DescriptorBinding>> bindingsPerSet;
	std::vector<VkPushConstantRange> pushConstants;
	bool supportsDefaultPushConstants = false;

	if (!BuildPipelineLayouts(*collection, setLayouts, bindingsPerSet, pushConstants, supportsDefaultPushConstants,
		indexed ? VK_SHADER_STAGE_VERTEX_BIT : (VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT))) {

		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 0;
	vertexInputInfo.pVertexBindingDescriptions = nullptr;
	vertexInputInfo.vertexAttributeDescriptionCount = 0;
	vertexInputInfo.pVertexAttributeDescriptions = nullptr;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	// Viewport and scissor are dynamic so editor window resizing and reduced
	// shadow targets use the current framebuffer dimensions instead of the size
	// that happened to be active when the pipeline was created.
	viewport.width = shadowPipeline ? static_cast<float>(SHADOW_MAP_RESOLUTION) : static_cast<float>(swapchainExtent.width);
	viewport.height = shadowPipeline ? static_cast<float>(SHADOW_MAP_RESOLUTION) : static_cast<float>(swapchainExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = shadowPipeline
		? VkExtent2D{SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION}
		: swapchainExtent;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;

	std::array<VkDynamicState, 2> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	// Shadow maps are visibility tests, not final shaded surfaces. Rendering
	// both sides makes primitive winding and negative scale less likely to make
	// a blocker disappear from the light's point of view.
	rasterizer.cullMode = shadowPipeline
		? VK_CULL_MODE_NONE
		: ((materialFlags & DoubleSided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = raster.samples;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	// Shadow pipelines still keep a tiny fragment stage for driver-compatible
	// depth rendering, but the color attachment is only present to satisfy the
	// shared render-pass shape and does not need stored color output.
	colorBlendAttachment.colorWriteMask = shadowPipeline ? 0u : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	colorBlendAttachment.blendEnable = VK_FALSE;

	// The identity target is integer typed. Required independentBlend permits
	// target0-only sky/UI/authored shaders to disable this slot's blending and
	// writes without changing their target0 blend state.
	VkPipelineColorBlendAttachmentState identityBlendAttachment{};
	identityBlendAttachment.colorWriteMask = useConventionalIdentityVariant
		? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
		: 0u;
	identityBlendAttachment.blendEnable = VK_FALSE;
	const std::array<VkPipelineColorBlendAttachmentState, 2> conventionalBlendAttachments = {
		colorBlendAttachment, identityBlendAttachment };
	std::array<VkPipelineColorBlendAttachmentState, 7> screenBlendAttachments{};
	if(screenSpaceMrt)
	{
		for(std::size_t i = 0; i < screenBlendAttachments.size(); ++i)
		{
			// Logical holes stay present in the seven-slot pipeline ABI but have
			// zero writes. Integer identity and every auxiliary target never blend.
			screenBlendAttachments[i].blendEnable = VK_FALSE;
			screenBlendAttachments[i].colorWriteMask = screenRasterOwner->colorReferences[i].attachment == VK_ATTACHMENT_UNUSED ? 0u
				: VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		}
	}

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.logicOp = VK_LOGIC_OP_COPY;
	colorBlending.attachmentCount = screenSpaceMrt ? 7u : conventionalVisibilityPass ? 2u : 1u;
	colorBlending.pAttachments = screenSpaceMrt ? screenBlendAttachments.data() : conventionalVisibilityPass
		? conventionalBlendAttachments.data() : &colorBlendAttachment;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.minDepthBounds = 0.0f;
	depthStencil.maxDepthBounds = 1.0f;
	depthStencil.stencilTestEnable = VK_FALSE;
	depthStencil.front = {};
	depthStencil.back = {};

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
	pipelineLayoutInfo.pPushConstantRanges = pushConstants.data();

	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {

		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {

			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}

		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline layout.");
		return;
	}

	VkPipelineFragmentShadingRateStateCreateInfoKHR vrsState{};
	vrsState.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
	vrsState.fragmentSize = { 1, 1 };


	if (vrsFeatures.primitiveFragmentShadingRate) {
		vrsState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
	} else {
		vrsState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	}


	if (UsesVrsAttachment()) {
		vrsState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
	} else {
		vrsState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	// Indexed reference variants require a pass without a VRS attachment.
	pipelineInfo.pNext = indexed ? nullptr : &vrsState;
	pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
	pipelineInfo.pStages = shaderStages.data();
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = pipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;
	pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

	VkPipeline graphicsPipeline = VK_NULL_HANDLE;

	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {

		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {

			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);

		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline.");
		return;
	}


	for (auto& stage : shaderStages) {
		vkDestroyShaderModule(device, stage.module, nullptr);
	}

	VkShaderStageFlags defaultPushConstantStages = 0;
	bool hasCompatibleDefaultPushRange = false;
	// vkCmdPushConstants validates every byte against every requested stage.
	// Draw submission now pushes each reflected range separately; the full-range
	// mask remains as conservative metadata for pipelines whose stages all share
	// the complete default mesh PushConstants contract.
	for (const VkPushConstantRange& range : pushConstants) {
		if (range.stageFlags != 0u
			&& range.size > 0u
			&& range.offset < sizeof(PushConstants)
			&& range.size <= sizeof(PushConstants) - range.offset) {
			hasCompatibleDefaultPushRange = true;
		}
		if (range.offset == 0u && range.size >= sizeof(PushConstants)) {
			defaultPushConstantStages |= range.stageFlags;
		}
	}
	supportsDefaultPushConstants = hasCompatibleDefaultPushRange;
	if (!supportsDefaultPushConstants) {
		TERRAENGINE_LOG(LogLevel::Warning, "Graphics pipeline push constants do not overlap the default mesh push-constant contract; draw submission will skip default push ranges.");
	}

	VulkanPipeline pipeline{};
	pipeline.pipeline = graphicsPipeline;
	pipeline.rasterRoute = raster.route;
	pipeline.rasterSamples = raster.samples;
	pipeline.rasterCullMode = rasterizer.cullMode;
	pipeline.rasterFrontFace = rasterizer.frontFace;
	pipeline.sourceCollection = collection;
	pipeline.collectionGeneration = key->collectionGeneration;
	pipeline.layout = pipelineLayout;
	pipeline.setLayouts = std::move(setLayouts);
	pipeline.bindingsPerSet = std::move(bindingsPerSet);
	pipeline.pushConstants = std::move(pushConstants);
	pipeline.supportsDefaultPushConstants = supportsDefaultPushConstants;
	pipeline.defaultPushConstantStages = defaultPushConstantStages;
	pipeline.hasTaskStage = hasTaskStage;
	pipeline.usesConventionalVisibilityIdentity = useConventionalIdentityVariant;
	pipeline.screenSpaceMask = raster.screenSpaceMask;
	pipeline.screenSpaceFragmentGeneration = raster.screenSpaceFragmentGeneration;
	pipeline.screenSpaceFragment = raster.screenSpaceFragment;

	pipelineCache[renderPass][*key] = std::move(pipeline);
}

