// VulkanAnimationResources.cpp — GPU animation and deformation resource publication
//
// This file owns one focused Vulkan resource responsibility. Allocations and retirement
// still flow through VulkanContext::Impl so frame-fence lifetime rules remain unchanged.

#include "VulkanResourceSource.h"

void VulkanContext::Impl::RetireVulkanAnimationFrameResources(VulkanAnimationFrameResources& resources)
{
	// Frame fences, rather than CPU pose readback, own GPU animation lifetime.
	// RetireBuffer places every old allocation in the existing frame-slot domain
	// so a replacement cannot free memory referenced by an earlier submission.
	for(VulkanBuffer& buffer : resources.buffers) {
		RetireBuffer(buffer);
	}
	RetireBuffer(resources.graphicsBridgeBuffer);
	// Bindless slots are frame-resource identities. Returning them only with the
	// retired buffers prevents a graphics draw from observing a recycled index
	// while an older frame can still sample its original animation allocation.
	const std::array<std::uint32_t, 4U> bindlessIndices{
		resources.generationRecordsBindlessIndex,
		resources.workPacketsBindlessIndex,
		resources.paletteMatricesBindlessIndex,
		resources.morphOutputsBindlessIndex };
	for(const std::uint32_t index : bindlessIndices) {
		if(index != INVALID_BINDLESS_BUFFER_INDEX) freeBufferIndices.push_back(index);
	}
	if(resources.graphicsBridgeBindlessIndex != INVALID_BINDLESS_BUFFER_INDEX)
		freeBufferIndices.push_back(resources.graphicsBridgeBindlessIndex);
	if(activeAnimationGraphicsBufferIndex == resources.graphicsBridgeBindlessIndex)
		activeAnimationGraphicsBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	resources = {};
}

void VulkanContext::Impl::RetireVulkanCpuAnimationFrameResources(
	VulkanCpuAnimationFrameResources& resources)
{
	RetireBuffer(resources.paletteBuffer);
	RetireBuffer(resources.morphWeightBuffer);
	RetireBuffer(resources.graphicsBridgeBuffer);
	const std::array<std::uint32_t, 3U> bindlessIndices{
		resources.paletteBindlessIndex,
		resources.morphWeightBindlessIndex,
		resources.graphicsBridgeBindlessIndex};
	for(const std::uint32_t index : bindlessIndices) {
		if(index != INVALID_BINDLESS_BUFFER_INDEX) {
			freeBufferIndices.push_back(index);
		}
	}
	if(activeCpuAnimationGraphicsBufferIndex
		== resources.graphicsBridgeBindlessIndex) {
		activeCpuAnimationGraphicsBufferIndex =
			INVALID_BINDLESS_BUFFER_INDEX;
	}
	resources = {};
}

bool VulkanContext::Impl::PublishVulkanCpuAnimationResources(
	const TerraEngine::Renderer::CpuAnimationDeformationPlan& plan)
{
	activeCpuAnimationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	if(!plan.IsComplete() || currentFrame >= MAX_FRAMES_IN_FLIGHT
		|| plan.records.size() > std::numeric_limits<std::uint32_t>::max()) {
		return false;
	}
	const bool currentFrameHasRecordedCommands =
		currentFrame < pendingPrimaryCommandBuffers.size()
		&& !pendingPrimaryCommandBuffers[currentFrame].empty();
	const bool currentFrameWasSubmitted =
		currentFrame < pendingPrimaryCommandBuffersSubmitted.size()
		&& pendingPrimaryCommandBuffersSubmitted[currentFrame] != 0U;
	if(currentFrameHasRecordedCommands || currentFrameWasSubmitted) {
		// Global bindless descriptors are mutable host state. CPU deformation is
		// published once before any view records, never between Scene/Game views.
		return false;
	}

	// Synchronized visual clones publish distinct instance records, but their
	// local-space palette and morph bytes are often identical. Build one immutable
	// upload slice per 128-bit content identity, then point every clone bridge at
	// that slice. Model transforms remain draw-owned, so sharing these bytes cannot
	// collapse object placement or instance generations.
	struct SharedCpuAnimationContentKey
	{
		std::uint64_t low = 0U;
		std::uint64_t high = 0U;

		bool operator==(const SharedCpuAnimationContentKey&) const = default;
	};
	struct SharedCpuAnimationContentKeyHash
	{
		std::size_t operator()(const SharedCpuAnimationContentKey& key) const noexcept
		{
			const std::uint64_t rotatedHigh = (key.high << 1U) | (key.high >> 63U);
			return static_cast<std::size_t>(key.low ^ rotatedHigh);
		}
	};
	struct SharedCpuAnimationUploadSlice
	{
		const TerraEngine::Renderer::CpuAnimationDeformationData* data = nullptr;
		std::uint64_t paletteOffsetBytes = 0U;
		std::uint64_t morphOffsetBytes = 0U;
	};

	std::size_t paletteCount = 0U;
	std::size_t morphWeightCount = 0U;
	std::vector<SharedCpuAnimationUploadSlice> sharedSlices;
	std::vector<std::size_t> recordSliceIndices;
	std::unordered_map<SharedCpuAnimationContentKey, std::size_t,
		SharedCpuAnimationContentKeyHash> sharedSliceByContent;
	try {
		sharedSlices.reserve(plan.records.size());
		recordSliceIndices.reserve(plan.records.size());
		sharedSliceByContent.reserve(plan.records.size());
	} catch(const std::bad_alloc&) {
		return false;
	}
	for(const TerraEngine::Renderer::CpuAnimationDeformationRecord& record
		: plan.records) {
		if(!record.IsComplete()
			|| record.rendererGeneration != plan.rendererGeneration) {
			return false;
		}

		const SharedCpuAnimationContentKey key{
			record.data->contentIdLow,
			record.data->contentIdHigh};
		const auto existing = sharedSliceByContent.find(key);
		if(existing != sharedSliceByContent.end()) {
			recordSliceIndices.push_back(existing->second);
			continue;
		}
		if(record.data->paletteMatrices.size()
				> std::numeric_limits<std::size_t>::max() - paletteCount
			|| record.data->morphWeights.size()
				> std::numeric_limits<std::size_t>::max() - morphWeightCount) {
			return false;
		}
		const std::size_t sliceIndex = sharedSlices.size();
		sharedSlices.push_back({
			record.data.get(),
			static_cast<std::uint64_t>(paletteCount)
				* sizeof(TerraEngine::Animation::GpuAnimationPaletteRecord),
			static_cast<std::uint64_t>(morphWeightCount) * sizeof(float)});
		sharedSliceByContent.emplace(key, sliceIndex);
		recordSliceIndices.push_back(sliceIndex);
		paletteCount += record.data->paletteMatrices.size();
		morphWeightCount += record.data->morphWeights.size();
	}
	if(paletteCount == 0U
		|| paletteCount > std::numeric_limits<VkDeviceSize>::max()
			/ sizeof(TerraEngine::Animation::GpuAnimationPaletteRecord)
		|| morphWeightCount > std::numeric_limits<VkDeviceSize>::max()
			/ sizeof(float)
		|| plan.records.size() > std::numeric_limits<VkDeviceSize>::max()
			/ sizeof(VulkanGpuAnimationGraphicsRecord)) {
		return false;
	}
	const VkDeviceSize paletteBytes = static_cast<VkDeviceSize>(
		paletteCount
			* sizeof(TerraEngine::Animation::GpuAnimationPaletteRecord));
	const VkDeviceSize morphWeightBytes = static_cast<VkDeviceSize>(
		morphWeightCount * sizeof(float));
	const VkDeviceSize bridgeBytes = static_cast<VkDeviceSize>(
		plan.records.size() * sizeof(VulkanGpuAnimationGraphicsRecord));
	if(maximumStorageBufferRange == 0U
		|| paletteBytes > maximumStorageBufferRange
		|| morphWeightBytes > maximumStorageBufferRange
		|| bridgeBytes == 0U || bridgeBytes > maximumStorageBufferRange) {
		return false;
	}

	if(cpuAnimationFrameResources.size() < MAX_FRAMES_IN_FLIGHT) {
		cpuAnimationFrameResources.resize(MAX_FRAMES_IN_FLIGHT);
	}
	VulkanCpuAnimationFrameResources& resources =
		cpuAnimationFrameResources[currentFrame];
	if(resources.live
		&& resources.rendererGeneration != plan.rendererGeneration) {
		RetireVulkanCpuAnimationFrameResources(resources);
	}
	// These vectors are private to the fence-owned frame slot. Clearing preserves
	// capacity, which avoids allocator traffic while still rebuilding every
	// pose-dependent byte from the current immutable CPU animation plan.
	auto& palettes = resources.paletteUploadScratch;
	auto& morphWeights = resources.morphWeightUploadScratch;
	auto& bridgeRecords = resources.bridgeUploadScratch;
	palettes.clear();
	morphWeights.clear();
	bridgeRecords.clear();
	try {
		palettes.reserve(paletteCount);
		morphWeights.reserve(morphWeightCount);
		bridgeRecords.reserve(plan.records.size());
		for(const SharedCpuAnimationUploadSlice& slice : sharedSlices) {
			if(slice.data == nullptr) return false;
			palettes.insert(palettes.end(),
				slice.data->paletteMatrices.begin(),
				slice.data->paletteMatrices.end());
			morphWeights.insert(morphWeights.end(),
				slice.data->morphWeights.begin(),
				slice.data->morphWeights.end());
		}
		const std::uint32_t planGeneration = static_cast<std::uint32_t>(
			(plan.frameId % std::numeric_limits<std::uint32_t>::max()) + 1U);
		for(std::size_t recordIndex = 0U;
			recordIndex < plan.records.size(); ++recordIndex) {
			const TerraEngine::Renderer::CpuAnimationDeformationRecord& source =
				plan.records[recordIndex];
			if(recordIndex >= recordSliceIndices.size()
				|| recordSliceIndices[recordIndex] >= sharedSlices.size()) {
				return false;
			}
			const SharedCpuAnimationUploadSlice& shared =
				sharedSlices[recordSliceIndices[recordIndex]];

			VulkanGpuAnimationGraphicsRecord bridge{};
			bridge.planGeneration = planGeneration;
			bridge.rendererGeneration = static_cast<std::uint32_t>(
				std::min<std::uint64_t>(
					plan.rendererGeneration,
					std::numeric_limits<std::uint32_t>::max()));
			bridge.workPacketIndex =
				static_cast<std::uint32_t>(recordIndex);
			bridge.generationRecordIndex = 0U;
			bridge.paletteOffsetBytes = shared.paletteOffsetBytes;
			bridge.morphOutputOffsetBytes = shared.morphOffsetBytes;
			bridge.jointCount = source.data->jointCount;
			bridge.morphCount = static_cast<std::uint32_t>(
				source.data->morphWeights.size());
			bridge.paletteEntryCount = static_cast<std::uint32_t>(
				source.data->paletteMatrices.size());
			bridge.stageMask = TerraEngine::Animation::GpuAnimationStageMask(
				TerraEngine::Animation::GpuAnimationStage::Palette);
			if(bridge.morphCount != 0U) {
				bridge.stageMask |=
					TerraEngine::Animation::GpuAnimationStageMask(
						TerraEngine::Animation::GpuAnimationStage::Morph);
			}
			bridge.validMarker = 1U;
			bridgeRecords.push_back(bridge);
		}
	} catch(const std::bad_alloc&) {
		return false;
	}

	const auto ensureBuffer = [this](VulkanBuffer& buffer,
		VkDeviceSize& capacity, VkDeviceSize requiredBytes,
		const char* name) {
		const VkDeviceSize safeBytes = std::max<VkDeviceSize>(
			requiredBytes, 16U);
		if(buffer.buffer == VK_NULL_HANDLE || capacity < safeBytes) {
			RetireBuffer(buffer);
			buffer = CreateBuffer(safeBytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_CPU_TO_GPU, name);
			capacity = safeBytes;
		}
	};
	try {
		ensureBuffer(resources.paletteBuffer,
			resources.paletteCapacityBytes, paletteBytes,
			"CPU Animation Palette Buffer");
		ensureBuffer(resources.morphWeightBuffer,
			resources.morphWeightCapacityBytes,
			std::max<VkDeviceSize>(morphWeightBytes, 16U),
			"CPU Animation Morph Weight Buffer");
		ensureBuffer(resources.graphicsBridgeBuffer,
			resources.graphicsBridgeCapacityBytes, bridgeBytes,
			"CPU Animation Graphics Bridge Buffer");
	} catch(const std::exception&) {
		RetireVulkanCpuAnimationFrameResources(resources);
		return false;
	}
	if(!WriteMappedBuffer(allocator, resources.paletteBuffer,
			palettes.data(), paletteBytes)
		|| (morphWeightBytes != 0U
			&& !WriteMappedBuffer(allocator,
				resources.morphWeightBuffer,
				morphWeights.data(), morphWeightBytes))) {
		RetireVulkanCpuAnimationFrameResources(resources);
		return false;
	}

	if(resources.paletteBindlessIndex == INVALID_BINDLESS_BUFFER_INDEX) {
		resources.paletteBindlessIndex = AllocateBufferIndex();
	}
	if(resources.morphWeightBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX) {
		resources.morphWeightBindlessIndex = AllocateBufferIndex();
	}
	for(VulkanGpuAnimationGraphicsRecord& bridge : bridgeRecords) {
		bridge.paletteMatricesBindlessIndex =
			resources.paletteBindlessIndex;
		bridge.morphOutputsBindlessIndex =
			resources.morphWeightBindlessIndex;
	}
	if(!WriteMappedBuffer(allocator, resources.graphicsBridgeBuffer,
			bridgeRecords.data(), bridgeBytes)) {
		RetireVulkanCpuAnimationFrameResources(resources);
		return false;
	}
	if(resources.graphicsBridgeBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX) {
		resources.graphicsBridgeBindlessIndex = AllocateBufferIndex();
	}
	// The publication rejected this path above if the current frame slot had
	// recorded or submitted commands. BeginFrame has already waited its fence,
	// so these three descriptor writes target a proven-idle set and must not turn
	// every CPU animator upload into a device-wide synchronization point.
	UpdateGlobalBufferForFrame(currentFrame,
		resources.paletteBindlessIndex, resources.paletteBuffer.buffer,
		0U, paletteBytes, true);
	UpdateGlobalBufferForFrame(currentFrame,
		resources.morphWeightBindlessIndex,
		resources.morphWeightBuffer.buffer, 0U,
		std::max<VkDeviceSize>(morphWeightBytes, 16U), true);
	UpdateGlobalBufferForFrame(currentFrame,
		resources.graphicsBridgeBindlessIndex,
		resources.graphicsBridgeBuffer.buffer, 0U, bridgeBytes, true);
	resources.frameId = plan.frameId;
	resources.rendererGeneration = plan.rendererGeneration;
	resources.live = true;
	activeCpuAnimationGraphicsBufferIndex =
		resources.graphicsBridgeBindlessIndex;
	return true;
}

