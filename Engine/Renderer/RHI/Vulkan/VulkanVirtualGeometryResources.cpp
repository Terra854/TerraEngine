// VulkanVirtualGeometryResources.cpp — Virtual-geometry packet, decode, and visibility resources
//
// This file owns one focused Vulkan resource responsibility. Allocations and retirement
// still flow through VulkanContext::Impl so frame-fence lifetime rules remain unchanged.

#include "VulkanResourceSource.h"

TerraEngine::Renderer::Vulkan::PreparedShadowPageSet VulkanContext::Impl::EnsureShadowAuthoredPagePayload(
	const TerraEngine::Renderer::Vulkan::ShadowPageCutRequest& request, bool allowUpload)
{
	using namespace TerraEngine::Renderer::Vulkan;
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	const auto reject = [](const char* reason) { return PreparedShadowPageSet{false, {}, reason}; };
	try {
		const auto cut = BuildShadowAuthoredPageCut(request);
		if (!cut.complete) return {false, {}, cut.fallbackReason};
		if (deviceLost || !initialized || allocator == VK_NULL_HANDLE
			|| std::any_of(globalDescriptorSets.begin(), globalDescriptorSets.end(), [](VkDescriptorSet set) { return set == VK_NULL_HANDLE; })
			|| request.mesh->publicationGeneration != request.cpuPublicationGeneration
			|| ResourceManager::Instance().AcquireMeshSnapshot(request.meshId) != request.mesh)
			return reject("Current native authored payload or device is unavailable.");

		PreparedShadowPageSet result;
		result.bindings.reserve(cut.segments.size());
		for (std::size_t index = 0U; index < cut.segments.size(); ++index) {
			PreparedShadowPageBinding binding;
			binding.segment = cut.segments[index]; binding.descriptorIndex = static_cast<std::uint32_t>(index);
			binding.cpuPublicationGeneration = request.cpuPublicationGeneration;
			result.bindings.push_back(binding);
		}
		const auto cached = std::find_if(shadowAuthoredPageResources.begin(), shadowAuthoredPageResources.end(), [&](const auto& resource) {
			return resource.meshId == request.meshId && resource.retainedMesh == request.mesh
				&& resource.cpuPublicationGeneration == request.cpuPublicationGeneration && resource.segments == cut.segments;
		});
		if (cached != shadowAuthoredPageResources.end()) {
			if (cached->payload.buffer == VK_NULL_HANDLE || cached->payload.size == 0U || cached->allocationGeneration == 0U
				|| cached->bindlessIndex == INVALID_BINDLESS_BUFFER_INDEX) return reject("Cached authored shadow payload is unavailable.");
			for (auto& binding : result.bindings) {
				binding.payloadBufferIndex = cached->bindlessIndex;
				binding.payloadAllocationGeneration = cached->allocationGeneration;
				// Every descriptor reads the same complete native word stream. Its
				// logical buffer extent is the proof, rather than an estimated page size.
				binding.payloadByteOffset = 0U;
				binding.payloadByteSize = cached->payload.size;
			}
			result.complete = true;
			return result;
		}
		// Recording consumes only bindings published before its primary began.
		// A changed cut or loader publication keeps the complete owner fallback;
		// the next frame can prepare its missing page descriptors safely.
		if (!allowUpload) return reject("Authored shadow page cut was not prepared before command recording.");

		// Each light-selected cut gets independent packet descriptors. Camera
		// visibility cannot remove an off-camera caster from this complete stream.
		std::vector<GpuVirtualGeometryPacketRecord> packets;
		packets.reserve(cut.segments.size());
		for (std::size_t index = 0U; index < cut.segments.size(); ++index) {
			const auto& segment = cut.segments[index];
			GpuVirtualGeometryPacketRecord packet;
			packet.packetId = static_cast<std::uint32_t>(index); packet.meshId = request.meshId;
			packet.pageId = segment.pageId; packet.fallbackPageId = segment.fallbackPageId;
			packet.clusterId = segment.clusterId; packet.selectedLod = segment.selectedLod;
			packet.meshletOffset = segment.ownerMeshletOffset; packet.meshletCount = segment.ownerMeshletCount;
			packet.payloadSource = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			packet.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
			packets.push_back(packet);
		}
		// This existing translator packs real owner topology into page-local shader
		// streams. No persistent streamed-page allocation receipt qualifies them.
		const auto payload = BuildVirtualGeometryPageLocalPayloadWords(packets);
		if (payload.words.empty() || payload.words.size() > (64U * 1024U * 1024U)
			|| ResourceManager::Instance().AcquireMeshSnapshot(request.meshId) != request.mesh)
			return reject("Authored page translation is empty, excessive, or raced a CPU replacement.");
		for (std::size_t index = 0U; index < cut.segments.size(); ++index) {
			const std::size_t row = index * kVirtualGeometryPagePayloadDescriptorStrideWords;
			if (row + kVirtualGeometryPagePayloadDescriptorStrideWords > payload.words.size())
				return reject("Authored descriptor table is incomplete.");
			const auto* words = payload.words.data() + row;
			const auto inRange = [&](std::uint32_t offset, std::uint64_t count) {
				return count != 0U && static_cast<std::uint64_t>(offset) + count <= payload.words.size();
			};
			const std::uint32_t meshletCount = words[kVirtualGeometryPagePayloadDescriptorMeshletCount];
			if (meshletCount != cut.segments[index].ownerMeshletCount
				|| words[kVirtualGeometryPagePayloadDescriptorBoundsCount] < meshletCount
				|| words[kVirtualGeometryPagePayloadDescriptorLocalVertexCount] == 0U
				|| !inRange(words[kVirtualGeometryPagePayloadDescriptorVertexWordOffset], words[kVirtualGeometryPagePayloadDescriptorVertexWordSize])
				|| !inRange(words[kVirtualGeometryPagePayloadDescriptorPrimitiveWordOffset], (static_cast<std::uint64_t>(words[kVirtualGeometryPagePayloadDescriptorPrimitiveByteSize]) + 3U) / 4U)
				|| !inRange(words[kVirtualGeometryPagePayloadDescriptorRemapWordOffset], words[kVirtualGeometryPagePayloadDescriptorRemapCount])
				|| !inRange(words[kVirtualGeometryPagePayloadDescriptorMeshletWordOffset], static_cast<std::uint64_t>(meshletCount) * sizeof(MeshletRecord) / 4U)
				|| !inRange(words[kVirtualGeometryPagePayloadDescriptorBoundsWordOffset], static_cast<std::uint64_t>(words[kVirtualGeometryPagePayloadDescriptorBoundsCount]) * sizeof(MeshletBounds) / 4U)
				|| !inRange(words[kVirtualGeometryPagePayloadDescriptorDecodeWordOffset], sizeof(VirtualGeometryPageLocalMeshDecodeInfo) / 4U))
				return reject("One authored page payload failed full shader stream validation.");
		}
		if (nextShadowPageAllocationGeneration == 0U) return reject("Authored shadow allocation generations are exhausted.");
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(physicalDevice, &properties);
		if (static_cast<std::uint64_t>(payload.words.size()) * 4U > properties.limits.maxStorageBufferRange)
			return reject("Authored shadow-local stream exceeds the device storage-buffer range.");
		ShadowAuthoredPageResource staged;
		staged.meshId = request.meshId; staged.retainedMesh = request.mesh;
		staged.cpuPublicationGeneration = request.cpuPublicationGeneration; staged.segments = cut.segments;
		shadowAuthoredPageResources.reserve(shadowAuthoredPageResources.size() + 1U);
		// The unpublished resource has never appeared in a GPU command. RAII can
		// release it immediately if allocation/upload/descriptor publication fails.
		struct UnpublishedGuard {
			Impl& impl; ShadowAuthoredPageResource& resource; bool committed = false;
			~UnpublishedGuard() noexcept {
				if (committed) return;
				impl.DestroyBufferImmediate(resource.payload);
				if (resource.bindlessIndex != INVALID_BINDLESS_BUFFER_INDEX) {
					try { impl.freeBufferIndices.push_back(resource.bindlessIndex); } catch (...) {}
				}
			}
		} guard{*this, staged};
		staged.payload = CreateBuffer(static_cast<VkDeviceSize>(payload.words.size()) * 4U,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "Authored Shadow Local Page Payload");
		if (staged.payload.buffer == VK_NULL_HANDLE
			|| !WriteMappedBuffer(allocator, staged.payload, payload.words.data(), staged.payload.size))
			return reject("Authored shadow-local payload allocation or upload failed.");
		staged.bindlessIndex = AllocateBufferIndex();
		if (!UpdateGlobalBuffer(staged.bindlessIndex, staged.payload.buffer, 0U, staged.payload.size))
			return reject("Authored shadow descriptor publication lacks actual idle proof.");
		if (deviceLost) return reject("Authored shadow descriptor publication lost the device.");
		staged.allocationGeneration = nextShadowPageAllocationGeneration++;
		for (auto& binding : result.bindings) {
			binding.payloadBufferIndex = staged.bindlessIndex;
			binding.payloadAllocationGeneration = staged.allocationGeneration;
			binding.payloadByteOffset = 0U;
			binding.payloadByteSize = staged.payload.size;
		}
		shadowAuthoredPageResources.push_back(std::move(staged));
		guard.committed = true;
		result.complete = true;
		return result;
	} catch (const std::exception&) {
		return reject("Authored shadow preparation failed; complete owner geometry remains authoritative.");
	}
}

void VulkanContext::Impl::DestroyShadowAuthoredPagePayloads(TerraEngine::UUID meshId)
{
	// The caller owns existing descriptor-idle/device-idle proof. This helper
	// adds no wait and destroys only resources that can no longer be sampled.
	for (auto it = shadowAuthoredPageResources.begin(); it != shadowAuthoredPageResources.end();) {
		if (meshId != TerraEngine::EMPTY_UUID && it->meshId != meshId) { ++it; continue; }
		if (it->bindlessIndex != INVALID_BINDLESS_BUFFER_INDEX) freeBufferIndices.push_back(it->bindlessIndex);
		DestroyBufferImmediate(it->payload);
		it = shadowAuthoredPageResources.erase(it);
	}
}

