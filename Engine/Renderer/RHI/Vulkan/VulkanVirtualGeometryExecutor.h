// VulkanVirtualGeometryExecutor.h - capability-truthful GPU-driven execution contract.

#pragma once

#include "Core/Export.h"
#include "Renderer/RHI/RHIDefinitions.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"
#include "VulkanCommonIncludes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <span>
#include <type_traits>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	// VulkanVirtualGeometryPacketUploadRecord is the byte-exact storage-buffer
	// wire format shared with the traversal, compaction, and indirect shaders.
	// Never upload GpuVirtualGeometryPacketRecord directly: that processor-facing
	// type owns diagnostic text and has platform-dependent padding.
	struct VulkanVirtualGeometryPacketUploadRecord
	{
		std::uint32_t packetId = 0u;
		std::uint32_t viewId = 0u;
		std::uint32_t viewType = 0u;
		std::uint32_t frameIdLow = 0u;
		std::array<std::uint32_t, 4> objectIdWords{};
		std::array<std::uint32_t, 4> meshIdWords{};
		std::array<std::uint32_t, 4> materialIdWords{};
		std::uint32_t selectedLod = 0u;
		std::uint32_t clusterId = 0u;
		std::uint32_t meshletOffset = 0u;
		std::uint32_t meshletCount = 0u;
		std::array<float, 4> boundsMin{};
		std::array<float, 4> boundsMax{};
		std::array<float, 4> projectedBounds{};
		std::uint32_t pageId = 0u;
		std::uint32_t fallbackPageId = 0u;
		std::uint32_t payloadSource = 0u;
		std::uint32_t emittedSource = 0u;
		std::array<std::uint32_t, 2> pagePayloadByteOffset{};
		std::array<std::uint32_t, 2> pagePayloadByteSize{};
		std::uint32_t payloadBufferIndex = 0xffffffffu;
		std::uint32_t payloadDescriptorIndex = 0xffffffffu;
		// Every packet in a compatible batch points at the first packet's draw
		// slot. Indirect generation atomically accumulates bounded task groups
		// there, so compaction order cannot split or redirect a batch.
		std::uint32_t generatedCommandIndex = 0xffffffffu;
		// Partial packet rejection cannot rewrite the task shader's contiguous
		// descriptor run. Every survivor therefore publishes the conservative
		// full-batch count, while an all-rejected batch remains zero work.
		std::uint32_t generatedCommandTaskGroupCount = 0u;
		std::uint32_t debugIdentity = 0u;
		std::uint32_t flags = 0u;
		// DXC rounds StructuredBuffer records containing float4 values to a
		// 16-byte stride. Keep the processor upload at that exact 192-byte stride.
		std::array<std::uint32_t, 2> tailPadding{};
	};

	static_assert(std::is_standard_layout_v<VulkanVirtualGeometryPacketUploadRecord>);
	static_assert(offsetof(VulkanVirtualGeometryPacketUploadRecord, frameIdLow) == 12u);
	static_assert(offsetof(VulkanVirtualGeometryPacketUploadRecord, selectedLod) == 64u);
	static_assert(offsetof(VulkanVirtualGeometryPacketUploadRecord, projectedBounds) == 112u);
	static_assert(offsetof(VulkanVirtualGeometryPacketUploadRecord, emittedSource) == 140u);
	static_assert(offsetof(VulkanVirtualGeometryPacketUploadRecord, generatedCommandIndex) == 168u);
	static_assert(offsetof(VulkanVirtualGeometryPacketUploadRecord, flags) == 180u);
	static_assert(offsetof(VulkanVirtualGeometryPacketUploadRecord, tailPadding) == 184u);
	static_assert(sizeof(VulkanVirtualGeometryPacketUploadRecord) == 192u);

	enum class VulkanVirtualGeometryBarrierKind : std::uint8_t
	{
		UploadToTraversal,
		TraversalToCompaction,
		CompactionToIndirect,
		ComputeToDraw,
		CounterToReadback
	};

	enum class VulkanVirtualGeometryExecutorStage : std::uint8_t
	{
		Traversal,
		Compaction,
		Indirect
	};

	struct VulkanVirtualGeometryExecutorResources
	{
		VkBuffer packetBuffer = VK_NULL_HANDLE;
		VkBuffer traversalInstanceBuffer = VK_NULL_HANDLE;
		VkBuffer traversalRequestBuffer = VK_NULL_HANDLE;
		VkBuffer traversalCounterBuffer = VK_NULL_HANDLE;
		VkBuffer compactedPacketBuffer = VK_NULL_HANDLE;
		VkBuffer generatedSubmissionBuffer = VK_NULL_HANDLE;
		VkBuffer counterReadbackBuffer = VK_NULL_HANDLE;
		VkBuffer zeroWorkIndirectBuffer = VK_NULL_HANDLE;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		bool resourcesOwnedByFrame = false;
	};

	struct VulkanVirtualGeometryExecutorPipelines
	{
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline traversal = VK_NULL_HANDLE;
		VkPipeline compaction = VK_NULL_HANDLE;
		VkPipeline indirectGeneration = VK_NULL_HANDLE;
	};

	// This layout is shared with VirtualGeometryPacketCompaction.comp.hlsl.  Keep
	// it as plain 32-bit values so the Vulkan push-constant ABI is unambiguous.
	struct VulkanVirtualGeometryCompactionPushConstants
	{
		std::uint32_t candidatePacketCount = 0u;
		std::uint32_t outputCapacity = 0u;
		std::uint32_t counterOffset = 0u;
		std::uint32_t failOpenOnInvalid = 1u;
	};

	struct VulkanVirtualGeometryTraversalPushConstants { std::uint32_t instanceCount, requestCapacity, expectedGeneration, failOpen; };
	struct VulkanVirtualGeometryIndirectPushConstants { std::uint32_t packetCount, argumentCapacity, commandStride, routeMask; };

	struct VulkanVirtualGeometryExecutorPlan
	{
		std::uint64_t frameId = 0u;
		std::uint64_t geometryGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		std::uint64_t commandGeneration = 0u;
		std::uint32_t requestedPacketCount = 0u;
		std::uint32_t boundedPacketCount = 0u;
		std::uint32_t maximumPacketCount = 0u;
		bool usesIndirectCount = false;
		bool zeroWork = false;
		bool resourcesReady = false;
		bool descriptorReady = false;
		bool pipelinesReady = false;
		// The renderer sets this only after traversal, compaction, and indirect
		// generation all record successfully for the current command generation.
		bool generatedSubmissionExecutionAvailable = false;
		std::vector<VulkanVirtualGeometryBarrierKind> barriers;
		std::string evidenceId;
		std::string unavailableReason;
	};

	struct VulkanVirtualGeometryExecutorRecordingResult
	{
		bool recorded = false;
		bool zeroWork = false;
		std::uint32_t dispatchCount = 0u;
		std::uint32_t barrierCount = 0u;
		std::string failureReason;
	};

	struct VulkanVirtualGeometryFailureReceipt
	{
		VirtualGeometryBackendStage stage = VirtualGeometryBackendStage::Traversal;
		std::uint64_t frameId = 0u;
		std::uint64_t commandGeneration = 0u;
		std::string evidenceId;
		std::string reason;
	};

	// Direct meshlets use the renderer's existing mesh-task indirect path. This
	// compact plan is deliberately separate from the VG executor so choosing a
	// direct route cannot allocate traversal, compaction, feedback, or decode
	// resources as an accidental side effect.
	struct VulkanDirectMeshletDrawRecord
	{
		std::uint64_t geometryIdentity = 0u;
		std::uint64_t geometryGeneration = 0u;
		std::uint64_t materialIdentity = 0u;
		std::uint32_t taskGroupCount = 0u;
		std::uint32_t instanceCount = 0u;
	};

	struct VulkanDirectMeshletBatchPlan
	{
		std::vector<VulkanDirectMeshletDrawRecord> draws;
		std::uint32_t suppressedVirtualGeometryExecutors = 0u;
		std::uint32_t suppressedPageRequests = 0u;
		bool valid = false;
		std::string rejectionReason;
	};

	// The ABI mirrors VirtualGeometryProductionCodecAbi.hlsli. Keep this plain
	// 32-bit record byte-exact so CPU validation and shader dispatch agree.
	struct VulkanVirtualGeometryProductionDecodeRequest
	{
		std::uint32_t profileId = 0u;
		std::uint32_t codecVersion = 0u;
		std::uint32_t encodedByteOffset = 0u;
		std::uint32_t encodedByteCount = 0u;
		std::uint32_t decodedByteOffset = 0u;
		std::uint32_t decodedByteCount = 0u;
		std::uint32_t outputCapacityBytes = 0u;
		std::uint32_t requestIdentity = 0u;
	};
	static_assert(sizeof(VulkanVirtualGeometryProductionDecodeRequest) == 32u);

	struct VulkanVirtualGeometryProductionDecodeResult
	{
		std::uint32_t requestIdentity = 0u;
		std::uint32_t status = 0u;
		std::uint32_t decodedByteCount = 0u;
		std::uint32_t reserved = 0u;
	};
	static_assert(sizeof(VulkanVirtualGeometryProductionDecodeResult) == 16u);

	struct VulkanVirtualGeometryProductionDecodeResources
	{
		VkBuffer requestBuffer = VK_NULL_HANDLE;
		VkBuffer encodedBuffer = VK_NULL_HANDLE;
		VkBuffer decodedBuffer = VK_NULL_HANDLE;
		VkBuffer resultReadbackBuffer = VK_NULL_HANDLE;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		std::uint32_t encodedCapacityBytes = 0u;
		std::uint32_t decodedCapacityBytes = 0u;
		bool resourcesOwnedByFrame = false;
	};

	struct VulkanVirtualGeometryProductionDecodePipelines
	{
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline expandedProductionDecode = VK_NULL_HANDLE;
	};

	struct VulkanVirtualGeometryProductionDecodePlan
	{
		VulkanVirtualGeometryProductionDecodeRequest request{};
		std::uint64_t generation = 0u;
		std::uint64_t expectedDecodedHash = 0u;
		bool supportedProfile = false;
		bool resourcesReady = false;
		bool pipelineReady = false;
		bool valid = false;
		std::string unavailableReason;
	};

	struct VulkanVirtualGeometryProductionDecodeParityReceipt
	{
		std::uint64_t generation = 0u;
		bool deviceDecodeAvailable = false;
		bool acceptedForPublication = false;
		std::string reason;
	};

	// Visibility is deliberately a separate readiness chain from generated draw
	// execution. A missing visibility pipeline must not downgrade the proven
	// traversal/compaction/indirect gate owned by the renderer.
	struct VulkanVirtualGeometryVisibilityResources
	{
		// The visibility shaders use storage buffers for their conservative depth
		// and identity records, so each pass receives its own compatible binding set.
		VkBuffer inputTriangleBuffer = VK_NULL_HANDLE;
		VkBuffer setupPacketBuffer = VK_NULL_HANDLE;
		VkBuffer setupMeshletBuffer = VK_NULL_HANDLE;
		VkBuffer pagePayloadBuffer = VK_NULL_HANDLE;
		VkBuffer depthBuffer = VK_NULL_HANDLE;
		VkBuffer visibilityRecordBuffer = VK_NULL_HANDLE;
		VkBuffer resolvedVisibilityBuffer = VK_NULL_HANDLE;
		VkBuffer shadingBinBuffer = VK_NULL_HANDLE;
		VkBuffer diagnosticBuffer = VK_NULL_HANDLE;
		VkDescriptorSet bindlessDescriptorSet = VK_NULL_HANDLE;
		VkDescriptorSet triangleSetupDescriptorSet = VK_NULL_HANDLE;
		VkDescriptorSet computeRasterDescriptorSet = VK_NULL_HANDLE;
		VkDescriptorSet visibilityResolveDescriptorSet = VK_NULL_HANDLE;
		VkDescriptorSet shadingBinsDescriptorSet = VK_NULL_HANDLE;
		bool resourcesOwnedByFrame = false;
	};

	struct VulkanVirtualGeometryVisibilityPipelines
	{
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline triangleSetup = VK_NULL_HANDLE;
		VkPipeline computeRaster = VK_NULL_HANDLE;
		VkPipeline visibilityResolve = VK_NULL_HANDLE;
		VkPipeline shadingBins = VK_NULL_HANDLE;
	};

	struct VulkanVirtualGeometryVisibilityPlan
	{
		std::uint64_t frameId = 0u;
		std::uint64_t geometryGeneration = 0u;
		std::uint32_t boundedPrimitiveCount = 0u;
		std::uint32_t meshletWorkCount = 0u;
		std::uint32_t targetWidth = 0u;
		std::uint32_t targetHeight = 0u;
		std::uint32_t pixelCount = 0u;
		std::uint32_t materialBinCount = 0u;
		bool zeroWork = false;
		bool ready = false;
		std::string evidenceId;
		std::string unavailableReason;
	};

	// Scratch outputs are cleared by transfer commands on the GPU. The byte plan
	// keeps size arithmetic testable and prevents a frame from allocating and
	// copying viewport-sized CPU zero arrays merely to initialize compute output.
	struct VulkanVirtualGeometryVisibilityClearPlan
	{
		VkDeviceSize depthBytes = 0u;
		VkDeviceSize visibilityRecordBytes = 0u;
		VkDeviceSize resolvedVisibilityBytes = 0u;
		VkDeviceSize shadingBinBytes = 0u;
		bool valid = false;
	};

	TERRAENGINE_API VulkanVirtualGeometryVisibilityClearPlan
		BuildVulkanVirtualGeometryVisibilityClearPlan(
			const VulkanVirtualGeometryVisibilityPlan& plan);

	enum class VulkanVirtualGeometryVisibilityStage : std::uint8_t
	{
		TriangleSetup,
		ComputeRaster,
		VisibilityResolve,
		ShadingBins
	};

	TERRAENGINE_API VulkanVirtualGeometryVisibilityPlan BuildVulkanVirtualGeometryVisibilityPlan(
		std::uint32_t requestedPrimitiveCount, std::uint32_t maximumPrimitiveCount,
		std::uint32_t meshletWorkCount,
		std::uint32_t targetWidth, std::uint32_t targetHeight,
		std::uint32_t materialBinCount,
		const VulkanVirtualGeometryVisibilityResources& resources,
		const VulkanVirtualGeometryVisibilityPipelines& pipelines,
		std::uint64_t frameId, std::uint64_t geometryGeneration);
	TERRAENGINE_API VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryVisibility(
		VkCommandBuffer commandBuffer, const VulkanVirtualGeometryVisibilityPlan& plan,
		const VulkanVirtualGeometryVisibilityResources& resources,
		const VulkanVirtualGeometryVisibilityPipelines& pipelines);
	TERRAENGINE_API VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryVisibilityStage(
		VkCommandBuffer commandBuffer, VulkanVirtualGeometryVisibilityStage stage,
		const VulkanVirtualGeometryVisibilityPlan& plan,
		const VulkanVirtualGeometryVisibilityResources& resources,
		const VulkanVirtualGeometryVisibilityPipelines& pipelines);

	TERRAENGINE_API VulkanVirtualGeometryExecutorPlan BuildVulkanVirtualGeometryExecutorPlan(std::uint32_t requestedPacketCount,
		std::uint32_t maximumPacketCount,
		bool usesIndirectCount,
		const VulkanVirtualGeometryExecutorResources &resources,
		const VulkanVirtualGeometryExecutorPipelines &pipelines,
		std::uint64_t frameId,
		std::uint64_t geometryGeneration,
		std::uint64_t residencyGeneration,
		std::uint64_t commandGeneration);
	TERRAENGINE_API VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryExecutor(VkCommandBuffer commandBuffer,
		const VulkanVirtualGeometryExecutorPlan &plan,
		const VulkanVirtualGeometryExecutorResources &resources,
		const VulkanVirtualGeometryExecutorPipelines &pipelines);
	TERRAENGINE_API VulkanVirtualGeometryExecutorRecordingResult RecordVulkanVirtualGeometryExecutorStage(
		VkCommandBuffer commandBuffer, VulkanVirtualGeometryExecutorStage stage,
		const VulkanVirtualGeometryExecutorPlan &plan,
		const VulkanVirtualGeometryExecutorResources &resources,
		const VulkanVirtualGeometryExecutorPipelines &pipelines);
	TERRAENGINE_API VulkanVirtualGeometryFailureReceipt BuildVulkanVirtualGeometryFailureReceipt(
		const VulkanVirtualGeometryExecutorPlan &plan, VirtualGeometryBackendStage stage);
	TERRAENGINE_API VulkanDirectMeshletBatchPlan BuildVulkanDirectMeshletBatchPlan(
		const std::vector<DirectMeshletBatch>& batches, bool meshTaskPathAvailable,
		std::uint32_t meshletsPerTaskGroup = 32u);
	TERRAENGINE_API VulkanVirtualGeometryExecutorRecordingResult RecordVulkanDirectMeshletBatches(
		VkCommandBuffer commandBuffer, const VulkanDirectMeshletBatchPlan& plan,
		VkBuffer existingIndirectBuffer, VkDeviceSize firstIndirectByteOffset);
	TERRAENGINE_API VulkanVirtualGeometryProductionDecodePlan
		BuildVulkanVirtualGeometryProductionDecodePlan(
			VulkanVirtualGeometryProductionDecodeRequest request,
			std::uint64_t generation, std::uint64_t expectedDecodedHash,
			const VulkanVirtualGeometryProductionDecodeResources& resources,
			const VulkanVirtualGeometryProductionDecodePipelines& pipelines);
	TERRAENGINE_API VulkanVirtualGeometryExecutorRecordingResult
		RecordVulkanVirtualGeometryProductionDecode(
			VkCommandBuffer commandBuffer,
			const VulkanVirtualGeometryProductionDecodePlan& plan,
			const VulkanVirtualGeometryProductionDecodeResources& resources,
			const VulkanVirtualGeometryProductionDecodePipelines& pipelines);
	TERRAENGINE_API VulkanVirtualGeometryProductionDecodeParityReceipt
		VerifyVulkanVirtualGeometryProductionDecodeParity(
			const VulkanVirtualGeometryProductionDecodePlan& plan,
			const VulkanVirtualGeometryProductionDecodeResult& deviceResult,
			std::span<const std::uint8_t> readbackBytes);
} // namespace TerraEngine::Renderer::Vulkan