bool VulkanContext::Impl::EnsureVulkanAnimationDescriptorSets()
{
	if(animationDescriptorSetLayout != VK_NULL_HANDLE
		&& animationDescriptorSets.size() == MAX_FRAMES_IN_FLIGHT) return true;
	if(device == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE) return false;

	std::array<VkDescriptorSetLayoutBinding, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount> bindings{};
	for(std::uint32_t binding = 0U; binding < bindings.size(); ++binding) {
		bindings[binding].binding = binding;
		bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[binding].descriptorCount = 1U;
		bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &animationDescriptorSetLayout) != VK_SUCCESS) {
		animationDescriptorSetLayout = VK_NULL_HANDLE;
		return false;
	}

	std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
	layouts.fill(animationDescriptorSetLayout);
	animationDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT, VK_NULL_HANDLE);
	VkDescriptorSetAllocateInfo allocateInfo{};
	allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocateInfo.descriptorPool = descriptorPool;
	allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
	allocateInfo.pSetLayouts = layouts.data();
	if(vkAllocateDescriptorSets(device, &allocateInfo, animationDescriptorSets.data()) != VK_SUCCESS) {
		vkDestroyDescriptorSetLayout(device, animationDescriptorSetLayout, nullptr);
		animationDescriptorSetLayout = VK_NULL_HANDLE;
		animationDescriptorSets.clear();
		return false;
	}
	return true;
}

bool VulkanContext::Impl::EnsureVulkanDeformationDescriptorSets()
{
	if(deformationDescriptorSetLayout != VK_NULL_HANDLE
		&& deformationDescriptorSets.size()
			== MAX_FRAMES_IN_FLIGHT)
	{
		return true;
	}
	if(device == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE)
	{
		return false;
	}
	std::array<
		VkDescriptorSetLayoutBinding,
		TerraEngine::Renderer::Vulkan::
			kVulkanDeformationDescriptorBindingCount> bindings{};
	for(std::uint32_t binding = 0u;
		binding < bindings.size();
		++binding)
	{
		bindings[binding].binding = binding;
		bindings[binding].descriptorType =
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[binding].descriptorCount = 1u;
		bindings[binding].stageFlags =
			VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType =
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount =
		static_cast<std::uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(
		device,
		&layoutInfo,
		nullptr,
		&deformationDescriptorSetLayout) != VK_SUCCESS)
	{
		deformationDescriptorSetLayout = VK_NULL_HANDLE;
		return false;
	}
	std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT>
		layouts{};
	layouts.fill(deformationDescriptorSetLayout);
	deformationDescriptorSets.assign(
		MAX_FRAMES_IN_FLIGHT,
		VK_NULL_HANDLE);
	VkDescriptorSetAllocateInfo allocateInfo{};
	allocateInfo.sType =
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocateInfo.descriptorPool = descriptorPool;
	allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
	allocateInfo.pSetLayouts = layouts.data();
	if(vkAllocateDescriptorSets(
		device,
		&allocateInfo,
		deformationDescriptorSets.data()) != VK_SUCCESS)
	{
		vkDestroyDescriptorSetLayout(
			device,
			deformationDescriptorSetLayout,
			nullptr);
		deformationDescriptorSetLayout = VK_NULL_HANDLE;
		deformationDescriptorSets.clear();
		return false;
	}
	return true;
}

void VulkanContext::Impl::RetireVulkanDeformationFrameResources(
	VulkanDeformationFrameResources& resources)
{
	for(VulkanBuffer& buffer : resources.buffers)
	{
		RetireBuffer(buffer);
	}
	RetireBuffer(resources.graphicsBridgeBuffer);
	const std::array<std::uint32_t, 4u> bindlessIndices{
		resources.currentVerticesBindlessIndex,
		resources.boundsBindlessIndex,
		resources.refinementBindlessIndex,
		resources.graphicsBridgeBindlessIndex };
	for(const std::uint32_t index : bindlessIndices)
	{
		if(index != INVALID_BINDLESS_BUFFER_INDEX)
		{
			freeBufferIndices.push_back(index);
		}
	}
	if(activeDeformationGraphicsBufferIndex
		== resources.graphicsBridgeBindlessIndex)
	{
		activeDeformationGraphicsBufferIndex =
			INVALID_BINDLESS_BUFFER_INDEX;
	}
	resources = {};
}

