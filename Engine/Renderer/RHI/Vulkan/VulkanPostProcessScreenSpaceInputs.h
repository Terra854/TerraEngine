// VulkanPostProcessScreenSpaceInputs.h - Exact resource inputs for screen lighting.
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>
#include "Renderer/GraphicsSettings/GraphicsSettingsTypes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	struct ScreenSpaceInputViewOwner
	{
		GraphicsSettings::GraphicsLifecycleStamp lifecycle{};
		GraphicsSettings::GraphicsViewIdentity view{};
		std::uint64_t sceneTargetGeneration = 0;
		bool operator==(const ScreenSpaceInputViewOwner&) const = default;
	};
	struct ScreenSpaceInputImage
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageUsageFlags usage = 0;
		VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
		VkExtent2D extent{};
		std::uint32_t mipLevels = 1;
		ScreenSpaceInputViewOwner owner;
		// Zero means no produced-frame proof was supplied, not previous/current
		// valid pixels. Allocation preparation never manufactures this receipt.
		std::uint64_t producedFrameId = 0;
	};

	// All radiance is scene-linear HDR before camera exposure. These separate
	// types prevent finished scene color from being an anonymous lighting term.
	// SSR replaces environment SPECULAR incident radiance; SSGI replaces diffuse
	// INDIRECT incident radiance. The surface consumer applies BRDF/albedo once.
	struct ScreenSpaceSceneRadiance { ScreenSpaceInputImage image; };
	struct ScreenSpaceEnvironmentSpecular { ScreenSpaceInputImage image; };
	struct ScreenSpaceIndirectDiffuse { ScreenSpaceInputImage image; };

	struct ScreenSpaceInputDemand
	{
		bool ambientOcclusion = false, reflections = false, indirectLighting = false;
		bool ambientOcclusionNeedsNormals = false;
		bool sharedHiZ = false;
		[[nodiscard]] bool Any() const noexcept { return ambientOcclusion || reflections || indirectLighting; }
		[[nodiscard]] bool NeedsRadiance() const noexcept { return reflections || indirectLighting; }
		[[nodiscard]] bool NeedsNormals() const noexcept { return reflections || indirectLighting || (ambientOcclusion && ambientOcclusionNeedsNormals); }
	};
	struct ScreenSpaceInputCreateInfo
	{
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkExtent2D extent{};
		ScreenSpaceInputViewOwner owner;
		ScreenSpaceInputDemand demand;
		ScreenSpaceSceneRadiance sceneRadiance;
		// Native standard-Z depth: near0, far/background1, readable depth aspect.
		ScreenSpaceInputImage depth;
		// Signed unit VIEW-space XYZ after normal maps, perceptual roughness W.
		std::optional<ScreenSpaceInputImage> normalsRoughness;
		std::optional<ScreenSpaceEnvironmentSpecular> environmentSpecular;
		std::optional<ScreenSpaceIndirectDiffuse> indirectDiffuse;
		// Optional same-view standard-Z MAX pyramid; absence allocates nothing.
		std::optional<ScreenSpaceInputImage> hiZ;
		VkSampleCountFlagBits geometrySamples = VK_SAMPLE_COUNT_1_BIT;
		// Nonzero requests exact current-frame validation for consumed borrowed
		// inputs. Zero is allocation-only; callers must not execute from it alone.
		std::uint64_t expectedFrameId = 0;
	};
	struct ScreenSpaceInputResources
	{
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		ScreenSpaceInputViewOwner owner;
		ScreenSpaceInputDemand demand;
		ScreenSpaceInputImage depth;
		std::optional<ScreenSpaceSceneRadiance> sceneRadiance;
		std::optional<ScreenSpaceInputImage> normalsRoughness;
		std::optional<ScreenSpaceEnvironmentSpecular> environmentSpecular;
		std::optional<ScreenSpaceIndirectDiffuse> indirectDiffuse;
		std::optional<ScreenSpaceInputImage> hiZ;
		VmaAllocation normalsAllocation = VK_NULL_HANDLE;
		bool requiresNormalsProducer = false;
		bool borrowedFramesVerified = false;
		ScreenSpaceInputResources() = default;
		ScreenSpaceInputResources(const ScreenSpaceInputResources&) = delete;
		ScreenSpaceInputResources& operator=(const ScreenSpaceInputResources&) = delete;
		~ScreenSpaceInputResources();
	};
	enum class ScreenSpaceInputPreparationState { Disabled, PreparedResources, Rejected };
	struct ScreenSpaceInputPreparation
	{
		ScreenSpaceInputPreparationState state = ScreenSpaceInputPreparationState::Rejected;
		std::shared_ptr<ScreenSpaceInputResources> resources;
		std::string reason;
	};

	// No native queries, allocations, submissions or old-owner mutation occur
	// when all consumers are disabled. An enabled result certifies only resource
	// preparation, not produced pixels, shader eligibility or effect capability.
	// Parent retirement must retain every borrowed image through actual GPU use.
	// A new owned normal attachment starts UNDEFINED and contains no geometry.
	// The real scene producer must write post-normal-map view normals/roughness,
	// then order COLOR_ATTACHMENT_WRITE -> COMPUTE_SHADER sampled reads in
	// SHADER_READ_ONLY. SSR/SSGI consume same-frame HDR/depth/normal/fallback
	// contributions BEFORE exposure and replace their respective incident terms;
	// neither may add a second complete IBL/direct-light contribution.
	ScreenSpaceInputPreparation PrepareScreenSpaceInputResources(const ScreenSpaceInputCreateInfo& info);
}