void VulkanContext::Impl::DestroyVirtualGeometryGpuPacketResources(VirtualGeometryGpuPacketFrameResources& resources)
{
	// The descriptor pool permits individual frees. Releasing this per-view set
	// before its buffers avoids accumulating stale compaction bindings across
	// editor view creation and retirement.
	if(device != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE
		&& resources.executorDescriptorSet != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(device, descriptorPool, 1u, &resources.executorDescriptorSet);
	}
	const std::array<VkDescriptorSet, 4> visibilityDescriptorSets{
		resources.visibilityTriangleSetupDescriptorSet,
		resources.visibilityComputeRasterDescriptorSet,
		resources.visibilityResolveDescriptorSet,
		resources.visibilityShadingBinsDescriptorSet };
	if(device != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE) {
		for(const VkDescriptorSet descriptorSet : visibilityDescriptorSets) {
			if(descriptorSet != VK_NULL_HANDLE) vkFreeDescriptorSets(device, descriptorPool, 1u, &descriptorSet);
		}
	}
	DestroyBufferImmediate(resources.packetBuffer);
	DestroyBufferImmediate(resources.traversalInstanceBuffer);
	DestroyBufferImmediate(resources.traversalRequestBuffer);
	DestroyBufferImmediate(resources.traversalCounterBuffer);
	DestroyBufferImmediate(resources.batchBuffer);
	DestroyBufferImmediate(resources.compactedPacketBuffer);
	DestroyBufferImmediate(resources.generatedSubmissionBuffer);
	DestroyBufferImmediate(resources.counterReadbackBuffer);
	DestroyBufferImmediate(resources.zeroWorkIndirectBuffer);
	DestroyBufferImmediate(resources.visibilityTriangleBuffer);
	DestroyBufferImmediate(resources.visibilitySetupPacketBuffer);
	DestroyBufferImmediate(resources.visibilitySetupMeshletBuffer);
	DestroyBufferImmediate(resources.visibilityDepthBuffer);
	DestroyBufferImmediate(resources.visibilityRecordBuffer);
	DestroyBufferImmediate(resources.resolvedVisibilityBuffer);
	DestroyBufferImmediate(resources.shadingBinBuffer);
	DestroyBufferImmediate(resources.pagePayloadBuffer);
	if (resources.packetBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.packetBufferIndex);
	}
	if (resources.batchBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.batchBufferIndex);
	}
	if (resources.compactedPacketBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.compactedPacketBufferIndex);
	}
	if (resources.generatedSubmissionBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.generatedSubmissionBufferIndex);
	}
	if (resources.counterReadbackBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.counterReadbackBufferIndex);
	}
	if (resources.pagePayloadBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(resources.pagePayloadBufferIndex);
	}
	resources = {};
}

void VulkanContext::Impl::PublishVirtualGeometryDecodedPageResources(
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation)
{
	if (currentFrame >= pendingVirtualGeometryPublicationsByFrame.size())
	{
		return;
	}

	auto& pending = pendingVirtualGeometryPublicationsByFrame[currentFrame];
	// Reserve before descriptor publication so committing native ownership cannot allocate.
	persistentVirtualGeometryPages.reserve(persistentVirtualGeometryPages.size() + presentation.virtualGeometryDecodedPageUploads.size());
	pending.reserve(pending.size() + presentation.virtualGeometryDecodedPageUploads.size());
	for (const VirtualGeometryDecodedPageUploadRecord& upload : presentation.virtualGeometryDecodedPageUploads)
	{
		if (upload.assetId == TerraEngine::EMPTY_UUID
			|| upload.pageId == 0xffffffffu
			|| upload.clusterId == 0xffffffffu
			|| upload.publicationGeneration == 0u
			|| upload.integrityValue == 0u
			|| upload.decodedBytes.empty())
		{
			continue;
		}

		auto resource = std::find_if(
			persistentVirtualGeometryPages.begin(),
			persistentVirtualGeometryPages.end(),
			[&upload](const PersistentVirtualGeometryPageResource& candidate)
			{
				return candidate.assetId == upload.assetId
					&& candidate.pageId == upload.pageId;
			});
		if (resource != persistentVirtualGeometryPages.end()
			&& resource->publicationGeneration >= upload.publicationGeneration)
		{
			continue;
		}

		if (nextVirtualGeometryPersistentPageAllocationId == 0U) continue;
		const bool replacing = resource != persistentVirtualGeometryPages.end();
		const std::uint64_t retiredAllocationId = replacing ? resource->allocationId : 0U;
		PersistentVirtualGeometryPageResource staged;
		staged.assetId = upload.assetId; staged.pageId = upload.pageId;
		staged.bindlessIndex = replacing ? resource->bindlessIndex : AllocateBufferIndex();
		staged.publicationGeneration = upload.publicationGeneration;
		staged.allocationId = nextVirtualGeometryPersistentPageAllocationId;
		struct UnpublishedPageGuard {
			Impl& owner; PersistentVirtualGeometryPageResource& page; bool newIndex; bool committed = false;
			~UnpublishedPageGuard() noexcept {
				if (committed) return;
				owner.DestroyBufferImmediate(page.buffer);
				if (newIndex) try { owner.freeBufferIndices.push_back(page.bindlessIndex); } catch (...) {}
			}
		} guard{*this, staged, !replacing};
		staged.buffer = CreateBuffer(static_cast<VkDeviceSize>(upload.decodedBytes.size()),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU, "Persistent Virtual Geometry Decoded Page");
		if (staged.buffer.buffer == VK_NULL_HANDLE || !WriteMappedBuffer(allocator,
			staged.buffer, upload.decodedBytes.data(), static_cast<VkDeviceSize>(upload.decodedBytes.size()))) continue;

		TerraEngine::Resource::VirtualGeometryPagePublicationCandidate candidate{};
		candidate.candidateId = staged.allocationId;
		candidate.publicationGeneration = upload.publicationGeneration;
		candidate.key.asset.assetId = upload.assetId;
		candidate.key.asset.sourceHash = upload.integrityValue;
		candidate.key.asset.buildRecipeHash =
			upload.integrityValue ^ 0x9e3779b97f4a7c15ull;
		if (candidate.key.asset.buildRecipeHash == 0u)
		{
			candidate.key.asset.buildRecipeHash = 1u;
		}
		candidate.key.generations = {
			upload.publicationGeneration,
			upload.publicationGeneration,
			upload.publicationGeneration,
			upload.publicationGeneration,
			upload.publicationGeneration };
		candidate.key.pageId = upload.pageId;
		candidate.key.pageContentGeneration = upload.integrityValue;
		candidate.payload = {
			upload.storedByteCount,
			upload.decodedByteCount,
			upload.integrityValue,
			true,
			true };
		candidate.hierarchy.clusterIds = { upload.clusterId };
		candidate.hierarchy.fallbackPageId =
			upload.fallbackPageId == 0xffffffffu
				? upload.pageId
				: upload.fallbackPageId;
		candidate.hierarchy.fallbackCoverageVerified = true;
		candidate.hierarchy.hierarchyAcyclic = true;
		candidate.descriptor.allocationId = staged.allocationId;
		candidate.descriptor.bufferIndex = staged.bindlessIndex;
		candidate.descriptor.descriptorIndex = staged.bindlessIndex;
		// BeginFrame changes this to true only after the owning slot fence.
		candidate.descriptor.uploadCompleted = false;
		for (const std::uint32_t dependency : upload.dependencyPageIds)
		{
			if (dependency != 0xffffffffu
				&& dependency != upload.pageId
				&& std::find(
					candidate.dependencyPageIds.begin(),
					candidate.dependencyPageIds.end(),
					dependency) == candidate.dependencyPageIds.end())
			{
				candidate.dependencyPageIds.push_back(dependency);
			}
		}
		candidate.dependenciesVerified = true;
		candidate.residencyTransitionVerified = true;
		if (retiredAllocationId != 0u)
		{
			candidate.retirements.push_back({
				retiredAllocationId,
				upload.publicationGeneration });
		}

		const auto duplicate = std::find_if(
			pending.begin(),
			pending.end(),
			[&candidate](const auto& existing)
			{
				return existing.key.asset.assetId == candidate.key.asset.assetId
					&& existing.key.pageId == candidate.key.pageId
					&& existing.publicationGeneration
						== candidate.publicationGeneration;
			});
		if (duplicate != pending.end()) continue;
		// Failed idle leaves the previous resource and descriptor exactly intact.
		// The staged mapped buffer has never appeared in any accepted GPU command.
		if (!UpdateGlobalBuffer(staged.bindlessIndex, staged.buffer.buffer, 0U, staged.buffer.size)) continue;
		if (replacing) {
			// The successful descriptor idle proof covers all old native readers.
			DestroyBufferImmediate(resource->buffer);
			*resource = staged;
		} else persistentVirtualGeometryPages.push_back(staged);
		guard.committed = true;
		++nextVirtualGeometryPersistentPageAllocationId;
		pending.push_back(std::move(candidate));
	}
}