bool VulkanContext::Impl::PublishVulkanDeformationResources(
	const TerraEngine::Renderer::GpuDeformationPlan& plan)
{
	using DescriptorBinding =
		TerraEngine::Renderer::Vulkan::
			VulkanDeformationDescriptorBinding;
	if(plan.work.empty())
	{
		return plan.valid;
	}
	if(!plan.IsComplete()
		|| currentFrame >= MAX_FRAMES_IN_FLIGHT
		|| (currentFrame < pendingPrimaryCommandBuffers.size()
			&& !pendingPrimaryCommandBuffers[currentFrame].empty())
		|| (currentFrame
				< pendingPrimaryCommandBuffersSubmitted.size()
			&& pendingPrimaryCommandBuffersSubmitted[currentFrame]
				!= 0u))
	{
		return false;
	}
	const auto descriptorPlan =
		TerraEngine::Renderer::Vulkan::
			BuildVulkanDeformationDescriptorSetPlan(
				plan,
				maximumStorageBufferRange);
	if(!descriptorPlan.valid || !descriptorPlan.hasWork
		|| !EnsureVulkanDeformationDescriptorSets())
	{
		return false;
	}
	if(deformationFrameResources.size() < MAX_FRAMES_IN_FLIGHT)
	{
		deformationFrameResources.resize(MAX_FRAMES_IN_FLIGHT);
	}
	VulkanDeformationFrameResources& resources =
		deformationFrameResources[currentFrame];
	if(resources.live
		&& resources.rendererGeneration != plan.rendererGeneration)
	{
		RetireVulkanDeformationFrameResources(resources);
	}

	// Resource publication is serialized on the protected render thread. Retain
	// scratch capacity across frames so a stable crowd does not repeatedly allocate
	// and release the same page-key list before comparing the frame-slot cache.
	thread_local std::vector<TerraEngine::Renderer::StaticPageKey>
		requestedStaticPages;
	requestedStaticPages.clear();
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Deformation Resolve Static Page Cache",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		try
		{
			requestedStaticPages.reserve(plan.work.size());
			for(const TerraEngine::Renderer::DeformationWorkRecord& work : plan.work)
			{
				if(requestedStaticPages.empty()
					|| requestedStaticPages.back() != work.key.page)
				{
					requestedStaticPages.push_back(work.key.page);
				}
			}
		}
		catch(const std::bad_alloc&)
		{
			return false;
		}
	}
	const bool reuseStaticPageUpload =
		resources.staticPageUploadValid
		&& resources.staticPageByteCount != 0u
		&& resources.buffers[static_cast<std::size_t>(
			DescriptorBinding::StaticPageBytes)].buffer != VK_NULL_HANDLE
		&& resources.capacities[static_cast<std::size_t>(
			DescriptorBinding::StaticPageBytes)] >= resources.staticPageByteCount
		&& resources.staticPageUploads.size()
			== requestedStaticPages.size()
		&& std::equal(
			requestedStaticPages.begin(),
			requestedStaticPages.end(),
			resources.staticPageUploads.begin(),
			[](const TerraEngine::Renderer::StaticPageKey& key,
				const VulkanDeformationFrameResources::StaticPageUploadRecord& cached)
			{
				return key == cached.key;
			});

	enum class PaletteAuthority
	{
		Unknown,
		Gpu,
		Cpu
	};
	PaletteAuthority authority = PaletteAuthority::Unknown;
	VkBuffer paletteBuffer = VK_NULL_HANDLE;
	VkDeviceSize paletteBytes = 0u;
	VkBuffer morphBuffer = VK_NULL_HANDLE;
	VkDeviceSize morphBytes = 0u;
	struct RefinementRecord
	{
		std::uint32_t parentWorkIndex = 0u;
		std::uint32_t firstChildWorkIndex = 0u;
		std::uint32_t childCount = 0u;
		std::uint32_t selectedWorkIndex = 0u;
		float geometricError = 0.0f;
		float deformationError = 0.0f;
		float projectedScale = 1.0f;
		float thresholdPixels = 1.0f;
		std::uint32_t flags = 0x03u;
		std::uint32_t requestFeedbackIndex = 0u;
		std::array<std::uint32_t, 2u> reserved{};
	};
	static_assert(sizeof(RefinementRecord) == 48u);
	thread_local std::vector<TerraEngine::Renderer::
		GpuDeformationGenerationRecord> generations;
	thread_local std::vector<TerraEngine::Renderer::
		GpuDeformationShaderWorkRecord> shaderWork;
	thread_local std::vector<std::uint8_t> staticPageBytes;
	thread_local std::vector<TerraEngine::Renderer::DeformedPageKey> workKeys;
	thread_local std::vector<
		VulkanDeformationFrameResources::StaticPageUploadRecord>
		rebuiltStaticPages;
	thread_local std::vector<RefinementRecord> refinement;
	generations.clear();
	shaderWork.clear();
	staticPageBytes.clear();
	workKeys.clear();
	rebuiltStaticPages.clear();
	refinement.clear();
	try
	{
		generations.reserve(plan.work.size());
		shaderWork.reserve(plan.work.size());
		workKeys.reserve(plan.work.size());
		refinement.reserve(plan.totalClusterBoundsCount);
		if(!reuseStaticPageUpload)
		{
			rebuiltStaticPages.reserve(requestedStaticPages.size());
		}
	}
	catch(const std::bad_alloc&)
	{
		return false;
	}

	const auto FindSection = [](
		const std::vector<TemfDeformablePageSection>& sections,
		TemfDeformablePageSectionKind kind)
		-> const TemfDeformablePageSection*
	{
		const auto found = std::find_if(
			sections.begin(),
			sections.end(),
			[kind](
				const TemfDeformablePageSection& section)
			{
				return section.kind == kind;
			});
		return found == sections.end() ? nullptr : &*found;
	};
	struct CpuPaletteOffsets
	{
		std::uint64_t paletteBytes = 0u;
		std::uint64_t morphBytes = 0u;
	};
	thread_local std::unordered_map<std::uint64_t, CpuPaletteOffsets>
		cpuPaletteOffsets;
	cpuPaletteOffsets.clear();
	try
	{
		if(pendingCpuAnimationPlan != nullptr)
		{
			cpuPaletteOffsets.reserve(pendingCpuAnimationPlan->records.size());
			CpuPaletteOffsets next{};
			for(const auto& record : pendingCpuAnimationPlan->records)
			{
				cpuPaletteOffsets.try_emplace(record.stableInstanceId, next);
				next.paletteBytes += record.data->paletteMatrices.size()
					* sizeof(TerraEngine::Animation::GpuAnimationPaletteRecord);
				next.morphBytes += record.data->morphWeights.size()
					* sizeof(float);
			}
		}
	}
	catch(const std::bad_alloc&)
	{
		return false;
	}
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Deformation Build Dynamic Uploads",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		try
		{
			std::size_t staticPageIndex = 0u;
			for(std::uint32_t workIndex = 0u;
			workIndex < plan.work.size();
			++workIndex)
			{
			const TerraEngine::Renderer::DeformationWorkRecord& source =
				plan.work[workIndex];
			const bool beginsStaticPage = workIndex == 0u
				|| plan.work[workIndex - 1u].key.page != source.key.page;
			if(beginsStaticPage && workIndex != 0u)
			{
				++staticPageIndex;
			}
			if(staticPageIndex >= requestedStaticPages.size())
			{
				return false;
			}

			if(!reuseStaticPageUpload && beginsStaticPage)
			{
				const auto meshOwner = ResourceManager::Instance().AcquireMeshSnapshot(source.key.page.meshId);
				const Mesh* mesh = meshOwner.get(); // Reload cannot release this page's source bytes mid-copy.
				if(mesh == nullptr || !mesh->hasDeformableDemandedPages)
				{
					return false;
				}
				const auto entryIt = std::find_if(
					mesh->deformableDemandedPageEntries.begin(),
					mesh->deformableDemandedPageEntries.end(),
					[&source](const TemfDeformableDemandedPageEntry& entry)
					{
						return entry.pageId == source.key.page.pageId
							&& entry.contentGeneration
								== source.key.page.pageContentGeneration;
					});
				if(entryIt == mesh->deformableDemandedPageEntries.end())
				{
					return false;
				}
				const std::size_t pageIndex = static_cast<std::size_t>(
					std::distance(mesh->deformableDemandedPageEntries.begin(), entryIt));
				if(pageIndex >= mesh->deformableDemandedPagePayloads.size()
					|| pageIndex >= mesh->deformableDemandedPageSections.size())
				{
					return false;
				}
				const std::span<const std::uint8_t> blobBytes =
					mesh->deformableDemandedPagePayloads[pageIndex];
				const auto& sections = mesh->deformableDemandedPageSections[pageIndex];
				const TemfDeformablePageSection* vertices = FindSection(
					sections, TemfDeformablePageSectionKind::PackedVertices);
				const TemfDeformablePageSection* skin = FindSection(
					sections, TemfDeformablePageSectionKind::SkinRecords);
				const TemfDeformablePageSection* joints = FindSection(
					sections, TemfDeformablePageSectionKind::JointPalette);
				const TemfDeformablePageSection* morphTargets = FindSection(
					sections, TemfDeformablePageSectionKind::MorphTargets);
				const TemfDeformablePageSection* morphDeltas = FindSection(
					sections, TemfDeformablePageSectionKind::MorphDeltas);
				const TemfDeformablePageSection* decode = FindSection(
					sections, TemfDeformablePageSectionKind::DecodeConstants);
				if(vertices == nullptr || skin == nullptr || joints == nullptr
					|| decode == nullptr || decode->byteSize != sizeof(QuantizationInfo)
					|| vertices->elementCount != source.vertexCount
					|| skin->elementCount != source.vertexCount
					|| decode->relativeByteOffset > blobBytes.size()
					|| sizeof(QuantizationInfo)
						> blobBytes.size() - decode->relativeByteOffset)
				{
					return false;
				}
				const std::size_t alignedBase =
					(staticPageBytes.size() + 15u) & ~std::size_t{15u};
				if(alignedBase > std::numeric_limits<std::uint32_t>::max()
					|| blobBytes.size()
						> std::numeric_limits<std::uint32_t>::max() - alignedBase)
				{
					return false;
				}
				staticPageBytes.resize(alignedBase, 0u);
				VulkanDeformationFrameResources::StaticPageUploadRecord cached{};
				cached.key = source.key.page;
				cached.blobBase = static_cast<std::uint32_t>(alignedBase);
				cached.packedVertexByteOffset = cached.blobBase
					+ vertices->relativeByteOffset;
				cached.skinRecordByteOffset = cached.blobBase
					+ skin->relativeByteOffset;
				cached.morphDeltaByteOffset = morphDeltas == nullptr ? 0u
					: cached.blobBase + morphDeltas->relativeByteOffset;
				cached.jointPaletteByteOffset = cached.blobBase
					+ joints->relativeByteOffset;
				cached.vertexCount = vertices->elementCount;
				cached.jointCount = joints->elementCount;
				cached.morphTargetCount = morphTargets == nullptr ? 0u
					: morphTargets->elementCount;
				cached.geometricError = entryIt->geometricError;
				cached.deformationError = entryIt->deformationError;
				QuantizationInfo quantization{};
				std::memcpy(&quantization,
					blobBytes.data() + decode->relativeByteOffset,
					sizeof(quantization));
				std::copy_n(quantization.positionOffset, 3u,
					cached.positionOffset.begin());
				std::copy_n(quantization.positionScale, 3u,
					cached.positionScale.begin());
				staticPageBytes.insert(staticPageBytes.end(),
					blobBytes.begin(), blobBytes.end());
				rebuiltStaticPages.push_back(std::move(cached));
			}
			const auto& staticPage = reuseStaticPageUpload
				? resources.staticPageUploads[staticPageIndex]
				: rebuiltStaticPages[staticPageIndex];
			if(staticPage.key != source.key.page
				|| staticPage.vertexCount != source.vertexCount)
			{
				return false;
			}

			std::uint64_t paletteOffsetBytes = 0u;
			std::uint64_t morphOffsetBytes = 0u;
			if(pendingAnimationImmutablePlan != nullptr
			&& source.sourceRecordIndex
				< pendingAnimationImmutablePlan->WorkPackets().size()
			&& currentFrame < animationFrameResources.size())
			{
			if(authority == PaletteAuthority::Cpu)
			{
				return false;
			}
			authority = PaletteAuthority::Gpu;
			const auto& packet =
				pendingAnimationImmutablePlan->WorkPackets()[
					source.sourceRecordIndex];
			paletteOffsetBytes = packet.paletteOffsetBytes;
			morphOffsetBytes = packet.morphOutputOffsetBytes;
			const auto& animationResources =
				animationFrameResources[currentFrame];
			const std::size_t paletteBinding =
				static_cast<std::size_t>(
					TerraEngine::Animation::
						GpuAnimationDescriptorBinding::
							PaletteMatrices);
			const std::size_t morphBinding =
				static_cast<std::size_t>(
					TerraEngine::Animation::
						GpuAnimationDescriptorBinding::
							MorphOutputs);
			paletteBuffer =
				animationResources.buffers[paletteBinding].buffer;
			paletteBytes =
				animationResources.buffers[paletteBinding].size;
			morphBuffer =
				animationResources.buffers[morphBinding].buffer;
			morphBytes =
				animationResources.buffers[morphBinding].size;
			}
			else if(pendingCpuAnimationPlan != nullptr
			&& currentFrame < cpuAnimationFrameResources.size())
			{
			if(authority == PaletteAuthority::Gpu)
			{
				return false;
			}
			authority = PaletteAuthority::Cpu;
			const auto offsets = cpuPaletteOffsets.find(
				source.sourceStableInstanceId);
			if(offsets == cpuPaletteOffsets.end())
			{
				return false;
			}
			paletteOffsetBytes = offsets->second.paletteBytes;
			morphOffsetBytes = offsets->second.morphBytes;
			const auto& cpuResources =
				cpuAnimationFrameResources[currentFrame];
			paletteBuffer = cpuResources.paletteBuffer.buffer;
			paletteBytes = cpuResources.paletteBuffer.size;
			morphBuffer = cpuResources.morphWeightBuffer.buffer;
			morphBytes = cpuResources.morphWeightBuffer.size;
			}
			else
			{
				return false;
			}
			if(paletteBuffer == VK_NULL_HANDLE
			|| paletteOffsetBytes % 16u != 0u
			|| morphOffsetBytes % sizeof(float) != 0u)
			{
				return false;
			}

			TerraEngine::Renderer::GpuDeformationGenerationRecord
			generation{};
		generation.stableInstanceId =
			source.key.deformation.stableInstanceId;
		generation.animationPublicationGeneration =
			source.key.deformation.animationPublicationGeneration;
		generation.pageContentGeneration =
			source.key.page.pageContentGeneration;
		generation.residencyGeneration =
			source.key.page.residencyGeneration;
		generation.rendererGeneration =
			source.key.deformation.rendererGeneration;
		generation.sceneGeneration =
			source.key.deformation.sceneGeneration;
		generation.meshPayloadGeneration =
			source.key.deformation.meshPayloadGeneration;
			generations.push_back(generation);

			TerraEngine::Renderer::GpuDeformationShaderWorkRecord
			work{};
		work.generationRecordIndex = workIndex;
		work.inputVertexByteOffset = staticPage.packedVertexByteOffset;
		work.skinRecordByteOffset = staticPage.skinRecordByteOffset;
		work.morphDeltaByteOffset = staticPage.morphDeltaByteOffset;
		work.vertexCount = source.vertexCount;
		work.jointCount = staticPage.jointCount;
		work.morphTargetCount = staticPage.morphTargetCount;
		work.paletteRowOffset =
			static_cast<std::uint32_t>(
				paletteOffsetBytes / 16u);
		work.morphWeightOffset =
			static_cast<std::uint32_t>(
				morphOffsetBytes / sizeof(float));
		work.currentOutputOffset = source.vertexOffset;
		work.previousOutputOffset = source.vertexOffset;
		work.jointPaletteByteOffset = staticPage.jointPaletteByteOffset;
		// Bounds validation needs the exact page identity, but batching removes the
		// former per-page push constant. Reuse the reserved ABI lane so each workgroup
		// reads its own identity without changing the frozen record layout.
		work.reserved[0] = source.key.page.pageId;
		work.positionOffset = staticPage.positionOffset;
		work.positionScale = staticPage.positionScale;
			shaderWork.push_back(work);
			workKeys.push_back(source.key);
			for(std::uint32_t cluster = 0u;
			cluster < source.clusterCount;
			++cluster)
			{
			RefinementRecord record{};
			record.parentWorkIndex = workIndex;
			record.selectedWorkIndex = workIndex;
			record.requestFeedbackIndex =
				source.clusterBoundsOffset + cluster;
			record.geometricError = staticPage.geometricError;
			record.deformationError = staticPage.deformationError;
				refinement.push_back(record);
			}
			}
		}
		catch(const std::bad_alloc&)
		{
			return false;
		}
	}
	const VkDeviceSize staticPageByteCount = reuseStaticPageUpload
		? resources.staticPageByteCount
		: static_cast<VkDeviceSize>(staticPageBytes.size());
	if(staticPageByteCount == 0u
		|| generations.size() != plan.work.size()
		|| shaderWork.size() != plan.work.size()
		|| refinement.size()
			!= plan.totalClusterBoundsCount
		|| paletteBytes == 0u)
	{
		return false;
	}

	const std::uint32_t previousFrame =
		(currentFrame + MAX_FRAMES_IN_FLIGHT - 1u)
		% MAX_FRAMES_IN_FLIGHT;
	const VulkanDeformationFrameResources* previousResources =
		previousFrame < deformationFrameResources.size()
			? &deformationFrameResources[previousFrame]
			: nullptr;
	const bool historyCompatible =
		previousResources != nullptr
		&& previousResources->live
		&& previousResources->rendererGeneration
			== plan.rendererGeneration
		&& previousResources->workKeys == workKeys;
	for(std::size_t index = 0u;
		index < shaderWork.size();
		++index)
	{
		if(historyCompatible && plan.work[index].historyValid)
		{
			shaderWork[index].flags |= 0x01u;
		}
	}
	const auto EnsureBuffer = [this, &resources](
		std::size_t binding,
		VkDeviceSize requiredBytes,
		const char* name)
	{
		const VkDeviceSize safeBytes =
			std::max<VkDeviceSize>(requiredBytes, 16u);
		VulkanBuffer& buffer = resources.buffers[binding];
		if(buffer.buffer == VK_NULL_HANDLE
			|| resources.capacities[binding] < safeBytes)
		{
			RetireBuffer(buffer);
			buffer = CreateBuffer(
				safeBytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_CPU_TO_GPU,
				name);
			resources.capacities[binding] = safeBytes;
		}
		return buffer.buffer != VK_NULL_HANDLE;
	};
	const auto BindingIndex = [](DescriptorBinding binding)
	{
		return static_cast<std::size_t>(binding);
	};
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Deformation Ensure Buffers",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		for(std::size_t binding = 0u;
			binding < TerraEngine::Renderer::Vulkan::
				kVulkanDeformationDescriptorBindingCount;
			++binding)
		{
			if(binding == BindingIndex(DescriptorBinding::Palettes)
				|| binding == BindingIndex(DescriptorBinding::MorphWeights)
				|| binding == BindingIndex(DescriptorBinding::PreviousVertices))
			{
				continue;
			}
			VkDeviceSize requiredBytes =
				descriptorPlan.bindings[binding].minimumByteCount;
			if(binding == BindingIndex(DescriptorBinding::StaticPageBytes))
			{
				requiredBytes = staticPageByteCount;
			}
			else if(binding == BindingIndex(DescriptorBinding::Refinement))
			{
				requiredBytes = refinement.size() * sizeof(RefinementRecord);
			}
			if(!EnsureBuffer(binding, requiredBytes,
				"Animated Virtual Geometry Frame Buffer"))
			{
				RetireVulkanDeformationFrameResources(resources);
				return false;
			}
		}
	}
	// Every active input/refinement element is overwritten below, and the
	// deformation dispatch writes every active vertex. Only diagnostic and
	// forward-compatible bounds tails need a deterministic sentinel. Bounding
	// these clears avoids touching retained capacity that this frame cannot read.
	const VkDeviceSize clusterClearBytes = std::max<VkDeviceSize>(16u,
		static_cast<VkDeviceSize>(plan.totalClusterBoundsCount)
			* sizeof(TerraEngine::Renderer::GpuDynamicBoundsRecord));
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Deformation Clear Safety Tails",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(!ClearMappedBufferRange(
			allocator,
			resources.buffers[BindingIndex(DescriptorBinding::ClusterBounds)],
			clusterClearBytes)
		|| !ClearMappedBufferRange(
			allocator,
			resources.buffers[BindingIndex(DescriptorBinding::Diagnostics)],
			16u))
		{
			RetireVulkanDeformationFrameResources(resources);
			return false;
		}
	}
	const auto Upload = [this, &resources](
		DescriptorBinding binding,
		const void* bytes,
		VkDeviceSize byteCount)
	{
		return byteCount == 0u
			|| WriteMappedBuffer(
				allocator,
				resources.buffers[
					static_cast<std::size_t>(binding)],
				bytes,
				byteCount);
	};
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Deformation Upload Active Ranges",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(!Upload(
			DescriptorBinding::Generations,
			generations.data(),
			generations.size() * sizeof(generations.front()))
		|| !Upload(
			DescriptorBinding::Work,
			shaderWork.data(),
			shaderWork.size() * sizeof(shaderWork.front()))
		|| (!reuseStaticPageUpload
			&& !Upload(
				DescriptorBinding::StaticPageBytes,
				staticPageBytes.data(),
				staticPageByteCount))
		|| !Upload(
			DescriptorBinding::Refinement,
			refinement.data(),
			refinement.size() * sizeof(refinement.front())))
		{
			RetireVulkanDeformationFrameResources(resources);
			return false;
		}
	}
	if(!reuseStaticPageUpload)
	{
		// Commit cache identity only after the upload succeeds. A failed upload
		// therefore cannot make a later frame trust partial mapped-buffer contents.
		resources.staticPageUploads = std::move(rebuiltStaticPages);
		resources.staticPageByteCount = staticPageByteCount;
		resources.staticPageUploadValid = true;
	}

	std::array<
		VkDescriptorBufferInfo,
		TerraEngine::Renderer::Vulkan::
			kVulkanDeformationDescriptorBindingCount> infos{};
	std::array<
		VkWriteDescriptorSet,
		TerraEngine::Renderer::Vulkan::
			kVulkanDeformationDescriptorBindingCount> writes{};
	for(std::uint32_t binding = 0u;
		binding < infos.size();
		++binding)
	{
		infos[binding].buffer =
			resources.buffers[binding].buffer;
		infos[binding].offset = 0u;
		infos[binding].range =
			std::max<VkDeviceSize>(
				16u,
				resources.buffers[binding].size);
		if(binding == BindingIndex(DescriptorBinding::Palettes))
		{
			infos[binding].buffer = paletteBuffer;
			infos[binding].range = paletteBytes;
		}
		else if(binding
			== BindingIndex(DescriptorBinding::MorphWeights))
		{
			infos[binding].buffer =
				morphBuffer != VK_NULL_HANDLE
					? morphBuffer
					: paletteBuffer;
			infos[binding].range =
				morphBuffer != VK_NULL_HANDLE
					? std::max<VkDeviceSize>(16u, morphBytes)
					: 16u;
		}
		else if(binding
			== BindingIndex(
				DescriptorBinding::PreviousVertices))
		{
			const VulkanBuffer& previousBuffer =
				historyCompatible
					? previousResources->buffers[
						BindingIndex(
							DescriptorBinding::
								CurrentVertices)]
					: resources.buffers[
						BindingIndex(
							DescriptorBinding::
								CurrentVertices)];
			infos[binding].buffer = previousBuffer.buffer;
			infos[binding].range = previousBuffer.size;
		}
		if(infos[binding].buffer == VK_NULL_HANDLE
			|| infos[binding].range == 0u
			|| infos[binding].range > maximumStorageBufferRange)
		{
			RetireVulkanDeformationFrameResources(resources);
			return false;
		}
		writes[binding].sType =
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstSet =
			deformationDescriptorSets[currentFrame];
		writes[binding].dstBinding = binding;
		writes[binding].descriptorType =
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[binding].descriptorCount = 1u;
		writes[binding].pBufferInfo = &infos[binding];
	}
	vkUpdateDescriptorSets(
		device,
		static_cast<std::uint32_t>(writes.size()),
		writes.data(),
		0u,
		nullptr);

	// Publish only the canonical current/history/bounds/refinement products to
	// graphics. The bridge deliberately omits palettes, skin records, and morph
	// weights, keeping custom surface stages animation-agnostic.
	const std::size_t currentBinding =
		BindingIndex(DescriptorBinding::CurrentVertices);
	const std::size_t boundsBinding =
		BindingIndex(DescriptorBinding::ClusterBounds);
	const std::size_t refinementBinding =
		BindingIndex(DescriptorBinding::Refinement);
	if(resources.currentVerticesBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		resources.currentVerticesBindlessIndex =
			AllocateBufferIndex();
	}
	if(resources.boundsBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		resources.boundsBindlessIndex = AllocateBufferIndex();
	}
	if(resources.refinementBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		resources.refinementBindlessIndex =
			AllocateBufferIndex();
	}
	// This function rejects recorded or submitted current-slot command buffers
	// before touching any resource. The frame fence is therefore the complete
	// safety proof for these bindless publications; another global idle wait
	// would serialize deformation against unrelated frames in flight.
	UpdateGlobalBufferForFrame(
		currentFrame,
		resources.currentVerticesBindlessIndex,
		resources.buffers[currentBinding].buffer,
		0u,
		resources.buffers[currentBinding].size,
		true);
	UpdateGlobalBufferForFrame(
		currentFrame,
		resources.boundsBindlessIndex,
		resources.buffers[boundsBinding].buffer,
		0u,
		resources.buffers[boundsBinding].size,
		true);
	UpdateGlobalBufferForFrame(
		currentFrame,
		resources.refinementBindlessIndex,
		resources.buffers[refinementBinding].buffer,
		0u,
		resources.buffers[refinementBinding].size,
		true);
	const std::uint32_t previousVerticesBindlessIndex =
		historyCompatible
			? previousResources->currentVerticesBindlessIndex
			: resources.currentVerticesBindlessIndex;
	if(previousVerticesBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		RetireVulkanDeformationFrameResources(resources);
		return false;
	}

	const std::vector<TerraEngine::Renderer::DeformationConsumerWorkRecord>&
		orderedWork = plan.orderedConsumers;
	if(orderedWork.empty())
	{
		return false;
	}
	auto& groups = resources.graphicsGroupBuildScratch;
	groups.clear();
	groups.reserve(orderedWork.size());
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Deformation Build Graphics Groups",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		for(std::size_t orderedIndex = 0u;
			orderedIndex < orderedWork.size();
			++orderedIndex)
		{
			const TerraEngine::Renderer::DeformationConsumerWorkRecord& member =
				orderedWork[orderedIndex];
			const auto& work = plan.work[member.workIndex];
			if(groups.empty()
				|| groups.back().stableInstanceId != member.stableInstanceId
				|| groups.back().meshId != work.key.page.meshId)
			{
				groups.push_back({
					member.stableInstanceId,
					work.key.page.meshId,
					orderedIndex,
					0u });
			}
			++groups.back().count;
		}
	}
	constexpr std::uint32_t kBridgeHeaderWords = 16u;
	constexpr std::uint32_t kBridgeRecordWords = 4u;
	constexpr std::uint32_t kBridgeMarker = 0x4445464du;
	auto& bridgeWords = resources.graphicsBridgeScratch;
	bridgeWords.clear();
	bridgeWords.reserve(
		groups.size() * kBridgeHeaderWords
			+ orderedWork.size() * kBridgeRecordWords);
	auto& publishedGroups = resources.graphicsGroupsScratch;
	publishedGroups.clear();
	publishedGroups.reserve(groups.size());
	for(std::uint32_t groupIndex = 0u;
		groupIndex < groups.size();
		++groupIndex)
	{
		const VulkanDeformationFrameResources::GraphicsGroupBuildRange& group =
			groups[groupIndex];
		const std::size_t headerOffset =
			bridgeWords.size();
		if(headerOffset
				> std::numeric_limits<std::uint32_t>::max()
					- kBridgeHeaderWords
			|| group.count
				> (std::numeric_limits<std::uint32_t>::max()
					- (headerOffset + kBridgeHeaderWords))
					/ kBridgeRecordWords)
		{
			RetireVulkanDeformationFrameResources(resources);
			return false;
		}
		const std::uint32_t recordWordOffset =
			static_cast<std::uint32_t>(
				headerOffset + kBridgeHeaderWords);
		bridgeWords.resize(
			headerOffset + kBridgeHeaderWords
				+ group.count * kBridgeRecordWords,
			0u);
		const auto& firstWork =
			plan.work[orderedWork[group.begin].workIndex];
		bridgeWords[headerOffset + 0u] =
			TerraEngine::Renderer::kAnimatedGeometryAbiVersion;
		bridgeWords[headerOffset + 1u] =
			static_cast<std::uint32_t>(
				plan.rendererGeneration);
		bridgeWords[headerOffset + 2u] = recordWordOffset;
		bridgeWords[headerOffset + 3u] =
			static_cast<std::uint32_t>(group.count);
		bridgeWords[headerOffset + 4u] =
			resources.currentVerticesBindlessIndex;
		bridgeWords[headerOffset + 5u] =
			previousVerticesBindlessIndex;
		bridgeWords[headerOffset + 6u] =
			resources.boundsBindlessIndex;
		bridgeWords[headerOffset + 7u] =
			resources.refinementBindlessIndex;
		bridgeWords[headerOffset + 8u] =
			static_cast<std::uint32_t>(
				group.stableInstanceId);
		bridgeWords[headerOffset + 9u] =
			static_cast<std::uint32_t>(
				group.stableInstanceId >> 32u);
		bridgeWords[headerOffset + 10u] =
			static_cast<std::uint32_t>(
				firstWork.key.page.meshPayloadGeneration);
		bridgeWords[headerOffset + 11u] =
			static_cast<std::uint32_t>(
				firstWork.key.page.meshPayloadGeneration
					>> 32u);
		bridgeWords[headerOffset + 12u] = 0x05u
			| (historyCompatible ? 0x02u : 0u);
		bridgeWords[headerOffset + 13u] =
			plan.totalVertexCount;
		bridgeWords[headerOffset + 14u] = groupIndex;
		bridgeWords[headerOffset + 15u] = kBridgeMarker;
		publishedGroups.push_back({
			group.stableInstanceId,
			group.meshId,
			static_cast<std::uint32_t>(headerOffset) });
		for(std::size_t localRecord = 0u;
			localRecord < group.count;
			++localRecord)
		{
			const std::uint32_t workIndex =
				orderedWork[group.begin + localRecord].workIndex;
			const auto& work = plan.work[workIndex];
			const std::size_t outputOffset =
				static_cast<std::size_t>(recordWordOffset)
					+ localRecord * kBridgeRecordWords;
			bridgeWords[outputOffset + 0u] =
				work.key.page.pageId;
			bridgeWords[outputOffset + 1u] =
				work.vertexOffset;
			bridgeWords[outputOffset + 2u] =
				workIndex;
			bridgeWords[outputOffset + 3u] = 0x05u
				| (shaderWork[workIndex].flags & 0x01u
					? 0x02u
					: 0u);
		}
	}
	if(publishedGroups != resources.graphicsGroups)
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Deformation Publish Graphics Group Index",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		resources.graphicsGroups.swap(publishedGroups);
		resources.graphicsGroupOffsets.clear();
		resources.graphicsGroupOffsets.reserve(resources.graphicsGroups.size());
		std::uint32_t largestDenseSlot = 0u;
		bool denseSlotsRepresentable = !resources.graphicsGroups.empty();
		const std::uint64_t denseSlotLimit =
			static_cast<std::uint64_t>(resources.graphicsGroups.size()) * 2u + 1024u;
		for(const VulkanDeformationFrameResources::GraphicsGroup& group :
			resources.graphicsGroups)
		{
			const std::uint64_t slot = group.stableInstanceId >> 32u;
			if(slot > denseSlotLimit
				|| slot > std::numeric_limits<std::uint32_t>::max())
			{
				denseSlotsRepresentable = false;
				break;
			}
			largestDenseSlot = std::max(largestDenseSlot,
				static_cast<std::uint32_t>(slot));
		}
		resources.graphicsGroupSlotHints.assign(
			denseSlotsRepresentable
				? static_cast<std::size_t>(largestDenseSlot) + 1u
				: 0u,
			{});
		std::uint64_t groupSignature = 1469598103934665603ULL;
		for(const VulkanDeformationFrameResources::GraphicsGroup& group :
			resources.graphicsGroups)
		{
			resources.graphicsGroupOffsets.emplace(
				VulkanDeformationFrameResources::GraphicsGroupKey{
					group.stableInstanceId, group.meshId},
				group.headerWordOffset);
			const std::uint64_t slot = group.stableInstanceId >> 32u;
			if(slot < resources.graphicsGroupSlotHints.size())
			{
				auto& hint = resources.graphicsGroupSlotHints[
					static_cast<std::size_t>(slot)];
				if(hint.stableInstanceId == 0u)
				{
					hint.stableInstanceId = group.stableInstanceId;
					hint.meshId = group.meshId;
					hint.headerWordOffset = group.headerWordOffset;
				}
				else if(hint.stableInstanceId != group.stableInstanceId
					|| hint.meshId != group.meshId)
				{
					hint.ambiguous = true;
				}
			}
			const auto combineGroupLane = [&groupSignature](std::uint64_t value)
			{
				groupSignature ^= value + 0x9e3779b97f4a7c15ULL
					+ (groupSignature << 6U) + (groupSignature >> 2U);
			};
			combineGroupLane(group.stableInstanceId);
			combineGroupLane(static_cast<std::uint64_t>(
				std::hash<TerraEngine::UUID>{}(group.meshId)));
			combineGroupLane(group.headerWordOffset);
		}
		resources.graphicsGroupSignature = groupSignature;
	}
	const VkDeviceSize bridgeBytes =
		bridgeWords.size() * sizeof(std::uint32_t);
	if(resources.graphicsBridgeBuffer.buffer == VK_NULL_HANDLE
		|| resources.graphicsBridgeCapacityBytes < bridgeBytes)
	{
		RetireBuffer(resources.graphicsBridgeBuffer);
		resources.graphicsBridgeBuffer = CreateBuffer(
			bridgeBytes,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU,
			"Animated Virtual Geometry Graphics Bridge");
		resources.graphicsBridgeCapacityBytes = bridgeBytes;
	}
	if(resources.graphicsBridgeBuffer.buffer == VK_NULL_HANDLE
		|| !WriteMappedBuffer(
			allocator,
			resources.graphicsBridgeBuffer,
			bridgeWords.data(),
			bridgeBytes))
	{
		RetireVulkanDeformationFrameResources(resources);
		return false;
	}
	if(resources.graphicsBridgeBindlessIndex
		== INVALID_BINDLESS_BUFFER_INDEX)
	{
		resources.graphicsBridgeBindlessIndex =
			AllocateBufferIndex();
	}
	UpdateGlobalBufferForFrame(
		currentFrame,
		resources.graphicsBridgeBindlessIndex,
		resources.graphicsBridgeBuffer.buffer,
		0u,
		bridgeBytes,
		true);
	resources.descriptorPlan = descriptorPlan;
	resources.workKeys = std::move(workKeys);
	resources.frameId = plan.frameId;
	resources.rendererGeneration = plan.rendererGeneration;
	resources.live = true;
	activeDeformationGraphicsBufferIndex =
		resources.graphicsBridgeBindlessIndex;
	return true;
}

