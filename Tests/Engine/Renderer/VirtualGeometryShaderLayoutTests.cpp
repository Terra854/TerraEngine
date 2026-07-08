#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanHiZ.h"
#include "Renderer/RenderTypes.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadDefaultFragmentShader()
		{
			// Shader debug modes are a source-level contract: the C++ enum and
			// HLSL constants must move together because the push constant only
			// carries a small numeric value into the fragment shader.
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/Default/Default.frag.hlsl";
			std::ifstream input(shaderPath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadDefaultTaskShader()
		{
			// Hi-Z culling behavior lives in shader source, so this helper lets
			// regression tests verify the task-stage contract without depending
			// on a specific graphics device being available on the test runner.
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/Default/Default.task.hlsl";
			std::ifstream input(shaderPath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadDefaultMeshShader()
		{
			// Mesh shader debug propagation is also a source-level contract; the
			// packet/batch counters are packed into interpolators before the
			// fragment visualizer can color selected virtual geometry.
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/Default/Default.mesh.hlsl";
			std::ifstream input(shaderPath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadVirtualGeometryPacketCompactionShader()
		{
			// The compaction shader is a separate source-level contract because
			// it will become the GPU authority for candidate packet filtering.
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/VirtualGeometryPacketCompaction.comp.hlsl";
			std::ifstream input(shaderPath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadVulkanContextSource()
		{
			// The Vulkan draw path owns the final push-constant submission.
			// Hi-Z culling depends on the task shader receiving its reflected
			// byte range even when another stage has a smaller legacy range.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Renderer/RHI/Vulkan/VulkanContext.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadVulkanContextImplSource()
		{
			// The implementation header owns backend lifetime state. Reading it
			// here lets the regression tests verify that resize intent is stored
			// until BeginFrame can safely recreate Vulkan objects.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Renderer/RHI/Vulkan/VulkanContextImpl.h";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadVulkanContextResourcesSource()
		{
			// Hi-Z descriptor publication is also a source-level contract for
			// editor multi-view rendering: one missing view must not block the
			// other view from sampling its current pyramid.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Renderer/RHI/Vulkan/VulkanContextResources.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadVulkanContextInitSource()
		{
			// Vulkan lifetime rules are split between backend initialization and
			// resource upload helpers, so tests read this file when validating
			// command-pool ownership contracts.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		std::string ReadRenderSystemSource()
		{
			// Virtual-geometry packet expansion owns the mapping from selected
			// clusters to submitted packet descriptors. Tests read the source so
			// package-import edge cases keep using packet ids instead of selection
			// row offsets when addressing page-local payload streams.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Renderer/RenderSystem.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		bool ShaderContains(const std::string& shader, const std::string& token)
		{
			return shader.find(token) != std::string::npos;
		}
	}

	void RegisterVirtualGeometryShaderLayoutTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.ShaderLayout.PushConstantsCarrySelectedLodRange", [](TestContext& context) {
			context.Expect(offsetof(PushConstants, virtualMeshletOffset) > offsetof(PushConstants, indirectDrawOffset), "Virtual geometry range should follow indirect draw metadata.");
			context.ExpectEqual(offsetof(PushConstants, virtualMeshletOffset) + sizeof(std::uint32_t), offsetof(PushConstants, virtualMeshletCount), "Virtual meshlet offset/count should be tightly packed.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualLod) + sizeof(std::uint32_t), offsetof(PushConstants, virtualGeometryFlags), "Selected LOD and flags should mirror shader uint fields.");
			context.ExpectEqual(offsetof(PushConstants, virtualGeometryFlags) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualClusterOffset), "Virtual cluster payload should begin immediately after virtual geometry flags.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualClusterOffset) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualClusterCount), "Selected cluster offset/count should be tightly packed for mesh shader decoding.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualPageNeedOffset) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualPageNeedCount), "Page need offset/count should be tightly packed for streaming diagnostics.");
			context.ExpectEqual(offsetof(PushConstants, virtualGeometryPagePayloadByteSize) + sizeof(std::uint32_t), offsetof(PushConstants, virtualGeometryPacketDescriptorIndex), "Page-local packet descriptor id should occupy the final scalar slot before editor tint padding.");
			context.Expect((offsetof(PushConstants, editorTint) % 16u) == 0u, "Editor tint should remain 16-byte aligned after virtual geometry push constants.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.ClusterSelectionRecordsStayCompact", [](TestContext& context) {
			context.Expect(sizeof(TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord) <= 64u, "Selected cluster records should stay compact enough for per-view payload uploads.");
			context.Expect(sizeof(TerraEngine::Renderer::VirtualGeometryPageNeedRecord) <= 80u, "Page need records should stay compact enough for streaming handoff snapshots.");
			context.ExpectEqual(static_cast<std::size_t>(48u), sizeof(MeshletCullingCountersGPU), "Meshlet GPU counters should include the Hi-Z diagnostic breakdown while remaining 16-byte aligned.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US1.GpuPacketAndBatchRecordsStayUploadable", [](TestContext& context) {
			context.Expect(std::is_copy_constructible_v<GpuVirtualGeometryPacketRecord>, "GPU virtual geometry packet records should remain value-copyable for immutable frame snapshots.");
			context.Expect(std::is_copy_constructible_v<GpuVirtualGeometryBatchDescriptor>, "GPU virtual geometry batch descriptors should remain value-copyable for immutable frame snapshots.");
			context.Expect(sizeof(GpuVirtualGeometryPacketRecord) <= 224u, "GPU packet mirrors should stay compact enough for dense per-view packet buffers.");
			context.Expect(sizeof(GpuVirtualGeometryBatchDescriptor) <= 160u, "GPU batch descriptors should stay compact enough for bounded packet batches.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.SelectedClusterRecordsCarrySparseIdentity", [](TestContext& context) {
			using TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord;
			context.Expect(std::is_standard_layout_v<VirtualGeometrySelectedClusterRecord>, "Selected cluster records should stay standard-layout for future GPU upload buffers.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, clusterId) < offsetof(VirtualGeometrySelectedClusterRecord, lodIndex), "Cluster id should be stored before LOD metadata for compact shader decoding.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, pageId) < offsetof(VirtualGeometrySelectedClusterRecord, fallbackClusterId), "Page id and fallback cluster id should both be present, not replaced by a selected-cluster counter only.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, usingFallback) > offsetof(VirtualGeometrySelectedClusterRecord, fallbackClusterId), "Fallback state should travel with each sparse cluster record.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US1.PacketBatchMetadataStaysVisible", [](TestContext& context) {
			const std::string taskShader = ReadDefaultTaskShader();
			const std::string meshShader = ReadDefaultMeshShader();

			context.Expect(ShaderContains(taskShader, "selectedVirtualClusterCount; // Number of selected clusters in this packet batch draw."), "Task shader push constants should document that cluster count can describe a packet batch.");
			context.Expect(ShaderContains(taskShader, "this draw references one record or a packet run"), "Task shader should preserve the packet-run contract without changing meshlet output.");
			context.Expect(ShaderContains(taskShader, "virtualGeometryPacketBufferIndex"), "Task shader push constants should carry the uploaded GPU packet table binding.");
			context.Expect(ShaderContains(taskShader, "virtualGeometryBatchBufferIndex"), "Task shader push constants should carry the uploaded GPU batch descriptor binding.");
			context.Expect(ShaderContains(taskShader, "LoadVirtualGeometryPacketRunSeed"), "Task shader should read packet-run metadata from the bindless packet table when it is available.");
			context.Expect(ShaderContains(meshShader, "packet-run ownership"), "Mesh shader debug propagation should name packet-run ownership.");
			context.Expect(ShaderContains(meshShader, "ResolveVirtualGeometryPacketRunDebugId"), "Mesh shader should derive debug identity from uploaded packet metadata when packet buffers are available.");
			context.Expect(ShaderContains(meshShader, "globalBuffers[NonUniformResourceIndex(bufferIndex)]"), "Mesh shader should use the global bindless buffer table for packet debug metadata.");
			context.Expect(ShaderContains(meshShader, "SceneUniforms.selectedVirtualClusterCount"), "Mesh shader should continue packing the selected packet count into debug data.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US3.PacketCompactionComputeShaderKeepsFailOpenContract", [](TestContext& context) {
			const std::string shader = ReadVirtualGeometryPacketCompactionShader();

			context.Expect(!shader.empty(), "Virtual geometry packet compaction shader should be present in Engine/Shaders/VirtualGeometry.");
			context.Expect(ShaderContains(shader, "StructuredBuffer<GpuVirtualGeometryPacketRecord> candidatePackets"), "Compaction shader should read candidate packet records.");
			context.Expect(ShaderContains(shader, "RWStructuredBuffer<GpuVirtualGeometryPacketRecord> compactedPackets"), "Compaction shader should write compacted packet records.");
			context.Expect(ShaderContains(shader, "RWStructuredBuffer<uint4> generatedSubmissions"), "Compaction shader should publish generated submission metadata.");
			context.Expect(ShaderContains(shader, "RWStructuredBuffer<GpuPacketCompactionCounters> compactionCounters"), "Compaction shader should publish candidate, compacted, rejected, and fail-open counters.");
			context.Expect(ShaderContains(shader, "Compaction.failOpenOnInvalid"), "Compaction shader should preserve fail-open coverage for invalid or missing-resource packets.");
			context.Expect(ShaderContains(shader, "ProjectedBoundsOutsideView"), "Compaction shader should own frustum-style projected-bounds rejection.");
			context.Expect(ShaderContains(shader, "compactedIndex >= Compaction.outputCapacity"), "Compaction shader should guard output overflow instead of writing past compacted capacity.");
			context.Expect(ShaderContains(shader, "[numthreads(64, 1, 1)]"), "Compaction shader should use an explicit, bounded thread-group size.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US4.PageLocalPacketsPreserveMaterialAndPayloadIdentity", [](TestContext& context) {
			const std::string shader = ReadVirtualGeometryPacketCompactionShader();
			const std::string fragmentShader = ReadDefaultFragmentShader();

			context.Expect(offsetof(GpuVirtualGeometryPacketRecord, materialId) < offsetof(GpuVirtualGeometryPacketRecord, pageId), "GPU packet records should carry material identity before page-local payload metadata.");
			context.Expect(offsetof(GpuVirtualGeometryPacketRecord, pageId) < offsetof(GpuVirtualGeometryPacketRecord, payloadSource), "Page id should stay adjacent to payload source metadata.");
			context.Expect(offsetof(GpuVirtualGeometryPacketRecord, payloadSource) < offsetof(GpuVirtualGeometryPacketRecord, pagePayloadByteOffset), "Payload source should be hashed and uploaded before the page-local byte range.");
			context.Expect(ShaderContains(shader, "uint4 materialIdWords;"), "Compaction shader packets should keep material identity for page-local batch boundaries.");
			context.Expect(ShaderContains(shader, "uint pageId;"), "Compaction shader packets should carry the requested page id.");
			context.Expect(ShaderContains(shader, "uint fallbackPageId;"), "Compaction shader packets should carry fallback page identity.");
			context.Expect(ShaderContains(shader, "uint payloadSource;"), "Compaction shader packets should label the resident payload source.");
			context.Expect(ShaderContains(shader, "uint emittedSource;"), "Compaction shader packets should label whether high detail or fallback coverage is emitted.");
			context.Expect(ShaderContains(shader, "uint2 pagePayloadByteOffset;"), "Compaction shader packets should keep page-local payload byte offsets.");
			context.Expect(ShaderContains(shader, "uint payloadBufferIndex;"), "Compaction shader packets should carry physical page payload buffer bindings.");
			context.Expect(ShaderContains(shader, "uint payloadDescriptorIndex;"), "Compaction shader packets should carry physical page payload descriptor indices.");
			context.Expect(ShaderContains(shader, "compactedPackets[compactedIndex] = packet;"), "Compaction shader should preserve material/page/payload identity when copying surviving packets.");
			context.Expect(ShaderContains(fragmentShader, "uint virtualGeometryPagePayloadBufferIndex;"), "Fragment push constants should keep the page payload buffer field so all default stages share the same range.");
			context.Expect(ShaderContains(fragmentShader, "uint virtualGeometryPayloadSource;"), "Fragment push constants should keep payload source identity even when fragment shading only reads common material/debug fields.");
			context.Expect(ShaderContains(fragmentShader, "uint virtualGeometryEmittedSource;"), "Fragment push constants should keep emitted source identity so the default push range stays byte-compatible across stages.");
			context.Expect(ShaderContains(fragmentShader, "uint virtualGeometryPagePayloadByteOffset;"), "Fragment push constants should keep page-local payload byte offsets to preserve the default Vulkan push range.");
			context.Expect(ShaderContains(fragmentShader, "uint virtualGeometryPagePayloadByteSize;"), "Fragment push constants should keep page-local payload byte sizes to preserve the default Vulkan push range.");
			context.Expect(ShaderContains(fragmentShader, "uint virtualGeometryPacketDescriptorIndex;"), "Fragment push constants should mirror the packet descriptor id so all default stages share one layout.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US2.PhysicalPagePayloadDescriptorWordsAreStable", [](TestContext& context) {
			const std::string taskShader = ReadDefaultTaskShader();
			const std::string meshShader = ReadDefaultMeshShader();
			const std::string vulkanResources = ReadVulkanContextResourcesSource();

			context.Expect(ShaderContains(taskShader, "kVirtualGeometryPayloadDescriptorStrideWords = 32u"), "Task shader should know the physical page payload descriptor stride.");
			context.Expect(ShaderContains(taskShader, "kVirtualGeometryPayloadDescriptorBufferIndexWord = 8u"), "Task shader should read the physical payload buffer binding word.");
			context.Expect(ShaderContains(taskShader, "kVirtualGeometryPayloadDescriptorIndexWord = 9u"), "Task shader should read the physical payload descriptor index word.");
			context.Expect(ShaderContains(taskShader, "LoadVirtualGeometryPagePayloadDescriptorWord"), "Task shader should read page payload descriptors through a single helper.");
			context.Expect(ShaderContains(meshShader, "kVirtualGeometryPayloadDescriptorStrideWords = 32u"), "Mesh shader should share the physical page payload descriptor stride.");
			context.Expect(ShaderContains(meshShader, "physicalPayloadBindingSeed"), "Mesh shader debug identity should include physical page payload bindings.");
			context.Expect(!ShaderContains(meshShader, "Geometry emission continues to come from"), "Mesh shader source must not document resident page payloads as metadata-only owning-mesh rendering.");
			context.Expect(ShaderContains(vulkanResources, "Virtual Geometry Physical Page Payload Descriptor Buffer"), "Vulkan should publish a physical page payload descriptor buffer, not a metadata-only source table.");
			context.Expect(ShaderContains(vulkanResources, "packet.payloadBufferIndex"), "Vulkan descriptor publication should include packet payload buffer bindings.");
			context.Expect(ShaderContains(vulkanResources, "packet.payloadDescriptorIndex"), "Vulkan descriptor publication should include packet payload descriptor indices.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US2.PageLocalGeometryStreamsAreShaderDecodable", [](TestContext& context) {
			const std::string taskShader = ReadDefaultTaskShader();
			const std::string meshShader = ReadDefaultMeshShader();
			const std::string vulkanResources = ReadVulkanContextResourcesSource();
			const std::string renderSystem = ReadRenderSystemSource();

			context.Expect(ShaderContains(taskShader, "TryLoadPageLocalPayloadDescriptor"), "Task shader should validate page-local stream descriptors before culling resident page packets.");
			context.Expect(ShaderContains(taskShader, "LoadPageLocalMeshletRecord"), "Task shader should cull resident pages from page-local meshlet records.");
			context.Expect(ShaderContains(taskShader, "LoadPageLocalMeshletBounds"), "Task shader should cull resident pages from page-local bounds.");
			context.Expect(ShaderContains(taskShader, "DecodePageLocalMeshletPosition"), "Task shader Hi-Z projection should reconstruct positions from page-local vertex/remap streams.");
			context.Expect(ShaderContains(taskShader, "SceneUniforms.virtualGeometryPacketDescriptorIndex"), "Task shader should address page-local descriptors by packet descriptor id, not selected cluster row.");
			context.Expect(ShaderContains(taskShader, "kVirtualGeometryPayloadDescriptorMaterialWordOffset"), "Task shader descriptor layout should keep page-local material stream slots.");
			context.Expect(ShaderContains(meshShader, "TryUsePageLocalGeometry"), "Mesh shader should choose page-local geometry for resident page and fallback packets.");
			context.Expect(ShaderContains(meshShader, "LoadPageLocalPackedVertexWord"), "Mesh shader should emit resident pages from page-local packed vertices.");
			context.Expect(ShaderContains(meshShader, "LoadPageLocalPrimitiveIndex"), "Mesh shader should emit resident pages from page-local primitive indices.");
			context.Expect(ShaderContains(meshShader, "LoadPageLocalMeshletRecord"), "Mesh shader should emit resident pages from page-local meshlet records.");
			context.Expect(ShaderContains(meshShader, "SceneUniforms.virtualGeometryPacketDescriptorIndex"), "Mesh shader should address page-local descriptors by packet descriptor id, not selected cluster row.");
			context.Expect(ShaderContains(meshShader, "kVirtualGeometryPayloadDescriptorMaterialWordOffset"), "Mesh shader descriptor layout should keep page-local material stream slots.");
			context.Expect(ShaderContains(vulkanResources, "BuildVirtualGeometryPageLocalPayloadWords"), "Vulkan should synthesize shader-readable page-local stream payload words.");
			context.Expect(ShaderContains(vulkanResources, "AppendVirtualGeometryPageLocalPacketPayload"), "Vulkan should append page-local stream bytes for every resident page packet.");
			context.Expect(ShaderContains(vulkanResources, "descriptorWordOffset"), "Vulkan should address page-local descriptor rows by index so payload appends cannot invalidate descriptor writes.");
			context.Expect(ShaderContains(vulkanResources, "AppendVirtualGeometryPageLocalPacketPayload(result.words, descriptorIndex, packet, &appendedGpuBytes)"), "Page-local payload publication should pass descriptor indices instead of raw vector pointers.");
			context.Expect(ShaderContains(vulkanResources, "AddVirtualGeometryPagePayloadBytes"), "Vulkan should attribute exact page-local payload bytes back to the owning mesh asset.");
			context.Expect(ShaderContains(vulkanResources, "pagePayloadBytesByMesh"), "Per-view page-local payload resources should publish exact per-mesh byte ownership.");
			context.Expect(ShaderContains(vulkanResources, "vulkan virtual geometry page-local payload"), "Backend residency snapshots should expose page-local VG payload bytes as mesh-owned Render GPU Bytes.");
			context.Expect(ShaderContains(renderSystem, "batchCommand.virtualGeometryPacketDescriptorIndex = firstPacket.packetId"), "RenderSystem should bind each draw to the packet-local descriptor generated for that packet.");
			context.Expect(ShaderContains(renderSystem, "record.packetId = packet.packetId"), "GPU packet publication should preserve CPU packet ids so page-local descriptors stay aligned after compaction.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US2.PageLocalDrawsLaunchLocalMeshletRanges", [](TestContext& context) {
			const std::string vulkanSource = ReadVulkanContextSource();

			context.Expect(ShaderContains(vulkanSource, "const bool pageLocalGeometry ="), "Vulkan draw preparation should classify resident page/fallback payload draws before clamping meshlet ranges.");
			context.Expect(ShaderContains(vulkanSource, "VirtualGeometryPagePayloadSource::ResidentPagePayload"), "Resident high-detail page packets should participate in the page-local launch path.");
			context.Expect(ShaderContains(vulkanSource, "VirtualGeometryPagePayloadSource::ResidentFallbackPayload"), "Resident fallback page packets should participate in the page-local launch path.");
			context.Expect(ShaderContains(vulkanSource, "push.virtualMeshletOffset = ownerMeshletOffset"), "Page-local draws should keep the owner meshlet offset available for descriptor fail-open rendering.");
			context.Expect(ShaderContains(vulkanSource, "? command.virtualMeshletCount"), "Page-local draws should launch the packet-local meshlet count instead of clamping by the owner mesh offset.");
			context.Expect(ShaderContains(vulkanSource, "std::min(command.virtualMeshletOffset, mesh.meshletCount)"), "Owner-mesh fallback draws should still clamp their global meshlet offset to the resident owner payload.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US2.PageLocalDescriptorUpdatesDrainRecordedViews", [](TestContext& context) {
			const std::string vulkanResources = ReadVulkanContextResourcesSource();

			context.Expect(ShaderContains(vulkanResources, "publication.buffer == buffer.buffer"), "View-local VG bindless descriptors should skip redundant descriptor-set updates when the published buffer did not change.");
			context.Expect(ShaderContains(vulkanResources, "WaitForDescriptorMutationIdle();"), "Changing a VG bindless descriptor should route through the shared descriptor-idle helper.");
			context.Expect(ShaderContains(vulkanResources, "pendingCommandBuffers.clear();"), "Unsubmitted recorded editor views should be discarded before a shared descriptor set is updated.");
			context.Expect(ShaderContains(vulkanResources, "vkQueueWaitIdle(graphicsQueue);"), "The descriptor mutation path should still drain already-submitted view work before mutating the shared bindless set.");
			context.Expect(ShaderContains(vulkanResources, "editorMode && currentFrame < pendingPrimaryCommandBuffers.size()"), "The central descriptor mutation idle helper should also drain recorded editor view command buffers before any descriptor set can be updated or freed.");
			context.Expect(ShaderContains(vulkanResources, "pendingPrimaryCommandBuffersSubmitted[currentFrame] != 0u"), "Already-submitted editor view command buffers should be tracked independently from the UI semaphore state.");
			context.Expect(ShaderContains(vulkanResources, "if (alreadySubmitted)"), "Already-submitted editor view command buffers should be waited on, not submitted a second time.");
			context.Expect(ShaderContains(vulkanResources, "failed to wait for submitted editor view command buffers before descriptor mutation"), "The submitted-view branch should report queue-idle failures before mutating descriptors.");
			context.Expect(ShaderContains(vulkanResources, "activeRenderSnapshot.state = RenderSnapshotState::Fallback"), "Dropped editor view command buffers should mark their render snapshot as a fallback instead of reporting submitted work.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.TextureUploadsUseDedicatedImmediateCommands", [](TestContext& context) {
			const std::string vulkanImpl = ReadVulkanContextImplSource();
			const std::string vulkanInit = ReadVulkanContextInitSource();
			const std::string vulkanResources = ReadVulkanContextResourcesSource();

			context.Expect(ShaderContains(vulkanImpl, "VkCommandPool immediateCommandPool"), "Single-time texture/layout uploads should own a command pool separate from per-frame Scene/Game/UI pools.");
			context.Expect(ShaderContains(vulkanImpl, "std::mutex immediateCommandMutex"), "Immediate command submission should be serialized because VkQueue and VkCommandPool operations are externally synchronized.");
			context.Expect(ShaderContains(vulkanInit, "VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT"), "The immediate command pool should be marked transient/resettable for short upload command buffers.");
			context.Expect(ShaderContains(vulkanInit, "vkDestroyCommandPool(device, immediateCommandPool"), "The dedicated immediate command pool should be destroyed during Vulkan cleanup.");
			context.Expect(ShaderContains(vulkanResources, "allocInfo.commandPool = immediateCommandPool != VK_NULL_HANDLE ? immediateCommandPool : commandPools[currentFrame]"), "BeginSingleTimeCommands should allocate from the immediate pool instead of the current frame pool.");
			context.Expect(ShaderContains(vulkanResources, "vkCreateFence(device, &fenceInfo, nullptr, &immediateFence)"), "EndSingleTimeCommands should use a fence so command buffers are complete before they are freed.");
			context.Expect(ShaderContains(vulkanResources, "vkWaitForFences(device, 1, &immediateFence, VK_TRUE, UINT64_MAX)"), "Immediate command buffers must wait on their own submit fence before vkFreeCommandBuffers.");
			context.Expect(ShaderContains(vulkanResources, "bool VulkanContext::Impl::EndSingleTimeCommands"), "Immediate submits should return success so callers can stop after failed layout/copy work.");
			context.Expect(ShaderContains(vulkanResources, "const VkResult idleResult = vkQueueWaitIdle(graphicsQueue);"), "Failed immediate submits should check whether the queue proved idle before freeing the command buffer.");
			context.Expect(ShaderContains(vulkanResources, "vkDestroyFence(device, immediateFence, nullptr);\n\t\tif (idleResult == VK_SUCCESS)"), "Temporary immediate fences must be destroyed even when command-buffer freeing remains gated by queue-idle success.");
			context.Expect(ShaderContains(vulkanResources, "uploadBarrier.subresourceRange.levelCount = residentMipCount"), "Texture upload should transition every resident mip in the same command buffer as the copy.");
			context.Expect(ShaderContains(vulkanResources, "vkCmdCopyBufferToImage(commandBuffer, stagingBuffer.buffer, vImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL"), "Texture upload should copy all resident mip regions after the in-buffer transfer transition.");
			context.Expect(ShaderContains(vulkanResources, "shaderReadBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL"), "Texture upload should transition resident mips to shader-read before publishing descriptors.");
			context.Expect(ShaderContains(vulkanResources, "if (!EndSingleTimeCommands(commandBuffer))"), "Texture upload should not cache or publish an image whose immediate command did not finish.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.EditorCommandPoolsResetOnlyAfterQueueIdle", [](TestContext& context) {
			const std::string vulkanSource = ReadVulkanContextSource();

			context.Expect(ShaderContains(vulkanSource, "PresentEditor could not prove the command pool idle before reset"), "PresentEditor should prove queue idle before resetting a frame command pool outside the normal render-submit path.");
			context.Expect(ShaderContains(vulkanSource, "PresentEditor kept failed UI command buffer alive because the queue did not prove idle"), "Failed UI submits should not free command buffers unless the graphics queue is known idle.");
			context.Expect(ShaderContains(vulkanSource, "Skipping editor render recovery because the graphics queue did not prove idle"), "Editor render recovery should not recreate swapchain resources while submitted work may still reference them.");
			context.Expect(ShaderContains(vulkanSource, "fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT"), "Failed UI submits should restore a signaled frame fence after the pre-submit reset.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.US5.NoSelectableHiZQualityTierWasIntroduced", [](TestContext& context) {
			const std::string taskShader = ReadDefaultTaskShader();
			const std::string compactionShader = ReadVirtualGeometryPacketCompactionShader();

			context.Expect(!taskShader.empty(), "The task shader should remain readable for Hi-Z preservation checks.");
			context.Expect(ShaderContains(taskShader, "LoadVirtualGeometryPacketRunSeed"), "Packet-buffer consumption should coexist with the existing task-shader Hi-Z path.");
			context.Expect(ShaderContains(taskShader, "Feature 030 preserves this single current-pyramid decision path"), "The shader should document that packet batching keeps the accepted Hi-Z path.");
			context.Expect(ShaderContains(taskShader, "if (!culled && hiZPyramidAvailable)"), "Task-shader Hi-Z should still be gated only by the current pyramid availability flag.");
			context.Expect(!ShaderContains(taskShader, "HiZQuality"), "Task shader should not expose a selectable Hi-Z quality tier.");
			context.Expect(!ShaderContains(taskShader, "hiZQuality"), "Task shader should not add a lower-case Hi-Z quality control.");
			context.Expect(!ShaderContains(taskShader, "VirtualGeometryHiZQuality"), "Task shader should not mirror a C++ Hi-Z quality enum.");
			context.Expect(!ShaderContains(taskShader, "kHiZQuality"), "Task shader should not branch on quality-tier constants.");
			context.Expect(!ShaderContains(compactionShader, "HiZQuality"), "Compaction shader should not introduce independent Hi-Z quality tiers either.");
			context.Expect(!ShaderContains(compactionShader, "qualityTier"), "Compaction shader should keep Hi-Z baseline metadata as a label, not a selectable tier.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.DefaultDrawsPushReflectedRangesForHiZ", [](TestContext& context) {
			const std::string vulkanSource = ReadVulkanContextSource();

			context.Expect(ShaderContains(vulkanSource, "bool PushDefaultMeshConstants"), "Vulkan draw submission should centralize default mesh push-constant emission.");
			context.Expect(ShaderContains(vulkanSource, "for (const VkPushConstantRange& range : pipeline.pushConstants)"), "Default mesh submission should iterate reflected push-constant ranges.");
			context.Expect(ShaderContains(vulkanSource, "range.stageFlags"), "Default mesh submission should preserve each reflected stage mask.");
			context.Expect(ShaderContains(vulkanSource, "pushBytes + range.offset"), "Default mesh submission should push the byte slice requested by each reflected range.");
			context.Expect(ShaderContains(vulkanSource, "PushDefaultMeshConstants(secondary, *draw.pipeline, draw.push);"), "Main visible draws should push reflected ranges so task-stage Hi-Z constants are never skipped.");
			context.Expect(ShaderContains(vulkanSource, "PushDefaultMeshConstants(cmd, *draw.pipeline, shadowPush);"), "Shadow draws should share the reflected push path without inventing a second contract.");
			context.Expect(!ShaderContains(vulkanSource, "vkCmdPushConstants(secondary, draw.pipeline->layout, draw.pipeline->defaultPushConstantStages, 0, sizeof(PushConstants)"), "Visible draws must not return to a single all-stage full-struct push; stale fragment ranges can make that skip task-stage Hi-Z constants.");
			context.Expect(!ShaderContains(vulkanSource, "vkCmdPushConstants(cmd, draw.pipeline->layout, draw.pipeline->defaultPushConstantStages, 0, sizeof(PushConstants)"), "Shadow draws must not return to the stale defaultPushConstantStages full-struct path.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.EditorPresentDrainsSubmittedRenderSemaphoreOnRecovery", [](TestContext& context) {
			const std::string vulkanSource = ReadVulkanContextSource();

			context.Expect(ShaderContains(vulkanSource, "drainSubmittedEditorRender"), "Editor present recovery should keep a helper that consumes submitted scene/game render semaphores.");
			context.Expect(ShaderContains(vulkanSource, "waitSubmitInfo.waitSemaphoreCount = 1;"), "The recovery submit must wait on the render-finished semaphore so the binary semaphore becomes reusable.");
			context.Expect(ShaderContains(vulkanSource, "waitSubmitInfo.commandBufferCount"), "The recovery path should remain a wait-only submit instead of recording throwaway rendering work.");
			context.Expect(ShaderContains(vulkanSource, "drainSubmittedEditorRender(\"resize\")"), "Resize recovery should drain a submitted render pass before rebuilding swapchain or offscreen resources.");
			context.Expect(ShaderContains(vulkanSource, "drainSubmittedEditorRender(\"out-of-date acquire\")"), "Out-of-date image acquire should drain a submitted render pass before swapchain recovery.");
			context.Expect(ShaderContains(vulkanSource, "drainSubmittedEditorRender(\"failed acquire\")"), "Failed image acquire should drain a submitted render pass instead of leaving renderFinished signaled.");
			context.Expect(ShaderContains(vulkanSource, "BeginFrame failed to drain stranded editor render semaphore"), "BeginFrame should not clear a stranded editor render submission without consuming its binary semaphore.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.GlobalHiZDescriptorsPublishValidViewWhenPeerMissing", [](TestContext& context) {
			const std::string vulkanResourcesSource = ReadVulkanContextResourcesSource();

			context.Expect(ShaderContains(vulkanResourcesSource, "const VulkanHiZ* fallbackHiZ = nullptr;"), "Global Hi-Z descriptor publication should find a valid per-view fallback.");
			context.Expect(ShaderContains(vulkanResourcesSource, "const VulkanHiZ& readableHiZ = isReadableHiZ(hiZ) ? hiZ : *fallbackHiZ;"), "A missing Scene/Game pyramid should not block the other view's descriptor update.");
			context.Expect(ShaderContains(vulkanResourcesSource, "samplerInfo.sampler = fallbackHiZ->sampler;"), "The global Hi-Z sampler should come from the same valid fallback image owner.");
			context.Expect(ShaderContains(vulkanResourcesSource, "VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 11: Hi-Z sampler"), "The global Hi-Z sampler binding should be update-after-bind so descriptor republish cannot invalidate already-recorded Scene/Game command buffers.");
			context.Expect(ShaderContains(vulkanResourcesSource, "VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, // 8: Point-shadow sampler"), "The point-shadow sampler binding should be update-after-bind because shadow resource rebuilds can republish it between editor views.");
			context.Expect(!ShaderContains(vulkanResourcesSource, "if (hiZ.fullView == VK_NULL_HANDLE || hiZ.sampler == VK_NULL_HANDLE) return;"), "Global Hi-Z descriptor publication must not return early just because one editor view is missing.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.HiZPyramidTracksEditorRenderTargetSize", [](TestContext& context) {
			const std::string vulkanSource = ReadVulkanContextSource();
			const std::string vulkanImplSource = ReadVulkanContextImplSource();
			const std::string vulkanResourcesSource = ReadVulkanContextResourcesSource();

			context.Expect(!ShaderContains(vulkanSource, "EnsureCurrentFrameHiZMatchesRenderTarget"), "RenderFrame must not recreate Hi-Z resources after a view command buffer may have bound the global descriptor set.");
			context.Expect(ShaderContains(vulkanSource, "impl->desiredHiZExtentByView[hiZViewIndex] = glm::uvec2(hiZTargetWidth, hiZTargetHeight);"), "RenderFrame should record the actual Scene/Game render target size without destroying Vulkan objects mid-frame.");
			context.Expect(ShaderContains(vulkanImplSource, "std::array<glm::uvec2, kHiZViewCount> desiredHiZExtentByView{}"), "Per-view desired Hi-Z sizes should live with the backend state so BeginFrame can consume them at a fence-safe boundary.");
			context.Expect(ShaderContains(vulkanSource, "doing this inside"), "The fence-safe resize path should document why RenderFrame is not allowed to rebuild the sampler-backed Hi-Z resources.");
			context.Expect(ShaderContains(vulkanSource, "impl->UpdateGlobalHiZ(impl->currentFrame);"), "After a fence-safe Hi-Z resize, the global descriptors should be republished before new command buffers bind them.");
			context.Expect(ShaderContains(vulkanSource, "currentFrameHiZ.width == hiZTargetWidth"), "Hi-Z readiness should compare against the current render target width, not just the swapchain width.");
			context.Expect(ShaderContains(vulkanSource, "currentFrameHiZ.height == hiZTargetHeight"), "Hi-Z readiness should compare against the current render target height, not just the swapchain height.");
			context.Expect(ShaderContains(vulkanSource, "hiZ.Create(impl->device, impl->allocator, desiredExtent.x, desiredExtent.y"), "Recreated Hi-Z pyramids should use the queued view render area extent.");
			context.Expect(ShaderContains(vulkanSource, "impl->InitializeHiZPyramidToFarDepth(hiZ);"), "Freshly resized Hi-Z pyramids should start with far depth so the first frame fails open instead of producing false occlusion.");
			context.Expect(ShaderContains(vulkanSource, "impl->readableHiZHistoryByViewFrame[hiZViewIndex][impl->currentFrame] = impl->CurrentHiZ(hiZViewIndex).history;"), "Readable Hi-Z history should be refreshed after resize before the validation gate is evaluated.");
			context.Expect(ShaderContains(vulkanResourcesSource, "const auto hasReadableCompletedHiZ"), "Completed stats should verify the readable Hi-Z resource, not only a raw image handle.");
			context.Expect(ShaderContains(vulkanResourcesSource, "hiZ.width == stats.renderSize.x && hiZ.height == stats.renderSize.y"), "Completed stats should mark Hi-Z active only when the backend pyramid matches the completed view size.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.FrameCullingDataCarriesHiZAvailabilityGate", [](TestContext& context) {
			context.ExpectEqual(offsetof(FrameCullingDataGPU, renderSettings1) + sizeof(glm::uvec4), offsetof(FrameCullingDataGPU, renderSettings2), "renderSettings2 should remain directly after renderSettings1 for the task shader payload.");
			context.ExpectEqual(sizeof(static_cast<FrameCullingDataGPU*>(nullptr)->renderSettings2), sizeof(glm::vec4), "renderSettings2 should remain a float4 so .w can gate Hi-Z without changing the shader layout.");
			context.Expect(offsetof(FrameCullingDataGPU, renderSettings2) < offsetof(FrameCullingDataGPU, globalTaskCullingEnabled), "The Hi-Z availability gate must remain in frame data before task-culling feature flags.");
			context.ExpectEqual(static_cast<std::size_t>(336u), sizeof(FrameCullingDataGPU), "Adding Hi-Z validation state should not change the task-shader frame-culling upload size.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.HiZUsesBoundedRectSampleBudget", [](TestContext& context) {
			const std::string shader = ReadDefaultTaskShader();
			context.Expect(!shader.empty(), "The default task shader should be readable for Hi-Z sampler contract validation.");
			context.Expect(ShaderContains(shader, "static const uint kMaxHiZRectAxisSamples = 32u;"), "The Hi-Z rectangle sampler should keep its bounded per-axis loop limit explicit and affordable for task shaders.");
			context.Expect(ShaderContains(shader, "static const uint kMaxHiZRectTexelSamples = kMaxHiZRectAxisSamples * kMaxHiZRectAxisSamples;"), "The Hi-Z rectangle sampler should derive the total budget from the axis cap so the loop bound and mip picker stay in sync.");
			context.Expect(ShaderContains(shader, "when the cap forces a coarser mip"), "The shader should document that coarse-mip fallback is conservative and may cull fewer meshlets rather than hiding visible geometry.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.TaskShaderAggregatesCountersPerTaskGroup", [](TestContext& context) {
			const std::string shader = ReadDefaultTaskShader();
			context.Expect(!shader.empty(), "The default task shader should be readable for counter aggregation contract validation.");
			context.Expect(ShaderContains(shader, "MeshletCullingCounters localCounters = MakeEmptyMeshletCullingCounters();"), "Task groups should accumulate meshlet counters locally before touching the global counter buffer.");
			context.Expect(ShaderContains(shader, "FlushMeshletCullingCounters(statsIndex, localCounters);"), "Task groups should flush one aggregated counter packet before dispatching mesh work.");
			context.Expect(!ShaderContains(shader, "InterlockedAdd(meshletCounters[statsIndex].meshletsTotal, 1u"), "Per-meshlet total-count atomics should stay removed from the hot loop.");
			context.Expect(!ShaderContains(shader, "InterlockedAdd(meshletCounters[statsIndex].meshletsDispatched, 1u"), "Per-meshlet dispatch-count atomics should stay removed from the hot loop.");
			context.Expect(!ShaderContains(shader, "InterlockedAdd(meshletCounters[statsIndex].hiZTests, 1u"), "Per-meshlet Hi-Z-test atomics should stay removed from the hot loop.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.HiZProjectsActualMeshletVertices", [](TestContext& context) {
			const std::string shader = ReadDefaultTaskShader();
			context.Expect(!shader.empty(), "The default task shader should be readable for vertex-projected Hi-Z contract validation.");
			context.Expect(ShaderContains(shader, "[[vk::binding(0, 1)]] ByteAddressBuffer meshletVertexBytes;"), "The task shader should read packed meshlet vertices when building Hi-Z rectangles.");
			context.Expect(ShaderContains(shader, "[[vk::binding(1, 1)]] ByteAddressBuffer primitiveIndices;"), "The task shader should read primitive indices for triangle coverage votes.");
			context.Expect(ShaderContains(shader, "[[vk::binding(2, 1)]] StructuredBuffer<MeshletRecord> meshletRecords;"), "The task shader should read meshlet records for per-meshlet vertex ranges.");
			context.Expect(ShaderContains(shader, "[[vk::binding(3, 1)]] ByteAddressBuffer meshletVertexRemap;"), "The task shader should read the remap table so Hi-Z tests use the same decoded vertices as mesh emission.");
			context.Expect(ShaderContains(shader, "TryProjectMeshletVertex"), "Hi-Z culling should share one decoded vertex projection helper between rectangle and point-coverage tests.");
			context.Expect(ShaderContains(shader, "TryBuildProjectedMeshletRect"), "Hi-Z culling should build a projected rectangle from actual meshlet vertices instead of only from the bounds sphere.");
			context.Expect(ShaderContains(shader, "IsMeshletVertexCoverageOccluded"), "Small projected meshlets should have an all-vertex Hi-Z fallback when their rectangle contains unrelated background texels.");
			context.Expect(ShaderContains(shader, "static const uint kMaxCoverageTriangleSamples = 64u;"), "Triangle coverage votes should keep a bounded loop count for task-shader culling.");
			context.Expect(ShaderContains(shader, "static const float kMaxTriangleCoverageRectExtentPixels = 512.0f;"), "Triangle coverage votes should stay bounded to medium projected meshlets.");
			context.Expect(ShaderContains(shader, "static const float kMaxRectCoverageRectExtentPixels = 320.0f;"), "Coarse rectangle coverage votes should stay bounded to modest projected meshlets.");
			context.Expect(ShaderContains(shader, "static const float kMaxCenterCoverageRectExtentPixels = 128.0f;"), "Center-tap fallback should stay limited to small projected meshlets.");
			context.Expect(ShaderContains(shader, "const float2 uvPadding = 8.0f / viewportSize;"), "Hi-Z should pad projected meshlet rectangles enough that wall-edge silhouettes fail open.");
			context.Expect(ShaderContains(shader, "static const uint kRequiredRectCoverageSamples = kRectCoverageSampleCount;"), "Coarse rectangle coverage should require every coarse sample to be hidden before culling.");
			context.Expect(ShaderContains(shader, "Coverage-vote fallbacks are intentionally diagnostic-only for now."), "Hi-Z coverage-vote fallbacks should fail open until they have a stronger silhouette-safe proof.");
			context.Expect(!ShaderContains(shader, "if (IsMeshletRectCoverageMostlyOccluded"), "Coarse rectangle votes should not actively cull wall-edge silhouettes.");
			context.Expect(!ShaderContains(shader, "if (IsMeshletTriangleCoverageOccluded"), "Triangle centroid votes should not actively cull wall-edge silhouettes.");
			context.Expect(!ShaderContains(shader, "if (IsMeshletVertexCoverageOccluded"), "Vertex votes should not actively cull wall-edge silhouettes.");
			context.Expect(!ShaderContains(shader, "IsSmallMeshletCenterOccluded(hiZViewIndex"), "A single center tap should not cull visible wall-edge silhouettes.");
			context.Expect(ShaderContains(shader, "const uint requiredOccludedVertices = vertexCount;"), "Vertex fallback should require every projected vertex to be hidden before culling.");
			context.Expect(ShaderContains(shader, "const uint requiredOccludedTriangles = sampledTriangleCount;"), "Triangle centroid sampling should require every sampled triangle to be hidden before culling.");
			context.Expect(ShaderContains(shader, "static const float kMaxVertexCoverageRectExtentPixels = 160.0f;"), "The all-vertex fallback should stay limited to modest projected meshlets to avoid silhouette popping.");
			context.Expect(!ShaderContains(shader, "TryGetHiZProjectionScale"), "The old sphere-projection scale path should stay removed from task-shader Hi-Z culling.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.HiZProjectedBoundsContractIsConservative", [](TestContext& context) {
			VulkanHiZProjectedBoundsInput valid{};
			valid.pyramidReady = true;
			valid.clipW = 4.0f;
			valid.uvCenter = glm::vec2(0.5f, 0.5f);
			valid.uvRadius = glm::vec2(0.04f, 0.04f);
			valid.viewportSize = glm::uvec2(1280u, 720u);
			const VulkanHiZProjectedBoundsResult validResult = ValidateVulkanHiZProjectedBounds(valid);
			context.Expect(validResult.sampleAllowed, "The CPU contract should agree with the task shader that fully contained projected bounds may sample Hi-Z.");

			VulkanHiZProjectedBoundsInput edge = valid;
			edge.uvCenter = glm::vec2(0.01f, 0.5f);
			edge.uvRadius = glm::vec2(0.04f, 0.04f);
			const VulkanHiZProjectedBoundsResult edgeResult = ValidateVulkanHiZProjectedBounds(edge);
			context.Expect(!edgeResult.sampleAllowed, "Projected bounds that cross the view edge should fail open before task-shader Hi-Z sampling.");
			context.Expect(edgeResult.failOpen, "Rejected projected bounds should be visible fallback, not hidden geometry.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.UserFacingVisualizerModesMatchFragmentShaderConstants", [](TestContext& context) {
			const std::string shader = ReadDefaultFragmentShader();
			context.Expect(!shader.empty(), "The default fragment shader should be readable for visualizer contract validation.");

			struct ModeConstant
			{
				VirtualGeometryDebugVisualizationMode mode = VirtualGeometryDebugVisualizationMode::None;
				const char* constantName = "";
				const char* expectedValue = "";
			};

			const std::array<ModeConstant, 8u> constants = {
				ModeConstant{ VirtualGeometryDebugVisualizationMode::LOD, "kVirtualGeometryDebugModeLOD", "1u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::MeshletId, "kVirtualGeometryDebugModeMeshletId", "2u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Bounds, "kVirtualGeometryDebugModeBounds", "3u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Culling, "kVirtualGeometryDebugModeCulling", "4u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::HiZ, "kVirtualGeometryDebugModeHiZ", "5u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Residency, "kVirtualGeometryDebugModeResidency", "6u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Error, "kVirtualGeometryDebugModeError", "7u" },
				ModeConstant{ VirtualGeometryDebugVisualizationMode::Cluster, "kVirtualGeometryDebugModeCluster", "8u" }
			};

			for (const ModeConstant& constant : constants)
			{
				const std::string declaration =
					std::string("static const uint ") + constant.constantName + " = " + constant.expectedValue + ";";
				const std::string branch =
					std::string("if (debugMode == ") + constant.constantName + ")";
				context.Expect(ShaderContains(shader, declaration), "Each exposed virtual geometry visualizer mode should keep its numeric HLSL constant aligned with the C++ enum.");
				context.Expect(ShaderContains(shader, branch), "Each exposed virtual geometry visualizer mode should have a fragment shader branch.");
				context.Expect(IsVirtualGeometryDebugVisualizationModeExposed(constant.mode), "The shader contract table should only list public editor-facing modes.");
			}

			context.Expect(!ShaderContains(shader, "kVirtualGeometryDebugModeBoundaries"), "Removed internal visualizer modes should not leave public shader constants behind.");
			context.Expect(!ShaderContains(shader, "kVirtualGeometryDebugModeScale"), "Removed internal visualizer modes should not leave public shader constants behind.");
			context.Expect(!ShaderContains(shader, "kVirtualGeometryDebugModeFallback"), "Removed internal visualizer modes should not leave public shader constants behind.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.FalseColorBranchesMatchPublishedLegend", [](TestContext& context) {
			const std::string shader = ReadDefaultFragmentShader();
			context.Expect(ShaderContains(shader, "BuildVirtualGeometryHashColor(selectedLod + 17u)"), "LOD mode should use a stable false color from the selected detail level.");
			context.Expect(ShaderContains(shader, "BuildVirtualGeometryHashColor(input.meshletIndex + 31u)"), "Meshlet Id mode should use a stable false color from the rendered meshlet id.");
			context.Expect(ShaderContains(shader, "lerp(float3(0.05f, 0.25f, 1.0f), float3(1.0f, 0.9f, 0.05f), radiusBand)"), "Bounds mode should keep the blue-to-yellow size ramp documented in the legend.");
			context.Expect(ShaderContains(shader, "return hiZTrusted ? float3(0.05f, 0.8f, 1.0f) : float3(0.1f, 0.85f, 0.25f);"), "Culling mode should distinguish drawn meshlets with and without trusted Hi-Z.");
			context.Expect(ShaderContains(shader, "return hiZTrusted ? float3(0.05f, 0.8f, 1.0f) : float3(0.35f, 0.35f, 0.35f);"), "Hi-Z mode should keep cyan active and gray missing-pyramid states.");
			context.Expect(ShaderContains(shader, "missingPageCount > 0u ? float3(1.0f, 0.55f, 0.05f) : float3(0.0f, 0.75f, 0.45f)"), "Residency mode should keep orange requested/missing and green resident states.");
			context.Expect(ShaderContains(shader, "missingPageCount > 0u ? float3(1.0f, 0.55f, 0.05f) : float3(0.2f, 0.9f, 0.9f)"), "Error mode should keep orange missing-page pressure and cyan clean state.");
			context.Expect(ShaderContains(shader, "BuildVirtualGeometryHashColor(clusterSeed + 73u)"), "Cluster mode should use a stable false color from selected cluster identity.");
		});
	}
}
