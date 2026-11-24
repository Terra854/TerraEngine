// Dedicated one-color fullscreen passes with explicit, retained sampled inputs.
#pragma once
#include "Renderer/Graph/PostProcessCustomPass.h"
#include "VulkanPostProcessHistory.h"
#include <volk.h>
#include <cstddef>
#include <optional>

namespace TerraEngine::Renderer::Vulkan
{
	enum class CustomPostProcessDepthEncoding : std::uint8_t { None,VulkanStandardZ };
	// Motion float4 = previousUV-currentUV, previous device depth, validity.
	enum class CustomPostProcessMotionEncoding : std::uint8_t { None,PreviousUvDeltaDepthValidity };
	struct CustomPostProcessImage
	{
		VkImage image=VK_NULL_HANDLE;
		VkImageView view=VK_NULL_HANDLE;
		VkFormat format=VK_FORMAT_UNDEFINED;
		VkImageUsageFlags usage=0;
		VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
		VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT;
		VkImageViewType viewType=VK_IMAGE_VIEW_TYPE_2D;
		VkExtent2D extent{};
		CustomPostProcessDepthEncoding depthEncoding=CustomPostProcessDepthEncoding::None;
		CustomPostProcessMotionEncoding motionEncoding=CustomPostProcessMotionEncoding::None;
		bool signedViewNormals=false,perceptualRoughness=false;
		PostProcessHistoryScope scope;
		// targetGeneration names the enclosing source-view lifetime, not an
		// independent auxiliary-image allocation counter. Assets use owner only.
		std::uint64_t frameId=0,targetGeneration=0;
		// Actual immutable image/asset allocation owner supplied by the protected
		// native caller. The adapter cannot reconstruct owner proof from handles.
		std::shared_ptr<const void> owner;
	};
	struct CustomPostProcessBinding
	{
		std::uint32_t binding=0;
		CustomPostProcessInputKind kind=CustomPostProcessInputKind::SourceColor;
		TerraEngine::UUID assetId=TerraEngine::EMPTY_UUID;
		CustomPostProcessImage image;
	};
	struct CustomPostProcessCreateInfo
	{
		VkPhysicalDevice physicalDevice=VK_NULL_HANDLE;
		VkDevice device=VK_NULL_HANDLE;
		std::shared_ptr<const PreparedPostProcessCustomPass> prepared;
		// Exact installed PostProcessFullscreen.vert SPIR-V, entry VSMain.
		std::span<const std::uint32_t> fullscreenVertex;
		CustomPostProcessStage stage=CustomPostProcessStage::HdrScene;
		CustomPostProcessDomain domain=CustomPostProcessDomain::LinearHdr;
		std::span<const CustomPostProcessBinding> bindings;
		CustomPostProcessImage output;
		// Explicit non-comparison sampler policy; unsupported filtering rejects.
		VkFilter samplingFilter=VK_FILTER_LINEAR;
	};
	struct CustomPostProcessPass;
	struct CustomPostProcessRecordedOutput
	{
		VkCommandBuffer command=VK_NULL_HANDLE;
		CustomPostProcessStage stage=CustomPostProcessStage::HdrScene;
		CustomPostProcessDomain domain=CustomPostProcessDomain::LinearHdr;
		CustomPostProcessImage image;
		std::uint64_t descriptorRevision=0,shaderPublicationGeneration=0;
	};
	// Pure CPU packing, independent of any descriptor/pipeline owner. Input must
	// contain exactly one value per schema name; failure preserves prior bytes.
	// Logical Boolean packing is not a claim of Vulkan shader ABI eligibility.
	[[nodiscard]] bool PackCustomPostProcessParameters(const PostProcessCustomPassDescriptor& schema,
		std::span<const CustomPostProcessParameterValue> values,std::vector<std::byte>& output,std::string& reason) noexcept;
	[[nodiscard]] std::shared_ptr<CustomPostProcessPass> PrepareCustomPostProcessPass(
		const CustomPostProcessCreateInfo& info,std::string& reason) noexcept;
	// Record only outside another render pass. Inputs must already have their
	// declared sampled layouts and visible writes; no missing input is replaced.
	// Push bytes are prepacked in worker-safe code against the exact schema.
	// The returned producer token is RECORDING proof only, never queue acceptance.
	// Retain pass, inputs and output until actual submitted GPU work completes.
	[[nodiscard]] std::optional<CustomPostProcessRecordedOutput> RecordCustomPostProcessPass(
		VkCommandBuffer command,const CustomPostProcessPass& pass,std::span<const std::byte> pushBytes) noexcept;
}