bool VulkanContext::Impl::PublishVulkanAnimationResources(
	const TerraEngine::Animation::GpuAnimationPlan& plan,
	std::uint32_t rendererGeneration)
{
	TERRAENGINE_PROFILE_SCOPE("Vulkan: Publish Animation Resources",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	activeAnimationGraphicsBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	if(plan.DispatchCommands().empty()) {
		// The dormant/static invariant is strict: zero animation work creates no
		// buffers, descriptor writes, uploads, barriers, or queue submissions.
		return true;
	}

	if(rendererGeneration == 0U || currentFrame >= MAX_FRAMES_IN_FLIGHT || plan.WorkPackets().empty()) return false;
	const bool currentFrameHasRecordedCommands = currentFrame < pendingPrimaryCommandBuffers.size()
		&& !pendingPrimaryCommandBuffers[currentFrame].empty();
	const bool currentFrameWasSubmitted = currentFrame < pendingPrimaryCommandBuffersSubmitted.size()
		&& pendingPrimaryCommandBuffersSubmitted[currentFrame] != 0U;
	if(currentFrameHasRecordedCommands || currentFrameWasSubmitted) {
		// Bindless set-0 descriptors are mutable host state. Refuse a late
		// animation publication instead of stalling or rewriting a descriptor a
		// submitted Scene/Game command buffer may still reference.
		return false;
	}

	TerraEngine::Renderer::Vulkan::VulkanAnimationDescriptorSetPlan descriptorPlan;
	const auto& payloads = plan.ResidentPayloads();
	bool requestsHoldLast = false;
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Validate Animation Resource Plan",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		descriptorPlan = TerraEngine::Renderer::Vulkan::
			BuildVulkanAnimationDescriptorSetPlan(plan);
		if(!descriptorPlan.valid || !descriptorPlan.hasWork) return false;
		// Validate every copied index before creating or updating any Vulkan object.
		// The renderer treats this immutable plan as one transaction: malformed
		// producer bytes must not expose a partially updated descriptor set.
		if(plan.generation == 0U || plan.GenerationRecordCount() == 0U
			|| !payloads.HasRequiredSampleAndHierarchyInputs()) return false;
		for(const TerraEngine::Animation::GpuAnimationWorkPacket& packet : plan.WorkPackets()) {
			if(packet.generationRecordIndex >= plan.GenerationRecordCount()
				|| packet.clipResourceIndex == TerraEngine::Animation::kGpuAnimationInvalidResourceIndex
				|| packet.programResourceIndex == TerraEngine::Animation::kGpuAnimationInvalidResourceIndex
				|| packet.skeletonResourceIndex == TerraEngine::Animation::kGpuAnimationInvalidResourceIndex
				|| packet.stageMask == 0U
				|| (packet.stageMask & ~TerraEngine::Animation::kGpuAnimationAllStageMask) != 0U
				|| (packet.maskResourceIndex != TerraEngine::Animation::kGpuAnimationInvalidResourceIndex
					&& payloads.maskTables.empty())
				|| (packet.morphCount != 0U && (packet.morphResourceIndex
					== TerraEngine::Animation::kGpuAnimationInvalidResourceIndex || payloads.morphInputs.empty()))) {
				return false;
			}
			const TerraEngine::Animation::GpuAnimationGenerationRecord& generation =
				plan.GenerationRecord(packet.generationRecordIndex);
			const std::uint64_t stableInstanceId =
				(static_cast<std::uint64_t>(generation.instanceSlot) << 32U)
				| static_cast<std::uint64_t>(generation.instanceGeneration);
			if(!TerraEngine::Animation::IsGpuAnimationGenerationRecordValid(generation)
				|| generation.rendererGeneration != rendererGeneration
				|| packet.stableInstanceId != stableInstanceId) {
				// Never relabel an immutable packet from an older renderer session with
				// the current recovery generation in the graphics bridge.
				return false;
			}
			// Fold this dynamic fallback fact into the mandatory packet validation.
			// Frames without hold-last work can then skip building a crowd-sized pose
			// history identity vector that no shader will read.
			requestsHoldLast = requestsHoldLast
				|| TerraEngine::Animation::HasGpuAnimationFlag(
					packet.flags,
					TerraEngine::Animation::GpuAnimationWorkFlag::UseHoldLastFallback);
		}
		for(const TerraEngine::Animation::GpuAnimationDispatchRecord& dispatch : plan.DispatchRecords()) {
			if(dispatch.workPacketIndex >= plan.WorkPackets().size()
				|| dispatch.generationRecordIndex >= plan.GenerationRecordCount()
				|| !TerraEngine::Animation::IsGpuAnimationStageValueValid(dispatch.stage)
				|| dispatch.elementCount == 0U) return false;
			const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
				plan.WorkPackets()[dispatch.workPacketIndex];
			if(dispatch.stableInstanceId != packet.stableInstanceId
				|| dispatch.generationRecordIndex != packet.generationRecordIndex) return false;
		}
		for(std::size_t commandIndex = 0U;
			commandIndex < plan.DispatchCommands().size(); ++commandIndex) {
			const TerraEngine::Animation::GpuAnimationDispatchCommand command =
				plan.ResolvedDispatchCommand(commandIndex);
			const std::size_t first = command.pushConstants.firstDispatchRecord;
			const std::size_t count = command.pushConstants.dispatchRecordCount;
			if(command.pushConstants.planGeneration != plan.generation
				|| !TerraEngine::Animation::IsGpuAnimationStageValueValid(command.pushConstants.stage)
				|| count == 0U
				|| count > TerraEngine::Animation::kGpuAnimationMaximumDispatchRecordsPerCommand
				|| first > plan.DispatchRecords().size()
				|| count > plan.DispatchRecords().size() - first
				|| command.groupCountX == 0U || command.groupCountY == 0U || command.groupCountZ == 0U) {
				return false;
			}
		}
	}
	if(maximumStorageBufferRange == 0U
		|| plan.WorkPackets().size() > std::numeric_limits<VkDeviceSize>::max()
			/ sizeof(VulkanGpuAnimationGraphicsRecord)) return false;
	const VkDeviceSize bridgeBytes = static_cast<VkDeviceSize>(
		plan.WorkPackets().size() * sizeof(VulkanGpuAnimationGraphicsRecord));
	if(bridgeBytes == 0U || bridgeBytes > maximumStorageBufferRange) return false;
	if(animationFrameResources.size() < MAX_FRAMES_IN_FLIGHT) {
		animationFrameResources.resize(MAX_FRAMES_IN_FLIGHT);
	}
	if(!EnsureVulkanAnimationDescriptorSets()) return false;
	VulkanAnimationFrameResources& resources = animationFrameResources[currentFrame];
	if(resources.live && resources.rendererGeneration != rendererGeneration) {
		// A completed frame slot may reuse and overwrite its buffers for a newer
		// plan generation. Only renderer-session replacement invalidates the native
		// allocations and bindless identities themselves.
		RetireVulkanAnimationFrameResources(resources);
	}
	std::vector<VulkanAnimationPoseHistoryIdentity>& currentPoseHistoryIdentities =
		resources.poseHistoryScratch;
	currentPoseHistoryIdentities.clear();
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Build Animation Pose History",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		try {
			if(requestsHoldLast) {
				currentPoseHistoryIdentities.reserve(plan.WorkPackets().size());
				for(const TerraEngine::Animation::GpuAnimationWorkPacket& packet : plan.WorkPackets()) {
					const TerraEngine::Animation::GpuAnimationGenerationRecord& generation =
						plan.GenerationRecord(packet.generationRecordIndex);
					currentPoseHistoryIdentities.push_back({
						packet.stableInstanceId,
						packet.localPoseOffsetBytes,
						packet.jointCount,
						generation.skeletonGeneration,
						generation.programGeneration});
				}
			}
		} catch(const std::bad_alloc&) {
			return false;
		}
	}
	bool holdLastHistoryCompatible = requestsHoldLast && resources.live
		&& resources.poseHistoryIdentities == currentPoseHistoryIdentities;

	bool descriptorBindingsChanged = false;
	bool residentBuffersRetained = true;
	std::array<bool, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount>
		bufferReallocated{};
	auto ensureBuffer = [this, &resources, &descriptorPlan,
		&descriptorBindingsChanged, &residentBuffersRetained,
		&bufferReallocated](
		std::size_t bindingIndex,
		VkDeviceSize requiredBytes,
		const char* name) {
		// Vulkan disallows zero-sized buffers. A sixteen-byte safe sentinel lets a
		// missing optional cooked input fail in shader diagnostics rather than
		// binding VK_NULL_HANDLE or reading arbitrary old memory.
		const VkDeviceSize safeBytes = std::max<VkDeviceSize>(requiredBytes, 16U);
		VulkanBuffer& buffer = resources.buffers[bindingIndex];
		if(buffer.buffer == VK_NULL_HANDLE || resources.capacities[bindingIndex] < safeBytes) {
			RetireBuffer(buffer);
			buffer = CreateBuffer(safeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_CPU_TO_GPU, name);
			resources.capacities[bindingIndex] = safeBytes;
			descriptorBindingsChanged = true;
			bufferReallocated[bindingIndex] = true;
			if(descriptorPlan.bindings[bindingIndex].source
				== TerraEngine::Renderer::Vulkan::
					VulkanAnimationDescriptorSource::ResidentInput) {
				residentBuffersRetained = false;
			}
		}
	};

	const auto payloadByteCount = [&payloads](TerraEngine::Animation::GpuAnimationDescriptorBinding binding) {
		using Binding = TerraEngine::Animation::GpuAnimationDescriptorBinding;
		switch(binding) {
		case Binding::ProgramWords: return static_cast<VkDeviceSize>(payloads.programWords.size() * sizeof(std::uint32_t));
		case Binding::PackedClipBytes: return static_cast<VkDeviceSize>(payloads.packedClipBytes.size());
		case Binding::SkeletonTables: return static_cast<VkDeviceSize>(payloads.skeletonTables.size());
		case Binding::MaskTables: return static_cast<VkDeviceSize>(payloads.maskTables.size());
		case Binding::MorphInputs: return static_cast<VkDeviceSize>(payloads.morphInputs.size());
		default: return VkDeviceSize{0U};
		}
	};
	std::array<VkDeviceSize, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount>
		descriptorByteCounts{};
	for(std::size_t index = 0U; index < descriptorPlan.bindings.size(); ++index) {
		const auto binding = static_cast<TerraEngine::Animation::GpuAnimationDescriptorBinding>(index);
		descriptorByteCounts[index] = std::max<VkDeviceSize>(16U,
			std::max<VkDeviceSize>(descriptorPlan.bindings[index].minimumByteCount,
				payloadByteCount(binding)));
		if(descriptorByteCounts[index] > maximumStorageBufferRange) {
			// VkDescriptorBufferInfo::range may not exceed the selected device's
			// storage-buffer limit. Fail open before allocating or updating a set.
			return false;
		}
	}
	const std::size_t localPoseBindingIndex = static_cast<std::size_t>(
		TerraEngine::Animation::GpuAnimationDescriptorBinding::LocalPoses);
	holdLastHistoryCompatible = holdLastHistoryCompatible
		&& resources.buffers[localPoseBindingIndex].buffer != VK_NULL_HANDLE
		&& resources.capacities[localPoseBindingIndex] >= descriptorByteCounts[localPoseBindingIndex];
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Prepare Animation Buffers",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		try {
			for(std::size_t index = 0U; index < descriptorPlan.bindings.size(); ++index) {
				ensureBuffer(index, descriptorByteCounts[index], "GPU Animation Frame Buffer");
				const std::size_t diagnosticsBindingIndex = static_cast<std::size_t>(
					TerraEngine::Animation::GpuAnimationDescriptorBinding::Diagnostics);
				// Ordinary sample work overwrites every active local-pose record and can
				// never read the previous frame slot. Only hold-last fallback may consume
				// retained bytes, so an incompatible history requires clearing only when
				// at least one packet actually requests that fallback behavior.
				const bool clearLocalPose = index == localPoseBindingIndex
					&& requestsHoldLast && !holdLastHistoryCompatible;
				const bool clearDiagnostics = index == diagnosticsBindingIndex;
				// Input ranges are overwritten below and active output ranges are fully
				// produced by their dispatch stages. Only local-pose fallback state and
				// atomic diagnostics require a zero sentinel before compute begins.
				if((clearLocalPose || clearDiagnostics)
					&& !ClearMappedBufferRange(allocator, resources.buffers[index],
						descriptorByteCounts[index], 0U)) {
					RetireVulkanAnimationFrameResources(resources);
					return false;
				}
			}
		} catch(const std::exception&) {
			RetireVulkanAnimationFrameResources(resources);
			return false;
		}
	}
	const bool reuseResidentPayloadUpload =
		TerraEngine::Renderer::Vulkan::
			CanReuseVulkanAnimationResidentPayloadUpload(
				resources.residentPayloadOwner,
				plan.sharedResidentPayloads,
				residentBuffersRetained);

	using Binding = TerraEngine::Animation::GpuAnimationDescriptorBinding;
	if(!TerraEngine::Renderer::Vulkan::BuildVulkanAnimationComputeUploadPlan(
		plan, resources.computeUploadPlan)) {
		RetireVulkanAnimationFrameResources(resources);
		return false;
	}
	const TerraEngine::Renderer::Vulkan::VulkanAnimationComputeUploadPlan&
		computeUpload = resources.computeUploadPlan;
	const std::size_t dispatchBindingIndex = static_cast<std::size_t>(
		Binding::DispatchRecords);
	const std::size_t workPacketBindingIndex = static_cast<std::size_t>(
		Binding::WorkPackets);
	const bool reuseDispatchUpload = computeUpload.topologyReused
		&& !bufferReallocated[dispatchBindingIndex];
	const bool reuseWorkPacketUpload = computeUpload.topologyReused
		&& !bufferReallocated[workPacketBindingIndex];
	const auto upload = [this, &resources](Binding binding, const void* bytes, VkDeviceSize size) {
		if(size == 0U) return true;
		return WriteMappedBuffer(allocator, resources.buffers[static_cast<std::size_t>(binding)], bytes, size);
	};
	{
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Upload Animation Inputs",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(!upload(Binding::GenerationRecords, computeUpload.generationRecords.data(),
			static_cast<VkDeviceSize>(computeUpload.generationRecords.size()
				* sizeof(TerraEngine::Animation::GpuAnimationGenerationRecord)))
			|| !upload(Binding::SampleTicks, computeUpload.sampleTicks.data(),
				static_cast<VkDeviceSize>(computeUpload.sampleTicks.size()
					* sizeof(std::uint32_t)))
			|| (!reuseWorkPacketUpload
				&& !upload(Binding::WorkPackets, computeUpload.workPackets.data(),
					static_cast<VkDeviceSize>(computeUpload.workPackets.size()
						* sizeof(TerraEngine::Animation::GpuAnimationWorkPacket))))
			|| (!reuseDispatchUpload
				&& !upload(Binding::DispatchRecords, computeUpload.dispatchRecords.data(),
					static_cast<VkDeviceSize>(computeUpload.dispatchRecords.size()
						* sizeof(TerraEngine::Animation::GpuAnimationDispatchRecord))))) {
			RetireVulkanAnimationFrameResources(resources);
			return false;
		}
		if(!reuseResidentPayloadUpload
			&& (!upload(Binding::ProgramWords, payloads.programWords.data(),
					static_cast<VkDeviceSize>(payloads.programWords.size() * sizeof(std::uint32_t)))
				|| !upload(Binding::PackedClipBytes, payloads.packedClipBytes.data(),
					static_cast<VkDeviceSize>(payloads.packedClipBytes.size()))
				|| !upload(Binding::SkeletonTables, payloads.skeletonTables.data(),
					static_cast<VkDeviceSize>(payloads.skeletonTables.size()))
				|| !upload(Binding::MaskTables, payloads.maskTables.data(),
					static_cast<VkDeviceSize>(payloads.maskTables.size()))
				|| !upload(Binding::MorphInputs, payloads.morphInputs.data(),
					static_cast<VkDeviceSize>(payloads.morphInputs.size())))) {
			RetireVulkanAnimationFrameResources(resources);
			return false;
		}
	}

	resources.descriptorPlan = descriptorPlan;
	resources.planGeneration = plan.generation;
	resources.rendererGeneration = rendererGeneration;
	std::array<VkDescriptorBufferInfo, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount> infos{};
	std::array<VkWriteDescriptorSet, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount> writes{};
	for(std::uint32_t binding = 0U; binding < infos.size(); ++binding) {
		infos[binding].buffer = resources.buffers[binding].buffer;
		infos[binding].offset = 0U;
		infos[binding].range = resources.capacities[binding];
		writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstSet = animationDescriptorSets[currentFrame];
		writes[binding].dstBinding = binding;
		writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[binding].descriptorCount = 1U;
		writes[binding].pBufferInfo = &infos[binding];
	}
	// Descriptor handles and ranges remain stable while a frame-slot allocation
	// has enough capacity. Avoid rewriting an already-bound set when the editor
	// asks for a dedicated capture later in the same frame.
	if(descriptorBindingsChanged)
	{
		vkUpdateDescriptorSets(
			device,
			static_cast<std::uint32_t>(writes.size()),
			writes.data(),
			0U,
			nullptr);
	}
	const auto publishInternalAnimationBuffer = [this, &resources,
		&bufferReallocated](
		std::uint32_t& bindlessIndex,
		TerraEngine::Animation::GpuAnimationDescriptorBinding binding) {
		const std::size_t bindingIndex = static_cast<std::size_t>(binding);
		const VulkanBuffer& buffer = resources.buffers[bindingIndex];
		if(buffer.buffer == VK_NULL_HANDLE || buffer.size == 0U) return INVALID_BINDLESS_BUFFER_INDEX;
		const bool hasPublishedIndex =
			bindlessIndex != INVALID_BINDLESS_BUFFER_INDEX;
		if(!hasPublishedIndex) bindlessIndex = AllocateBufferIndex();
		// Global set 0, binding 9 is updated only before this frame records. These
		// indices stay internal; the graphics bridge below is the sole exposed one.
		// Bind the complete allocation so changing active packet counts does not turn
		// an otherwise stable frame slot into host descriptor mutation every frame.
		if(TerraEngine::Renderer::Vulkan::
			ShouldRefreshVulkanAnimationBindlessBuffer(
				hasPublishedIndex, bufferReallocated[bindingIndex])) {
			UpdateGlobalBufferForFrame(currentFrame, bindlessIndex, buffer.buffer,
				0U, resources.capacities[bindingIndex], true);
		}
		return bindlessIndex;
	};
	try {
		TERRAENGINE_PROFILE_SCOPE("Vulkan: Publish Animation Graphics Bridge",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		(void)publishInternalAnimationBuffer(
			resources.generationRecordsBindlessIndex, Binding::GenerationRecords);
		(void)publishInternalAnimationBuffer(
			resources.workPacketsBindlessIndex, Binding::WorkPackets);
		(void)publishInternalAnimationBuffer(
			resources.paletteMatricesBindlessIndex, Binding::PaletteMatrices);
		(void)publishInternalAnimationBuffer(
			resources.morphOutputsBindlessIndex, Binding::MorphOutputs);

		bool graphicsBridgeReallocated = false;
		if(resources.graphicsBridgeBuffer.buffer == VK_NULL_HANDLE
			|| resources.graphicsBridgeCapacityBytes < bridgeBytes) {
			RetireBuffer(resources.graphicsBridgeBuffer);
			resources.graphicsBridgeBuffer = CreateBuffer(bridgeBytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU,
				"GPU Animation Graphics Bridge Buffer");
			resources.graphicsBridgeCapacityBytes = bridgeBytes;
			graphicsBridgeReallocated = true;
		}
		const bool reuseGraphicsBridgeUpload = computeUpload.topologyReused
			&& !graphicsBridgeReallocated
			&& resources.graphicsBridgeBindlessIndex
				!= INVALID_BINDLESS_BUFFER_INDEX;
		if(!reuseGraphicsBridgeUpload) {
			void* mappedBridge = resources.graphicsBridgeBuffer.mappedData;
			const bool temporaryBridgeMapping = mappedBridge == nullptr;
			if((temporaryBridgeMapping
					&& vmaMapMemory(allocator,
						resources.graphicsBridgeBuffer.allocation,
						&mappedBridge) != VK_SUCCESS)
				|| mappedBridge == nullptr) {
				RetireVulkanAnimationFrameResources(resources);
				return false;
			}
			VulkanGpuAnimationGraphicsRecord* bridgeRecords =
				static_cast<VulkanGpuAnimationGraphicsRecord*>(mappedBridge);
			for(std::size_t packetIndex = 0U; packetIndex < plan.WorkPackets().size(); ++packetIndex) {
				const TerraEngine::Animation::GpuAnimationWorkPacket& packet = plan.WorkPackets()[packetIndex];
				VulkanGpuAnimationGraphicsRecord& record = bridgeRecords[packetIndex];
				record = {};
				record.planGeneration = plan.generation;
				record.rendererGeneration = rendererGeneration;
				record.workPacketIndex = static_cast<std::uint32_t>(packetIndex);
				record.generationRecordIndex = packet.generationRecordIndex;
				record.paletteMatricesBindlessIndex = resources.paletteMatricesBindlessIndex;
				record.morphOutputsBindlessIndex = resources.morphOutputsBindlessIndex;
				record.paletteOffsetBytes = packet.paletteOffsetBytes;
				record.morphOutputOffsetBytes = packet.morphOutputOffsetBytes;
				record.jointCount = packet.jointCount;
				record.morphCount = packet.morphCount;
				record.paletteEntryCount = packet.paletteEntryCount;
				record.flags = packet.flags;
				record.stageMask = packet.stageMask;
				record.validMarker = 1U;
			}
			vmaFlushAllocation(allocator, resources.graphicsBridgeBuffer.allocation,
				0U, bridgeBytes);
			if(temporaryBridgeMapping) {
				vmaUnmapMemory(allocator, resources.graphicsBridgeBuffer.allocation);
			}
		}
		const bool hasPublishedBridgeIndex =
			resources.graphicsBridgeBindlessIndex != INVALID_BINDLESS_BUFFER_INDEX;
		if(!hasPublishedBridgeIndex)
			resources.graphicsBridgeBindlessIndex = AllocateBufferIndex();
		if(TerraEngine::Renderer::Vulkan::
			ShouldRefreshVulkanAnimationBindlessBuffer(
				hasPublishedBridgeIndex, graphicsBridgeReallocated)) {
			UpdateGlobalBufferForFrame(currentFrame,
				resources.graphicsBridgeBindlessIndex,
				resources.graphicsBridgeBuffer.buffer, 0U,
				resources.graphicsBridgeCapacityBytes, true);
		}
		activeAnimationGraphicsBufferIndex = resources.graphicsBridgeBindlessIndex;
	} catch(const std::exception&) {
		RetireVulkanAnimationFrameResources(resources);
		return false;
	}
	resources.live = true;
	// Owned fallback payloads have no cross-frame identity and deliberately upload
	// every time. Shared immutable payloads remain retained until this slot grows,
	// retires, or observes a different publication.
	resources.residentPayloadOwner = plan.sharedResidentPayloads;
	resources.poseHistoryIdentities.swap(currentPoseHistoryIdentities);
	resources.completionPending = false;
	return true;
}

