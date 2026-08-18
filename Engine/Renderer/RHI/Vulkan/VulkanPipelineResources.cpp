// VulkanPipelineResources.cpp — Reflected Vulkan pipeline layouts and pipeline cache entries
//
// This file owns one focused Vulkan resource responsibility. Allocations and retirement
// still flow through VulkanContext::Impl so frame-fence lifetime rules remain unchanged.

#include "VulkanResourceSource.h"

// VulkanContext::Impl::BuildPipelineLayouts is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants) {
	if (globalDescriptorSetLayout == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Global descriptor set layout is not initialized; cannot build reflected pipeline layouts.");
		return false;
	}

	// struct AggregatedBinding groups related state and operations so callers have one documented contract.
	struct AggregatedBinding {
		DescriptorBinding binding{};
		VkShaderStageFlags stages = 0;
		bool initialized = false;
	};

	std::map<uint32_t, std::map<uint32_t, AggregatedBinding>> setBindings;
	std::map<std::pair<uint32_t, uint32_t>, VkShaderStageFlags> pushConstantMerge;
	supportsDefaultPushConstants = false;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& stage : collection.stages) {
		Shader* shader = ResourceManager::Instance().GetShader(stage.shaderId);
		if (!shader) {
			TERRAENGINE_LOG(LogLevel::Error, "Shader stage missing for collection; cannot build pipeline layouts");
			return false;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const DescriptorSetLayoutDesc& setDesc : shader->descriptorSets) {
			for (uint32_t idx = 0; idx < setDesc.bindingCount; ++idx) {
				const uint32_t bindingIndex = setDesc.firstBinding + idx;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (bindingIndex >= shader->descriptorBindings.size()) continue;
			const DescriptorBinding& srcBinding = shader->descriptorBindings[bindingIndex];
			auto& aggregated = setBindings[setDesc.setIndex][srcBinding.binding];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!aggregated.initialized) {
				aggregated.binding = srcBinding;
				if (aggregated.binding.arrayCount == 0) {
					aggregated.binding.arrayCount = 1; // default to 1 so descriptors get valid writes
				}
				aggregated.initialized = true;
			} else {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (aggregated.binding.type != srcBinding.type || aggregated.binding.arrayCount != srcBinding.arrayCount) {
					TERRAENGINE_LOG(LogLevel::Warning, "Descriptor binding mismatch across stages for set " << setDesc.setIndex << ", binding " << srcBinding.binding);
				}
				}
				StageVisibilityMask visibility = srcBinding.visibility;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (visibility == 0) {
					visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
				}
				aggregated.stages |= StageVisibilityToVulkan(visibility);
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const PushConstantRange& pc : shader->pushConstants) {
			StageVisibilityMask visibility = pc.visibility;
			if (visibility == 0) {
				visibility = StageToMask(static_cast<ShaderStage>(shader->reflection.source.stage));
			}
			const VkShaderStageFlags vkStages = StageVisibilityToVulkan(visibility);
			std::pair<uint32_t, uint32_t> key = {pc.offsetBytes, pc.sizeBytes};
			pushConstantMerge[key] |= vkStages;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (pc.offsetBytes == 0
				&& pc.sizeBytes >= sizeof(PushConstants)
				&& (vkStages & VK_SHADER_STAGE_MESH_BIT_EXT)) {
				supportsDefaultPushConstants = true;
			}
		}
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [key, stages] : pushConstantMerge) {
		VkPushConstantRange range{};
		range.offset = key.first;
		range.size = key.second;
		range.stageFlags = stages;
		pushConstants.push_back(range);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (pushConstants.empty()) {
		VkPushConstantRange fallback{};
		fallback.offset = 0;
		fallback.size = sizeof(PushConstants);
		fallback.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_TASK_BIT_EXT;
		pushConstants.push_back(fallback);
		supportsDefaultPushConstants = true;
	}

	// Set 0 is always the Global Bindless Heap
	setLayouts.push_back(globalDescriptorSetLayout);
	bindingsPerSet.push_back({});

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create descriptor set layout from reflection");
			for (size_t i = 1; i < setLayouts.size(); ++i) { // Start from 1 to avoid destroying global layout
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (setLayouts[i] != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayouts[i], nullptr);
			}
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (setLayouts.size() <= setIndex) {
			setLayouts.resize(setIndex + 1, VK_NULL_HANDLE);
			bindingsPerSet.resize(setIndex + 1);
		}
		setLayouts[setIndex] = layout;
		bindingsPerSet[setIndex] = std::move(storedBindings);
	}

	return true;
}

