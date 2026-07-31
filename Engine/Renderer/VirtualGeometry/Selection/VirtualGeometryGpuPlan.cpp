#include "VirtualGeometryGpuPlan.h"

#include <algorithm>

namespace TerraEngine::Renderer
{
	namespace
	{
		constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
		constexpr std::uint64_t kFnvPrime = 1099511628211ull;

		void HashValue(std::uint64_t &hash, std::uint64_t value)
		{
			hash ^= value;
			hash *= kFnvPrime;
		}
	}

	bool VirtualGeometryGpuPlan::IsComplete() const
	{
		if(!valid || capacity == 0u || compaction.outputCapacity != capacity
			|| traversal.requestCapacity != capacity
			|| generatedWork.commandCapacity != capacity)
		{
			return false;
		}
		if(packets.size() != compaction.candidatePacketCount
			|| generatedWork.commandCount != packets.size()
			|| replay.expectedAuthoritativeCounter != packets.size())
		{
			return false;
		}
		return generatedWork.zeroWork == packets.empty();
	}

	VirtualGeometryBackendSelection SelectVirtualGeometryBackend(
		const BackendCapabilityProfile &capabilities)
	{
		VirtualGeometryBackendSelection selection{};
		const bool isVulkan = capabilities.backendId == "vulkan";
		const std::uint32_t declaredCapacity =
			capabilities.virtualGeometryGeneratedWork.maximumCommandCount;
		selection.commandCapacity = declaredCapacity;
		selection.delayedCounterReadbackAvailable = isVulkan
			&& capabilities.supportsVirtualGeometryCounterReadback;
		const bool hasCompleteDeviceRoute = isVulkan
			&& capabilities.supportsVirtualGeometryGpuPacketBuffers
			&& capabilities.supportsVirtualGeometryGpuCompaction
			&& capabilities.supportsVirtualGeometryGeneratedSubmissions
			&& capabilities.virtualGeometryGeneratedWork.supportsDeviceAuthoredWork
			&& declaredCapacity != 0u;
		if(hasCompleteDeviceRoute)
		{
			selection.route = VirtualGeometrySubmissionRoute::VulkanDeviceAuthored;
			selection.generatedWorkKind = VirtualGeometryGeneratedWorkKind::MeshTasks;
			selection.deviceAuthoredWorkAvailable = true;
			return selection;
		}

		if(isVulkan && capabilities.supportsVirtualGeometryGpuPacketBuffers)
		{
			selection.route = VirtualGeometrySubmissionRoute::VulkanPreparedFallback;
			selection.commandCapacity = declaredCapacity;
			selection.fallbackReason = "Vulkan has prepared virtual-geometry packet buffers, but no completed device-authored traversal/compaction/indirect route.";
			return selection;
		}

		selection.fallbackReason = isVulkan
			? "Vulkan has not exposed the required virtual-geometry GPU packet route; the processor oracle remains authoritative."
			: "Only the Vulkan backend is in scope for virtual-geometry device-authored submission; the processor oracle remains authoritative.";
		return selection;
	}

	VirtualGeometryGpuPlan BuildVirtualGeometryGpuPlan(
		const VirtualGeometrySelectionResult &selection,
		const VirtualGeometryGpuPlanOptions &options)
	{
		VirtualGeometryGpuPlan plan{};
		plan.capacity = options.packetCapacity;
		plan.frameId = options.frameId;
		plan.generation = options.generation;
		plan.failOpen = selection.failOpen;
		plan.traversal = {
			static_cast<std::uint32_t>(std::min<std::size_t>(selection.selected.size(), options.packetCapacity)),
			options.packetCapacity,
			options.generation,
			true };
		plan.compaction = { 0u, options.packetCapacity, true };
		plan.generatedWork.kind = options.generatedWorkKind;
		plan.generatedWork.commandCapacity = options.packetCapacity;
		plan.generatedWork.usesIndirectCount = options.usesIndirectCount;

		if(options.packetCapacity == 0u)
		{
			plan.overflow = !selection.selected.empty();
			plan.failOpen = plan.failOpen || plan.overflow;
			plan.rejectionReason = "Virtual-geometry GPU plan requires a non-zero packet capacity.";
			return plan;
		}

		std::vector<std::uint64_t> selected = selection.selected;
		std::sort(selected.begin(), selected.end());
		selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
		plan.deterministicHash = kFnvOffsetBasis;
		for(const std::uint64_t id : selected)
		{
			if(plan.packets.size() >= options.packetCapacity)
			{
				plan.overflow = true;
				plan.failOpen = true;
				break;
			}
			plan.packets.push_back(id);
			HashValue(plan.deterministicHash, id);
		}

		plan.compaction.candidatePacketCount = static_cast<std::uint32_t>(plan.packets.size());
		plan.generatedWork.commandCount = static_cast<std::uint32_t>(plan.packets.size());
		plan.generatedWork.zeroWork = plan.packets.empty();
		plan.replay = { plan.deterministicHash, options.frameId, options.generation,
			static_cast<std::uint32_t>(plan.packets.size()) };
		plan.valid = true;
		return plan;
	}

	VirtualGeometryGpuPlan BuildVirtualGeometryGpuPlan(const VirtualGeometrySelectionResult &selection, std::uint32_t capacity)
	{
		VirtualGeometryGpuPlanOptions options{};
		options.packetCapacity = capacity;
		return BuildVirtualGeometryGpuPlan(selection, options);
	}
} // namespace TerraEngine::Renderer
