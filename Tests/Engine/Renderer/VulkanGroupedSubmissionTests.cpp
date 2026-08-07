#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadEngineSource(const char* relativePath)
		{
			// This focused contract covers the protected Vulkan boundary without
			// needing a physical GPU in the unit-test process.
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ relativePath;
			std::ifstream stream(sourcePath, std::ios::binary);
			return stream ? std::string(std::istreambuf_iterator<char>(stream), {}) : std::string{};
		}
	}

	void RegisterVulkanGroupedSubmissionTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanGroupedSubmission.BuildsOnePlanForCompatibleGroup", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			for (std::uint32_t index = 0u; index < 4u; ++index)
			{
				RenderFrameRenderableRecord record;
				record.objectId = GenerateUUID();
				record.meshId = GenerateUUID();
				record.materialId = GenerateUUID();
				record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 2u, 1u };
				snapshot.renderables.push_back(record);
			}
			for (std::size_t index = 1u; index < snapshot.renderables.size(); ++index)
			{
				snapshot.renderables[index].meshId = snapshot.renderables.front().meshId;
				snapshot.renderables[index].materialId = snapshot.renderables.front().materialId;
			}
			const auto topology = BuildPersistentRenderTopology(snapshot, 3u);
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1920u, 1080u), 1u, 4u };
			const auto dynamic = BuildDynamicRenderFrameState(snapshot, topology, view, 9u);
			const auto plan = TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(dynamic, 8u, 2u);
			context.Expect(plan.valid, "A complete compatible dynamic state must produce a valid grouped stream plan.");
			context.ExpectEqual(static_cast<std::size_t>(4), plan.instanceCount, "Grouped stream plan must include every compatible instance record.");
			context.ExpectEqual(static_cast<std::size_t>(3), plan.avoidedSubmissionCount, "Grouped plan must report avoided per-model submissions.");
		});

		registry.Add("Renderer.VulkanGroupedSubmission.CapacityAndGenerationFailuresFallbackAtomically", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			DynamicRenderFrameState state;
			state.valid = true;
			state.frameGeneration = 9u;
			state.rendererGeneration = 3u;
			state.instances.resize(2u);
			SharedRenderGroup group;
			group.key.meshId = GenerateUUID();
			group.key.materialId = GenerateUUID();
			group.key.meshGeneration = 1u;
			group.key.materialGeneration = 1u;
			group.key.pipelineGeneration = 1u;
			group.key.residencyGeneration = 1u;
			group.key.rendererGeneration = 3u;
			group.firstInstance = 0u;
			group.instanceCount = 2u;
			group.executable = true;
			state.groups.push_back(group);
			const auto rejected = TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(state, 1u, 3u);
			context.Expect(!rejected.valid && rejected.requiresCompleteFallback, "Capacity failure must reject the whole grouped stream and request complete fallback.");
			const auto stale = TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(state, 8u, 4u);
			context.Expect(!stale.valid && stale.rejectionReason == RenderGroupRejectionReason::StaleGeneration, "Renderer-generation mismatch must reject grouped publication atomically.");
		});

		registry.Add("Renderer.VulkanGroupedSubmission.ValidatesOwnedSpansWithoutFrameCopies", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			std::vector<RenderInstanceRecord> instances(3u);
			SharedRenderGroup group;
			group.key.meshId = GenerateUUID();
			group.key.materialId = GenerateUUID();
			group.key.meshGeneration = 4u;
			group.key.materialGeneration = 4u;
			group.key.pipelineGeneration = 4u;
			group.key.residencyGeneration = 4u;
			group.key.rendererGeneration = 7u;
			group.firstInstance = 0u;
			group.instanceCount = 3u;
			group.executable = true;
			const std::array groups{group};
			const auto plan = TerraEngine::Renderer::Vulkan::BuildGroupedInstanceStreamPlan(
				std::span<const RenderInstanceRecord>(instances),
				std::span<const SharedRenderGroup>(groups), 11u, 7u, true, 8u, 7u);

			context.Expect(plan.valid && !plan.requiresCompleteFallback,
				"Owned render spans must validate without copying through a temporary dynamic frame.");
			context.ExpectEqual(static_cast<std::size_t>(3), plan.instanceCount,
				"The zero-copy validation path must preserve the complete grouped population.");
			context.ExpectEqual(static_cast<std::size_t>(2), plan.avoidedSubmissionCount,
				"The span path must preserve grouped submission savings.");
		});

		registry.Add("Renderer.VulkanGroupedSubmission.PageLocalDeviceAuthoredGroupsUseAtomicFrameSlotIndirectFallback", [](TestContext& context)
		{
			const std::string source = ReadEngineSource("Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");
			const std::string renderSystemSource = ReadEngineSource("Engine/Renderer/RenderSystem.cpp");
			const std::string taskShader = ReadEngineSource("Engine/Shaders/Default/Default.task.hlsl");
			const std::size_t uploadGate = source.find("if(groupedInstanceStreamReady");
			const std::size_t groupedGate = source.find("if(groupedInstanceStreamReady", uploadGate + 1u);
			const std::size_t groupedGateEnd = source.find('{', groupedGate);
			const std::size_t memberValidation = source.rfind(
				"const auto memberMatchesRepresentative", groupedGate);
			const std::size_t memberValidationEnd = source.find(
				"if(groupedInstanceStreamReady", memberValidation);
			const std::string memberValidationBody = memberValidation != std::string::npos
				&& memberValidationEnd != std::string::npos
				? source.substr(memberValidation, memberValidationEnd - memberValidation)
				: std::string{};
			const std::size_t generatedSelection = source.find("draw.useGeneratedVirtualGeometryIndirect = deviceAuthoredCommand", groupedGate);
			const std::string groupedGateExpression = groupedGate != std::string::npos
				&& groupedGateEnd != std::string::npos
				? source.substr(groupedGate, groupedGateEnd - groupedGate)
				: std::string{};

			context.Expect(groupedGate != std::string::npos
				&& memberValidationBody.find("member.virtualGeometryPageId") == std::string::npos
				&& memberValidationBody.find("member.virtualGeometryPacketDescriptorIndex") == std::string::npos
				&& groupedGateExpression.find("!pageLocalGeometry") == std::string::npos
				&& groupedGateExpression.find("!deviceAuthoredCommand") == std::string::npos,
				"Validated page-local and device-authored VG groups must leave packet/page selectors to the per-instance stream instead of rejecting them at the representative gate.");
			context.Expect(renderSystemSource.find("Packet descriptors, page IDs, page byte ranges") != std::string::npos
				&& renderSystemSource.find("a.virtualGeometryPacketDescriptorIndex") == std::string::npos
				&& renderSystemSource.find("a.virtualGeometryPageId") == std::string::npos,
				"RenderSystem compatibility buckets must not split page-local crowds by selectors that the task shader reads per instance.");
			context.Expect(generatedSelection != std::string::npos,
				"A grouped device-authored packet must use the validated frame-slot indirect count instead of a one-packet generated command.");
			context.Expect(taskShader.find("TryLoadRenderInstance(") != std::string::npos
				&& taskShader.find("groupedInstanceRecord.geometrySelectors.y") != std::string::npos
				&& taskShader.find("TryUsePageLocalGeometryForSource(packetDescriptorIndexForInstance") != std::string::npos
				&& taskShader.find("cullingEnabled = (!groupedInstances || groupedInstanceRecordValid)") != std::string::npos
				&& taskShader.find("gTaskPayload.selectedVirtualLods[visibleCount]") != std::string::npos
				&& taskShader.find("mul(cullingModelMatrix, float4(bounds.center, 1.0f))") != std::string::npos,
				"Grouped task work must cull from the current member's validated transform and packet/page selectors, while invalid stream records fail open.");
		});

		registry.Add("Renderer.VulkanGroupedSubmission.MultiOwnerAnimationUsesPerInstanceTaskBridge", [](TestContext& context)
		{
			const std::string source = ReadEngineSource(
				"Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");
			const std::string taskShader = ReadEngineSource(
				"Engine/Shaders/Default/Default.task.hlsl");
			const std::string meshShader = ReadEngineSource(
				"Engine/Shaders/Default/Default.mesh.hlsl");
			const std::size_t bridgeProof = source.find(
				"const bool groupedPerInstanceAnimationBridgeAvailable");
			const std::size_t taskPipelineSelection = source.find(
				"if (collectionId == defaultMeshCollectionId", bridgeProof);
			const std::size_t taskPipelineSelectionEnd = source.find(
				"collectionId = defaultTaskCollectionId", taskPipelineSelection);
			const std::string taskPipelineSelectionBody =
				bridgeProof != std::string::npos
					&& taskPipelineSelection != std::string::npos
					&& taskPipelineSelectionEnd != std::string::npos
				? source.substr(bridgeProof,
					taskPipelineSelectionEnd - bridgeProof)
				: std::string{};

			context.Expect(
				taskPipelineSelectionBody.find("groupedInstanceStreamReady")
					!= std::string::npos
				&& taskPipelineSelectionBody.find(
					"groupedSubmissionByCommand[commandIndex] >= 0")
					!= std::string::npos
				&& taskPipelineSelectionBody.find(
					"|| groupedPerInstanceAnimationBridgeAvailable")
					!= std::string::npos,
				"A fully resolved grouped stream must select the task pipeline even when a multi-owner animation has no canonical page-deformation group.");
			context.Expect(
				taskShader.find(
					"? groupedInstanceRecord.animationSelectors.x")
					!= std::string::npos
				&& taskShader.find(
					"? groupedInstanceRecord.animationSelectors.y")
					!= std::string::npos
				&& meshShader.find(
					"gMeshShaderAnimationBufferIndex = instanceRecord.animationSelectors.x")
					!= std::string::npos
				&& meshShader.find(
					"gMeshShaderAnimationRecordIndex = instanceRecord.animationSelectors.y")
					!= std::string::npos,
				"Grouped task and mesh stages must consume each member's exact animation bridge instead of the representative push constants.");
		});

		registry.Add("Renderer.VulkanGroupedSubmission.UploadUsesCacheableReusableStaging", [](TestContext& context)
		{
			const std::string source = ReadEngineSource(
				"Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");
			context.Expect(source.find(
				"thread_local std::vector<glm::uvec2> resolvedAnimationSelectors")
					!= std::string::npos
				&& source.find(
					"thread_local std::vector<RenderInstanceRecord> stagedGroupedInstanceRecords")
					== std::string::npos,
				"Grouped uploads must retain only compact selector staging capacity between frames, not a second full instance stream.");
			context.Expect(source.find(
				"std::memcpy(mapped, frameData.groupedInstanceRecords.data()") != std::string::npos
				&& source.find("uploadedRecords[recordIndex].animationSelectors.x")
					!= std::string::npos,
				"The authoritative instance stream must upload once before only its backend-owned selector words are patched.");
		});
	}
}
