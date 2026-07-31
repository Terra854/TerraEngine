#include "Renderer/RHI/Vulkan/VulkanVirtualGeometryExecutor.h"
#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"
#include "TestHarness.h"

#include <algorithm>
#include <array>

namespace TerraEngine::Tests
{
	void RegisterVulkanVirtualGeometryIndirectTests(TestRegistry &registry)
	{
		registry.Add("Vulkan.VirtualGeometry.IndirectZeroWorkRequiresCompletionEvidence",
			[](TestContext &context)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryMeasurementRecord measurement{};
				measurement.availability = VirtualGeometryMeasurementAvailability::Measured;
				measurement.durationNanoseconds = 0;
				measurement.queryIdentity = "zero-count";
				measurement.executionEvidenceId = "completed-submit";
				context.Expect(measurement.HasAuthoritativeDuration(), "Zero-work indirect result is measured only with completed device evidence.");
			});

		registry.Add("Vulkan.VirtualGeometry.GeneratedIndirectDeclaresComputeToDrawBoundary",
			[](TestContext &context)
			{
				using namespace TerraEngine::Renderer::Vulkan;
				VulkanVirtualGeometryExecutorResources resources{};
				resources.resourcesOwnedByFrame = true;
				resources.packetBuffer = reinterpret_cast<VkBuffer>(1u);
				resources.traversalInstanceBuffer = reinterpret_cast<VkBuffer>(2u);
				resources.traversalRequestBuffer = reinterpret_cast<VkBuffer>(3u);
				resources.traversalCounterBuffer = reinterpret_cast<VkBuffer>(4u);
				resources.compactedPacketBuffer = reinterpret_cast<VkBuffer>(5u);
				resources.generatedSubmissionBuffer = reinterpret_cast<VkBuffer>(6u);
				resources.counterReadbackBuffer = reinterpret_cast<VkBuffer>(7u);
				resources.zeroWorkIndirectBuffer = reinterpret_cast<VkBuffer>(8u);
				resources.descriptorSet = reinterpret_cast<VkDescriptorSet>(9u);
				VulkanVirtualGeometryExecutorPipelines pipelines{};
				pipelines.layout = reinterpret_cast<VkPipelineLayout>(1u);
				pipelines.traversal = reinterpret_cast<VkPipeline>(2u);
				pipelines.compaction = reinterpret_cast<VkPipeline>(3u);
				pipelines.indirectGeneration = reinterpret_cast<VkPipeline>(4u);
				const auto plan = BuildVulkanVirtualGeometryExecutorPlan(1u, 1u, false, resources, pipelines, 1u, 1u, 1u, 1u);
				context.Expect(std::find(plan.barriers.begin(), plan.barriers.end(), VulkanVirtualGeometryBarrierKind::ComputeToDraw) != plan.barriers.end(),
					"GPU-authored indirect commands require an explicit compute-to-draw boundary.");
			});

		registry.Add("Vulkan.VirtualGeometry.ExecutorPlanDeclaresEveryOrderedStorageBoundary",
			[](TestContext &context)
			{
				using namespace TerraEngine::Renderer::Vulkan;
				VulkanVirtualGeometryExecutorResources resources{};
				resources.resourcesOwnedByFrame = true;
				resources.packetBuffer = reinterpret_cast<VkBuffer>(1u);
				resources.traversalInstanceBuffer = reinterpret_cast<VkBuffer>(2u);
				resources.traversalRequestBuffer = reinterpret_cast<VkBuffer>(3u);
				resources.traversalCounterBuffer = reinterpret_cast<VkBuffer>(4u);
				resources.compactedPacketBuffer = reinterpret_cast<VkBuffer>(5u);
				resources.generatedSubmissionBuffer = reinterpret_cast<VkBuffer>(6u);
				resources.counterReadbackBuffer = reinterpret_cast<VkBuffer>(7u);
				resources.zeroWorkIndirectBuffer = reinterpret_cast<VkBuffer>(8u);
				resources.descriptorSet = reinterpret_cast<VkDescriptorSet>(9u);
				VulkanVirtualGeometryExecutorPipelines pipelines{};
				pipelines.layout = reinterpret_cast<VkPipelineLayout>(1u);
				pipelines.traversal = reinterpret_cast<VkPipeline>(2u);
				pipelines.compaction = reinterpret_cast<VkPipeline>(3u);
				pipelines.indirectGeneration = reinterpret_cast<VkPipeline>(4u);
				const auto plan = BuildVulkanVirtualGeometryExecutorPlan(
					1u, 1u, false, resources, pipelines, 1u, 1u, 1u, 1u);
				const std::array<VulkanVirtualGeometryBarrierKind, 5u> expected{
					VulkanVirtualGeometryBarrierKind::UploadToTraversal,
					VulkanVirtualGeometryBarrierKind::TraversalToCompaction,
					VulkanVirtualGeometryBarrierKind::CompactionToIndirect,
					VulkanVirtualGeometryBarrierKind::ComputeToDraw,
					VulkanVirtualGeometryBarrierKind::CounterToReadback };
				context.Expect(
					plan.barriers.size() == expected.size()
						&& std::equal(plan.barriers.begin(), plan.barriers.end(), expected.begin()),
					"The immutable executor plan must publish every upload, traversal, compaction, indirect, draw, and readback boundary in recording order.");
			});

		registry.Add("Vulkan.VirtualGeometry.ExecutorKeepsMirrorsUnavailableWithoutPipelines",
			[](TestContext &context)
			{
				Renderer::Vulkan::VulkanVirtualGeometryExecutorResources resources;
				resources.resourcesOwnedByFrame = true;
				const Renderer::Vulkan::VulkanVirtualGeometryExecutorPipelines pipelines{};
				const auto plan = Renderer::Vulkan::BuildVulkanVirtualGeometryExecutorPlan(12u, 64u, false, resources, pipelines, 9u, 2u, 2u, 3u);
				context.Expect(
					!plan.resourcesReady && !plan.pipelinesReady, "Frame ownership alone cannot turn CPU mirrors into live Vulkan compute resources.");
				context.Expect(!plan.unavailableReason.empty(), "An unavailable executor must surface a diagnosed fallback reason.");
				const auto failure =
					Renderer::Vulkan::BuildVulkanVirtualGeometryFailureReceipt(plan, TerraEngine::Renderer::VirtualGeometryBackendStage::Compaction);
				context.Expect(
					!failure.evidenceId.empty() && !failure.reason.empty(), "Failed GPU-driven stages keep identity and reason without claiming execution.");
			});
	}
} // namespace TerraEngine::Tests
