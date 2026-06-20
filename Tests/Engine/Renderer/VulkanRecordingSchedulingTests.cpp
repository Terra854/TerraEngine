#include "TestHarness.h"

#include "Renderer/Graph/RenderWorkPacket.h"
#include "Renderer/RHI/Vulkan/VulkanCommandRecording.h"
#include "Renderer/RHI/Vulkan/VulkanDescriptorPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanPipelineStateKeys.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/RHI/Vulkan/VulkanTransitionPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"

namespace TerraEngine::Tests {

	void RegisterVulkanRecordingSchedulingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanRecording.BuildsWorkerBatchesFromPackets", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 5u;
			TerraEngine::Renderer::RenderWorkPacketPlan packetPlan;
			TerraEngine::Renderer::RenderWorkPacket packet;
			packet.packetId = "draw.0";
			packet.passId = "draw";
			packet.executionPolicy = TerraEngine::Renderer::RenderPacketExecutionPolicy::Worker;
			packetPlan.packets.push_back(packet);
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;

			const auto plan = TerraEngine::Renderer::BuildBackendRecordingBatchPlan(snapshot, packetPlan, profile, TerraEngine::Renderer::RenderBackendWorkKind::Geometry);
			const auto vulkanPlan = TerraEngine::Renderer::Vulkan::BuildVulkanRecordingBatchPlan(snapshot, packetPlan, profile, TerraEngine::Renderer::RenderBackendWorkKind::Geometry);
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.workerBatchCount, "Worker packets should become worker recording batches.");
			context.ExpectEqual(plan.workerBatchCount, vulkanPlan.workerBatchCount, "Vulkan batch planning should preserve backend-neutral worker counts.");
			context.Expect(plan.batches.front().batchId.find("geometry") != std::string::npos, "Batch ids should include deterministic work kind names.");
		});

		registry.Add("Renderer.VulkanRecording.PreparesImmutableBackendPlans", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 6u;
			snapshot.resourceRevision = 77u;
			snapshot.materials.push_back({ TerraEngine::GenerateUUID(), "mat", true, false });
			snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "tex", false, true });
			snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });

			const auto descriptors = TerraEngine::Renderer::Vulkan::BuildDescriptorBindingPlan(snapshot, 2u);
			const auto uploads = TerraEngine::Renderer::Vulkan::BuildUploadPreparationPlan(snapshot);
			const auto keys = TerraEngine::Renderer::Vulkan::BuildPipelineStateKeys(snapshot);

			context.ExpectEqual(static_cast<std::uint64_t>(6u), descriptors.frameId, "Descriptor plans should carry immutable frame identity.");
			context.ExpectEqual(static_cast<std::uint64_t>(77u), uploads.resourceRevision, "Upload plans should preserve resource revisions.");
			context.ExpectEqual(static_cast<std::size_t>(1u), keys.size(), "Pipeline keys should be generated from snapshot renderables.");
		});

		registry.Add("Renderer.VulkanRecording.ThreadPlanCoversAllThreadingStages", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 9u;
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
			profile.supportsSecondaryRecording = true;
			profile.supportsParallelShadowRecording = true;
			profile.supportsParallelPerViewRecording = true;
			profile.supportsParallelPipelineStatePreparation = true;
			profile.supportsParallelDescriptorPreparation = true;
			profile.supportsParallelUploadPlanning = true;
			profile.supportsParallelTransitionPlanning = true;
			profile.supportsParallelCulling = true;
			profile.supportsParallelLightPreparation = true;
			profile.supportsParallelUiPreparation = true;
			profile.supportsPacketLocalDiagnostics = true;
			profile.supportsRenderTaskGroups = true;

			const auto plan = TerraEngine::Renderer::Vulkan::BuildVulkanRendererThreadingPlan(snapshot, profile);
			context.ExpectEqual(static_cast<std::size_t>(20u), plan.stages.size(), "The Vulkan threading plan should cover all selected renderer threading opportunities.");
			context.Expect(plan.workerEligibleStageCount >= 19u, "All non-submit ownership stages should be worker eligible when backend capabilities allow it.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.coordinatorStageCount, "Final submit, present, resize, and resource retirement should remain the only required coordinator stage.");
			context.Expect(plan.stages.back().stageId == "final-submit-present-retire", "The coordinator boundary should be explicit and easy to diagnose.");
		});

		registry.Add("Renderer.VulkanRecording.CachesStablePreparationPlans", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 10u;
			snapshot.resourceRevision = 44u;
			snapshot.sceneRevision = 91u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 2u });
			snapshot.materials.push_back({ TerraEngine::GenerateUUID(), "mat", true, false });
			snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "tex", true, false });
			snapshot.meshes.push_back({ TerraEngine::GenerateUUID(), "mesh", true, false });
			snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), snapshot.meshes.front().resourceId, snapshot.materials.front().resourceId });

			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
			profile.supportsParallelCulling = true;
			profile.supportsParallelDescriptorPreparation = true;
			profile.supportsParallelPipelineStatePreparation = true;
			profile.supportsParallelUploadPlanning = true;
			profile.supportsParallelUiPreparation = true;
			profile.maxRecordingWorkers = 4u;
			const std::vector<TerraEngine::Renderer::RenderPassNode> passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);

			TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache cache;
			const auto first = cache.BuildFramePreparation(snapshot, passes, profile);
			const auto second = cache.BuildFramePreparation(snapshot, passes, profile);

			context.ExpectEqual(static_cast<std::size_t>(0u), first.cacheHitCount, "The first frame should populate preparation caches.");
			context.Expect(second.cacheHitCount >= 3u, "Stable frames should reuse descriptor, pipeline, and upload preparation caches.");
			context.ExpectEqual(static_cast<std::size_t>(1u), second.renderPacketCache.cacheHits, "Persistent draw packet signatures should hit for unchanged renderables.");
			context.ExpectEqual(static_cast<std::size_t>(0u), second.renderPacketCache.cacheMisses, "Unchanged renderables should not rebuild persistent draw packets.");
		});

		registry.Add("Renderer.VulkanRecording.InvalidatesUploadPlansAndRetiresResourcesDeterministically", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 20u;
			snapshot.resourceRevision = 1u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(640u, 360u), 1u });
			snapshot.meshes.push_back({ TerraEngine::GenerateUUID(), "streaming-mesh", false, false });
			snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "streaming-texture", false, false });

			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelUploadPlanning = true;
			profile.supportsParallelRecording = true;
			profile.maxRecordingWorkers = 2u;
			const std::vector<TerraEngine::Renderer::RenderPassNode> passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);

			TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache cache;
			const auto first = cache.BuildFramePreparation(snapshot, passes, profile);
			snapshot.resourceRevision = 2u;
			const auto second = cache.BuildFramePreparation(snapshot, passes, profile);

			cache.EnqueueResourceForRetirement({ 7u, 21u, "old-buffer" });
			const auto tooEarly = cache.RetireCompletedResources(20u);
			const auto retired = cache.RetireCompletedResources(21u);

			context.Expect(first.uploadPlan.queuedForUploadWorker, "Missing resources should create immutable upload packets for worker preparation.");
			context.Expect(!second.uploadPlan.cacheHit, "Changing the resource revision should invalidate upload preparation.");
			context.ExpectEqual(static_cast<std::size_t>(0u), tooEarly.size(), "Resource retirement must wait until the completed frame reaches the fence frame.");
			context.ExpectEqual(static_cast<std::size_t>(1u), retired.size(), "Completed frame retirement should release eligible resource records deterministically.");
		});

		registry.Add("Renderer.VulkanRecording.BuildsAllRemainingPreparationPlans", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 30u;
			snapshot.viewRequests.push_back({ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(1920u, 1080u), 1u });
			for (std::size_t index = 0u; index < 128u; ++index)
			{
				snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });
				snapshot.materials.push_back({ snapshot.renderables.back().materialId, "mat", true, false });
			}
			snapshot.lights.push_back({ TerraEngine::GenerateUUID(), 1u, true, false });

			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
			profile.supportsSecondaryRecording = true;
			profile.supportsParallelShadowRecording = true;
			profile.supportsParallelCulling = true;
			profile.supportsParallelPipelineStatePreparation = true;
			profile.supportsParallelUiPreparation = true;
			profile.maxRecordingWorkers = 8u;

			const auto visibility = TerraEngine::Renderer::Vulkan::BuildVisibilityPreparationPlan(snapshot, profile);
			const auto indirect = TerraEngine::Renderer::Vulkan::BuildIndirectDrawPreparationPlan(snapshot, profile);
			const auto shadows = TerraEngine::Renderer::Vulkan::BuildShadowRecordingPreparationPlan(snapshot, profile);
			const auto shaderMaterials = TerraEngine::Renderer::Vulkan::BuildShaderMaterialPreparationPlan(snapshot, profile);
			const auto ui = TerraEngine::Renderer::Vulkan::BuildUiGeometryPreparationPlan(snapshot, profile);

			context.Expect(visibility.workerEligible, "Large visibility and light assignment work should be worker eligible.");
			context.Expect(indirect.gpuDrivenEligible, "Renderable snapshots should be convertible into GPU-driven indirect draw preparation.");
			context.ExpectEqual(static_cast<std::size_t>(6u), shadows.secondaryRecordingCount, "Point-light shadows should prepare one secondary recording per cube face.");
			context.Expect(shaderMaterials.workerEligible, "Large material sets should be prepared off the coordinator path.");
			context.ExpectEqual(static_cast<std::size_t>(6u), ui.estimatedVertexCount, "UI preparation should estimate a quad for each renderable UI view.");
		});
	}

}
