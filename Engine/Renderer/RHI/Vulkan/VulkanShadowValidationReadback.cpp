// Full native depth/color readback is opt-in and never submits or waits here.
#include "VulkanShadowValidationReadback.h"
#include "VulkanReadbackConversion.h"
#include <cstring>
#include <cmath>
#include <bit>
#include <stdexcept>

namespace TerraEngine::Renderer
{
	struct VulkanShadowValidationReadback::State
	{
		struct Request
		{
			std::uint64_t id = 0, submit = 0;
			ShadowValidationFrameIdentity identity;
			ShadowValidationObservation observation;
			VkBuffer buffer = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_UNDEFINED;
			std::uint32_t width = 0, height = 0;
			std::uint64_t bytes = 0;
			std::uint32_t sourceCandidateCount = 0;
			bool color = false, lighting = false, canceled = false;
		};
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkPhysicalDevice physical = VK_NULL_HANDLE;
		std::uint64_t nextId = 1, session = 0;
		ShadowValidationReceiptTracker receipts;
		std::vector<Request> requests;
		std::vector<ShadowValidationSampleIntent> sampledIntents;
		std::vector<RendererEfficiencyEvidence> completed;
		void Unavailable(const ShadowValidationObservation& observation, const char* reason)
		{
			auto row = observation.evidence; row.status = RendererEfficiencyEvidenceStatus::Unavailable;
			row.unavailableReason = reason; row.artifacts.reset(); completed.push_back(std::move(row));
			std::erase_if(sampledIntents, [&](const auto& item) {
				return item.viewType == observation.evidence.viewType && item.viewId == observation.evidence.viewId
					&& item.frameSerial == observation.evidence.frameSerial;
			});
		}
		void Destroy(Request& request)
		{
			if (request.buffer) vmaDestroyBuffer(allocator, request.buffer, request.allocation);
			request.buffer = VK_NULL_HANDLE; request.allocation = VK_NULL_HANDLE;
		}
		bool Allocate(Request& request)
		{
			if (!allocator || !receipts.Reserve(request.id, request.identity.commandSerial, request.bytes)) return false;
			VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			buffer.size = request.bytes; buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
			if (vmaCreateBuffer(allocator, &buffer, &allocation, &request.buffer, &request.allocation, nullptr) != VK_SUCCESS)
			{ receipts.Release(request.id); return false; }
			return true;
		}
	};
	VulkanShadowValidationReadback::VulkanShadowValidationReadback() : state(std::make_unique<State>()) {}
	VulkanShadowValidationReadback::~VulkanShadowValidationReadback()
	{
		// Caller must Shutdown(true) before allocator teardown. Refuse to destroy
		// outstanding GPU objects merely because the C++ wrapper is being removed.
		Shutdown(false);
	}
	void VulkanShadowValidationReadback::Init(VmaAllocator allocator, VkPhysicalDevice physicalDevice)
	{
		if (state->allocator == allocator && state->physical == physicalDevice) return;
		if (!state->requests.empty()) throw std::logic_error("Cannot rebind an active shadow readback allocator.");
		state->allocator = allocator; state->physical = physicalDevice;
	}
	namespace
	{
		ShadowValidationFrameIdentity Identity(const RendererEfficiencyEvidence& row)
		{
			return {row.effectiveOptions.rendererSessionGeneration, row.frameSerial, row.commandSerial, row.viewType, row.viewId};
		}
		void CopyImage(VkCommandBuffer command, VkImage image, VkBuffer buffer, VkImageAspectFlags aspect,
			std::uint32_t layer, std::uint32_t width, std::uint32_t height, VkImageLayout layout)
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.oldLayout = layout; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image; barrier.subresourceRange = {aspect, 0, 1, layer, 1};
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkBufferImageCopy copy{}; copy.imageSubresource = {aspect, 0, layer, 1}; copy.imageExtent = {width, height, 1};
			vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = layout;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
			host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			host.buffer = buffer; host.offset = 0; host.size = VK_WHOLE_SIZE;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
		}
	}
	bool VulkanShadowValidationReadback::RecordFace(VkCommandBuffer command, VkImage image, VkFormat format,
		std::uint32_t layer, const ShadowValidationObservation& observation)
	{
		if (observation.evidence.fixtureCorrelation.operationId.empty()) return false;
		const auto identity = Identity(observation.evidence);
		if (observation.evidence.kind != RendererEfficiencyEvidenceKind::Shadow || !command || !image || !state->physical || format != VK_FORMAT_D32_SFLOAT || !identity.commandSerial
			|| !identity.rendererSessionGeneration || observation.key.rendererSessionGeneration != identity.rendererSessionGeneration
			|| observation.evidence.fixtureCorrelation.rendererSessionGeneration != identity.rendererSessionGeneration
			|| observation.evidence.fixtureCorrelation.settingsGeneration != observation.evidence.effectiveOptions.settingsGeneration
			|| (state->session && identity.rendererSessionGeneration != state->session)
			|| observation.key.viewType != identity.viewType || observation.key.viewId != identity.viewId
			|| !observation.key.extent[0] || !observation.key.extent[1] || layer > 5
			|| observation.evidence.faceIndex != layer || observation.key.faceIndex != layer
			|| observation.key.depthFormat != static_cast<std::uint32_t>(format)
			|| (static_cast<std::uint32_t>(observation.key.resourceKind) != 1U && layer != 0U))
		{ state->Unavailable(observation, "Unsupported or incomplete native shadow readback input."); return false; }
		state->session = identity.rendererSessionGeneration;
		const auto sampled = std::find_if(state->sampledIntents.begin(), state->sampledIntents.end(), [&](const auto& item) {
			return item.viewType == identity.viewType && item.viewId == identity.viewId;
		});
		if(sampled != state->sampledIntents.end())
		{
			// The controller holds one sample token until every view/face completes.
			// Frames rendered during that fence delay must not duplicate its exported
			// stepFrame or make reference/candidate correspondence depend on speed.
			if(!AdmitShadowValidationSample(*sampled, observation.evidence)) return false;
		}
		else
		{
			if(state->sampledIntents.size() >= 16)
			{ state->Unavailable(observation, "Validation view count exceeds the bounded observation ownership table."); return false; }
			state->sampledIntents.push_back({identity.viewType, identity.viewId, observation.evidence.fixtureCorrelation, observation.evidence.fixtureControlGeneration, identity.frameSerial});
		}
		if (std::any_of(state->requests.begin(), state->requests.end(), [&](const auto& r) {
			return !r.color && !r.lighting && r.identity == identity && r.observation.key.lightId == observation.key.lightId
				&& r.observation.evidence.faceIndex == observation.evidence.faceIndex; }))
		{ state->Unavailable(observation, "Duplicate native shadow face observation."); return false; }
		VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(state->physical, format, &properties);
		if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
		{ state->Unavailable(observation, "Native depth format cannot be copied on this adapter."); return false; }
		State::Request request; request.id = state->nextId++; request.identity = identity; request.observation = observation;
		request.width = observation.key.extent[0]; request.height = observation.key.extent[1]; request.format = format;
		// Check pixel count before multiplication, including malicious dimensions.
		if (static_cast<std::uint64_t>(request.width) * request.height > (512ULL * 1024 * 1024) / 4)
		{ state->Unavailable(observation, "Full native depth observation exceeds bounded capacity."); return false; }
		request.bytes = static_cast<std::uint64_t>(request.width) * request.height * 4;
		if (!state->Allocate(request)) { state->Unavailable(observation, "Native shadow readback capacity or allocation unavailable."); return false; }
		// Grow host ownership before emitting commands; allocation failure cannot
		// orphan a Vulkan staging object already referenced by the primary.
		try { state->requests.push_back(std::move(request)); }
		catch (...) { state->Destroy(request); state->receipts.Release(request.id); throw; }
		CopyImage(command, image, state->requests.back().buffer, VK_IMAGE_ASPECT_DEPTH_BIT, layer,
			state->requests.back().width, state->requests.back().height, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
		return true;
	}
	bool VulkanShadowValidationReadback::RecordLighting(VkCommandBuffer command, VkBuffer source,
		std::uint64_t offset, std::uint64_t size, const RendererEfficiencyEvidence& evidence, std::uint32_t sourceCandidateCount)
	{
		constexpr std::uint64_t blockBytes = 176ULL * 1024;
		ShadowValidationObservation observation; observation.evidence = evidence;
		const auto identity = Identity(evidence);
		const auto& upload = evidence.lightingUploads[evidence.analysisPurpose ? 1 : 0];
		if (evidence.fixtureCorrelation.operationId.empty()) return false;
		if (evidence.kind != RendererEfficiencyEvidenceKind::Lighting || !command || !source || !state->physical
			|| !identity.commandSerial || !identity.frameSerial || !identity.rendererSessionGeneration
			|| !evidence.fixtureControlGeneration || !evidence.effectiveOptions.optionsSessionGeneration
			|| evidence.submitSerial || evidence.completionSerial || !upload.available || !upload.succeeded
			|| upload.analysisPurpose != evidence.analysisPurpose || !upload.allocationId || !upload.allocationGeneration
			|| upload.frameSlot >= 3 || upload.snapshotSlot >= 8 || upload.activeRecordCount > 1024
			|| sourceCandidateCount > 4096 || sourceCandidateCount < upload.activeRecordCount
			|| evidence.fixtureCorrelation.fixtureStepId.empty() || !evidence.effectiveOptions.settingsGeneration
			|| (identity.viewType != RenderViewType::Scene && identity.viewType != RenderViewType::Game)
			|| size != blockBytes || offset != upload.regionOffsetBytes || upload.regionSizeBytes != blockBytes
			|| offset != upload.snapshotSlot * blockBytes || offset > upload.allocationSizeBytes
			|| size > upload.allocationSizeBytes - offset
			|| evidence.fixtureCorrelation.rendererSessionGeneration != identity.rendererSessionGeneration
			|| evidence.fixtureCorrelation.settingsGeneration != evidence.effectiveOptions.settingsGeneration
			|| (state->session && state->session != identity.rendererSessionGeneration))
		{ state->Unavailable(observation, "Incomplete native lighting slice/upload ownership proof."); return false; }
		state->session = identity.rendererSessionGeneration;
		const auto sampled = std::find_if(state->sampledIntents.begin(), state->sampledIntents.end(), [&](const auto& item) {
			return item.viewType == identity.viewType && item.viewId == identity.viewId;
		});
		if (sampled != state->sampledIntents.end())
		{
			if (!AdmitShadowValidationSample(*sampled, evidence)) return false;
		}
		else
		{
			if (state->sampledIntents.size() >= 16)
			{ state->Unavailable(observation, "Lighting view count exceeds the bounded sample ownership table."); return false; }
			state->sampledIntents.push_back({identity.viewType,identity.viewId,evidence.fixtureCorrelation,evidence.fixtureControlGeneration,identity.frameSerial});
		}
		if (std::any_of(state->requests.begin(), state->requests.end(), [&](const auto& request) {
			return request.lighting && request.identity.frameSerial == identity.frameSerial
				&& request.identity.rendererSessionGeneration == identity.rendererSessionGeneration
				&& request.identity.viewType == identity.viewType && request.identity.viewId == identity.viewId
				&& request.observation.evidence.analysisPurpose == evidence.analysisPurpose;
		})) { state->Unavailable(observation, "Duplicate native lighting purpose observation."); return false; }
		State::Request request; request.id = state->nextId++; request.identity = identity;
		request.observation = std::move(observation); request.lighting = true; request.bytes = blockBytes;
		// Retain the actual pre-budget count with this command-owned staging copy.
		request.sourceCandidateCount = sourceCandidateCount;
		if (!state->Allocate(request)) { state->Unavailable(request.observation, "Native lighting staging allocation/capacity unavailable."); return false; }
		try { state->requests.push_back(std::move(request)); }
		catch (...) { state->Destroy(request); state->receipts.Release(request.id); throw; }
		// Host uploads and shader reads must precede this copy. Staging becomes
		// host-readable only after the primary's actual submit and completion.
		VkBufferMemoryBarrier sourceBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		sourceBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		sourceBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		sourceBarrier.srcQueueFamilyIndex = sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		sourceBarrier.buffer = source; sourceBarrier.offset = offset; sourceBarrier.size = size;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &sourceBarrier, 0, nullptr);
		VkBufferCopy copy{offset, 0, size}; vkCmdCopyBuffer(command, source, state->requests.back().buffer, 1, &copy);
		VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		host.buffer = state->requests.back().buffer; host.offset = 0; host.size = size;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
		return true;
	}
	bool VulkanShadowValidationReadback::RecordFrameColor(VkCommandBuffer command, VkImage image, VkFormat format,
		std::uint32_t width, std::uint32_t height, VkImageLayout layout, const ShadowValidationFrameIdentity& identity)
	{
		// Color copies exist only for a frame opted into shadow or lighting proof.
		if (std::none_of(state->requests.begin(), state->requests.end(), [&](const auto& r) { return !r.color && r.identity == identity; })) return false;
		if (std::any_of(state->requests.begin(), state->requests.end(), [&](const auto& r) { return r.color && r.identity == identity; })) return false;
		const auto bpp = VulkanReadback::BytesPerPixel(format);
		if (!command || !image || !bpp || !width || !height || !state->physical || layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
		if (static_cast<std::uint64_t>(width) * height > (512ULL * 1024 * 1024) / bpp) return false;
		VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(state->physical, format, &properties);
		if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) return false;
		State::Request request; request.id = state->nextId++; request.identity = identity; request.color = true;
		request.width = width; request.height = height; request.format = format; request.bytes = static_cast<std::uint64_t>(width) * height * bpp;
		if (!state->Allocate(request)) return false;
		try { state->requests.push_back(std::move(request)); }
		catch (...) { state->Destroy(request); state->receipts.Release(request.id); throw; }
		CopyImage(command, image, state->requests.back().buffer, VK_IMAGE_ASPECT_COLOR_BIT, 0, width, height, layout); return true;
	}
	bool VulkanShadowValidationReadback::ConfirmSubmitted(std::span<const std::uint64_t> commands, std::uint64_t submit)
	{
		// GPU acceptance already happened in the caller. Retain physical staging
		// ownership even if bookkeeping rejects a malformed/replayed receipt.
		for (auto command : commands) PreserveSubmittedCommand(command, submit);
		return state->receipts.Confirm(commands, submit); // Pure nonallocating gate.
	}
	void VulkanShadowValidationReadback::PreserveSubmittedCommand(std::uint64_t command, std::uint64_t submit) noexcept
	{
		if (!command || !submit) return;
		// A captured inventory can name an earlier primary in the same accepted
		// batch. Promote every matching copied use with its actual queue receipt.
		for (auto& r : state->requests)
			for (auto& allocation : r.observation.evidence.staticShadowExecution.allocations)
				for (auto& use : allocation.uses)
					if (use.commandSerial == command && !use.submitSerial) use.submitSerial = submit;
		for (auto& r : state->requests) if (r.identity.commandSerial == command && (!r.submit || r.submit == submit)) {
			r.submit = submit;
			for (auto& action : r.observation.evidence.fixtureActions) MarkShadowValidationFixtureActionSubmitted(action, command, submit);
		}
		state->receipts.PreserveSubmittedCommand(command, submit);
	}
	void VulkanShadowValidationReadback::CompleteSubmission(std::uint64_t submit, std::uint64_t completion)
	{
		// Reserve terminal row storage before advancing the gate. An allocation
		// failure must leave every staging owner available for a later retry.
		state->completed.reserve(state->completed.size() + state->requests.size());
		const auto ids = state->receipts.Complete(submit, completion);
		std::vector<std::pair<ShadowValidationFrameIdentity, std::shared_ptr<const RendererEfficiencyFrameColor>>> colors;
		std::vector<std::pair<bool, RendererEfficiencyEvidence>> terminal;
		terminal.reserve(ids.size());
		const auto read = [&](State::Request& r, std::vector<std::uint8_t>& bytes) {
			void* mapped = nullptr; if (vmaMapMemory(state->allocator, r.allocation, &mapped) != VK_SUCCESS || !mapped) return false;
			const bool okay = vmaInvalidateAllocation(state->allocator, r.allocation, 0, r.bytes) == VK_SUCCESS;
			try { if (okay) { bytes.resize(static_cast<std::size_t>(r.bytes)); std::memcpy(bytes.data(), mapped, bytes.size()); } }
			catch (...) { vmaUnmapMemory(state->allocator, r.allocation); throw; }
			vmaUnmapMemory(state->allocator, r.allocation); return okay;
		};
		// Read shared native color first so every face gets the same immutable
		// actual view/frame bytes. PNG conversion remains the host export's job.
		for (auto& r : state->requests) if (r.color && !r.canceled && std::find(ids.begin(), ids.end(), r.id) != ids.end())
		{
			try {
				auto color = std::make_shared<RendererEfficiencyFrameColor>(); color->width = r.width; color->height = r.height; color->sourceFormat = r.format;
				if (read(r, color->bytes)) colors.emplace_back(r.identity, std::move(color));
			} catch (...) { /* Faces below explicitly report unavailable color. */ }
		}
		for (auto& r : state->requests) if (!r.color && !r.canceled && std::find(ids.begin(), ids.end(), r.id) != ids.end())
		{
			auto row = r.observation.evidence; row.submitSerial = submit; row.completionSerial = completion;
			// A fresh static source recorded in this exact primary receives the same
			// real queue receipt. Cached sources retain their earlier accepted receipt.
			if (row.staticShadowExecution.copyOverlay
				&& row.staticShadowExecution.sourceCommandSerial == row.commandSerial)
				row.staticShadowExecution.sourceSubmitSerial = submit;
			for (auto& allocation : row.staticShadowExecution.allocations)
				for (auto& use : allocation.uses)
					if (!use.submitSerial && use.commandSerial == row.commandSerial) use.submitSerial = submit;
			for (auto& action : row.fixtureActions) MarkShadowValidationFixtureActionCompleted(action, submit, completion);
			try
			{
				// Inventory bytes were captured at recording. This later exact fence
				// receipt proves when those copied accepted references completed.
				if (row.staticShadowExecution.available
					&& std::none_of(row.staticShadowExecution.acceptedSubmissions.begin(), row.staticShadowExecution.acceptedSubmissions.end(),
						[&](const auto& accepted) { return accepted.commandSerial == row.commandSerial; }))
					for (const auto& allocation : row.staticShadowExecution.allocations)
					{
						const auto ownUse = std::find_if(allocation.uses.begin(), allocation.uses.end(),
							[&](const auto& use) { return use.commandSerial == row.commandSerial; });
						if (ownUse != allocation.uses.end()) {
							row.staticShadowExecution.acceptedSubmissions.push_back({row.commandSerial, submit, ownUse->frameSlot});
							break;
						}
					}
				if (row.staticShadowExecution.available
					&& std::find(row.staticShadowExecution.completedSubmissions.begin(), row.staticShadowExecution.completedSubmissions.end(), submit)
						== row.staticShadowExecution.completedSubmissions.end())
					row.staticShadowExecution.completedSubmissions.push_back(submit);
				if (r.lighting)
				{
					const auto color = std::find_if(colors.begin(), colors.end(), [&](const auto& item) { return item.first == r.identity; });
					if (color == colors.end()) throw std::runtime_error("Same-command native lighting frame color is unavailable.");
					auto artifacts = std::make_shared<RendererEfficiencyArtifacts>(); artifacts->frameColor = color->second;
					artifacts->fullSourceCandidateCount = r.sourceCandidateCount;
					if (!read(r, artifacts->lightingPayload) || artifacts->lightingPayload.size() != 176ULL * 1024)
						throw std::runtime_error("Full native lighting staging map/invalidation failed.");
					row.resolution = {color->second->width,color->second->height}; row.artifacts = std::move(artifacts);
					row.status = RendererEfficiencyEvidenceStatus::Valid; row.unavailableReason.clear();
					terminal.emplace_back(false, std::move(row));
					continue;
				}
				if (!r.observation.preparationComplete) throw std::runtime_error("Eligible shadow draw coverage was not fully prepared.");
				const auto color = std::find_if(colors.begin(), colors.end(), [&](const auto& item) { return item.first == r.identity; });
				if (color == colors.end()) throw std::runtime_error("Same-command native frame color is unavailable.");
				if (static_cast<std::uint32_t>(r.observation.key.resourceKind) == 1U)
				{
					std::uint8_t faces = 0;
					for (const auto& other : state->requests) if (!other.color && !other.lighting && !other.canceled && other.submit == submit
						&& other.identity == r.identity && other.observation.key.lightId == r.observation.key.lightId
						&& other.observation.evidence.faceIndex < 6) faces |= static_cast<std::uint8_t>(1U << other.observation.evidence.faceIndex);
					if (faces != 0x3f) throw std::runtime_error("Full six-face native cube observation is incomplete.");
				}
				auto artifacts = std::make_shared<RendererEfficiencyArtifacts>();
				artifacts->shadowKey = r.observation.key; artifacts->eligibleMembership = r.observation.membership;
				artifacts->submittedSegments = r.observation.submittedSegments; artifacts->contentToken = r.observation.contentToken;
				artifacts->depthSourceFormat = r.format; artifacts->frameColor = color->second;
				if (!read(r, artifacts->depthF32LE)) throw std::runtime_error("Native depth staging map/invalidation failed.");
				if (!DecodeShadowDepthCoverage(artifacts->depthF32LE, r.width, r.height, artifacts->coverageU8))
					throw std::runtime_error("Native depth encoding, full extent or pixels are invalid.");
				row.resolution = {r.width, r.height}; row.artifacts = std::move(artifacts);
				row.status = RendererEfficiencyEvidenceStatus::Valid; row.unavailableReason.clear();
			}
			catch (const std::exception& error) { row.status = RendererEfficiencyEvidenceStatus::Unavailable; row.unavailableReason = error.what(); row.artifacts.reset(); }
			terminal.emplace_back(static_cast<std::uint32_t>(r.observation.key.resourceKind) == 1U, std::move(row));
		}
		// A cube is one observation group. A map failure on its last face must
		// also revoke earlier decoded faces, rather than exporting a partial cube.
		for (auto& [cube, row] : terminal)
		{
			if (cube)
			{
				std::uint8_t validFaces = 0;
				for (const auto& [otherCube, other] : terminal)
					if (otherCube && other.lightId == row.lightId && other.viewId == row.viewId && other.viewType == row.viewType
						&& other.commandSerial == row.commandSerial && other.frameSerial == row.frameSerial
						&& other.status == RendererEfficiencyEvidenceStatus::Valid && other.faceIndex < 6)
						validFaces |= static_cast<std::uint8_t>(1U << other.faceIndex);
				if (validFaces != 0x3f)
				{
					row.status = RendererEfficiencyEvidenceStatus::Unavailable; row.artifacts.reset();
					row.unavailableReason = "Complete native six-face cube payload is unavailable.";
				}
			}
		}
		for (auto& item : terminal) {
			if (item.second.status == RendererEfficiencyEvidenceStatus::Unavailable)
				std::erase_if(state->sampledIntents, [&](const auto& sample) {
					return sample.viewType == item.second.viewType && sample.viewId == item.second.viewId && sample.frameSerial == item.second.frameSerial;
				});
			state->completed.push_back(std::move(item.second));
		}
		for (auto& r : state->requests) if (std::find(ids.begin(), ids.end(), r.id) != ids.end()) { state->Destroy(r); state->receipts.Release(r.id); }
		std::erase_if(state->requests, [&](const auto& r) { return std::find(ids.begin(), ids.end(), r.id) != ids.end(); });
	}
	bool VulkanShadowValidationReadback::AttachFixtureAction(std::uint64_t command, const RendererEfficiencyFixtureActionReceipt& action)
	{
		if (!command || action.commandSerial != command || !action.recorded || action.correlation.operationId.empty()
			|| action.submitted || action.completed || action.submitSerial || action.completionSerial) return false;
		bool attached = false;
		const auto attach = [&](RendererEfficiencyEvidence& row) {
			if (row.commandSerial != command || row.frameSerial != action.frameSerial || row.fixtureCorrelation != action.correlation) return;
			row.fixtureActions.push_back(action); attached = true;
		};
		for (auto& request : state->requests) if (!request.color && !request.canceled && request.submit == 0) attach(request.observation.evidence);
		for (auto& row : state->completed) if (!row.submitSerial) attach(row);
		return attached;
	}
	void VulkanShadowValidationReadback::DiscardCommand(std::uint64_t command)
	{
		const auto ids = state->receipts.Discard(command);
		for (auto& r : state->requests) if (std::find(ids.begin(), ids.end(), r.id) != ids.end())
		{
			if(!r.color)
			{
				// An actual dropped command has no rendered observation. Release its
				// token so recovery may collect the first subsequent successful frame.
				std::erase_if(state->sampledIntents, [&](const auto& item) {
					return item.viewType == r.identity.viewType && item.viewId == r.identity.viewId
						&& item.frameSerial == r.identity.frameSerial;
				});
				if(!r.canceled) state->Unavailable(r.observation, "Shadow copy command was not submitted.");
			}
			state->Destroy(r);
		}
		std::erase_if(state->requests, [&](const auto& r) { return std::find(ids.begin(), ids.end(), r.id) != ids.end(); });
	}
	void VulkanShadowValidationReadback::Reset(std::uint64_t session, bool deviceLost)
	{
		for (auto& r : state->requests) { if (!r.color && !r.canceled) state->Unavailable(r.observation, "Shadow readback session was canceled."); r.canceled = true; }
		// A resource edit can cancel evidence while existing recorded primaries
		// are still submitted by descriptor-mutation synchronization. Retain their
		// staging until explicit command discard or actual submit/completion; a
		// cache reset alone does not prove that a recorded copy will never execute.
		if (deviceLost) Shutdown(true);
		state->sampledIntents.clear();
		state->session = session;
	}
	bool VulkanShadowValidationReadback::Shutdown(bool safe)
	{
		if (!safe && std::any_of(state->requests.begin(), state->requests.end(), [](const auto& r) { return r.submit != 0; })) return false;
		for (auto& r : state->requests) state->Destroy(r);
		state->requests.clear(); state->receipts = ShadowValidationReceiptTracker{}; return true;
	}
	std::vector<RendererEfficiencyEvidence> VulkanShadowValidationReadback::DrainCompleted()
	{
		std::vector<RendererEfficiencyEvidence> result; result.swap(state->completed); return result;
	}
}
