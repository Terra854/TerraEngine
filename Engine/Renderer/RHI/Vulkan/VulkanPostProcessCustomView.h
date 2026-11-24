// Source-color-only integration of persistent custom passes at three view stages.
#pragma once
#include "VulkanPostProcessCustomChain.h"
#include "VulkanPostProcessCustomImages.h"

namespace TerraEngine::Renderer::Vulkan
{
	struct PostProcessCustomViewConfiguration
	{
		std::shared_ptr<const PreparedPostProcessCustomPassSet> prepared;
		PostProcessHistoryScope scope;
		std::uint64_t planFrameId = 0u, authoringRevision = 0u;
	};
	struct CustomViewStagePlan
	{
		CustomPostProcessStage stage=CustomPostProcessStage::HdrScene;
		// One actual pool index per enabled pass, in already validated order.
		std::vector<std::uint32_t> destinations;
		std::optional<std::uint32_t> finalImageIndex;
	};
	// Pure selection only: no Vulkan calls or producer claims. Both ping images
	// belong to the caller's actual adopted set; source aliases are skipped.
	[[nodiscard]] bool SelectCustomPostProcessViewStage(const PreparedPostProcessCustomPassSet& prepared,
		CustomPostProcessStage stage,const CustomPostProcessImage& source,
		std::span<const CustomPostProcessImage> images,std::array<std::uint32_t,2> pingIndices,
		CustomViewStagePlan& output,std::string& reason) noexcept;
	struct CustomViewCreateInfo
	{
		VkPhysicalDevice physicalDevice=VK_NULL_HANDLE;
		VkDevice device=VK_NULL_HANDLE;
		std::shared_ptr<const PreparedPostProcessCustomPassSet> prepared;
		std::shared_ptr<const CustomPostProcessImageSet> images;
		std::array<CustomPostProcessImage,3> stageSources{};
		std::array<std::array<std::uint32_t,2>,3> pingIndices{};
		// Actual/planned initial layouts established by caller before the stage.
		// Reused ping images must enter sampled-read, matching prior pass finals.
		std::array<VkImageLayout,3> imageLayouts{};
		std::span<const std::uint32_t> fullscreenVertex;
		VkFilter samplingFilter=VK_FILTER_LINEAR;
	};
	struct CustomPostProcessView;
	struct CustomPostProcessViewFrame;
	struct CustomViewFrameRequest
	{
		std::uint64_t frameId=0,authoringRevision=0;
		PostProcessHistoryScope scope;
		std::array<CustomPostProcessImage,3> stageSources{};
		std::span<const CustomChainParameterSnapshot> parameters;
	};
	// Protected preparation, preserving the previous owner on any failure. The
	// image set must genuinely own transferred allocations, never an outer target
	// containing this view (which would create an ownership cycle).
	[[nodiscard]] std::shared_ptr<const CustomPostProcessView> PrepareCustomPostProcessView(
		const CustomViewCreateInfo& info,std::string& reason) noexcept;
	[[nodiscard]] std::optional<CustomViewStagePlan> ReadCustomPostProcessViewStage(
		const CustomPostProcessView& view,CustomPostProcessStage stage);
	// Worker-safe parameter packing. Caller retains the immutable result through
	// actual submit completion; recording alone never publishes final pixels.
	[[nodiscard]] std::shared_ptr<const CustomPostProcessViewFrame> PrepareCustomPostProcessViewFrame(
		std::shared_ptr<const CustomPostProcessView> view,const CustomViewFrameRequest& request,std::string& reason) noexcept;
	[[nodiscard]] CustomChainRecordResult RecordCustomPostProcessViewStage(VkCommandBuffer command,
		const CustomPostProcessViewFrame& frame,CustomPostProcessStage stage,CustomChainStageRecordedUse& output,
		CustomChainRecordObserver observer=nullptr,void* context=nullptr) noexcept;
}
