// Prepared native CMAA2 ownership; graph publication retains actual GPU borrowers.
#pragma once
#include "Renderer/RHI/Vulkan/VulkanPostProcessTargets.h"
#include <memory>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	struct Cmaa2Pass;
	// Allocation-free physical admission for the standard finished-image usage
	// mask. This does not qualify installed shaders, current memory availability,
	// logical-device feature creation, or submission/lifetime ownership.
	bool SupportsCmaa2View(VkPhysicalDevice physical, VkExtent2D extent, VkFormat format) noexcept;

	// Prepare performs allocation/descriptor/pipeline IO only, never submission.
	// Caller supplies the physical device owning targets.device and retains exact
	// source/destination records and this owner through actual GPU completion.
	std::shared_ptr<Cmaa2Pass> PrepareCmaa2Pass(VkPhysicalDevice physical,
		PostProcessTargetBundle& targets, const PostProcessTargetImage& source,
		PostProcessTargetImage& destination, std::string& reason);

	// Record only on the view's serialized compute-capable queue, outside a
	// render pass. Source must already be SHADER_READ_ONLY. No allocation/IO/job
	// waits occur here; result becomes sampled-readable by compute and fragment.
	void RecordCmaa2Pass(VkCommandBuffer command, Cmaa2Pass& pass);
}