std::optional<PipelineKey> VulkanContext::Impl::BuildPipelineKey(
	TerraEngine::UUID shaderCollectionId,
	uint32_t materialFlags,
	bool usesConventionalVisibilityIdentity) const
{
	// Pipeline lookup must use every generation and ABI field used by insertion.
	// Otherwise a successfully-created pipeline is unreachable as soon as the
	// cache key grows beyond the collection UUID and material flags.
	ShaderCollection* collection =
		ResourceManager::Instance().GetShaderCollection(shaderCollectionId);
	if(collection == nullptr)
	{
		return std::nullopt;
	}

	std::uint64_t shaderModuleGeneration = 1469598103934665603ull;
	for(const ShaderCollection::StageBinding& binding : collection->stages)
	{
		for(const std::uint8_t byte : binding.shaderId)
		{
			shaderModuleGeneration ^= byte;
			shaderModuleGeneration *= 1099511628211ull;
		}
		const Shader* shader = ResourceManager::Instance().GetShader(binding.shaderId);
		if(shader == nullptr)
		{
			return std::nullopt;
		}
		shaderModuleGeneration ^= shader->bytecode.size();
		shaderModuleGeneration *= 1099511628211ull;
	}

	return PipelineKey{
		shaderCollectionId,
		materialFlags,
		collection->collectionGeneration,
		shaderModuleGeneration,
		collection->geometryAbiVersion,
		static_cast<std::uint32_t>(collection->geometryPolicy),
		usesConventionalVisibilityIdentity
	};
}