// Prepare one bounded ExpandedProduction parity request for this frame slot.
// The upload record already contains immutable CPU-decoded canonical bytes, so
// the simple ExpandedProduction kernel can copy those exact bytes and prove
// that its ABI, descriptor ranges, and host readback contract are sound.
void VulkanContext::Impl::PrepareVulkanVirtualGeometryProductionDecode(
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation)
{
	if(currentFrame >= virtualGeometryProductionDecodeResourcesByFrame.size()) {
		return;
	}
	auto& resources = virtualGeometryProductionDecodeResourcesByFrame[currentFrame];
	if(resources.preparedFrameId == backendFrameSerial) {
		return;
	}
	resources.preparedFrameId = backendFrameSerial;
	// A slot may have completed an earlier request. Start this new frame with no
	// recordable plan so an absent/invalid candidate cannot replay stale bytes.
	resources.plan = {};
	resources.completionPending = false;

	constexpr std::size_t kMaximumDecodeBytes = 262144u;
	const auto candidate = std::find_if(presentation.virtualGeometryDecodedPageUploads.begin(),
		presentation.virtualGeometryDecodedPageUploads.end(), [kMaximumDecodeBytes](const VirtualGeometryDecodedPageUploadRecord& upload) {
		return upload.publicationGeneration != 0u
			&& !upload.decodedBytes.empty()
			&& upload.decodedBytes.size() <= kMaximumDecodeBytes
			&& upload.decodedBytes.size() % sizeof(std::uint32_t) == 0u;
		});
	if(candidate == presentation.virtualGeometryDecodedPageUploads.end()) {
		return;
	}
	if(!EnsureVulkanVirtualGeometryProductionDecodePipeline()) {
		return;
	}

	const auto ensureCapacity = [this](VulkanBuffer& buffer, VkDeviceSize requiredBytes, const char* name) {
		if(buffer.buffer != VK_NULL_HANDLE && buffer.size >= requiredBytes) {
			return true;
		}
		RetireBuffer(buffer);
		try {
			buffer = CreateBuffer(requiredBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_CPU_TO_GPU, name);
		}
		catch(const std::exception&) {
			return false;
		}
		return buffer.buffer != VK_NULL_HANDLE;
	};
	const VkDeviceSize decodedBytes = static_cast<VkDeviceSize>(candidate->decodedBytes.size());
	if(!ensureCapacity(resources.requestBuffer,
		sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeRequest),
		"Virtual Geometry Production Decode Request")
		|| !ensureCapacity(resources.encodedBuffer, decodedBytes,
			"Virtual Geometry Production Decode Encoded")
		|| !ensureCapacity(resources.decodedBuffer, decodedBytes,
			"Virtual Geometry Production Decode Decoded")
		|| !ensureCapacity(resources.resultReadbackBuffer,
		sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResult),
			"Virtual Geometry Production Decode Result")) {
		virtualGeometryProductionDecodeUnavailableReason = "Could not allocate bounded ExpandedProduction decode buffers.";
		return;
	}

	const std::uint32_t boundedBytes = static_cast<std::uint32_t>(decodedBytes);
	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeRequest request{};
	request.profileId = 1u;
	request.codecVersion = 1u;
	request.encodedByteCount = boundedBytes;
	request.decodedByteCount = boundedBytes;
	request.outputCapacityBytes = boundedBytes;
	request.requestIdentity = static_cast<std::uint32_t>(candidate->publicationGeneration);
	if(request.requestIdentity == 0u) {
		request.requestIdentity = 1u;
	}
	const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResult clearedResult{};
	if(!WriteMappedBuffer(allocator, resources.requestBuffer, &request, sizeof(request))
		|| !WriteMappedBuffer(allocator, resources.encodedBuffer, candidate->decodedBytes.data(), decodedBytes)
		|| !ClearMappedBuffer(allocator, resources.decodedBuffer)
		|| !WriteMappedBuffer(allocator, resources.resultReadbackBuffer, &clearedResult, sizeof(clearedResult))) {
		virtualGeometryProductionDecodeUnavailableReason = "Could not upload bounded ExpandedProduction decode inputs.";
		return;
	}
	if(resources.descriptorSet == VK_NULL_HANDLE) {
		VkDescriptorSetAllocateInfo allocationInfo{};
		allocationInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocationInfo.descriptorPool = descriptorPool;
		allocationInfo.descriptorSetCount = 1u;
		allocationInfo.pSetLayouts = &virtualGeometryProductionDecodeDescriptorSetLayout;
		if(vkAllocateDescriptorSets(device, &allocationInfo, &resources.descriptorSet) != VK_SUCCESS) {
			virtualGeometryProductionDecodeUnavailableReason = "Could not allocate the ExpandedProduction decode descriptor set.";
			return;
		}
	}
	const std::array<VkDescriptorBufferInfo, 4> bufferInfos{
		VkDescriptorBufferInfo{ resources.requestBuffer.buffer, 0u, resources.requestBuffer.size },
		VkDescriptorBufferInfo{ resources.encodedBuffer.buffer, 0u, resources.encodedBuffer.size },
		VkDescriptorBufferInfo{ resources.decodedBuffer.buffer, 0u, resources.decodedBuffer.size },
		VkDescriptorBufferInfo{ resources.resultReadbackBuffer.buffer, 0u, resources.resultReadbackBuffer.size } };
	std::array<VkWriteDescriptorSet, 4> writes{};
	for(std::uint32_t binding = 0u; binding < writes.size(); ++binding) {
		writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[binding].dstSet = resources.descriptorSet;
		writes[binding].dstBinding = binding;
		writes[binding].descriptorCount = 1u;
		writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[binding].pBufferInfo = &bufferInfos[binding];
	}
	vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0u, nullptr);

	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResources decodeResources{};
	decodeResources.requestBuffer = resources.requestBuffer.buffer;
	decodeResources.encodedBuffer = resources.encodedBuffer.buffer;
	decodeResources.decodedBuffer = resources.decodedBuffer.buffer;
	decodeResources.resultReadbackBuffer = resources.resultReadbackBuffer.buffer;
	decodeResources.descriptorSet = resources.descriptorSet;
	decodeResources.encodedCapacityBytes = static_cast<std::uint32_t>(resources.encodedBuffer.size);
	decodeResources.decodedCapacityBytes = static_cast<std::uint32_t>(resources.decodedBuffer.size);
	decodeResources.resourcesOwnedByFrame = true;
	resources.plan = TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryProductionDecodePlan(
		request, candidate->publicationGeneration,
		CalculateVirtualGeometryCompactHash(candidate->decodedBytes), decodeResources,
		virtualGeometryProductionDecodePipelines);
	if(!resources.plan.valid) {
		virtualGeometryProductionDecodeUnavailableReason = resources.plan.unavailableReason;
	}
}

