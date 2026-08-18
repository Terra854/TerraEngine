// VulkanGpuDiagnostics.cpp — GPU timestamps, completed statistics, and validation capture
//
// This file owns one focused Vulkan resource responsibility. Allocations and retirement
// still flow through VulkanContext::Impl so frame-fence lifetime rules remain unchanged.

#include "VulkanResourceSource.h"

// VulkanContext::Impl::CreateGpuTimestampQueries is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateGpuTimestampQueries() {
	gpuTimestampQueriesSupported = false;
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

// VulkanContext::Impl::DestroyGpuTimestampQueries is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyGpuTimestampQueries() {
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

// VulkanContext::Impl::MarkGpuTimestampBegin is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// VulkanContext::Impl::MarkGpuTimestampEnd is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// VulkanContext::Impl::ResolveCompletedGpuTimestamps is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// VulkanContext::Impl::RecordPendingStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingStats(uint32_t statsSlotIndex, const RenderStatsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingStatsBySlot[statsSlotIndex] = snapshot;
}

// VulkanContext::Impl::RecordPendingBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingBudgetDiagnostics(uint32_t statsSlotIndex, const FrameBudgetDiagnosticsSnapshot& snapshot) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingBudgetDiagnosticsBySlot[statsSlotIndex] = snapshot;
}

// VulkanContext::Impl::RecordPendingLightAnalysisCandidates is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingLightAnalysisCandidates(uint32_t statsSlotIndex, const std::vector<FrameLightCandidate>& candidates) {
	if (statsSlotIndex >= kStatsSlotCount) {
		return;
	}
	pendingLightAnalysisCandidatesBySlot[statsSlotIndex] = candidates;
}

// VulkanContext::Impl::PublishCompletedFrameStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::PublishCompletedFrameStats(uint32_t frameIndex) {
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assignment.reusedPreviousOwner)
			{
				assignmentEntry.message += " Reused previous assignment for stability.";
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex < lightAnalysisSSBOs.size() && lightAnalysisSSBOs[frameIndex].buffer != VK_NULL_HANDLE) {
		const VulkanBuffer& analysisBuffer = lightAnalysisSSBOs[frameIndex];
		void* mappedAnalysis = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vmaMapMemory(allocator, analysisBuffer.allocation, &mappedAnalysis) == VK_SUCCESS) {
			vmaInvalidateAllocation(allocator, analysisBuffer.allocation, 0, analysisBuffer.size);
			const auto* analysisRecords = static_cast<const GPULightAnalysisRecord*>(mappedAnalysis);
			const auto readSnapshot = [&](uint32_t statsSlotIndex) {
				GpuLightAnalysisViewSnapshot snapshot{};
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (statsSlotIndex >= pendingLightAnalysisCandidatesBySlot.size()) {
					return snapshot;
				}

				const std::vector<FrameLightCandidate>& candidates = pendingLightAnalysisCandidatesBySlot[statsSlotIndex];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (candidates.empty()) {
					snapshot.valid = true;
					return snapshot;
				}

				const std::size_t clampedCount = std::min<std::size_t>(candidates.size(), MAX_GLOBAL_LIGHTS);
				const std::size_t baseIndex = static_cast<std::size_t>(statsSlotIndex) * static_cast<std::size_t>(MAX_GLOBAL_LIGHTS);
				snapshot.recordsByLightId.reserve(clampedCount);
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (std::size_t lightIndex = 0; lightIndex < clampedCount; ++lightIndex) {
					const GPULightAnalysisRecord& record = analysisRecords[baseIndex + lightIndex];
					snapshot.recordsByLightId.emplace(candidates[lightIndex].lightId, record);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterSnapshot.valid) {
			previousClusterValidationSnapshot = clusterSnapshot;
			previousClusterValidationSnapshotValid = true;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterSnapshot.valid) {
		const FrameBudgetDiagnosticsSnapshot& captureDiagnostics = (clusterSnapshot.viewType == RenderViewType::Scene)
			? sceneBudgetDiagnostics
			: gameBudgetDiagnostics;
		WriteClusterValidationCapture(clusterSnapshot, captureDiagnostics);
	}
}

// VulkanContext::Impl::GetLatestStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
RenderStatsSnapshot VulkanContext::Impl::GetLatestStats(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestStatsByView[index];
}

// VulkanContext::Impl::GetLatestBudgetDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
FrameBudgetDiagnosticsSnapshot VulkanContext::Impl::GetLatestBudgetDiagnostics(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestBudgetDiagnosticsByView[index];
}

// VulkanContext::Impl::GetLatestLightingDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
LightingDiagnosticsSnapshot VulkanContext::Impl::GetLatestLightingDiagnostics(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestLightingDiagnosticsByView[index];
}

// VulkanContext::Impl::GetLatestGpuLightAnalysis is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VulkanContext::Impl::GpuLightAnalysisViewSnapshot VulkanContext::Impl::GetLatestGpuLightAnalysis(RenderViewType viewType) const {
	const uint32_t index = (viewType == RenderViewType::Scene) ? 1u : 0u;
	std::lock_guard<std::mutex> lock(statsMutex);
	return latestGpuLightAnalysisByView[index];
}