void VulkanContext::Impl::MarkVulkanAnimationGenerationSubmitted(std::uint32_t rendererGeneration)
{
	if(currentFrame >= animationFrameResources.size()) return;
	VulkanAnimationFrameResources& resources = animationFrameResources[currentFrame];
	if(!resources.live || resources.rendererGeneration != rendererGeneration
		|| resources.planGeneration == 0U || pendingAnimationFrameId == 0U
		|| !animationComputeRecordedForCurrentFrame
		|| recordedAnimationDispatchCountForCurrentFrame == 0U) {
		return;
	}
	resources.frameId = pendingAnimationFrameId;
	resources.submissionSerial = nextAnimationSubmissionSerial++;
	resources.recordedDispatchCount =
		recordedAnimationDispatchCountForCurrentFrame;
	resources.completionPending = true;
	const TerraEngine::Renderer::AnimationGpuBackendExecutionRecord submitted{
		resources.frameId,
		resources.submissionSerial,
		resources.planGeneration,
		resources.rendererGeneration,
		resources.recordedDispatchCount};
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestAnimationGpuBackendExecution.latestSubmitted = submitted;
	}
}

void VulkanContext::Impl::CompleteVulkanAnimationFrameResources(std::uint32_t frameIndex)
{
	if(frameIndex >= animationFrameResources.size()) return;
	VulkanAnimationFrameResources& resources = animationFrameResources[frameIndex];
	if(!resources.live || !resources.completionPending) return;
	// This function is called only after the owning frame fence has completed.
	// It publishes a generation-stamped receipt and deliberately performs no
	// pose readback or wait for current-frame compute work.
	while(completedAnimationGenerations.size() >= MAX_FRAMES_IN_FLIGHT) {
		// Completion is an observation channel, not a pose cache. Retain a bounded
		// recent history so a stalled diagnostic consumer cannot grow renderer RAM.
		completedAnimationGenerations.pop_front();
	}
	completedAnimationGenerations.push_back({
		resources.frameId,
		resources.planGeneration,
		resources.rendererGeneration,
		resources.submissionSerial,
		resources.recordedDispatchCount});
	const TerraEngine::Renderer::AnimationGpuBackendExecutionRecord completed{
		resources.frameId,
		resources.submissionSerial,
		resources.planGeneration,
		resources.rendererGeneration,
		resources.recordedDispatchCount};
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestAnimationGpuBackendExecution.latestCompleted = completed;
	}
	resources.completionPending = false;
}
