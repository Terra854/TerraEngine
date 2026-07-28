// VulkanDescriptorPlanning.h - immutable descriptor preparation records.

#pragma once

#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Renderer/Deformation/AnimatedVirtualGeometryRuntime.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Core/Export.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer::Vulkan {

	struct DescriptorBindingPlan
	{
		std::uint64_t frameId = 0u;
		std::uint32_t viewId = 0u;
		std::vector<std::string> records;
		bool coordinatorOwnedAllocator = true;
		std::string fallbackReason;
	};

	TERRAENGINE_API DescriptorBindingPlan BuildDescriptorBindingPlan(const RenderFrameSnapshot& snapshot, std::uint32_t viewId);

	// The animation descriptor set is intentionally expressed without Vulkan
	// handles. Worker preparation can validate the frozen ABI, calculate bounded
	// allocation sizes, and retain the result after the live Vulkan context has
	// moved to its protected recording boundary.
	enum class VulkanAnimationDescriptorAccess : std::uint8_t
	{
		ReadOnly,
		ReadWrite
	};

	enum class VulkanAnimationDescriptorSource : std::uint8_t
	{
		// Records copied from the immutable GpuAnimationPlan for this generation.
		FrameUpload,
		// Cooked program/clip/skeleton/mask/morph data is already GPU-resident.
		ResidentInput,
		// Compute stages create these values; they must never be read back as poses.
		ComputeOutput
	};

	struct VulkanAnimationDescriptorBindingPlan
	{
		Animation::GpuAnimationDescriptorBinding binding = Animation::GpuAnimationDescriptorBinding::GenerationRecords;
		VulkanAnimationDescriptorAccess access = VulkanAnimationDescriptorAccess::ReadOnly;
		VulkanAnimationDescriptorSource source = VulkanAnimationDescriptorSource::FrameUpload;
		std::uint64_t minimumByteCount = 0U;
	};

	struct VulkanAnimationDescriptorSetPlan
	{
		std::uint32_t set = Animation::kGpuAnimationDescriptorSet;
		std::uint32_t layoutVersion = Animation::kGpuAnimationDescriptorLayoutVersion;
		std::uint32_t planGeneration = 0U;
		std::array<VulkanAnimationDescriptorBindingPlan, Animation::kGpuAnimationDescriptorBindingCount> bindings{};
		bool hasWork = false;
		bool valid = false;
		std::string rejectionReason;
	};

	// Calculates all fifteen append-only descriptor bindings. A plan without
	// dispatch work is deliberately valid but inactive, allowing the caller to
	// perform no allocation, upload, descriptor mutation, or queue submission.
	TERRAENGINE_API VulkanAnimationDescriptorSetPlan BuildVulkanAnimationDescriptorSetPlan(
		const Animation::GpuAnimationPlan& plan);

	enum class VulkanDeformationDescriptorBinding : std::uint8_t
	{
		Generations,
		Work,
		StaticPageBytes,
		Palettes,
		MorphWeights,
		CurrentVertices,
		PreviousVertices,
		MeshletBounds,
		ClusterBounds,
		Refinement,
		Diagnostics,
		Count
	};

	inline constexpr std::uint32_t kVulkanDeformationDescriptorSet = 2u;
	inline constexpr std::size_t kVulkanDeformationDescriptorBindingCount =
		static_cast<std::size_t>(VulkanDeformationDescriptorBinding::Count);

	struct VulkanDeformationDescriptorBindingPlan
	{
		VulkanDeformationDescriptorBinding binding =
			VulkanDeformationDescriptorBinding::Generations;
		VulkanAnimationDescriptorAccess access =
			VulkanAnimationDescriptorAccess::ReadOnly;
		VulkanAnimationDescriptorSource source =
			VulkanAnimationDescriptorSource::FrameUpload;
		std::uint64_t minimumByteCount = 0u;
	};

	struct VulkanDeformationDescriptorSetPlan
	{
		std::uint32_t set = kVulkanDeformationDescriptorSet;
		std::uint32_t layoutVersion = kAnimatedGeometryAbiVersion;
		std::array<VulkanDeformationDescriptorBindingPlan,
			kVulkanDeformationDescriptorBindingCount> bindings{};
		DeformationWorkingSetPlan workingSet{};
		bool hasWork = false;
		bool valid = false;
		std::string rejectionReason;
	};

	// Descriptor sizing is calculated before protected Vulkan allocation. This
	// keeps overflow and budget rejection atomic and makes zero-work frames a
	// true no-op for descriptors and device memory.
	TERRAENGINE_API VulkanDeformationDescriptorSetPlan
		BuildVulkanDeformationDescriptorSetPlan(
			const GpuDeformationPlan& plan,
			std::uint64_t workingSetLimitBytes = 0u);

}
