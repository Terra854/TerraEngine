#include "TestHarness.h"

#include "Renderer/Graph/RenderWorkPacket.h"
#include "Renderer/RHI/Vulkan/VulkanCommandRecording.h"
#include "Renderer/RHI/Vulkan/VulkanDescriptorPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanPipelineStateKeys.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/RHI/Vulkan/VulkanTransitionPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::Tests {

	void RegisterVulkanRecordingSchedulingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanRecording.ReusesOnlyExactResidentAnimationPayload", [](TestContext& context) {
			auto first = std::make_shared<TerraEngine::Animation::GpuAnimationResidentPayloads>();
			first->programWords = {1u, 2u};
			auto equalBytesDifferentOwner =
				std::make_shared<TerraEngine::Animation::GpuAnimationResidentPayloads>(*first);
			context.Expect(
				TerraEngine::Renderer::Vulkan::
					CanReuseVulkanAnimationResidentPayloadUpload(first, first, true),
				"A retained frame-slot buffer may reuse the exact immutable cooked payload owner.");
			context.Expect(
				!TerraEngine::Renderer::Vulkan::
					CanReuseVulkanAnimationResidentPayloadUpload(
						first, equalBytesDifferentOwner, true)
				&& !TerraEngine::Renderer::Vulkan::
					CanReuseVulkanAnimationResidentPayloadUpload(first, first, false)
				&& !TerraEngine::Renderer::Vulkan::
					CanReuseVulkanAnimationResidentPayloadUpload({}, first, true),
				"A new publication, resized buffer, or empty prior identity must force a complete resident upload.");
			context.Expect(
				!TerraEngine::Renderer::Vulkan::
					ShouldRefreshVulkanAnimationBindlessBuffer(true, false)
				&& TerraEngine::Renderer::Vulkan::
					ShouldRefreshVulkanAnimationBindlessBuffer(false, false)
				&& TerraEngine::Renderer::Vulkan::
					ShouldRefreshVulkanAnimationBindlessBuffer(true, true),
				"Stable frame-slot descriptors stay untouched, while first publication and buffer replacement refresh them.");
		});

		registry.Add("Renderer.VulkanRecording.AnimationBackendReceiptRequiresRecordedSubmittedWork", [](TestContext& context) {
			TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot snapshot;
			context.Expect(!snapshot.HasSubmittedWork() && !snapshot.HasCompletedWork(),
				"An empty backend receipt must not be presented as evidence of GPU animation execution.");

			snapshot.latestSubmitted = {
				71U,
				3U,
				9U,
				4U,
				8U};
			context.Expect(snapshot.HasSubmittedWork() && !snapshot.HasCompletedWork(),
				"A submission receipt becomes valid only with frame, generation, serial, and nonzero recorded-dispatch identity.");

			snapshot.latestCompleted = snapshot.latestSubmitted;
			context.Expect(snapshot.HasCompletedWork(),
				"Fence-owned completion should preserve the exact submitted animation receipt without pose readback.");

			snapshot.latestSubmitted.recordedDispatchCount = 0U;
			context.Expect(!snapshot.HasSubmittedWork(),
				"A zero-dispatch plan must never satisfy truthful backend execution diagnostics.");
		});

		registry.Add("Renderer.VulkanRecording.AnimationPlanIsPublishedAfterBackendFrameReset", [](TestContext& context) {
			// Vulkan BeginFrame retires the reused slot and deliberately clears its
			// pending animation plan. Freeze the coordinator call order so a future
			// refactor cannot silently erase newly published compute work again.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream sourceStream(sourcePath, std::ios::binary);
			const std::string source{
				std::istreambuf_iterator<char>(sourceStream),
				std::istreambuf_iterator<char>()};
			const std::size_t beginFrame = source.find("rhi->BeginFrame();");
			const std::size_t consumePlan = source.find(
				"rhi->ConsumeRenderSubmissionPlan(renderSubmissionPlan)");
			context.Expect(beginFrame != std::string::npos
					&& consumePlan != std::string::npos
					&& beginFrame < consumePlan,
				"The backend frame slot must reset before it receives this frame's immutable animation plan.");
		});

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

		registry.Add("Renderer.VulkanRecording.CollapsesInstancedPipelineStateKeys", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			const TerraEngine::UUID sharedMesh = TerraEngine::GenerateUUID();
			const TerraEngine::UUID sharedMaterial = TerraEngine::GenerateUUID();
			const TerraEngine::UUID secondMaterial = TerraEngine::GenerateUUID();
			// Two instances share one complete pipeline identity. A third material
			// requires one additional key, independent of how many objects use it.
			snapshot.renderables.push_back({
				TerraEngine::GenerateUUID(), sharedMesh, sharedMaterial });
			snapshot.renderables.push_back({
				TerraEngine::GenerateUUID(), sharedMesh, sharedMaterial });
			snapshot.renderables.push_back({
				TerraEngine::GenerateUUID(), sharedMesh, secondMaterial });

			const auto keys =
				TerraEngine::Renderer::Vulkan::BuildPipelineStateKeys(snapshot);
			context.ExpectEqual(static_cast<std::size_t>(2u), keys.size(),
				"Instanced draws must share one immutable pipeline-state key.");
		});

		registry.Add("Renderer.VulkanRecording.BoundsVirtualGeometryMaterialPipelineKeys", [](TestContext& context) {
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			MaterialCapabilitySignature compatible{};
			compatible.requiredFeatureMask = ToVirtualGeometryMaterialFeatureMask(
				VirtualGeometryMaterialFeature::VisibilityBuffer);
			compatible.programmableFeatureMask = ToVirtualGeometryMaterialFeatureMask(
				VirtualGeometryMaterialFeature::ProgrammableShading);
			compatible.shadingBin = 3u;
			compatible.maximumShadingBins = 4u;
			compatible.compatibilitySignature = 0x45u;
			compatible.valid = true;
			const auto programmable = TerraEngine::Renderer::Vulkan::BuildPipelineStateKey(
				materialId, meshId, compatible);
			compatible.shadingBin = compatible.maximumShadingBins;
			const auto fallback = TerraEngine::Renderer::Vulkan::BuildPipelineStateKey(
				materialId, meshId, compatible);
			context.Expect(!programmable.usesIndexedFallback
				&& programmable.shadingBin == 3u
				&& fallback.usesIndexedFallback && fallback.shadingBin == 0u,
				"Only a valid bounded TMVG signature may select a programmable Vulkan pipeline key.");
		});

		registry.Add("Renderer.VulkanRecording.ConventionalIdentityVariantUsesDistinctPipelineKey", [](TestContext& context) {
			MaterialCapabilitySignature capability{};
			capability.maximumShadingBins = 1u;
			capability.compatibilitySignature = 0x92u;
			capability.valid = true;
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const auto target0Only = TerraEngine::Renderer::Vulkan::BuildPipelineStateKey(
				materialId, meshId, capability, false);
			const auto identityTarget = TerraEngine::Renderer::Vulkan::BuildPipelineStateKey(
				materialId, meshId, capability, true);
			context.Expect(
				!target0Only.usesConventionalVisibilityIdentity
					&& identityTarget.usesConventionalVisibilityIdentity
					&& target0Only.sortKey != identityTarget.sortKey,
				"The typed conventional visibility shader variant must never reuse a target0-only Vulkan pipeline key.");
		});

		registry.Add("Renderer.VulkanRecording.ConventionalIdentityReadbackIsTypedAndFailClosed", [](TestContext& context) {
			TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult empty{};
			empty.completed = true;
			empty.identity = glm::uvec4(0u);
			context.Expect(!empty.authoritative && empty.identity == glm::uvec4(0u),
				"A zero uint4 identity must never be presented as an authoritative GPU pick.");

			const std::filesystem::path vulkanSource = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContext.cpp";
			std::ifstream stream(vulkanSource, std::ios::binary);
			const std::string source{ std::istreambuf_iterator<char>(stream), {} };
			context.Expect(source.find("inFlightConventionalVisibilityIdentityReadbacks") != std::string::npos
				&& source.find("vkGetFenceStatus") != std::string::npos
				&& source.find("Identity pixel was zero") != std::string::npos,
				"The Vulkan identity path must publish only fence-completed, nonzero typed pixels.");
			const std::filesystem::path renderSystemSource = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Renderer" / "RenderSystem.cpp";
			std::ifstream renderSystemStream(renderSystemSource, std::ios::binary);
			const std::string renderSystem{ std::istreambuf_iterator<char>(renderSystemStream), {} };
			context.Expect(renderSystem.find("candidate.payloadDescriptorIndex == result.identity.z") != std::string::npos
				&& renderSystem.find("latestSceneAuthoritativeIdentityPick") != std::string::npos,
				"RenderSystem must map a completed identity only to its immutable packet descriptor record.");
		});

		registry.Add("Renderer.VulkanRecording.ConventionalIdentityBlendRequiresIndependentBlend", [](TestContext& context) {
			const std::filesystem::path vulkanContextSource = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContext.cpp";
			std::ifstream vulkanContextStream(vulkanContextSource, std::ios::binary);
			const std::string vulkanContext{ std::istreambuf_iterator<char>(vulkanContextStream), {} };
			const std::filesystem::path initializationSource = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContextInit.cpp";
			std::ifstream initializationStream(initializationSource, std::ios::binary);
			const std::string initialization{ std::istreambuf_iterator<char>(initializationStream), {} };

			// Target0-only pipelines deliberately differ from the integer identity
			// attachment, so the required core feature must be both checked and enabled.
			context.Expect(vulkanContext.find("identityBlendAttachment.colorWriteMask = 0u;")
					!= std::string::npos
				&& vulkanContext.find("identityBlendAttachment.blendEnable = VK_FALSE;")
					!= std::string::npos
				&& initialization.find("features2.features.independentBlend != VK_TRUE")
					!= std::string::npos
				&& initialization.find("deviceFeatures.independentBlend = VK_TRUE;")
					!= std::string::npos
				&& initialization.find("independentBlend is required for the conventional two-target visibility pass.")
					!= std::string::npos,
				"Conventional two-target pipelines must reject devices without and enable core independentBlend.");
		});

		registry.Add("Renderer.VulkanRecording.PrimaryCommandBuffersSubmitOncePerFrameSlot", [](TestContext& context) {
			const std::filesystem::path vulkanContextSource = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContext.cpp";
			std::ifstream vulkanContextStream(vulkanContextSource, std::ios::binary);
			const std::string vulkanContext{ std::istreambuf_iterator<char>(vulkanContextStream), {} };
			const std::filesystem::path resourceSource = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContextResources.cpp";
			std::ifstream resourceStream(resourceSource, std::ios::binary);
			const std::string resources{ std::istreambuf_iterator<char>(resourceStream), {} };
			// The marker is set after queue submission and cleared only when the
			// owning frame slot becomes reusable. Descriptor-mutation prefix
			// submissions must use the same ownership marker because asset
			// finalization can re-enter that path before the outer call unwinds.
			context.Expect(vulkanContext.find("pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] != 0u")
					!= std::string::npos
				&& resources.find("pendingPrimaryCommandBuffersSubmitted[currentFrame] = 1u;")
					!= std::string::npos
				&& resources.find("descriptor mutation editor render semaphore drain")
					!= std::string::npos
				&& resources.find("editorRenderSubmitted = false;")
					!= std::string::npos
				&& vulkanContext.find("BeginFrame") != std::string::npos,
				"EndFrame and descriptor mutation must reject a second primary submission and consume an editor render signal before reusing its binary semaphore.");
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
			// ImplementationStage adds four explicit animation stages: two worker-safe planning
			// stages and two protected recording/completion stages. Keeping them in
			// this count makes scheduler ownership changes visible to diagnostics.
			context.ExpectEqual(static_cast<std::size_t>(24u), plan.stages.size(), "The Vulkan threading plan should cover all selected renderer and animation threading opportunities.");
			context.Expect(plan.workerEligibleStageCount >= 21u, "All planning stages should be worker eligible when backend capabilities allow it.");
			context.ExpectEqual(static_cast<std::size_t>(3u), plan.coordinatorStageCount, "Final submission plus animation recording and completion must remain protected coordinator stages.");
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
			const auto compact = cache.BuildFramePreparation(
				snapshot, passes, profile, false);

			context.ExpectEqual(static_cast<std::size_t>(0u), first.cacheHitCount, "The first frame should populate preparation caches.");
			context.Expect(second.cacheHitCount >= 3u, "Stable frames should reuse descriptor, pipeline, and upload preparation caches.");
			context.ExpectEqual(static_cast<std::size_t>(1u), second.renderPacketCache.cacheHits, "Persistent draw packet signatures should hit for unchanged renderables.");
			context.ExpectEqual(static_cast<std::size_t>(0u), second.renderPacketCache.cacheMisses, "Unchanged renderables should not rebuild persistent draw packets.");
			context.ExpectEqual(static_cast<std::size_t>(1u), compact.renderPacketCache.packetCount,
				"Compact preparation should retain the packet population count.");
			context.ExpectEqual(static_cast<std::size_t>(0u), compact.renderPacketCache.cacheHits,
				"Compact preparation must skip diagnostic-only persistent packet scans.");
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