// VulkanContext::Impl::EnsurePipeline is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::EnsurePipeline(
	TerraEngine::UUID shaderCollectionId,
	VkRenderPass renderPass,
	uint32_t materialFlags,
	bool usesConventionalVisibilityIdentity) {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (renderPass == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "Cannot ensure pipeline: render pass handle is null.");
		return;
	}
	ShaderCollection* collection =
		ResourceManager::Instance().GetShaderCollection(shaderCollectionId);
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
	const std::optional<PipelineKey> key = BuildPipelineKey(
		shaderCollectionId, materialFlags, usesConventionalVisibilityIdentity);
	if(!key.has_value())
	{
		TERRAENGINE_LOG(LogLevel::Error,
			"Failed to build graphics pipeline: shader module generation is missing.");
		return;
	}

	auto passIt = pipelineCache.find(renderPass);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (passIt != pipelineCache.end()) {
		auto cachedPipelineIt = passIt->second.find(*key);
		if (cachedPipelineIt != passIt->second.end()) {
			VulkanPipeline& cached = cachedPipelineIt->second;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (cached.pipeline != VK_NULL_HANDLE && cached.layout != VK_NULL_HANDLE) {
				return;
			}

			// A stale/invalid cache entry can appear when creation failed previously.
			// Remove it now so the next path can attempt a clean rebuild.
			if (device != VK_NULL_HANDLE) {
				if (cached.pipeline != VK_NULL_HANDLE) {
					vkDestroyPipeline(device, cached.pipeline, nullptr);
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (cached.layout != VK_NULL_HANDLE) {
					vkDestroyPipelineLayout(device, cached.layout, nullptr);
				}
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (size_t layoutIndex = 1; layoutIndex < cached.setLayouts.size(); ++layoutIndex) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	const bool conventionalVisibilityPass = UsesConventionalVisibilityAttachments(renderPass);
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
	bool hasMeshStage = false;
	bool hasTaskStage = false;
	bool identityVariantLoadFailed = false;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& stage : collection->stages) {
		Shader* shader = ResourceManager::Instance().GetShader(stage.shaderId);
		if (shader) {
			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			std::vector<std::uint32_t> conventionalIdentityBytecode;
			const bool replaceFragmentWithIdentity = useConventionalIdentityVariant
				&& stage.stage == ShaderStage::Fragment;
			if (replaceFragmentWithIdentity) {
				// The artifact is intentionally raw Vulkan SPIR-V rather than a
				// cross-backend .tspf resource. Resolve it from the installed engine
				// root so projects launched from arbitrary working directories agree.
				const std::filesystem::path variantPath = ResourceManager::Instance().GetEngineRoot()
					/ "Shaders" / "Default.ConventionalVisibilityIdentity.frag.spv";
				std::ifstream variantFile(variantPath, std::ios::ate | std::ios::binary);
				if (!variantFile.is_open()) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Failed to open Vulkan conventional visibility identity shader: " << variantPath);
					identityVariantLoadFailed = true;
					continue;
				}
				const std::streamsize byteCount = variantFile.tellg();
				if (byteCount <= 0 || (byteCount % static_cast<std::streamsize>(sizeof(std::uint32_t))) != 0) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Vulkan conventional visibility identity shader has an invalid SPIR-V byte count: " << variantPath);
					identityVariantLoadFailed = true;
					continue;
				}
				conventionalIdentityBytecode.resize(static_cast<std::size_t>(byteCount) / sizeof(std::uint32_t));
				variantFile.seekg(0);
				variantFile.read(reinterpret_cast<char*>(conventionalIdentityBytecode.data()), byteCount);
				if (!variantFile) {
					TERRAENGINE_LOG(LogLevel::Error,
						"Failed to read Vulkan conventional visibility identity shader: " << variantPath);
					identityVariantLoadFailed = true;
					continue;
				}
				createInfo.codeSize = static_cast<std::size_t>(byteCount);
				createInfo.pCode = conventionalIdentityBytecode.data();
			} else {
				createInfo.codeSize = shader->bytecode.size();
				createInfo.pCode = reinterpret_cast<const uint32_t*>(shader->bytecode.data());
			}

			VkShaderModule shaderModule;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module");
				continue;
			}

			VkPipelineShaderStageCreateInfo stageInfo{};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (stage.stage == ShaderStage::Fragment) {
				stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			}
			else if (stage.stage == ShaderStage::Mesh) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!meshShaderSupported) {
					TERRAENGINE_LOG(LogLevel::Error, "Cannot build mesh pipeline because mesh shader support is unavailable on this device.");
					vkDestroyShaderModule(device, shaderModule, nullptr);
					continue;
				}
				stageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
				hasMeshStage = true;
			}
			else if (stage.stage == ShaderStage::Task) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (shaderStages.empty()) {
		TERRAENGINE_LOG(LogLevel::Error, "No shader stages available to build pipeline");
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!hasMeshStage) {
		TERRAENGINE_LOG(LogLevel::Error, "Graphics pipeline must include a mesh stage in this renderer.");
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!BuildPipelineLayouts(*collection, setLayouts, bindingsPerSet, pushConstants, supportsDefaultPushConstants)) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.logicOp = VK_LOGIC_OP_COPY;
	colorBlending.attachmentCount = conventionalVisibilityPass ? 2u : 1u;
	colorBlending.pAttachments = conventionalVisibilityPass
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline layout.");
		return;
	}

	VkPipelineFragmentShadingRateStateCreateInfoKHR vrsState{};
	vrsState.sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR;
	vrsState.fragmentSize = { 1, 1 };

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vrsFeatures.primitiveFragmentShadingRate) {
		vrsState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
	} else {
		vrsState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (UsesVrsAttachment()) {
		vrsState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR;
	} else {
		vrsState.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.pNext = &vrsState;
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t layoutIndex = 1; layoutIndex < setLayouts.size(); ++layoutIndex) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (setLayouts[layoutIndex] != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(device, setLayouts[layoutIndex], nullptr);
			}
		}
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& stage : shaderStages) {
			vkDestroyShaderModule(device, stage.module, nullptr);
		}
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create graphics pipeline.");
		return;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
	pipeline.layout = pipelineLayout;
	pipeline.setLayouts = std::move(setLayouts);
	pipeline.bindingsPerSet = std::move(bindingsPerSet);
	pipeline.pushConstants = std::move(pushConstants);
	pipeline.supportsDefaultPushConstants = supportsDefaultPushConstants;
	pipeline.defaultPushConstantStages = defaultPushConstantStages;
	pipeline.hasTaskStage = hasTaskStage;
	pipeline.usesConventionalVisibilityIdentity = useConventionalIdentityVariant;

	pipelineCache[renderPass][*key] = std::move(pipeline);
}

