#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "Renderer/RHI/Vulkan/VulkanCommandRecording.h"
#include "Renderer/RHI/Vulkan/VulkanVirtualGeometryExecutor.h"
#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <string>
namespace TerraEngine::Tests
{
	void RegisterVulkanVirtualGeometryCompactionTests(TestRegistry &r)
	{
		r.Add("Vulkan.VirtualGeometry.CompactionUnavailableIsNotExecuted",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				auto e = MakePlannedVirtualGeometryCapability({ VirtualGeometryCapabilityStage::Compaction,
					VirtualGeometryCapabilityBackend::Vulkan,
					VirtualGeometryCapabilityRoute::MeshShader,
					"unsupported",
					{} });
				EvaluateVirtualGeometryCapabilitySupport(e, false, "pipeline unavailable", "");
				c.Expect(!e.CanReportExecuted() && !e.supported, "Unavailable Vulkan pipeline must remain explicit and non-executed.");
			});

		r.Add("Vulkan.VirtualGeometry.ExecutorPlanRequiresEveryLiveStage",
			[](TestContext &c)
			{
				Renderer::Vulkan::VulkanVirtualGeometryExecutorResources resources{};
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
				Renderer::Vulkan::VulkanVirtualGeometryExecutorPipelines pipelines{};
				pipelines.layout = reinterpret_cast<VkPipelineLayout>(10u);
				pipelines.traversal = reinterpret_cast<VkPipeline>(11u);
				pipelines.compaction = reinterpret_cast<VkPipeline>(12u);
				pipelines.indirectGeneration = reinterpret_cast<VkPipeline>(13u);
				const auto plan = Renderer::Vulkan::BuildVulkanVirtualGeometryExecutorPlan(
					8u, 16u, false, resources, pipelines, 9u, 10u, 11u, 12u);
				c.Expect(plan.resourcesReady && plan.descriptorReady && plan.pipelinesReady,
					"The executor is live only when traversal, compaction, and indirect generation own complete resources and pipelines.");
				c.Expect(!plan.generatedSubmissionExecutionAvailable,
					"Generated submission execution remains unproven until the matching frame fence publishes completion evidence.");
			});

