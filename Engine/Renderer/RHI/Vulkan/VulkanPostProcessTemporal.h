// VulkanPostProcessTemporal.h - retained compute reconstruction resources.
#pragma once
#include "VulkanPostProcessTargets.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	struct TemporalPass;
	struct TemporalPassInputs
	{
		// Metadata is copied; the parent retains these exact image/view owners
		// until every recorded/submitted borrower has been discarded/completed.
		PostProcessTargetImage sceneColor, sceneDepth, motion, output;
		VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		VkSampleCountFlagBits sceneSamples = VK_SAMPLE_COUNT_1_BIT;
		std::uint32_t historySlotCount = 3; //2..8; read and write slots always differ.
	};
	struct TemporalImageState
	{
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkAccessFlags access = 0;
	};
	struct TemporalHistorySlotState
	{
		VkImageLayout colorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImageLayout depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	};
	struct TemporalAcceptedHistory
	{
		// Supplied only by the parent's real successful-submit provenance owner.
		// An allocated slot or successful command recording is not accepted history.
		std::uint64_t passGeneration = 0, frameId = 0, historyToken = 0;
		std::uint32_t slot = 0;
	};
	struct TemporalRecordRequest
	{
		std::uint64_t recordingId = 0, frameId = 0, historyToken = 0;
		std::uint32_t writeSlot = 0, readSlot = 1;
		std::optional<TemporalAcceptedHistory> previous;
		// Actual scheduled layouts belong to the parent, which must discard or
		// restore its recording-state transaction if this primary never submits.
		TemporalImageState sceneColor, sceneDepth, motion, output;
		std::span<const TemporalHistorySlotState> history;
		float historyWeight = 0.9f;
		float absoluteDepthTolerance = 0.001f, relativeDepthTolerance = 0.01f;
		float neighborhoodDepthTolerance = 0.01f;
		float sharpenStrength = 0.0f;
		bool reverseDepth = false;
		std::array<float, 2> currentJitterUV{}, previousJitterUV{};
	};
	struct TemporalRecordedUse
	{
		// A recording receipt, never a successful-submit or completion receipt.
		std::uint64_t passGeneration = 0, recordingId = 0, frameId = 0, historyToken = 0;
		std::uint32_t writeSlot = 0, readSlot = 0;
		TemporalHistorySlotState writeState, readState;
		TemporalImageState outputState;
	};

	// Allocation and immutable descriptor/pipeline preparation only: no command
	// buffer creation, submission, initialization work, fence waits or history
	// publication. The parent retains old candidates on failure and owns fences.
	std::shared_ptr<TemporalPass> PrepareTemporalPass(VkPhysicalDevice physical,
		PostProcessTargetBundle& targets, const TemporalPassInputs& inputs, std::string& reason);
	// Protected same-queue recording. The parent proves write-slot reuse safety
	// and exact previous acceptance, and keeps the owner through GPU completion.
	// No layout or accepted-history state changes in this CPU owner during Record.
	// First/reset frames select a distinct legal dummy read slot with previous=null.
	[[nodiscard]] bool RecordTemporalPass(VkCommandBuffer command, TemporalPass& pass,
		const TemporalRecordRequest& request, TemporalRecordedUse& recorded) noexcept;
	[[nodiscard]] std::uint64_t TemporalPassGeneration(const TemporalPass& pass) noexcept;
	[[nodiscard]] std::uint32_t TemporalPassSlotCount(const TemporalPass& pass) noexcept;
	// Borrowed allocation metadata only. The consumer must separately prove
	// the slot's exact accepted frame, token, layout and completion ownership.
	[[nodiscard]] PostProcessTargetImage ReadTemporalHistoryDepthTarget(const TemporalPass& pass, std::uint32_t slot) noexcept;
	[[nodiscard]] PostProcessTargetKey TemporalPassTargetKey(const TemporalPass& pass) noexcept;
	[[nodiscard]] VkDevice TemporalPassDevice(const TemporalPass& pass) noexcept;
}
