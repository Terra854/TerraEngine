// Persistent native custom pipelines with immutable per-record parameter plans.
#pragma once
#include "VulkanPostProcessCustom.h"
#include <array>
#include <string_view>

namespace TerraEngine::Renderer::Vulkan
{
	struct CustomPostProcessChain;
	struct CustomPostProcessChainFrame;
	// SourceColor is wired by the chain, never supplied by this resolver. Each
	// other declared semantic must return its actual retained, qualified input.
	using CustomChainInputResolver=bool(*)(void*,const PreparedPostProcessCustomPass&,
		const CustomPostProcessInput&,CustomPostProcessBinding&,std::string&);
	using CustomChainOutputSelector=bool(*)(void*,const PreparedPostProcessCustomPass&,
		const CustomPostProcessImage&,CustomPostProcessImage&,std::string&);
	struct CustomPostProcessChainCreateInfo
	{
		VkPhysicalDevice physicalDevice=VK_NULL_HANDLE;
		VkDevice device=VK_NULL_HANDLE;
		std::shared_ptr<const PreparedPostProcessCustomPassSet> prepared;
		std::span<const std::uint32_t> fullscreenVertex;
		// Explicit native source at EACH used insertion point; HDR source is not
		// substituted for a missing display-linear or pre-UI source.
		std::array<CustomPostProcessImage,3> stageSources{};
		CustomChainInputResolver resolveInput=nullptr;
		CustomChainOutputSelector selectOutput=nullptr;
		void* context=nullptr;
		VkFilter samplingFilter=VK_FILTER_LINEAR;
	};
	struct CustomChainParameterSnapshot
	{
		TerraEngine::UUID passAssetId=TerraEngine::EMPTY_UUID;
		std::uint64_t descriptorRevision=0;
		std::span<const CustomPostProcessParameterValue> values;
	};
	struct CustomPostProcessChainFrameRequest
	{
		std::uint64_t frameId=0,parameterRevision=0;
		PostProcessHistoryScope scope;
		std::array<CustomPostProcessImage,3> stageSources{};
		// Complete one-per-enabled-pass values, copied to packed immutable bytes
		// by worker-safe preparation. The callback reads copied resource snapshots
		// only: no live ECS, GPU allocations, submission or cache mutation.
		std::span<const CustomChainParameterSnapshot> parameters;
		CustomChainInputResolver resolveInput=nullptr;
		void* context=nullptr;
	};
	enum class CustomChainPassRecordStatus : std::uint8_t { Begin,Recorded,Failed };
	struct CustomChainPassRecordEvent
	{
		CustomChainPassRecordStatus status=CustomChainPassRecordStatus::Begin;
		VkCommandBuffer command=VK_NULL_HANDLE;
		std::string_view stableId;
		TerraEngine::UUID assetId=TerraEngine::EMPTY_UUID;
		CustomPostProcessStage stage=CustomPostProcessStage::HdrScene;
		std::uint32_t ordinal=0;
		std::uint64_t frameId=0,parameterRevision=0,descriptorRevision=0,shaderPublicationGeneration=0;
		PostProcessHistoryScope scope;
		std::uint64_t targetGeneration=0;
		VkImage destinationImage=VK_NULL_HANDLE;
		VkExtent2D extent{};
		// Begin names a planned destination. Only Recorded carries a producer.
		std::optional<CustomPostProcessRecordedOutput> output;
	};
	// Observer can record real timestamps/receipts, but cannot throw or reject a
	// command after drawing. Recorded is not queue accepted, completed or timed.
	using CustomChainRecordObserver=void(*)(void*,const CustomChainPassRecordEvent&) noexcept;
	struct CustomChainStageRecordedUse
	{
		std::uint64_t frameId=0,parameterRevision=0;
		CustomPostProcessStage stage=CustomPostProcessStage::HdrScene;
		std::uint32_t passCount=0;
		std::optional<CustomPostProcessRecordedOutput> output;
	};
	enum class CustomChainRecordResult : std::uint8_t { Rejected,Skipped,Recorded };
	// Native preparation is transactional: no caller-visible chain is replaced
	// until every enabled ordered pass, input and output has been prepared.
	[[nodiscard]] std::shared_ptr<const CustomPostProcessChain> PrepareCustomPostProcessChain(
		const CustomPostProcessChainCreateInfo& info,std::string& reason) noexcept;
	[[nodiscard]] std::shared_ptr<const CustomPostProcessChainFrame> PrepareCustomPostProcessChainFrame(
		std::shared_ptr<const CustomPostProcessChain> chain,
		const CustomPostProcessChainFrameRequest& request,std::string& reason) noexcept;
	// Record outside another render pass under the protected queue owner. No
	// native allocations or parameter packing occur here. Rejected after a native
	// recording failure requires discarding the ENTIRE primary command buffer.
	// Caller retains frame (and hence chain/images) until real GPU completion.
	[[nodiscard]] CustomChainRecordResult RecordCustomPostProcessChainStage(VkCommandBuffer command,
		const CustomPostProcessChainFrame& frame,CustomPostProcessStage stage,
		CustomChainStageRecordedUse& output,CustomChainRecordObserver observer=nullptr,void* context=nullptr) noexcept;
}