		r.Add("Vulkan.VirtualGeometry.CommandPlanRecordsAllDeviceAuthoredStages",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				using namespace TerraEngine::Renderer::Vulkan;
				const VirtualGeometrySelectionResult selection{ { 4u, 5u }, {}, false };
				const VirtualGeometryGpuPlan immutablePlan = BuildVirtualGeometryGpuPlan(
					selection, { 8u, 9u, 10u, VirtualGeometryGeneratedWorkKind::MeshTasks, false });
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
				pipelines.layout = reinterpret_cast<VkPipelineLayout>(10u);
				pipelines.traversal = reinterpret_cast<VkPipeline>(11u);
				pipelines.compaction = reinterpret_cast<VkPipeline>(12u);
				pipelines.indirectGeneration = reinterpret_cast<VkPipeline>(13u);
				const VulkanVirtualGeometryCommandPlan commandPlan =
					BuildVulkanVirtualGeometryCommandPlan(immutablePlan, resources, pipelines);
				c.Expect(commandPlan.valid && commandPlan.protectedRecordingRequired
					&& commandPlan.stages.size() == 3u,
					"Vulkan records immutable traversal, compaction, and indirect stages as one protected ordered command plan.");
			});

		r.Add("Vulkan.VirtualGeometry.VisibilityRequiresCompleteFrameResourcesAndPipelines",
			[](TestContext &c)
			{
				Renderer::Vulkan::VulkanVirtualGeometryVisibilityResources resources{};
				Renderer::Vulkan::VulkanVirtualGeometryVisibilityPipelines pipelines{};
				const auto missing = Renderer::Vulkan::BuildVulkanVirtualGeometryVisibilityPlan(
					8u, 16u, 64u, 64u, 8u,
					resources, pipelines, 3u, 4u);
				c.Expect(!missing.ready && !missing.unavailableReason.empty(), "Visibility cannot claim execution without complete frame resources and pipelines.");
				resources.resourcesOwnedByFrame = true;
				resources.inputTriangleBuffer = reinterpret_cast<VkBuffer>(1u); resources.depthBuffer = reinterpret_cast<VkBuffer>(2u);
				resources.visibilityRecordBuffer = reinterpret_cast<VkBuffer>(3u); resources.resolvedVisibilityBuffer = reinterpret_cast<VkBuffer>(4u);
				resources.shadingBinBuffer = reinterpret_cast<VkBuffer>(5u); resources.diagnosticBuffer = reinterpret_cast<VkBuffer>(6u);
				resources.computeRasterDescriptorSet = reinterpret_cast<VkDescriptorSet>(7u);
				resources.visibilityResolveDescriptorSet = reinterpret_cast<VkDescriptorSet>(8u);
				resources.shadingBinsDescriptorSet = reinterpret_cast<VkDescriptorSet>(9u);
				pipelines.layout = reinterpret_cast<VkPipelineLayout>(10u); pipelines.computeRaster = reinterpret_cast<VkPipeline>(11u); pipelines.visibilityResolve = reinterpret_cast<VkPipeline>(12u); pipelines.shadingBins = reinterpret_cast<VkPipeline>(13u);
				const auto ready = Renderer::Vulkan::BuildVulkanVirtualGeometryVisibilityPlan(
					21u, 16u, 64u, 32u, 8u,
					resources, pipelines, 3u, 4u);
				c.Expect(
					ready.ready && ready.boundedPrimitiveCount == 16u
						&& ready.pixelCount == 2048u && !ready.zeroWork,
					"Visibility independently bounds primitive work and the real pixel target before compute raster, resolve, and shading bins.");
			});

		r.Add("Vulkan.VirtualGeometry.PacketPublicationRejectsCrossViewGenerations",
			[](TestContext &context)
			{
				const std::filesystem::path sourcePath =
					std::filesystem::path(TERRAENGINE_SOURCE_DIR)
						/ "Engine/Renderer/RHI/Vulkan/VulkanContextResources.cpp";
				std::ifstream sourceStream(sourcePath, std::ios::binary);
				const std::string source{
					std::istreambuf_iterator<char>(sourceStream),
					std::istreambuf_iterator<char>() };
				context.Expect(
					source.find("belongsToCurrentPublication") != std::string::npos
						&& source.find("canonicalPacketPublication") != std::string::npos
						&& source.find("rejected-publication") != std::string::npos,
					"Vulkan must reject immutable packet input from another Scene, Game, reflection, or capture view before frame-owned resources are published.");
			});

		r.Add("Vulkan.VirtualGeometry.VisibilityPreparationIsDemandDriven",
			[](TestContext &context)
			{
				FrameRenderData frameData{};
				context.Expect(!frameData.virtualGeometryVisibilityDiagnosticsRequested,
					"Ordinary dense VG frames must not pay for the diagnostic compute-visibility mirror.");

				const std::filesystem::path sourcePath =
					std::filesystem::path(TERRAENGINE_SOURCE_DIR)
						/ "Engine/Renderer/RHI/Vulkan/VulkanContextResources.cpp";
				std::ifstream sourceStream(sourcePath, std::ios::binary);
				const std::string source{
					std::istreambuf_iterator<char>(sourceStream),
					std::istreambuf_iterator<char>() };
				const std::size_t productionGate = source.find(
					"productionComputeRasterRequested");
				const std::size_t gate = source.find(
					"if(!frameData.virtualGeometryVisibilityDiagnosticsRequested",
					productionGate);
				const std::size_t triangleExpansion = source.find(
					"std::vector<VirtualGeometryRasterTriangleUpload> visibilityTriangles");
				context.Expect(productionGate != std::string::npos
						&& gate != std::string::npos
						&& triangleExpansion != std::string::npos
						&& gate < triangleExpansion,
					"Diagnostic or production compute demand must be proven before CPU triangle expansion and pixel-buffer preparation.");
			});
	}
} // namespace TerraEngine::Tests
