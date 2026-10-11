// This collector records into the caller's primary. Only existing submit/fence
// hooks advance ownership; it never submits, waits, or reads unfinished staging.
#include "VulkanLightingDepthEvidence.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace TerraEngine::Renderer
{
	namespace
	{
		namespace Timing = VulkanLightingDepthTiming;
		constexpr std::uint64_t kBoundsBytes = 144 * 8;
		constexpr std::size_t kSampleCount = Timing::kFrameCount * Timing::kSnapshotCount;
		constexpr VkQueryResultFlags kReadFlags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
		struct Buffer { VkBuffer handle = VK_NULL_HANDLE; VmaAllocation allocation = VK_NULL_HANDLE; std::uint64_t bytes = 0; };
		bool SameCapture(const VulkanLightingDepthEvidence::Request& a, const VulkanLightingDepthEvidence::Request& b)
		{
			const auto& x = a.evidence; const auto& y = b.evidence;
			return a.frameSlot == b.frameSlot && a.snapshotSlot == b.snapshotSlot && a.width == b.width && a.height == b.height
				&& a.cooperative == b.cooperative && a.validation == b.validation && a.oracleRecipe == b.oracleRecipe
				&& x.frameSerial == y.frameSerial && x.commandSerial == y.commandSerial && x.viewType == y.viewType && x.viewId == y.viewId
				&& x.fixtureControlGeneration == y.fixtureControlGeneration && x.fixtureCorrelation == y.fixtureCorrelation
				&& x.effectiveOptions.rendererSessionGeneration == y.effectiveOptions.rendererSessionGeneration
				&& x.effectiveOptions.settingsGeneration == y.effectiveOptions.settingsGeneration
				&& x.effectiveOptions.optionsSessionGeneration == y.effectiveOptions.optionsSessionGeneration && x.effectiveOptions.options == y.effectiveOptions.options;
		}
		void BufferBarrier(VkCommandBuffer cmd, const Buffer& buffer, VkAccessFlags from, VkAccessFlags to, VkPipelineStageFlags before, VkPipelineStageFlags after)
		{
			VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
			barrier.srcAccessMask = from; barrier.dstAccessMask = to; barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.buffer = buffer.handle; barrier.size = buffer.bytes;
			vkCmdPipelineBarrier(cmd, before, after, 0, 0, nullptr, 1, &barrier, 0, nullptr);
		}
	}
	struct VulkanLightingDepthEvidence::State
	{
		struct Sample
		{
			bool occupied = false, ended = false, boundsRecorded = false, oracleAttempted = false, oracleRecorded = false, canceled = false, failed = false, payloadRead = false;
			VkCommandBuffer command = VK_NULL_HANDLE;
			Request request;
			Timing::Owner productionOwner{}, oracleOwner{};
			Timing::QueryRange productionRange{}, oracleRange{};
			std::uint64_t submit = 0, completion = 0;
			Buffer production, upload, culling, bounds, coverage, oracleReadback;
			VkImage image = VK_NULL_HANDLE; VmaAllocation imageAllocation = VK_NULL_HANDLE;
			VkImageView imageView = VK_NULL_HANDLE; VkDescriptorSet descriptor = VK_NULL_HANDLE;
			std::shared_ptr<RendererEfficiencyLightingDepthArtifacts> payload;
			bool productionResolved = false, oracleResolved = false;
			std::shared_ptr<const RendererEfficiencyFrameColor> color;
		};
		Config config{}; // SPIRV spans are cleared immediately after initialization.
		VkQueryPool queryPool = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline cooperativePipeline = VK_NULL_HANDLE, referencePipeline = VK_NULL_HANDLE;
		std::uint64_t poolGeneration = 0;
		bool initialized = false, accepting = false;
		Timing::TimingState timing;
		std::array<Sample, kSampleCount> samples{};
		std::vector<LightingDepthSampleIntent> sampledIntents;
		std::vector<RendererEfficiencyEvidence> pendingColor, completed;
		bool CreateBuffer(Buffer& out, std::uint64_t bytes, VkBufferUsageFlags usage, VmaMemoryUsage memory)
		{
			VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; create.size = bytes; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{}; allocation.usage = memory;
			if (vmaCreateBuffer(config.allocator, &create, &allocation, &out.handle, &out.allocation, nullptr) != VK_SUCCESS) return false;
			out.bytes = bytes; return true;
		}
		void DestroyBuffer(Buffer& buffer)
		{
			if (buffer.handle) vmaDestroyBuffer(config.allocator, buffer.handle, buffer.allocation);
			buffer = {};
		}
		void Destroy(Sample& sample)
		{
			// Called only before recording, after explicit discard, or after a real
			// fence/device teardown proof. False query availability is not that proof.
			if (sample.descriptor) vkFreeDescriptorSets(config.device, descriptorPool, 1, &sample.descriptor);
			if (sample.imageView) vkDestroyImageView(config.device, sample.imageView, nullptr);
			if (sample.image) vmaDestroyImage(config.allocator, sample.image, sample.imageAllocation);
			DestroyBuffer(sample.production); DestroyBuffer(sample.upload); DestroyBuffer(sample.culling);
			DestroyBuffer(sample.bounds); DestroyBuffer(sample.coverage); DestroyBuffer(sample.oracleReadback);
			sample = {};
		}
		bool Write(const Buffer& buffer, const void* data, std::uint64_t bytes)
		{
			void* mapped = nullptr;
			if (bytes > buffer.bytes || vmaMapMemory(config.allocator, buffer.allocation, &mapped) != VK_SUCCESS || !mapped) return false;
			std::memcpy(mapped, data, static_cast<std::size_t>(bytes));
			const auto result = vmaFlushAllocation(config.allocator, buffer.allocation, 0, bytes);
			vmaUnmapMemory(config.allocator, buffer.allocation); return result == VK_SUCCESS;
		}
		bool Read(const Buffer& buffer, std::vector<std::uint8_t>& bytes)
		{
			void* mapped = nullptr;
			if (!buffer.handle || vmaMapMemory(config.allocator, buffer.allocation, &mapped) != VK_SUCCESS || !mapped) return false;
			bool success = false;
			try
			{
				if (vmaInvalidateAllocation(config.allocator, buffer.allocation, 0, buffer.bytes) == VK_SUCCESS)
				{ bytes.resize(static_cast<std::size_t>(buffer.bytes)); std::memcpy(bytes.data(), mapped, bytes.size()); success = true; }
			}
			catch (...) { vmaUnmapMemory(config.allocator, buffer.allocation); throw; }
			vmaUnmapMemory(config.allocator, buffer.allocation); return success;
		}
		Timing::Owner Owner(const Request& request, Timing::Purpose purpose) const
		{
			return {config.rendererSessionGeneration, config.backendResourceGeneration, poolGeneration, request.evidence.frameSerial,
				request.evidence.commandSerial, request.frameSlot, request.snapshotSlot, request.evidence.viewId, purpose,
				request.evidence.viewType == RenderViewType::Scene ? Timing::View::Scene : Timing::View::Game, Timing::Scope::IsolatedDepth};
		}
		Sample* Find(VkCommandBuffer command, const Request& request)
		{
			if (request.frameSlot >= Timing::kFrameCount || request.snapshotSlot >= Timing::kSnapshotCount) return nullptr;
			auto& sample = samples[request.frameSlot * Timing::kSnapshotCount + request.snapshotSlot];
			return sample.occupied && !sample.submit && !sample.completion && sample.command == command && SameCapture(sample.request, request) ? &sample : nullptr;
		}
		bool RequestValid(const Request& request) const
		{
			const auto& row = request.evidence;
			if (!accepting || !request.width || !request.height || request.frameSlot >= Timing::kFrameCount || request.snapshotSlot >= Timing::kSnapshotCount
				|| !row.frameSerial || !row.commandSerial || row.submitSerial || row.completionSerial
				|| row.effectiveOptions.rendererSessionGeneration != config.rendererSessionGeneration
				|| (row.viewType != RenderViewType::Game && row.viewType != RenderViewType::Scene)) return false;
			if (!request.validation) return !request.oracleRecipe;
			const auto& c = row.fixtureCorrelation;
			return row.kind == RendererEfficiencyEvidenceKind::LightingDepth && row.fixtureControlGeneration && row.effectiveOptions.optionsSessionGeneration
				&& !c.operationId.empty() && !c.fixtureStepId.empty() && c.mutationGeneration && c.settingsGeneration
				&& c.settingsGeneration == row.effectiveOptions.settingsGeneration && c.rendererSessionGeneration == config.rendererSessionGeneration;
		}
		bool Nonempty(const std::vector<std::uint8_t>& bytes, bool& valid) const
		{
			valid = bytes.size() == kBoundsBytes; bool nonempty = false;
			if (!valid) return false;
			for (std::size_t i = 0; i < 144; ++i)
			{
				float values[2]; std::memcpy(values, bytes.data() + i * 8, 8);
				if (!std::isfinite(values[0]) || !std::isfinite(values[1]) || values[0] < 0 || values[1] > 1 || values[1] < values[0]) { valid = false; return false; }
				if (values[0] != 1 || values[1] != 1) nonempty = true;
			}
			return nonempty;
		}
		bool ResolveQuery(const Timing::Owner& owner, RendererEfficiencyDepthQueryEvidence& result, bool nonempty, bool abandon)
		{
			struct QueryValue { std::uint64_t ticks, available; } values[2]{};
			const auto native = vkGetQueryPoolResults(config.device, queryPool, result.range.firstQuery, 2, sizeof(values), values, sizeof(QueryValue), kReadFlags);
			result.queryReadFlags = static_cast<std::uint32_t>(kReadFlags); result.noWaitRead = true;
			const Timing::QueryRead read{values[0].ticks, values[1].ticks, values[0].available != 0, values[1].available != 0, native == VK_SUCCESS || native == VK_NOT_READY};
			if (!timing.Resolve(owner, read, nonempty, result.result)) return false;
			if (result.result.status == Timing::Status::Pending && abandon) timing.AbandonCompleted(owner, result.result);
			return result.result.status != Timing::Status::Pending;
		}
		void Finish(Sample& sample, bool abandon)
		{
			if (!sample.occupied || !sample.completion) return;
			if (sample.canceled)
			{
				// A real completion lets a retired owner release without mapping or
				// querying canceled diagnostics. Reset alone never reaches this path.
				Timing::Result ignored;
				if (!sample.productionResolved) timing.Resolve(sample.productionOwner, {}, false, ignored);
				if (sample.oracleRecorded && !sample.oracleResolved) timing.Resolve(sample.oracleOwner, {}, false, ignored);
				Destroy(sample); return;
			}
			if (!sample.payloadRead)
			{
				if (sample.request.validation)
				{
					if (!sample.boundsRecorded || !Read(sample.production, sample.payload->productionBounds)) sample.failed = true;
					if (sample.oracleRecorded)
					{
						std::vector<std::uint8_t> combined;
						if (!Read(sample.oracleReadback, combined)) sample.failed = true;
						else
						{
							const auto inputBytes = static_cast<std::size_t>(sample.upload.bytes);
							sample.payload->oracleInput.assign(combined.begin(), combined.begin() + inputBytes);
							sample.payload->oracleBounds.assign(combined.begin() + inputBytes, combined.begin() + inputBytes + kBoundsBytes);
							sample.payload->oracleCoverage.assign(combined.begin() + inputBytes + kBoundsBytes, combined.end());
						}
					}
					else if (sample.request.oracleRecipe) sample.failed = true;
				}
				sample.payloadRead = true;
			}
			bool valid = true; const bool nonempty = sample.request.validation ? Nonempty(sample.payload->productionBounds, valid) : false;
			if (!valid) sample.failed = true;
			bool oracleValid = true;
			const bool oracleNonempty = sample.oracleRecorded ? Nonempty(sample.payload->oracleBounds, oracleValid) : false;
			if (!oracleValid) sample.failed = true;
			if (!sample.productionResolved) sample.productionResolved = ResolveQuery(sample.productionOwner, sample.payload->productionQuery, nonempty, abandon);
			if (sample.oracleRecorded && !sample.oracleResolved) sample.oracleResolved = ResolveQuery(sample.oracleOwner, *sample.payload->oracleQuery, oracleNonempty, abandon);
			if (!sample.productionResolved || (sample.oracleRecorded && !sample.oracleResolved)) return;
			if (!sample.canceled)
			{
				auto row = sample.request.evidence; row.submitSerial = sample.submit; row.completionSerial = sample.completion;
				row.kind = RendererEfficiencyEvidenceKind::LightingDepth;
				const bool success = !sample.failed && sample.payload->productionQuery.result.status == Timing::Status::Valid
					&& (!sample.oracleRecorded || sample.payload->oracleQuery->result.status == Timing::Status::Valid);
				row.status = success ? RendererEfficiencyEvidenceStatus::Valid : RendererEfficiencyEvidenceStatus::Unavailable;
				row.unavailableReason = success ? "" : "Depth markers, query availability or actual GPU payload readback are unavailable.";
				row.lightingDepthTiming.status = row.status;
				if (success) { row.lightingDepthTiming.durationMilliseconds = sample.payload->productionQuery.result.durationNanoseconds / 1e6; row.lightingDepthTiming.unavailableReason.clear(); }
				auto artifacts = std::make_shared<RendererEfficiencyArtifacts>(); artifacts->lightingDepth = sample.payload; artifacts->frameColor = sample.color; row.artifacts = std::move(artifacts);
				if (sample.request.validation && success && !sample.color) pendingColor.push_back(std::move(row));
				else completed.push_back(std::move(row));
			}
			Destroy(sample);
		}
	};

	VulkanLightingDepthEvidence::VulkanLightingDepthEvidence() : state(std::make_unique<State>()) {}
	VulkanLightingDepthEvidence::~VulkanLightingDepthEvidence() { Shutdown(false); }
	std::uint64_t VulkanLightingDepthEvidence::QueryPoolGeneration() const { return state->poolGeneration; }
	bool VulkanLightingDepthEvidence::Initialize(const Config& config)
	{
		if (state->initialized || !config.device || !config.physical || !config.allocator || !config.rendererSessionGeneration || !config.backendResourceGeneration
			|| !config.timestampValidBits || config.timestampValidBits > 64 || !std::isfinite(config.periodNanoseconds) || config.periodNanoseconds <= 0
			|| config.cooperativeOracleSpirv.empty() || config.referenceOracleSpirv.empty()) return false;
		state->config = config; state->config.cooperativeOracleSpirv = {}; state->config.referenceOracleSpirv = {};
		// Pool contents are reset only in the owning command after range admission.
		VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; queries.queryType = VK_QUERY_TYPE_TIMESTAMP; queries.queryCount = Timing::kQueryCount;
		if (vkCreateQueryPool(config.device, &queries, nullptr, &state->queryPool) != VK_SUCCESS) { Shutdown(true); return false; }
		std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
		for (std::uint32_t i = 0; i < 4; ++i) { bindings[i].binding = i; bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
		VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; layout.bindingCount = 4; layout.pBindings = bindings.data();
		if (vkCreateDescriptorSetLayout(config.device, &layout, nullptr, &state->descriptorLayout) != VK_SUCCESS) { Shutdown(true); return false; }
		const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 24}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 72}};
		VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; pool.maxSets = 24; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
		if (vkCreateDescriptorPool(config.device, &pool, nullptr, &state->descriptorPool) != VK_SUCCESS) { Shutdown(true); return false; }
		VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
		VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pipelineLayout.setLayoutCount = 1; pipelineLayout.pSetLayouts = &state->descriptorLayout; pipelineLayout.pushConstantRangeCount = 1; pipelineLayout.pPushConstantRanges = &push;
		if (vkCreatePipelineLayout(config.device, &pipelineLayout, nullptr, &state->pipelineLayout) != VK_SUCCESS) { Shutdown(true); return false; }
		auto createPipeline = [&](std::span<const std::uint32_t> words, VkPipeline& pipeline)
		{
			VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; moduleInfo.codeSize = words.size_bytes(); moduleInfo.pCode = words.data(); VkShaderModule module = VK_NULL_HANDLE;
			if (vkCreateShaderModule(config.device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return false;
			VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; info.layout = state->pipelineLayout;
			info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = module; info.stage.pName = "main";
			const auto result = vkCreateComputePipelines(config.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline); vkDestroyShaderModule(config.device, module, nullptr); return result == VK_SUCCESS;
		};
		if (!createPipeline(config.cooperativeOracleSpirv, state->cooperativePipeline) || !createPipeline(config.referenceOracleSpirv, state->referencePipeline)) { Shutdown(true); return false; }
		++state->poolGeneration;
		if (!state->timing.Configure(config.rendererSessionGeneration, config.backendResourceGeneration, state->poolGeneration, config.timestampValidBits, config.periodNanoseconds)) { Shutdown(true); return false; }
		try { state->pendingColor.reserve(48); state->completed.reserve(48); state->sampledIntents.reserve(16); }
		catch (...) { Shutdown(true); return false; }
		state->initialized = state->accepting = true; return true;
	}
	bool VulkanLightingDepthEvidence::BeginProduction(VkCommandBuffer command, const Request& request)
	{
		if (!command || !state->RequestValid(request)) return false;
		auto intent = std::find_if(state->sampledIntents.begin(), state->sampledIntents.end(), [&](const auto& item) { return item.view == request.evidence.viewType && item.output == request.evidence.viewId; });
		if (request.validation)
		{
			// Existing lighting/color collection samples the first actual frame of
			// this token. Later frames cannot substitute a different depth owner.
			if (intent != state->sampledIntents.end() && !LightingDepthSampleCanRecord(*intent, request.evidence)) return false;
			if (intent == state->sampledIntents.end() && state->sampledIntents.size() >= 16) return false;
		}
		auto& sample = state->samples[request.frameSlot * Timing::kSnapshotCount + request.snapshotSlot];
		// A late unavailable query may be abandoned only after its actual fence.
		if (sample.occupied && sample.completion) state->Finish(sample, true);
		if (sample.occupied) return false;
		const auto occupied = std::count_if(state->samples.begin(), state->samples.end(), [](const auto& item) { return item.occupied; });
		if (state->pendingColor.size() + state->completed.size() + static_cast<std::size_t>(occupied) >= 48) return false;
		std::optional<LightingDepthSampleIntent> replacement;
		if (request.validation) replacement.emplace(LightingDepthSampleIntent{request.evidence.viewType, request.evidence.viewId,
			request.evidence.fixtureCorrelation, request.evidence.fixtureControlGeneration, request.evidence.frameSerial});
		sample.request = request; sample.command = command; sample.productionOwner = state->Owner(request, Timing::Purpose::Production);
		sample.payload = std::make_shared<RendererEfficiencyLightingDepthArtifacts>(); sample.payload->productionWidth = request.width; sample.payload->productionHeight = request.height; sample.payload->oracleRecipe = request.oracleRecipe;
		if (request.validation && !state->CreateBuffer(sample.production, kBoundsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)) { state->Destroy(sample); return false; }
		if (!state->timing.Begin(sample.productionOwner, sample.productionRange)) { state->Destroy(sample); return false; }
		if (request.validation)
		{
			// Capacity and allocating string copies were prepared before reservation.
			if (intent == state->sampledIntents.end()) state->sampledIntents.push_back(std::move(*replacement)); else *intent = std::move(*replacement);
		}
		sample.occupied = true; auto& query = sample.payload->productionQuery; query.range = sample.productionRange; query.cooperative = request.cooperative; query.computeStageMarkers = true; query.beginStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		vkCmdResetQueryPool(command, state->queryPool, sample.productionRange.firstQuery, 2);
		vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, state->queryPool, sample.productionRange.firstQuery); return true;
	}
	bool VulkanLightingDepthEvidence::EndProduction(VkCommandBuffer command, const Request& request)
	{
		auto* sample = state->Find(command, request); if (!sample || sample->ended || !state->timing.End(sample->productionOwner)) return false;
		vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, state->queryPool, sample->productionRange.firstQuery + 1);
		sample->payload->productionQuery.endStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; sample->ended = true; return true;
	}
	bool VulkanLightingDepthEvidence::RecordProductionBounds(VkCommandBuffer command, const Request& request, VkBuffer source, std::uint64_t offset, std::uint64_t allocationBytes)
	{
		auto* sample = state->Find(command, request);
		if (!sample || !sample->ended || sample->boundsRecorded || !request.validation || !source || offset > allocationBytes || kBoundsBytes > allocationBytes - offset || offset % 4) return false;
		VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.buffer = source; barrier.offset = offset; barrier.size = kBoundsBytes;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
		VkBufferCopy copy{offset, 0, kBoundsBytes}; vkCmdCopyBuffer(command, source, sample->production.handle, 1, &copy);
		BufferBarrier(command, sample->production, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr); sample->boundsRecorded = true; return true;
	}
	bool VulkanLightingDepthEvidence::RecordOracle(VkCommandBuffer command, const Request& request, const LightingDepthReduction::DepthRecipe& recipe,
		std::span<const std::uint32_t> input, const FrameCullingDataGPU& culling)
	{
		auto* sample = state->Find(command, request);
		const auto pixels = static_cast<std::uint64_t>(recipe.extent.width) * recipe.extent.height;
		if (!sample || !sample->ended || sample->oracleAttempted || !request.validation || !request.oracleRecipe || *request.oracleRecipe != recipe
			|| !recipe.extent.width || !recipe.extent.height || recipe.extent.width > 1920 || recipe.extent.height > 1080 || input.size() != pixels) return false;
		// Build every owner and host upload before recording the first oracle GPU
		// operation. A later failure retains these owners with the real primary.
		sample->oracleAttempted = true;
		auto fail = [&]() { sample->failed = true; return false; };
		const auto imageBytes = pixels * sizeof(std::uint32_t);
		if (!state->CreateBuffer(sample->upload, imageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			|| !state->CreateBuffer(sample->culling, sizeof(FrameCullingDataGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			|| !state->CreateBuffer(sample->bounds, kBoundsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
			|| !state->CreateBuffer(sample->coverage, kBoundsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
			|| !state->CreateBuffer(sample->oracleReadback, imageBytes + 2 * kBoundsBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)) return fail();
		auto isolatedCulling = culling;
		isolatedCulling.clusterDepthAndViewport.z = static_cast<float>(recipe.extent.width);
		isolatedCulling.clusterDepthAndViewport.w = static_cast<float>(recipe.extent.height);
		if (!state->Write(sample->upload, input.data(), imageBytes) || !state->Write(sample->culling, &isolatedCulling, sizeof(isolatedCulling))) return fail();
		VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_R32_SFLOAT;
		image.extent = {recipe.extent.width, recipe.extent.height, 1}; image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		image.sharingMode = VK_SHARING_MODE_EXCLUSIVE; image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		if (vmaCreateImage(state->config.allocator, &image, &allocation, &sample->image, &sample->imageAllocation, nullptr) != VK_SUCCESS) return fail();
		VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = sample->image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
		view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (vkCreateImageView(state->config.device, &view, nullptr, &sample->imageView) != VK_SUCCESS) return fail();
		VkDescriptorSetAllocateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; descriptor.descriptorPool = state->descriptorPool; descriptor.descriptorSetCount = 1; descriptor.pSetLayouts = &state->descriptorLayout;
		if (vkAllocateDescriptorSets(state->config.device, &descriptor, &sample->descriptor) != VK_SUCCESS) return fail();
		VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, sample->imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		const VkDescriptorBufferInfo buffers[]{{sample->bounds.handle, 0, kBoundsBytes}, {sample->culling.handle, 0, sizeof(FrameCullingDataGPU)}, {sample->coverage.handle, 0, kBoundsBytes}};
		std::array<VkWriteDescriptorSet, 4> writes{};
		for (std::uint32_t i = 0; i < 4; ++i)
		{
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = sample->descriptor; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
			writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			if (i == 0) writes[i].pImageInfo = &imageInfo; else writes[i].pBufferInfo = &buffers[i - 1];
		}
		vkUpdateDescriptorSets(state->config.device, 4, writes.data(), 0, nullptr);
		sample->oracleOwner = state->Owner(request, Timing::Purpose::Oracle);
		if (!state->timing.Begin(sample->oracleOwner, sample->oracleRange)) return fail();
		sample->oracleRecorded = true; sample->payload->oracleQuery.emplace(); auto& query = *sample->payload->oracleQuery;
		query.range = sample->oracleRange; query.cooperative = request.cooperative; query.computeStageMarkers = true;
		query.beginStageMask = query.endStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = sample->image; barrier.subresourceRange = view.subresourceRange;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy imageCopy{}; imageCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; imageCopy.imageExtent = image.extent;
		vkCmdCopyBufferToImage(command, sample->upload.handle, sample->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopy);
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		BufferBarrier(command, sample->culling, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, request.cooperative ? state->cooperativePipeline : state->referencePipeline);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, state->pipelineLayout, 0, 1, &sample->descriptor, 0, nullptr);
		const std::array<std::uint32_t, 16> push{}; vkCmdPushConstants(command, state->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, push.data());
		vkCmdResetQueryPool(command, state->queryPool, sample->oracleRange.firstQuery, 2);
		vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, state->queryPool, sample->oracleRange.firstQuery);
		vkCmdDispatch(command, request.cooperative ? 16U : 2U, request.cooperative ? 9U : 2U, 1);
		vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, state->queryPool, sample->oracleRange.firstQuery + 1);
		state->timing.End(sample->oracleOwner);
		// Transfers are deliberately outside the isolated oracle marker pair.
		BufferBarrier(command, sample->bounds, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		BufferBarrier(command, sample->coverage, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		vkCmdCopyImageToBuffer(command, sample->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sample->oracleReadback.handle, 1, &imageCopy);
		VkBufferCopy boundsCopy{0, imageBytes, kBoundsBytes}, coverageCopy{0, imageBytes + kBoundsBytes, kBoundsBytes};
		vkCmdCopyBuffer(command, sample->bounds.handle, sample->oracleReadback.handle, 1, &boundsCopy);
		vkCmdCopyBuffer(command, sample->coverage.handle, sample->oracleReadback.handle, 1, &coverageCopy);
		BufferBarrier(command, sample->oracleReadback, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier); return true;
	}
	bool VulkanLightingDepthEvidence::ConfirmSubmitted(std::span<const std::uint64_t> commands, std::uint64_t submit)
	{
		if (!state->timing.ConfirmSubmitted(commands, submit)) return false;
		for (auto& sample : state->samples) if (sample.occupied && std::find(commands.begin(), commands.end(), sample.productionOwner.commandSerial) != commands.end())
		{ sample.submit = submit; sample.request.evidence.submitSerial = submit; }
		return true;
	}
	void VulkanLightingDepthEvidence::PreserveSubmittedCommand(std::uint64_t command, std::uint64_t submit) noexcept
	{
		if (!command || !submit) return;
		// No allocation follows accepted submission. Even a later malformed batch
		// cannot turn a GPU-referenced owner into an unsubmitted disposable sample.
		for (auto& sample : state->samples) if (sample.occupied && sample.productionOwner.commandSerial == command && (!sample.submit || sample.submit == submit))
		{ sample.submit = submit; sample.request.evidence.submitSerial = submit; }
		state->timing.ConfirmSubmitted(std::span<const std::uint64_t>(&command, 1), submit);
	}
	void VulkanLightingDepthEvidence::CompleteSubmission(std::uint64_t submit, std::uint64_t completion)
	{
		if (!state->timing.CompleteSubmission(submit, completion)) return;
		for (auto& sample : state->samples) if (sample.occupied && sample.submit == submit && (!sample.completion || sample.completion == completion))
		{ sample.completion = completion; sample.request.evidence.completionSerial = completion; }
	}
	bool VulkanLightingDepthEvidence::AttachCompletedFrameColor(const RendererEfficiencyEvidence& color)
	{
		bool attached = false;
		for (auto& sample : state->samples) if (sample.occupied && !sample.canceled && sample.completion)
		{
			if (LightingDepthColorOwnerMatches(sample.request.evidence, color)) { sample.color = color.artifacts->frameColor; attached = true; }
		}
		for (auto& row : state->pendingColor)
		{
			if (!LightingDepthColorOwnerMatches(row, color)) continue;
			// This artifact has not been published: its unique collector-owned
			// allocation already exists. Assigning the shared color cannot allocate.
			std::const_pointer_cast<RendererEfficiencyArtifacts>(row.artifacts)->frameColor = color.artifacts->frameColor; attached = true;
		}
		return attached;
	}
	void VulkanLightingDepthEvidence::DiscardCommand(std::uint64_t command)
	{
		if (!state->timing.DiscardCommand(command)) return;
		for (auto& sample : state->samples) if (sample.occupied && !sample.submit && sample.productionOwner.commandSerial == command)
		{
			std::erase_if(state->sampledIntents, [&](auto& intent) { return ReleaseLightingDepthSampleOnDiscard(intent, sample.request.evidence, false); });
			state->Destroy(sample);
		}
	}
	void VulkanLightingDepthEvidence::Reset(std::uint64_t newSession, bool deviceLost)
	{
		state->accepting = false;
		if (deviceLost) { Shutdown(true); return; }
		state->timing.Reset(newSession, state->config.backendResourceGeneration, state->poolGeneration);
		for (auto& sample : state->samples) if (sample.occupied) sample.canceled = true;
		state->pendingColor.clear(); state->completed.clear();
		state->sampledIntents.clear();
	}
	bool VulkanLightingDepthEvidence::Shutdown(bool safe)
	{
		if (!safe && std::any_of(state->samples.begin(), state->samples.end(), [](const auto& sample) { return sample.occupied; })) return false;
		for (auto& sample : state->samples) state->Destroy(sample);
		if (state->config.device)
		{
			if (state->cooperativePipeline) vkDestroyPipeline(state->config.device, state->cooperativePipeline, nullptr);
			if (state->referencePipeline) vkDestroyPipeline(state->config.device, state->referencePipeline, nullptr);
			if (state->pipelineLayout) vkDestroyPipelineLayout(state->config.device, state->pipelineLayout, nullptr);
			if (state->descriptorPool) vkDestroyDescriptorPool(state->config.device, state->descriptorPool, nullptr);
			if (state->descriptorLayout) vkDestroyDescriptorSetLayout(state->config.device, state->descriptorLayout, nullptr);
			if (state->queryPool) vkDestroyQueryPool(state->config.device, state->queryPool, nullptr);
		}
		state->cooperativePipeline = state->referencePipeline = VK_NULL_HANDLE; state->pipelineLayout = VK_NULL_HANDLE;
		state->descriptorPool = VK_NULL_HANDLE; state->descriptorLayout = VK_NULL_HANDLE; state->queryPool = VK_NULL_HANDLE;
		state->config = {}; state->initialized = state->accepting = false; state->timing = {};
		state->sampledIntents.clear(); state->pendingColor.clear(); state->completed.clear(); return true;
	}
	std::vector<RendererEfficiencyEvidence> VulkanLightingDepthEvidence::DrainCompleted()
	{
		for (auto& sample : state->samples) state->Finish(sample, false);
		std::vector<RendererEfficiencyEvidence> result; result.reserve(state->completed.size() + state->pendingColor.size());
		for (auto& row : state->completed) result.push_back(std::move(row)); state->completed.clear();
		for (auto it = state->pendingColor.begin(); it != state->pendingColor.end();)
		{
			if (!it->artifacts->frameColor) { ++it; continue; }
			result.push_back(std::move(*it)); it = state->pendingColor.erase(it);
		}
		return result;
	}
}