// BeginFrame has already waited for this slot's fence before calling here.
// Mapping therefore consumes completed device writes without adding a CPU/GPU
// wait; failures leave the most recently verified device generation intact.
void VulkanContext::Impl::CompleteVulkanVirtualGeometryProductionDecode(std::uint32_t frameIndex)
{
	if(frameIndex >= virtualGeometryProductionDecodeResourcesByFrame.size()) {
		return;
	}
	auto& resources = virtualGeometryProductionDecodeResourcesByFrame[frameIndex];
	if(!resources.completionPending) {
		return;
	}
	TERRAENGINE_PROFILE_SCOPE("Vulkan: Complete Virtual Geometry Production Decode", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	resources.completionPending = false;
	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeResult result{};
	std::vector<std::uint8_t> decodedBytes(resources.plan.request.decodedByteCount);
	void* mappedResult = nullptr;
	void* mappedDecoded = nullptr;
	const bool mapped = resources.resultReadbackBuffer.allocation != VK_NULL_HANDLE
		&& resources.decodedBuffer.allocation != VK_NULL_HANDLE
		&& vmaInvalidateAllocation(allocator, resources.resultReadbackBuffer.allocation, 0u,
			resources.resultReadbackBuffer.size) == VK_SUCCESS
		&& vmaInvalidateAllocation(allocator, resources.decodedBuffer.allocation, 0u,
			resources.decodedBuffer.size) == VK_SUCCESS
		&& vmaMapMemory(allocator, resources.resultReadbackBuffer.allocation, &mappedResult) == VK_SUCCESS
		&& vmaMapMemory(allocator, resources.decodedBuffer.allocation, &mappedDecoded) == VK_SUCCESS;
	if(mapped) {
		std::memcpy(&result, mappedResult, sizeof(result));
		std::memcpy(decodedBytes.data(), mappedDecoded, decodedBytes.size());
	}
	if(mappedDecoded != nullptr) vmaUnmapMemory(allocator, resources.decodedBuffer.allocation);
	if(mappedResult != nullptr) vmaUnmapMemory(allocator, resources.resultReadbackBuffer.allocation);
	const auto receipt = mapped
		? TerraEngine::Renderer::Vulkan::VerifyVulkanVirtualGeometryProductionDecodeParity(
			resources.plan, result, decodedBytes)
		: TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodeParityReceipt{
			resources.plan.generation, false, false,
			"ExpandedProduction decode readback could not be mapped after its completed fence." };
	if(receipt.acceptedForPublication) {
		completedVirtualGeometryProductionDecodeEvidence = true;
		publishedVirtualGeometryProductionDecodeGeneration = receipt.generation;
		virtualGeometryProductionDecodeUnavailableReason.clear();
	}
	else {
		virtualGeometryProductionDecodeUnavailableReason = receipt.reason;
	}
}

void VulkanContext::Impl::PublishVirtualGeometryGpuPacketResources(
	VkCommandBuffer commandBuffer,
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
	const TerraEngine::Renderer::RenderFramePublication& publication,
	std::uint32_t publicationViewIndex,
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint64_t frameId,
	std::uint32_t statsSlotIndex,
	std::uint32_t lightDataOffset,
	std::uint32_t clusterDataOffset,
	std::uint32_t lightIndexOffset)
{
	activeVirtualGeometryPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryBatchBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryCompactedPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryGeneratedSubmissionBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryCounterReadbackBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryPagePayloadBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	// This reason is scoped to one immutable view publication. Retaining it
	// would make a repaired Scene or capture generation look unavailable because
	// an earlier view rejected stale packet input.
	virtualGeometryExecutorUnavailableReason.clear();
	virtualGeometryVisibilityUnavailableReason.clear();
	// Do not retain a previous view's success or failure if this frame has no
	// packet publication. Capability diagnostics are scoped to the current
	// command buffer, not the last frame that happened to have geometry.
	latestVirtualGeometryExecutorPlan = {};
	latestVirtualGeometryExecutorFailure = {};
	latestVirtualGeometryExecutorResources = {};
	latestVirtualGeometryVisibilityPlan = {};
	latestVirtualGeometryVisibilityResources = {};
	if (currentFrame >= virtualGeometryGpuPacketResourcesByFrame.size())
	{
		return;
	}

	std::vector<VirtualGeometryGpuPacketFrameResources>& frameResources = virtualGeometryGpuPacketResourcesByFrame[currentFrame];
	auto resourceIt = std::find_if(
		frameResources.begin(),
		frameResources.end(),
		[viewType, viewId](const VirtualGeometryGpuPacketFrameResources& resources) {
			return resources.viewType == viewType && resources.viewId == viewId;
		});
	if (resourceIt == frameResources.end())
	{
		VirtualGeometryGpuPacketFrameResources resources;
		resources.resourceSerial = nextVirtualGeometryGpuPacketResourceSerial++;
		resources.viewType = viewType;
		resources.viewId = viewId;
		frameResources.push_back(std::move(resources));
		resourceIt = frameResources.end();
		--resourceIt;
	}

	VirtualGeometryGpuPacketFrameResources& resources = *resourceIt;
	// Scene-view snapshots can borrow immutable packet evidence from the retained
	// frame cache. Resolve that ownership detail once so the backend consumes the
	// same value-oriented interface for borrowed and independently owned data.
	const auto& gpuPackets = presentation.VirtualGeometryPackets();
	const auto& gpuBatches = presentation.VirtualGeometryBatches();
	const auto& gpuFrameSets = presentation.VirtualGeometryFrameSets();
	const auto& gpuCompactionDispatches =
		presentation.VirtualGeometryCompactionDispatches();
	const auto& generatedSubmissions =
		presentation.VirtualGeometryGeneratedSubmissions();
	resources.frameId = frameId;
	resources.viewType = viewType;
	resources.viewId = viewId;
	resources.live = !gpuPackets.empty() || !gpuBatches.empty();
	resources.retireAfterFrameId = frameId + static_cast<std::uint64_t>(MAX_FRAMES_IN_FLIGHT);
	const bool reusePacketInputs = presentation.virtualGeometryPacketEvidenceSignature != 0u
		&& resources.packetInputCacheValid
		&& resources.packetInputReuseSignature
			== presentation.virtualGeometryPacketEvidenceSignature
		&& resources.packetBuffer.buffer != VK_NULL_HANDLE
		&& resources.batchBuffer.buffer != VK_NULL_HANDLE
		&& resources.traversalInstanceBuffer.buffer != VK_NULL_HANDLE;
	// Capture, reflection, Scene, and Game command buffers may share a frame
	// slot, but never a packet generation. Validate every immutable producer
	// record against this output before allocating or binding Vulkan buffers so a
	// stale editor/capture mirror safely falls back instead of crossing views.
	const auto belongsToCurrentPublication = [viewType, viewId](const auto& record) {
		return record.frameId != 0u && record.viewType == viewType
			&& record.viewId == viewId;
	};
	const bool hasCanonicalFrameSet = std::any_of(
		gpuFrameSets.begin(),
		gpuFrameSets.end(),
		[&belongsToCurrentPublication](const GpuVirtualGeometryPacketFrameSet& frameSet) {
			return belongsToCurrentPublication(frameSet);
		});
	// The retained signature was produced from the same immutable packet evidence
	// that populated these frame-slot buffers. On a cache hit, validating every
	// packet and batch again cannot discover a newer identity; validate the small
	// frame-set header and reserve the full scans for changed evidence.
	const bool canonicalPacketPublication = hasCanonicalFrameSet
		&& (reusePacketInputs
			|| (std::all_of(gpuPackets.begin(),
			gpuPackets.end(), belongsToCurrentPublication)
		&& std::all_of(gpuBatches.begin(),
			gpuBatches.end(), belongsToCurrentPublication)
		&& std::all_of(gpuCompactionDispatches.begin(),
			gpuCompactionDispatches.end(), belongsToCurrentPublication)
		&& std::all_of(generatedSubmissions.begin(),
			generatedSubmissions.end(),
			belongsToCurrentPublication)));
	if(resources.live && !canonicalPacketPublication)
	{
		resources.live = false;
		virtualGeometryExecutorUnavailableReason =
			"Virtual-geometry packet publication does not match the current frame-view identity.";
	}
	if (currentFrame < pendingVirtualGeometryPageRetirementsByFrame.size())
	{
		std::vector<TerraEngine::Renderer::Vulkan::VirtualGeometryPageRetirementPlan>& pendingPageRetirements =
			pendingVirtualGeometryPageRetirementsByFrame[currentFrame];
		for (const VirtualGeometryPageRetirementRecord& retirement : presentation.virtualGeometryPageRetirements)
		{
			if (retirement.protectedFallback || retirement.retired)
			{
				continue;
			}
			// Page retirement is represented as a value plan first. Actual
			// Vulkan buffer releases still go through the existing deferred
			// RetireBuffer path after all frame fences that could sample the
			// page payload have completed.
			TerraEngine::Renderer::Vulkan::VirtualGeometryPageRetirementPlan plan;
			plan.pageId = retirement.pageId;
			plan.canRetire = true;
			plan.retireAfterFrameId = std::max(retirement.frameId, frameId) + static_cast<std::uint64_t>(MAX_FRAMES_IN_FLIGHT);
			plan.reason = retirement.reason.empty()
				? "virtual geometry page queued from frame retirement record"
				: retirement.reason;
			pendingPageRetirements.push_back(std::move(plan));
		}
	}
	if (!resources.live)
	{
		latestVirtualGeometryExecutorPlan.frameId = frameId;
		latestVirtualGeometryExecutorPlan.evidenceId =
			"vulkan-vg-executor.rejected-publication."
				+ std::to_string(frameId) + "." + std::to_string(viewId);
		latestVirtualGeometryExecutorPlan.unavailableReason =
			virtualGeometryExecutorUnavailableReason;
		return;
	}

	auto ensureBuffer = [&](VulkanBuffer& buffer, std::size_t& capacity,
		std::size_t elementCount, std::size_t elementSize, const char* name,
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
		if (elementCount == 0u)
		{
			return;
		}

		const VkDeviceSize requiredBytes = static_cast<VkDeviceSize>(elementCount * elementSize);
		if (buffer.buffer == VK_NULL_HANDLE || capacity < elementCount || buffer.size < requiredBytes)
		{
			RetireBuffer(buffer);
			buffer = CreateBuffer(requiredBytes, usage, VMA_MEMORY_USAGE_CPU_TO_GPU, name);
			capacity = elementCount;
		}
	};
	auto ensureAndWriteBuffer = [&](VulkanBuffer& buffer, std::size_t& capacity,
		std::size_t elementCount, std::size_t elementSize, const void* source,
		const char* name,
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
		if (elementCount == 0u || source == nullptr)
		{
			return;
		}
		ensureBuffer(buffer, capacity, elementCount, elementSize, name, usage);
		const VkDeviceSize requiredBytes =
			static_cast<VkDeviceSize>(elementCount * elementSize);
		WriteMappedBuffer(allocator, buffer, source, requiredBytes);
	};

	// Resource publication runs serially on the protected render thread. Retain
	// its large transient arrays per thread so stable crowds reuse capacity instead
	// of allocating and freeing several megabytes for every view and frame.
	thread_local std::vector<TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord>
		packetUploadRecords;
	if(!reusePacketInputs)
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Build Virtual Geometry Packet Upload",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		packetUploadRecords.clear();
		packetUploadRecords.reserve(gpuPackets.size());
		for (const GpuVirtualGeometryPacketRecord& packet : gpuPackets)
		{
			packetUploadRecords.push_back(BuildVirtualGeometryPacketUploadRecord(packet));
		}
		for (const GpuVirtualGeometryBatchDescriptor& batch : gpuBatches)
		{
			const std::size_t batchBegin = batch.packetOffset;
			const std::size_t batchEnd = batchBegin + batch.packetCount;
			if (batch.packetCount == 0u
				|| batchBegin >= packetUploadRecords.size()
				|| batchEnd > packetUploadRecords.size())
			{
				continue;
			}
			// Graphics records the compatible batch at its first packet descriptor.
			// Carry that same stable slot through compaction so the device can
			// aggregate every surviving packet without relying on compacted order.
			const std::uint32_t generatedCommandIndex =
				packetUploadRecords[batchBegin].packetId;
			const std::uint32_t generatedCommandTaskGroupCount =
				(std::max(batch.submittedMeshlets, 1u) + 31u) / 32u;
			for (std::size_t packetIndex = batchBegin; packetIndex < batchEnd; ++packetIndex)
			{
				packetUploadRecords[packetIndex].generatedCommandIndex =
					generatedCommandIndex;
				packetUploadRecords[packetIndex].generatedCommandTaskGroupCount =
					generatedCommandTaskGroupCount;
			}
		}
		{
			TERRAENGINE_PROFILE_SCOPE(
				"Vulkan: Upload Virtual Geometry Packets",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			ensureAndWriteBuffer(
				resources.packetBuffer,
				resources.packetCapacity,
				packetUploadRecords.size(),
				sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord),
				packetUploadRecords.data(),
				"Virtual Geometry GPU Packet Buffer");
		}
	}
	// Traversal consumes a compact, fixed-stride instance table rather than the
	// larger packet record. Every entry carries this frame resource generation so
	// a stale table fail-opens to a bounded fallback request in the shader.
	struct TraversalInstanceWords { std::uint32_t root, generation, flags, reserved; };
	thread_local std::vector<TraversalInstanceWords> traversalInstances;
	if(!reusePacketInputs)
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Publish Virtual Geometry Traversal Inputs",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		traversalInstances.clear();
		traversalInstances.reserve(gpuPackets.size());
		for(const GpuVirtualGeometryPacketRecord& packet : gpuPackets) {
			traversalInstances.push_back({ packet.clusterId, static_cast<std::uint32_t>(resources.resourceSerial), packet.flags, 0u });
		}
		ensureAndWriteBuffer(resources.traversalInstanceBuffer, resources.traversalInstanceCapacity,
			traversalInstances.size(), sizeof(TraversalInstanceWords), traversalInstances.data(), "Virtual Geometry Traversal Instance Buffer");
		ensureAndWriteBuffer(
			resources.batchBuffer,
			resources.batchCapacity,
			gpuBatches.size(),
			sizeof(GpuVirtualGeometryBatchDescriptor),
			gpuBatches.data(),
			"Virtual Geometry GPU Batch Descriptor Buffer");
		resources.packetInputReuseSignature =
			presentation.virtualGeometryPacketEvidenceSignature;
		resources.packetInputCacheValid = resources.packetBuffer.buffer != VK_NULL_HANDLE
			&& resources.batchBuffer.buffer != VK_NULL_HANDLE
			&& resources.traversalInstanceBuffer.buffer != VK_NULL_HANDLE;
	}
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Prepare Virtual Geometry Device Outputs",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	const std::size_t traversalRequestCount =
		std::max<std::size_t>(
			3u, gpuPackets.size() * 3u);
	ensureBuffer(resources.traversalRequestBuffer, resources.traversalRequestCapacity,
		traversalRequestCount, sizeof(std::uint32_t), "Virtual Geometry Traversal Request Buffer",
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	std::size_t traversalCounterCapacity = 4u;
	ensureBuffer(resources.traversalCounterBuffer, traversalCounterCapacity,
		4u, sizeof(std::uint32_t), "Virtual Geometry Traversal Counter Buffer",
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	// Compaction is the only producer of this output. The command buffer clears
	// the retained allocation below, avoiding a large temporary zero array and a
	// host-to-device copy for data that the device immediately overwrites.
	ensureBuffer(
		resources.compactedPacketBuffer,
		resources.compactedPacketCapacity,
		gpuPackets.size(),
		sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryPacketUploadRecord),
		"Virtual Geometry GPU Compacted Packet Buffer",
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	// The compute shader owns these output ranges. Clear them in device command
	// order so counters cannot inherit a prior frame's values;
	// the existing CPU draw records remain the independent fail-open authority.
	const std::size_t generatedSubmissionWordCount =
		std::max<std::size_t>(4u, gpuPackets.size() * 4u);
	ensureBuffer(
		resources.generatedSubmissionBuffer,
		resources.generatedSubmissionCapacity,
		generatedSubmissionWordCount,
		sizeof(std::uint32_t),
		"Virtual Geometry Generated Submission Buffer",
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
			| VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	// Older frame-slot resources predate indirect usage. Recreate before any
	// generated command can be consumed so Vulkan validation never sees a
	// storage-only buffer at an indirect or transfer-clear boundary.
	if(!resources.generatedSubmissionSupportsIndirectAndTransferClear
		&& resources.generatedSubmissionBuffer.buffer != VK_NULL_HANDLE) {
		RetireBuffer(resources.generatedSubmissionBuffer);
		resources.generatedSubmissionBuffer = CreateBuffer(
			static_cast<VkDeviceSize>(generatedSubmissionWordCount * sizeof(std::uint32_t)),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
				| VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU, "Virtual Geometry Generated Submission Buffer");
		resources.generatedSubmissionCapacity = generatedSubmissionWordCount;
		resources.generatedSubmissionSupportsIndirectAndTransferClear = true;
	}
	ensureBuffer(
		resources.counterReadbackBuffer,
		resources.counterReadbackCapacity,
		4u,
		sizeof(std::uint32_t),
		"Virtual Geometry Counter Readback Buffer",
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	std::array<std::uint32_t, 3> zeroWorkIndirect{};
	std::size_t zeroWorkIndirectCapacity = 3u;
	if(resources.zeroWorkIndirectBuffer.buffer == VK_NULL_HANDLE) {
		resources.zeroWorkIndirectBuffer = CreateBuffer(sizeof(zeroWorkIndirect),
			VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VMA_MEMORY_USAGE_CPU_TO_GPU, "Virtual Geometry Zero Work Indirect Buffer");
		// The fallback command is immutable zero work. Initialize a new allocation
		// once instead of remapping and rewriting the same three words every frame.
		WriteMappedBuffer(allocator, resources.zeroWorkIndirectBuffer,
			zeroWorkIndirect.data(), sizeof(zeroWorkIndirect));
	}

	if(commandBuffer != VK_NULL_HANDLE)
	{
		// These ranges are produced by the traversal, compaction, and indirect
		// stages. Clearing them on the device removes multi-megabyte CPU writes
		// while preserving the same explicit zero-before-use contract.
		const std::array<const VulkanBuffer*, 5u> deviceClearedBuffers{
			&resources.traversalRequestBuffer,
			&resources.traversalCounterBuffer,
			&resources.compactedPacketBuffer,
			&resources.generatedSubmissionBuffer,
			&resources.counterReadbackBuffer };
		for(const VulkanBuffer* buffer : deviceClearedBuffers)
		{
			if(buffer->buffer != VK_NULL_HANDLE && buffer->size != 0u)
			{
				vkCmdFillBuffer(commandBuffer, buffer->buffer, 0u, buffer->size, 0u);
			}
		}
		VkMemoryBarrier clearBarrier{};
		clearBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
			| VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
		vkCmdPipelineBarrier(commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
				| VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
			0u, 1u, &clearBarrier, 0u, nullptr, 0u, nullptr);
	}
	}

	// The page-local virtual geometry payload appends a shader-decodable stream
	// after the packet descriptor table. Building that stream performs remapping
	// and temporary allocations, so keep the result in this frame slot when
	// the packet cut and loaded mesh instances are unchanged.
	const bool reusePagePayloadEvidence =
		presentation.virtualGeometryPacketEvidenceSignature != 0u
		&& resources.pagePayloadCacheValid
		&& resources.pagePayloadEvidenceSignature
			== presentation.virtualGeometryPacketEvidenceSignature;
	std::uint64_t pagePayloadSignature = resources.pagePayloadReuseSignature;
	if(!reusePagePayloadEvidence)
	{
		TERRAENGINE_PROFILE_SCOPE(
			"Vulkan: Build Virtual Geometry Page Payload Signature",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		pagePayloadSignature = BuildVirtualGeometryPagePayloadResourceSignature(
			gpuPackets);
	}
	const bool reusePagePayload = reusePagePayloadEvidence
		|| TerraEngine::Renderer::Vulkan::CanReuseVirtualGeometryPagePayload(
			resources.pagePayloadReuseSignature,
			pagePayloadSignature,
			resources.pagePayloadCacheValid,
			resources.pagePayloadBuffer.buffer != VK_NULL_HANDLE);
	if (!reusePagePayload)
	{
		VirtualGeometryPageLocalPayloadBuildResult pagePayloadBuild;
		{
			TERRAENGINE_PROFILE_SCOPE("Vulkan: Build Virtual Geometry Page Payload", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			pagePayloadBuild = BuildVirtualGeometryPageLocalPayloadWords(gpuPackets);
		}
		resources.pagePayloadBytesByMesh = std::move(pagePayloadBuild.bytesByMesh);
		{
			TERRAENGINE_PROFILE_SCOPE("Vulkan: Upload Virtual Geometry Page Payload", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			ensureAndWriteBuffer(
				resources.pagePayloadBuffer,
				resources.pagePayloadCapacity,
				pagePayloadBuild.words.size(),
				sizeof(std::uint32_t),
				pagePayloadBuild.words.data(),
				"Virtual Geometry Physical Page Payload Descriptor Buffer");
		}
		resources.pagePayloadReuseSignature = pagePayloadSignature;
		resources.pagePayloadCacheValid = resources.pagePayloadBuffer.buffer != VK_NULL_HANDLE
			&& !pagePayloadBuild.words.empty();
	}
	if(resources.pagePayloadCacheValid)
	{
		resources.pagePayloadEvidenceSignature =
			presentation.virtualGeometryPacketEvidenceSignature;
	}

	auto publishBindlessBufferForCurrentFrame = [&](
		std::uint32_t& bindlessIndex,
		VirtualGeometryGpuPacketFrameResources::BindlessBufferPublication& publication,
		const VulkanBuffer& buffer) -> std::uint32_t {
		if (buffer.buffer == VK_NULL_HANDLE || buffer.size == 0u)
		{
			return INVALID_BINDLESS_BUFFER_INDEX;
		}
		if (bindlessIndex == INVALID_BINDLESS_BUFFER_INDEX)
		{
			bindlessIndex = AllocateBufferIndex();
			publication = {};
		}
		const bool alreadyPublished =
			publication.buffer == buffer.buffer
			&& publication.range == buffer.size;
		if (!alreadyPublished)
		{
			// The global bindless descriptor set is shared by every editor view
			// in this frame slot. Only publish when the handle or range changes;
			// otherwise an unnecessary descriptor update can invalidate a Scene
			// View command buffer that was recorded before Game View submission.
			UpdateGlobalBufferForFrame(currentFrame, bindlessIndex, buffer.buffer, 0u, buffer.size);
			publication.buffer = buffer.buffer;
			publication.range = buffer.size;
		}
		return bindlessIndex;
	};
	activeVirtualGeometryPacketBufferIndex = publishBindlessBufferForCurrentFrame(resources.packetBufferIndex, resources.packetPublication, resources.packetBuffer);
	activeVirtualGeometryBatchBufferIndex = publishBindlessBufferForCurrentFrame(resources.batchBufferIndex, resources.batchPublication, resources.batchBuffer);
	activeVirtualGeometryCompactedPacketBufferIndex = publishBindlessBufferForCurrentFrame(resources.compactedPacketBufferIndex, resources.compactedPacketPublication, resources.compactedPacketBuffer);
	activeVirtualGeometryGeneratedSubmissionBufferIndex = publishBindlessBufferForCurrentFrame(resources.generatedSubmissionBufferIndex, resources.generatedSubmissionPublication, resources.generatedSubmissionBuffer);
	activeVirtualGeometryCounterReadbackBufferIndex = publishBindlessBufferForCurrentFrame(resources.counterReadbackBufferIndex, resources.counterReadbackPublication, resources.counterReadbackBuffer);
	// These page payload source buffers are owned by the current frame slot.
	// They stay alive until the frame is safely retired, which prevents Scene
	// View/Game View from sampling a buffer that another view already reused.
	activeVirtualGeometryPagePayloadBufferIndex = publishBindlessBufferForCurrentFrame(resources.pagePayloadBufferIndex, resources.pagePayloadPublication, resources.pagePayloadBuffer);

	// Build a private descriptor set rather than borrowing the graphics bindless
	// set: the compaction shader has a fixed four-storage-buffer ABI at set 0.
	// Allocation or shader setup failures remain a diagnosed CPU fallback.
	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorResources executorResources;
	executorResources.packetBuffer = resources.packetBuffer.buffer;
	executorResources.traversalInstanceBuffer = resources.traversalInstanceBuffer.buffer;
	executorResources.traversalRequestBuffer = resources.traversalRequestBuffer.buffer;
	executorResources.traversalCounterBuffer = resources.traversalCounterBuffer.buffer;
	executorResources.compactedPacketBuffer = resources.compactedPacketBuffer.buffer;
	executorResources.generatedSubmissionBuffer = resources.generatedSubmissionBuffer.buffer;
	executorResources.counterReadbackBuffer = resources.counterReadbackBuffer.buffer;
	executorResources.zeroWorkIndirectBuffer = resources.zeroWorkIndirectBuffer.buffer;
	executorResources.resourcesOwnedByFrame = resources.live;
	const bool executorBuffersValid = executorResources.packetBuffer != VK_NULL_HANDLE
		&& executorResources.compactedPacketBuffer != VK_NULL_HANDLE
		&& executorResources.generatedSubmissionBuffer != VK_NULL_HANDLE
		&& executorResources.counterReadbackBuffer != VK_NULL_HANDLE
		&& executorResources.traversalInstanceBuffer != VK_NULL_HANDLE
		&& executorResources.traversalRequestBuffer != VK_NULL_HANDLE
		&& executorResources.traversalCounterBuffer != VK_NULL_HANDLE
		&& executorResources.zeroWorkIndirectBuffer != VK_NULL_HANDLE;
	if(!executorBuffersValid) {
		virtualGeometryExecutorUnavailableReason = "Virtual-geometry compaction storage buffers are unavailable for this view.";
	}
	else if(EnsureVulkanVirtualGeometryExecutorPipeline()) {
		if(resources.executorDescriptorSet == VK_NULL_HANDLE) {
			VkDescriptorSetAllocateInfo allocationInfo{};
			allocationInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocationInfo.descriptorPool = descriptorPool;
			allocationInfo.descriptorSetCount = 1u;
			allocationInfo.pSetLayouts = &virtualGeometryExecutorDescriptorSetLayout;
			if(vkAllocateDescriptorSets(device, &allocationInfo, &resources.executorDescriptorSet) != VK_SUCCESS) {
				virtualGeometryExecutorUnavailableReason = "Could not allocate the virtual-geometry compaction descriptor set.";
			}
		}
		if(resources.executorDescriptorSet != VK_NULL_HANDLE) {
			const std::array<VkDescriptorBufferInfo, 7> bufferInfos{
				VkDescriptorBufferInfo{ resources.traversalInstanceBuffer.buffer, 0u, resources.traversalInstanceBuffer.size },
				VkDescriptorBufferInfo{ resources.traversalRequestBuffer.buffer, 0u, resources.traversalRequestBuffer.size },
				VkDescriptorBufferInfo{ resources.traversalCounterBuffer.buffer, 0u, resources.traversalCounterBuffer.size },
				VkDescriptorBufferInfo{ resources.packetBuffer.buffer, 0u, resources.packetBuffer.size },
				VkDescriptorBufferInfo{ resources.compactedPacketBuffer.buffer, 0u, resources.compactedPacketBuffer.size },
				VkDescriptorBufferInfo{ resources.generatedSubmissionBuffer.buffer, 0u, resources.generatedSubmissionBuffer.size },
				VkDescriptorBufferInfo{ resources.counterReadbackBuffer.buffer, 0u, resources.counterReadbackBuffer.size } };
			std::array<VirtualGeometryGpuPacketFrameResources::BindlessBufferPublication, 7u>
				currentPublications{};
			for(std::size_t binding = 0u; binding < bufferInfos.size(); ++binding) {
				currentPublications[binding] = {
					bufferInfos[binding].buffer,
					bufferInfos[binding].range };
			}
			if(currentPublications != resources.executorDescriptorPublications) {
				std::array<VkWriteDescriptorSet, 7> writes{};
				for(std::uint32_t binding = 0u; binding < writes.size(); ++binding) {
					writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					writes[binding].dstSet = resources.executorDescriptorSet;
					writes[binding].dstBinding = binding;
					writes[binding].descriptorCount = 1u;
					writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					writes[binding].pBufferInfo = &bufferInfos[binding];
				}
				vkUpdateDescriptorSets(device,
					static_cast<std::uint32_t>(writes.size()),
					writes.data(), 0u, nullptr);
				resources.executorDescriptorPublications = currentPublications;
			}
			executorResources.descriptorSet = resources.executorDescriptorSet;
		}
	}
	const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorPipelines executorPipelines =
		virtualGeometryExecutorPipelines;
	const std::uint64_t executorGeneration = resources.resourceSerial;
	const std::uint32_t boundedCapacity = static_cast<std::uint32_t>(
		std::min<std::size_t>(resources.packetCapacity, std::numeric_limits<std::uint32_t>::max()));
	latestVirtualGeometryExecutorPlan =
		TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryExecutorPlan(
			static_cast<std::uint32_t>(std::min<std::size_t>(gpuPackets.size(), std::numeric_limits<std::uint32_t>::max())),
			boundedCapacity,
			false,
			executorResources,
			executorPipelines,
			frameId,
			executorGeneration,
			executorGeneration,
			executorGeneration);
	latestVirtualGeometryExecutorFailure =
		TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryFailureReceipt(
			latestVirtualGeometryExecutorPlan,
			TerraEngine::Renderer::VirtualGeometryBackendStage::Compaction);
	latestVirtualGeometryExecutorResources = executorResources;
	if(!virtualGeometryExecutorUnavailableReason.empty()
		&& latestVirtualGeometryExecutorPlan.unavailableReason.empty()) {
		latestVirtualGeometryExecutorPlan.unavailableReason = virtualGeometryExecutorUnavailableReason;
		latestVirtualGeometryExecutorFailure =
			TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryFailureReceipt(
				latestVirtualGeometryExecutorPlan,
				TerraEngine::Renderer::VirtualGeometryBackendStage::Compaction);
	}

	if(!reusePacketInputs)
	{
		resources.productionComputeRasterRequested = std::any_of(
			gpuPackets.begin(),
			gpuPackets.end(),
			[](const GpuVirtualGeometryPacketRecord& packet)
			{
				const std::uint32_t route =
					(packet.flags >> kVirtualGeometryRasterRouteShift)
						& kVirtualGeometryRasterRouteMask;
				return route == static_cast<std::uint32_t>(
					TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute);
			});
	}
	const bool productionComputeRasterRequested =
		resources.productionComputeRasterRequested;
	if(!presentation.virtualGeometryVisibilityDiagnosticsRequested
		&& !productionComputeRasterRequested)
	{
		// Fixed-function raster remains the zero-overhead default. Allocate and
		// expand visibility data only for an explicit diagnostic or a packet whose
		// projected microtriangles were assigned to production compute raster.
		virtualGeometryVisibilityUnavailableReason =
			"Virtual-geometry visibility diagnostics were not requested for this view.";
		return;
	}

	// Upload only packet state and compact meshlet work. The GPU setup stage reads
	// packed page-local vertices/topology and writes the exact RasterTriangle ABI,
	// eliminating per-frame CPU vertex decode and per-triangle expansion.
	constexpr std::size_t kMaximumVisibilityPrimitiveCount = 1u << 20u;
	constexpr std::uint64_t kMaximumVisibilityPixelCount = 1u << 24u;
	constexpr std::uint32_t kRasterDoubleSidedFlag = 1u << 31u;
	const VulkanDeformationFrameResources* currentDeformationResources = nullptr;
	if(deformationComputeRecordedForCurrentFrame
		&& pendingDeformationImmutablePlan != nullptr
		&& activeDeformationGraphicsBufferIndex != INVALID_BINDLESS_BUFFER_INDEX
		&& currentFrame < deformationFrameResources.size())
	{
		const VulkanDeformationFrameResources& resources =
			deformationFrameResources[currentFrame];
		if(resources.live
			&& resources.frameId == pendingDeformationImmutablePlan->frameId)
		{
			currentDeformationResources = std::addressof(resources);
		}
	}
	std::uint64_t visibilityInputSignature =
		presentation.virtualGeometryVisibilitySetupSignature;
	const auto combineVisibilityLane = [&visibilityInputSignature](
		std::uint64_t value)
	{
		visibilityInputSignature ^= value + 0x9e3779b97f4a7c15ULL
			+ (visibilityInputSignature << 6U)
			+ (visibilityInputSignature >> 2U);
	};
	combineVisibilityLane(statsSlotIndex);
	combineVisibilityLane(lightDataOffset);
	combineVisibilityLane(clusterDataOffset);
	combineVisibilityLane(lightIndexOffset);
	combineVisibilityLane(activeDeformationGraphicsBufferIndex);
	combineVisibilityLane(currentDeformationResources != nullptr
		? currentDeformationResources->graphicsGroupSignature
		: 0u);
	const bool reuseVisibilityInputs =
		presentation.virtualGeometryVisibilitySetupSignature != 0u
		&& resources.visibilityInputCacheValid
		&& resources.visibilityInputReuseSignature == visibilityInputSignature;
	std::size_t visibilityTriangleCount =
		reuseVisibilityInputs ? resources.visibilityInputTriangleCount : 0u;
	std::size_t visibilityMeshletCount =
		reuseVisibilityInputs ? resources.visibilityInputMeshletCount : 0u;
	if(!reuseVisibilityInputs)
	{
		thread_local std::vector<VirtualGeometryRasterPacketSetupUpload>
			visibilityPackets;
		thread_local std::vector<VirtualGeometryRasterMeshletSetupUpload>
			visibilityMeshlets;
		visibilityPackets.clear();
		visibilityMeshlets.clear();
		visibilityPackets.reserve(gpuPackets.size());
		visibilityMeshlets.reserve(gpuPackets.size());
		// Resolve compact view ordinals through the publication roots.  This replaces
		// the former RenderItemRecord lookup while preserving the exact object, mesh,
		// material, model, and editor-tint evidence required by compute raster setup.
		struct PublicationDrawEvidence
		{
			TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
			TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
			TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
			glm::mat4 modelMatrix = glm::mat4(1.0f);
			glm::vec4 editorTint = glm::vec4(0.0f);
			std::uint32_t meshletOffset = 0u;
			std::uint32_t meshletCount = 0u;
			std::uint32_t selectedLod = 0u;
			std::uint32_t visibilityIdentityIndex = 0u;
		};
		std::vector<PublicationDrawEvidence> publicationDraws;
		if(publicationViewIndex < publication.views.size()
			&& publication.persistent != nullptr && publication.dynamic != nullptr)
		{
			const TerraEngine::Renderer::RenderViewCommandResolver commandResolver =
				TerraEngine::Renderer::MakeRenderViewCommandResolver(
					publication, publicationViewIndex);
			publicationDraws.reserve(commandResolver.Count());
			for(std::size_t selectionIndex = 0u;
				selectionIndex < commandResolver.Count(); ++selectionIndex)
			{
				RenderItemRecord command{};
				if(!commandResolver.Resolve(selectionIndex, command)) continue;
				publicationDraws.push_back({ command.objectId,
					command.meshId, command.materialId, command.modelMatrix,
					command.editorTint, command.virtualMeshletOffset,
					command.virtualMeshletCount, command.selectedVirtualLod,
					command.conventionalVisibilityIdentityIndex });
			}
		}
		std::unordered_multimap<TerraEngine::UUID, const PublicationDrawEvidence*>
			publicationDrawsByObject;
		publicationDrawsByObject.reserve(publicationDraws.size());
		for(const PublicationDrawEvidence& draw : publicationDraws) {
			publicationDrawsByObject.emplace(draw.objectId, &draw);
		}
		for(const GpuVirtualGeometryPacketRecord& packet : gpuPackets)
		{
			if(visibilityTriangleCount >= kMaximumVisibilityPrimitiveCount)
			{
				break;
			}
			const std::uint32_t packetRoute =
				(packet.flags >> kVirtualGeometryRasterRouteShift)
				& kVirtualGeometryRasterRouteMask;
			if(productionComputeRasterRequested
				&& packetRoute != static_cast<std::uint32_t>(
					TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute))
			{
				continue;
			}
			const auto meshOwner = ResourceManager::Instance().AcquireMeshSnapshot(packet.meshId);
			const Mesh* mesh = meshOwner.get();
			const PublicationDrawEvidence* drawEvidence = nullptr;
			const auto [drawBegin, drawEnd] =
				publicationDrawsByObject.equal_range(packet.objectId);
			for(auto candidateIt = drawBegin; candidateIt != drawEnd; ++candidateIt)
			{
				const PublicationDrawEvidence& candidate = *candidateIt->second;
				const std::uint64_t commandEnd =
					static_cast<std::uint64_t>(candidate.meshletOffset)
					+ candidate.meshletCount;
				const std::uint64_t packetEnd =
					static_cast<std::uint64_t>(packet.meshletOffset)
					+ packet.meshletCount;
				if(candidate.objectId == packet.objectId && candidate.meshId == packet.meshId
					&& candidate.materialId == packet.materialId
					&& candidate.meshletOffset <= packet.meshletOffset
					&& commandEnd >= packetEnd)
				{
					drawEvidence = candidateIt->second;
					break;
				}
			}
			if(mesh == nullptr || drawEvidence == nullptr)
			{
				continue;
			}
		const glm::mat4 worldTransform = drawEvidence->modelMatrix;
		const std::uint32_t materialIndex = materialIndexMap.contains(
			packet.materialId)
			? materialIndexMap.at(packet.materialId)
			: 0u;
		const auto materialOwner = ResourceManager::Instance().AcquireMaterialSnapshot(packet.materialId);
		const Material* packetMaterial = materialOwner.get();
		const bool doubleSided = packetMaterial != nullptr
			&& (packetMaterial->header.flags & DoubleSided) != 0u;
		VirtualGeometryRasterPacketSetupUpload setup;
		setup.viewProjection = presentation.projectionMatrix * presentation.viewMatrix;
		setup.model = worldTransform;
		setup.normalTransform = glm::mat4(glm::transpose(
			glm::inverse(glm::mat3(worldTransform))));
		setup.editorTint = drawEvidence->editorTint;
		setup.payloadState = glm::uvec4(
			packet.packetId,
			packet.pageId,
			packetMaterial != nullptr ? packetMaterial->header.flags : 0u,
			INVALID_BINDLESS_BUFFER_INDEX);
		const std::uint32_t materialBin =
			(packet.flags >> kVirtualGeometryMaterialBinShift)
			& kVirtualGeometryMaterialBinMask;
		const std::uint32_t rasterRoute =
			(packet.flags >> kVirtualGeometryRasterRouteShift)
			& kVirtualGeometryRasterRouteMask;
		setup.identity = glm::uvec4(
			0u,
			materialBin | (rasterRoute << 8u),
			static_cast<std::uint32_t>(executorGeneration),
			packet.flags | (doubleSided ? kRasterDoubleSidedFlag : 0u));
		setup.shadingOffsets = glm::uvec4(
			materialIndex,
			statsSlotIndex,
			lightDataOffset,
			clusterDataOffset);
		setup.shadingState = glm::uvec4(
			lightIndexOffset,
			packet.flags,
			packet.meshletOffset,
			packet.packetId);
		// Each immutable packet names one selected cluster/page demand. The compact
		// selection carries the LOD while packet records carry the exact coverage.
		const std::uint32_t selectedClusterCount = 1u;
		const std::uint32_t selectedPageNeedCount = 1u;
		setup.debugState = glm::uvec4(
			(drawEvidence->selectedLod & 0xffu)
				| (selectedClusterCount << 8u)
				| (selectedPageNeedCount << 16u)
				| ((packet.debugIdentity & 0xffu) << 24u),
			// The composite's integer attachment must name the same immutable
			// object table as direct mesh draws, including distant compute LODs.
			drawEvidence->visibilityIdentityIndex,
			packet.packetId,
			selectedPageNeedCount);
		setup.deformationState = glm::uvec4(
			INVALID_BINDLESS_BUFFER_INDEX,
			packet.meshletOffset,
			0u,
			0u);
		const std::uint32_t setupPacketIndex =
			static_cast<std::uint32_t>(visibilityPackets.size());
		const std::size_t packetTriangleBase = visibilityTriangleCount;
		const std::uint32_t endMeshlet = std::min<std::uint32_t>(
			packet.meshletOffset + packet.meshletCount,
			static_cast<std::uint32_t>(mesh->meshletRecords.size()));
		for(std::uint32_t meshletIndex = packet.meshletOffset;
			meshletIndex < endMeshlet
				&& visibilityTriangleCount < kMaximumVisibilityPrimitiveCount;
			++meshletIndex)
		{
			const MeshletRecord& meshlet = mesh->meshletRecords[meshletIndex];
			if(meshlet.vertexCount == 0u
				|| (meshlet.primitiveIndexCount % 3u) != 0u
				|| static_cast<std::size_t>(meshlet.primitiveIndexOffset)
					+ meshlet.primitiveIndexCount
					> mesh->meshletPrimitiveIndexData.size())
			{
				continue;
			}
			const std::uint32_t triangleCount =
				meshlet.primitiveIndexCount / 3u;
			// Empty meshlets carry no setup work. Skipping them also prevents a
			// zero-length work record from referencing a packet record that is not
			// published below.
			if(triangleCount == 0u)
			{
				continue;
			}
			const std::size_t boundedTriangleCount = std::min<std::size_t>(
				triangleCount,
				kMaximumVisibilityPrimitiveCount - visibilityTriangleCount);
			visibilityMeshlets.push_back({
				setupPacketIndex,
				meshletIndex - packet.meshletOffset,
				static_cast<std::uint32_t>(visibilityTriangleCount),
				static_cast<std::uint32_t>(boundedTriangleCount) });
			visibilityTriangleCount += boundedTriangleCount;
		}
		if(visibilityTriangleCount != packetTriangleBase)
		{
			visibilityPackets.push_back(setup);
		}
	}
	ensureAndWriteBuffer(resources.visibilitySetupPacketBuffer,
		resources.visibilitySetupPacketCapacity, visibilityPackets.size(),
		sizeof(VirtualGeometryRasterPacketSetupUpload), visibilityPackets.data(),
		"Virtual Geometry Compute Raster Packet Setup Buffer");
	ensureAndWriteBuffer(resources.visibilitySetupMeshletBuffer,
		resources.visibilitySetupMeshletCapacity, visibilityMeshlets.size(),
		sizeof(VirtualGeometryRasterMeshletSetupUpload), visibilityMeshlets.data(),
		"Virtual Geometry Compute Raster Meshlet Setup Buffer");
	visibilityMeshletCount = visibilityMeshlets.size();
	if(visibilityTriangleCount != 0u)
	{
		const VkDeviceSize requiredTriangleBytes = static_cast<VkDeviceSize>(
			visibilityTriangleCount * 320u);
		if(resources.visibilityTriangleBuffer.buffer == VK_NULL_HANDLE
			|| resources.visibilityTriangleCapacity < visibilityTriangleCount
			|| resources.visibilityTriangleBuffer.size < requiredTriangleBytes)
		{
			RetireBuffer(resources.visibilityTriangleBuffer);
			resources.visibilityTriangleBuffer = CreateBuffer(
				requiredTriangleBytes,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				VMA_MEMORY_USAGE_GPU_ONLY,
				"Virtual Geometry GPU-Native Raster Triangle Buffer");
			resources.visibilityTriangleCapacity = visibilityTriangleCount;
		}
	}
	resources.visibilityInputReuseSignature = visibilityInputSignature;
	resources.visibilityInputTriangleCount = visibilityTriangleCount;
	resources.visibilityInputMeshletCount = visibilityMeshletCount;
	resources.visibilityInputCacheValid =
		presentation.virtualGeometryVisibilitySetupSignature != 0u
		&& resources.visibilitySetupPacketBuffer.buffer != VK_NULL_HANDLE
		&& resources.visibilitySetupMeshletBuffer.buffer != VK_NULL_HANDLE
		&& resources.visibilityTriangleBuffer.buffer != VK_NULL_HANDLE;
	}

	// Visibility records are pixel-addressed, not primitive-addressed. Preserve
	// the view's real aspect ratio and fail closed instead of allocating an
	// unbounded target supplied by malformed automation input.
	glm::uvec2 visibilityTargetSize(0u);
	for(const GpuVirtualGeometryPacketFrameSet& frameSet :
		gpuFrameSets)
	{
		if(frameSet.viewType == viewType && frameSet.viewId == viewId)
		{
			visibilityTargetSize = frameSet.targetSize;
			break;
		}
	}
	const std::uint64_t requestedPixelCount =
		static_cast<std::uint64_t>(visibilityTargetSize.x)
		* static_cast<std::uint64_t>(visibilityTargetSize.y);
	const std::size_t visibilityPixelCount =
		requestedPixelCount != 0u
			&& requestedPixelCount <= kMaximumVisibilityPixelCount
		? static_cast<std::size_t>(requestedPixelCount)
		: 0u;
	if(visibilityPixelCount == 0u)
	{
		virtualGeometryVisibilityUnavailableReason =
			"Virtual-geometry visibility target is empty or exceeds the bounded pixel capacity.";
	}
	constexpr std::size_t kVisibilityMaterialBinCount = 64u;
	constexpr VkBufferUsageFlags kVisibilityScratchUsage =
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
		| VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	// These are GPU-written scratch buffers. Allocation remains a protected CPU
	// operation, but initialization is recorded as four transfer fills immediately
	// before compute raster instead of copying viewport-sized host arrays.
	ensureBuffer(resources.visibilityDepthBuffer,
		resources.visibilityDepthCapacity, visibilityPixelCount,
		sizeof(std::uint32_t), "Virtual Geometry Visibility Depth Buffer",
		kVisibilityScratchUsage);
	ensureBuffer(resources.visibilityRecordBuffer,
		resources.visibilityRecordCapacity, visibilityPixelCount * 4u,
		sizeof(std::uint32_t), "Virtual Geometry Visibility Record Buffer",
		kVisibilityScratchUsage);
	ensureBuffer(resources.resolvedVisibilityBuffer,
		resources.resolvedVisibilityCapacity, visibilityPixelCount * 4u,
		sizeof(std::uint32_t), "Virtual Geometry Resolved Visibility Buffer",
		kVisibilityScratchUsage);
	ensureBuffer(resources.shadingBinBuffer,
		resources.shadingBinCapacity, kVisibilityMaterialBinCount,
		sizeof(std::uint32_t), "Virtual Geometry Shading Bin Buffer",
		kVisibilityScratchUsage);

	TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityResources visibilityResources;
	visibilityResources.inputTriangleBuffer =
		resources.visibilityTriangleBuffer.buffer;
	visibilityResources.setupPacketBuffer = resources.visibilitySetupPacketBuffer.buffer;
	visibilityResources.setupMeshletBuffer = resources.visibilitySetupMeshletBuffer.buffer;
	visibilityResources.pagePayloadBuffer = resources.pagePayloadBuffer.buffer;
	visibilityResources.depthBuffer = resources.visibilityDepthBuffer.buffer;
	visibilityResources.visibilityRecordBuffer = resources.visibilityRecordBuffer.buffer;
	visibilityResources.resolvedVisibilityBuffer = resources.resolvedVisibilityBuffer.buffer;
	visibilityResources.shadingBinBuffer = resources.shadingBinBuffer.buffer;
	visibilityResources.diagnosticBuffer = resources.counterReadbackBuffer.buffer;
	visibilityResources.bindlessDescriptorSet = currentFrame < globalDescriptorSets.size()
		? globalDescriptorSets[currentFrame] : VK_NULL_HANDLE;
	visibilityResources.resourcesOwnedByFrame = resources.live;
	if(visibilityResources.inputTriangleBuffer == VK_NULL_HANDLE
		|| visibilityResources.setupPacketBuffer == VK_NULL_HANDLE
		|| visibilityResources.setupMeshletBuffer == VK_NULL_HANDLE
		|| visibilityResources.pagePayloadBuffer == VK_NULL_HANDLE
		|| visibilityResources.bindlessDescriptorSet == VK_NULL_HANDLE
		|| visibilityResources.depthBuffer == VK_NULL_HANDLE
		|| visibilityResources.visibilityRecordBuffer == VK_NULL_HANDLE
		|| visibilityResources.resolvedVisibilityBuffer == VK_NULL_HANDLE
		|| visibilityResources.shadingBinBuffer == VK_NULL_HANDLE
		|| visibilityResources.diagnosticBuffer == VK_NULL_HANDLE) {
		virtualGeometryVisibilityUnavailableReason = "Virtual-geometry visibility frame storage buffers are unavailable for this view.";
	}
	else if(EnsureVulkanVirtualGeometryVisibilityPipelines()) {
		if(resources.visibilityTriangleSetupDescriptorSet == VK_NULL_HANDLE
			|| resources.visibilityComputeRasterDescriptorSet == VK_NULL_HANDLE
			|| resources.visibilityResolveDescriptorSet == VK_NULL_HANDLE
			|| resources.visibilityShadingBinsDescriptorSet == VK_NULL_HANDLE) {
			const std::array<VkDescriptorSetLayout, 4> layouts{
				virtualGeometryVisibilityDescriptorSetLayout,
				virtualGeometryVisibilityDescriptorSetLayout,
				virtualGeometryVisibilityDescriptorSetLayout,
				virtualGeometryVisibilityDescriptorSetLayout };
			std::array<VkDescriptorSet, 4> descriptorSets{};
			VkDescriptorSetAllocateInfo allocationInfo{};
			allocationInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocationInfo.descriptorPool = descriptorPool;
			allocationInfo.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
			allocationInfo.pSetLayouts = layouts.data();
			if(vkAllocateDescriptorSets(device, &allocationInfo, descriptorSets.data()) != VK_SUCCESS) {
				virtualGeometryVisibilityUnavailableReason = "Could not allocate virtual-geometry visibility descriptor sets.";
			}
			else {
				resources.visibilityTriangleSetupDescriptorSet = descriptorSets[0];
				resources.visibilityComputeRasterDescriptorSet = descriptorSets[1];
				resources.visibilityResolveDescriptorSet = descriptorSets[2];
				resources.visibilityShadingBinsDescriptorSet = descriptorSets[3];
			}
		}
		if(resources.visibilityTriangleSetupDescriptorSet != VK_NULL_HANDLE
			&& resources.visibilityComputeRasterDescriptorSet != VK_NULL_HANDLE
			&& resources.visibilityResolveDescriptorSet != VK_NULL_HANDLE
			&& resources.visibilityShadingBinsDescriptorSet != VK_NULL_HANDLE) {
			const VkDescriptorBufferInfo diagnosticInfo{
				resources.counterReadbackBuffer.buffer, 0u,
				resources.counterReadbackBuffer.size };
			const std::array<std::array<VkDescriptorBufferInfo, 6>, 4> bufferInfos{{
				{{ { resources.visibilitySetupPacketBuffer.buffer, 0u, resources.visibilitySetupPacketBuffer.size }, { resources.visibilitySetupMeshletBuffer.buffer, 0u, resources.visibilitySetupMeshletBuffer.size }, { resources.pagePayloadBuffer.buffer, 0u, resources.pagePayloadBuffer.size }, { resources.visibilityTriangleBuffer.buffer, 0u, resources.visibilityTriangleBuffer.size }, diagnosticInfo, diagnosticInfo }},
				{{ { resources.visibilityTriangleBuffer.buffer, 0u, resources.visibilityTriangleBuffer.size }, { resources.visibilityDepthBuffer.buffer, 0u, resources.visibilityDepthBuffer.size }, { resources.visibilityRecordBuffer.buffer, 0u, resources.visibilityRecordBuffer.size }, diagnosticInfo, diagnosticInfo, diagnosticInfo }},
				{{ { resources.visibilityRecordBuffer.buffer, 0u, resources.visibilityRecordBuffer.size }, { resources.resolvedVisibilityBuffer.buffer, 0u, resources.resolvedVisibilityBuffer.size }, diagnosticInfo, { resources.visibilityTriangleBuffer.buffer, 0u, resources.visibilityTriangleBuffer.size }, diagnosticInfo, diagnosticInfo }},
				{{ { resources.resolvedVisibilityBuffer.buffer, 0u, resources.resolvedVisibilityBuffer.size }, { resources.shadingBinBuffer.buffer, 0u, resources.shadingBinBuffer.size }, diagnosticInfo, diagnosticInfo, diagnosticInfo, diagnosticInfo }} }};
			const std::array<VkDescriptorSet, 4> descriptorSets{
				resources.visibilityTriangleSetupDescriptorSet,
				resources.visibilityComputeRasterDescriptorSet,
				resources.visibilityResolveDescriptorSet,
				resources.visibilityShadingBinsDescriptorSet };
			std::array<VirtualGeometryGpuPacketFrameResources::BindlessBufferPublication, 24u>
				currentPublications{};
			for(std::size_t passIndex = 0u; passIndex < descriptorSets.size(); ++passIndex) {
				for(std::size_t binding = 0u;
					binding < bufferInfos[passIndex].size(); ++binding) {
					const std::size_t publicationIndex =
						passIndex * bufferInfos[passIndex].size() + binding;
					currentPublications[publicationIndex] = {
						bufferInfos[passIndex][binding].buffer,
						bufferInfos[passIndex][binding].range };
				}
			}
			if(currentPublications != resources.visibilityDescriptorPublications) {
				std::array<VkWriteDescriptorSet, 24> writes{};
				for(std::uint32_t passIndex = 0u; passIndex < descriptorSets.size(); ++passIndex) {
					for(std::uint32_t binding = 0u; binding < bufferInfos[passIndex].size(); ++binding) {
						VkWriteDescriptorSet& write = writes[passIndex * bufferInfos[passIndex].size() + binding];
						write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
						write.dstSet = descriptorSets[passIndex];
						write.dstBinding = binding;
						write.descriptorCount = 1u;
						write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
						write.pBufferInfo = &bufferInfos[passIndex][binding];
					}
				}
				vkUpdateDescriptorSets(device,
					static_cast<std::uint32_t>(writes.size()), writes.data(),
					0u, nullptr);
				resources.visibilityDescriptorPublications = currentPublications;
			}
			visibilityResources.triangleSetupDescriptorSet = resources.visibilityTriangleSetupDescriptorSet;
			visibilityResources.computeRasterDescriptorSet = resources.visibilityComputeRasterDescriptorSet;
			visibilityResources.visibilityResolveDescriptorSet = resources.visibilityResolveDescriptorSet;
			visibilityResources.shadingBinsDescriptorSet = resources.visibilityShadingBinsDescriptorSet;
		}
	}
	const std::uint32_t boundedVisibilityCapacity =
		static_cast<std::uint32_t>(std::min<std::size_t>(
			resources.visibilityTriangleCapacity,
			std::numeric_limits<std::uint32_t>::max()));
	const glm::uvec2 boundedVisibilityTargetSize =
		visibilityPixelCount != 0u
			? visibilityTargetSize
			: glm::uvec2(0u);
	latestVirtualGeometryVisibilityPlan = TerraEngine::Renderer::Vulkan::BuildVulkanVirtualGeometryVisibilityPlan(
		static_cast<std::uint32_t>(std::min<std::size_t>(
			visibilityTriangleCount,
			std::numeric_limits<std::uint32_t>::max())),
		boundedVisibilityCapacity,
		static_cast<std::uint32_t>(std::min<std::size_t>(
			visibilityMeshletCount,
			std::numeric_limits<std::uint32_t>::max())),
		boundedVisibilityTargetSize.x,
		boundedVisibilityTargetSize.y,
		static_cast<std::uint32_t>(kVisibilityMaterialBinCount),
		visibilityResources,
		virtualGeometryVisibilityPipelines,
		frameId,
		executorGeneration);
	latestVirtualGeometryVisibilityResources = visibilityResources;
	if(!virtualGeometryVisibilityUnavailableReason.empty()
		&& latestVirtualGeometryVisibilityPlan.unavailableReason.empty()) {
		latestVirtualGeometryVisibilityPlan.unavailableReason = virtualGeometryVisibilityUnavailableReason;
	}

	// Ordinary hardware commands remain the diagnosed fail-open fallback. When all
	// executor stages record successfully, the main mesh-task pass consumes the
	// GPU-authored 16-byte indirect records from this frame-owned buffer.
}