// VulkanContext::Impl::RecordPendingClusterValidationSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecordPendingClusterValidationSnapshot(uint32_t frameIndex, const ClusterValidationSnapshot& snapshot) {
	if (frameIndex >= pendingClusterValidationByFrame.size()) {
		return;
	}
	pendingClusterValidationByFrame[frameIndex] = snapshot;
}

// VulkanContext::Impl::ReadCompletedClusterValidationSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ClusterValidationSnapshot VulkanContext::Impl::ReadCompletedClusterValidationSnapshot(uint32_t frameIndex) const {
	if (!clusterValidationCaptureEnabled) {
		return {};
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex >= pendingClusterValidationByFrame.size()) {
		return {};
	}

	ClusterValidationSnapshot snapshot = pendingClusterValidationByFrame[frameIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!snapshot.valid || !snapshot.buildMetadata.clusteredLightingAvailable) {
		return snapshot;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (frameIndex >= clusterGridSSBOs.size() || frameIndex >= lightIndexSSBOs.size()) {
		snapshot.valid = false;
		return snapshot;
	}

	const VulkanBuffer& clusterBuffer = clusterGridSSBOs[frameIndex];
	const VulkanBuffer& lightIndexBuffer = lightIndexSSBOs[frameIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (clusterBuffer.buffer == VK_NULL_HANDLE || lightIndexBuffer.buffer == VK_NULL_HANDLE) {
		snapshot.valid = false;
		return snapshot;
	}

	const std::uint32_t clusterCount = snapshot.buildMetadata.clusterCount;
	snapshot.ranges.clear();
	snapshot.ranges.reserve(clusterCount);

	void* mappedClusters = nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (cluster.count == 0u) {
			emptyClusterCount += 1u;
		}
		maxClusterLightCount = std::max(maxClusterLightCount, cluster.count);
		actualAssignedLightCount += cluster.count;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (rangeEnd > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
			usedIndexCount = std::numeric_limits<std::uint32_t>::max();
		}
		else {
			usedIndexCount = std::max(usedIndexCount, static_cast<std::uint32_t>(rangeEnd));
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (usedIndexCount > 0u) {
		void* mappedLightIndices = nullptr;
		if (vmaMapMemory(allocator, lightIndexBuffer.allocation, &mappedLightIndices) == VK_SUCCESS) {
			const VkDeviceSize lightIndexReadOffset = static_cast<VkDeviceSize>(snapshot.buildMetadata.lightIndexOffset) * sizeof(std::uint32_t);
			const VkDeviceSize lightIndexReadSize = static_cast<VkDeviceSize>(sizeof(std::uint32_t) * usedIndexCount);
			vmaInvalidateAllocation(allocator, lightIndexBuffer.allocation, lightIndexReadOffset, lightIndexReadSize);

			const std::uint32_t* lightIndexData = static_cast<const std::uint32_t*>(mappedLightIndices) + snapshot.buildMetadata.lightIndexOffset;
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (previousClusterValidationSnapshotValid
		&& previousClusterValidationSnapshot.viewType == snapshot.viewType
		&& previousClusterValidationSnapshot.ranges.size() == snapshot.ranges.size()
		&& previousClusterValidationSnapshot.buildMetadata.activeLightCount == snapshot.buildMetadata.activeLightCount) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t rangeIndex = 0; rangeIndex < snapshot.ranges.size(); ++rangeIndex) {
			const ClusterLightRangeSnapshot& currentRange = snapshot.ranges[rangeIndex];
			const ClusterLightRangeSnapshot& previousRange = previousClusterValidationSnapshot.ranges[rangeIndex];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// VulkanContext::Impl::WriteClusterValidationCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::WriteClusterValidationCapture(const ClusterValidationSnapshot& snapshot, const FrameBudgetDiagnosticsSnapshot& diagnostics) const {
	if (!clusterValidationCaptureEnabled || !snapshot.valid) {
		return;
	}

	try {
		namespace fs = std::filesystem;
		const fs::path outputPath = clusterValidationCapturePath;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		capture["shadow_pass_recorded"] = diagnostics.shadowPassRecorded;
		capture["shadow_assignments"] = nlohmann::json::array();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

