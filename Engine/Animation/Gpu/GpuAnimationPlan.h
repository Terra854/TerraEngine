// GpuAnimationPlan.h - Render-API-neutral GPU animation ABI and batch planning.
//
// Compute shaders, renderer back ends, and CPU planning consume this contract.
// Every shader-visible record uses fixed-width fields and an explicit alignment
// so a compiler or graphics API cannot silently change the bytes on upload.

#pragma once

#include "Animation/Codec/AnimationCodec.h"
#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace TerraEngine::Animation {
	// Descriptor set zero remains owned by the renderer's global bindless data.
	// Animation has a dedicated set so this ABI stays independent of Vulkan,
	// Direct3D, Metal, and renderer-owned descriptor object lifetimes.
	inline constexpr std::uint32_t kGpuAnimationGlobalBindlessDescriptorSet = 0U;
	inline constexpr std::uint32_t kGpuAnimationDescriptorSet = 1U;
	inline constexpr std::uint32_t kGpuAnimationDescriptorLayoutVersion = 1U;
	inline constexpr std::uint32_t kGpuAnimationInvalidResourceIndex = std::numeric_limits<std::uint32_t>::max();

	// Bindings are append-only. The matching HLSL declarations must use these
	// exact values when the animation compute shader files are introduced.
	enum class GpuAnimationDescriptorBinding : std::uint32_t {
		GenerationRecords = 0U,
		WorkPackets = 1U,
		DispatchRecords = 2U,
		ProgramWords = 3U,
		PackedClipBytes = 4U,
		SkeletonTables = 5U,
		MaskTables = 6U,
		MorphInputs = 7U,
		Scratch = 8U,
		LocalPoses = 9U,
		ModelPoses = 10U,
		PaletteMatrices = 11U,
		MorphOutputs = 12U,
		BoundsOutputs = 13U,
		Diagnostics = 14U
	};

	inline constexpr std::uint32_t kGpuAnimationDescriptorBindingCount = 15U;

	// Resident inputs are copied into the immutable frame plan before it crosses
	// the renderer boundary.  Keeping the bytes here avoids retaining a span into
	// an asset cache that may be reloaded while a worker is lowering the frame.
	struct GpuAnimationResidentPayloads {
		std::vector<std::uint32_t> programWords;
		std::vector<std::uint8_t> packedClipBytes;
		std::vector<std::uint8_t> skeletonTables;
		std::vector<std::uint8_t> maskTables;
		std::vector<std::uint8_t> morphInputs;

		[[nodiscard]] bool HasRequiredSampleAndHierarchyInputs() const noexcept
		{
			return !programWords.empty() && !packedClipBytes.empty() && !skeletonTables.empty();
		}
	};

	// The morph resource starts with a checked table header and the deformable
	// mesh's whole-animation conservative bounds. Records then map mesh-ordered
	// output lanes to cooked scalar tracks sampled at the packet's fixed tick.
	inline constexpr std::uint32_t kGpuAnimationMorphTableMagic =
		0x31544d41U; // "AMT1"
	inline constexpr std::uint32_t kGpuAnimationMorphTableVersion = 1U;
	inline constexpr std::uint32_t kGpuAnimationMorphInputRecordBytes = 16U;

	struct alignas(16) GpuAnimationMorphTableHeader {
		std::uint32_t magic = kGpuAnimationMorphTableMagic;
		std::uint32_t versionAndStride =
			kGpuAnimationMorphTableVersion
				| (kGpuAnimationMorphInputRecordBytes << 16U);
		std::uint32_t morphCount = 0U;
		std::uint32_t reserved = 0U;
	};

	struct alignas(16) GpuAnimationMorphBoundsRecord {
		std::array<float, 4U> minimum{};
		std::array<float, 4U> maximum{};
	};

	struct alignas(16) GpuAnimationMorphInputRecord {
		std::uint32_t trackLocatorOffsetBytes = 0U;
		std::array<std::uint32_t, 3U> reserved{};
	};

	static_assert(sizeof(GpuAnimationMorphTableHeader) == 16U);
	static_assert(sizeof(GpuAnimationMorphBoundsRecord) == 32U);
	static_assert(sizeof(GpuAnimationMorphInputRecord) == 16U);
	static_assert(alignof(GpuAnimationMorphInputRecord) == 16U);

	// Program joint records point at one clip-resident directory per T/R/S track.
	// Each directory selects the cooked segment containing the packet's resolved
	// clip-local tick, so long clips, loop seams, and reverse playback use the same
	// immutable codec bytes as the CPU evaluator without expanding the work packet.
	inline constexpr std::uint32_t kGpuAnimationTrackDirectoryMagic =
		0x31445441U; // "ATD1"
	inline constexpr std::uint32_t kGpuAnimationTrackDirectoryVersion = 1U;
	inline constexpr std::uint32_t kGpuAnimationTrackDirectoryEntryBytes = 16U;
	inline constexpr std::uint32_t kGpuAnimationMaximumTrackSegmentsV1 = 4096U;

	struct alignas(16) GpuAnimationTrackDirectoryHeader {
		std::uint32_t magic = kGpuAnimationTrackDirectoryMagic;
		std::uint32_t versionAndStride =
			kGpuAnimationTrackDirectoryVersion
				| (kGpuAnimationTrackDirectoryEntryBytes << 16U);
		std::uint32_t entryCount = 0U;
		std::uint32_t reserved = 0U;
	};

	struct alignas(16) GpuAnimationTrackDirectoryEntry {
		std::uint32_t startTick = 0U;
		std::uint32_t endTick = 0U;
		std::uint32_t trackHeaderOffsetBytes = 0U;
		std::uint32_t reserved = 0U;
	};

	static_assert(sizeof(GpuAnimationTrackDirectoryHeader) == 16U);
	static_assert(sizeof(GpuAnimationTrackDirectoryEntry)
		== kGpuAnimationTrackDirectoryEntryBytes);

	// Optional visual procedural records live in the program-word atlas selected
	// by programResourceIndex. The directory occupies the first sixteen bytes of
	// that program's fixed 64-byte header; shader and CPU validation use the same
	// bounded version-one limits before enabling the stage.
	inline constexpr std::uint32_t kGpuAnimationProceduralVersion = 1U;
	inline constexpr std::uint32_t kGpuAnimationProceduralRecordBytes = 96U;
	inline constexpr std::uint32_t kGpuAnimationMaximumProceduralRecordsV1 = 64U;

	// Ragdoll physics output is copied into the same immutable frame-owned
	// program-word upload. generation.reserved contains this header's aligned byte
	// offset only while AllowRagdollVisual is set; it is never a CPU/GPU address.
	inline constexpr std::uint32_t kGpuAnimationRagdollTableMagic = 0x31474152U; // "RAG1"
	inline constexpr std::uint32_t kGpuAnimationRagdollVersion = 1U;
	inline constexpr std::uint32_t kGpuAnimationRagdollRecordBytes = 96U;
	inline constexpr std::uint32_t kGpuAnimationMaximumRagdollRecordsV1 = 256U;
	inline constexpr std::uint32_t kGpuAnimationRagdollTableEnabled = 0x01U;
	inline constexpr std::uint32_t kGpuAnimationRagdollAllTableFlags =
		kGpuAnimationRagdollTableEnabled;

	struct alignas(16) GpuAnimationRagdollTableHeader {
		std::uint32_t magic = kGpuAnimationRagdollTableMagic;
		std::uint32_t versionAndStride = kGpuAnimationRagdollVersion
			| (kGpuAnimationRagdollRecordBytes << 16U);
		std::uint32_t recordCount = 0U;
		std::uint32_t flags = 0U;
		std::uint32_t physicsGeneration = 0U;
		std::uint32_t skeletonGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		std::uint32_t reserved = 0U;
	};

	struct alignas(16) GpuAnimationRagdollRecord {
		std::uint32_t jointIndex = 0U;
		std::uint32_t bodyGeneration = 0U;
		std::uint32_t physicsGeneration = 0U;
		std::uint32_t skeletonGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		std::uint32_t flags = 0U;
		float profileWeight = 0.0F;
		float activationWeight = 0.0F;
		float bodyWeight = 0.0F;
		std::uint32_t reserved = 0U;
		std::array<float, 4U> row0{};
		std::array<float, 4U> row1{};
		std::array<float, 4U> row2{};
	};

	// Stage numbers are part of the C++/HLSL ABI. New stages append after
	// Palette; changing an existing number would reinterpret recorded plans.
	enum class GpuAnimationStage : std::uint32_t {
		Sample = 0U,
		Blend = 1U,
		Hierarchy = 2U,
		Procedural = 3U,
		Ragdoll = 4U,
		Morph = 5U,
		Bounds = 6U,
		Palette = 7U
	};

	inline constexpr std::uint32_t kGpuAnimationStageCount = 8U;
	inline constexpr std::uint32_t kGpuAnimationAllStageMask = (1U << kGpuAnimationStageCount) - 1U;

	[[nodiscard]] constexpr std::uint32_t GpuAnimationStageMask(GpuAnimationStage stage) noexcept
	{
		return 1U << static_cast<std::uint32_t>(stage);
	}

	[[nodiscard]] constexpr bool IsGpuAnimationStageValueValid(std::uint32_t stage) noexcept
	{
		return stage < kGpuAnimationStageCount;
	}

	// Work flags are consumed directly by shaders. Their frozen values describe
	// playback direction, safe-output policy, optional channels, and authority.
	enum class GpuAnimationWorkFlag : std::uint32_t {
		Looping = 0x001U,
		Reverse = 0x002U,
		// Bind pose is the only stateless safe output. Keep the legacy alias so
		// already-recorded callers retain their exact version-one bit value.
		UseBindPoseFallback = 0x004U,
		UseReferenceFallback = UseBindPoseFallback,
		UseHoldLastFallback = 0x008U,
		HasMorph = 0x010U,
		HasRootMotion = 0x020U,
		HybridCpuAuthority = 0x040U,
		DeterministicReference = 0x080U,
		AllowVisualProcedural = 0x100U,
		AllowRagdollVisual = 0x200U,
		SuspendVisualEvaluation = 0x400U
	};

	inline constexpr std::uint32_t kGpuAnimationAllWorkFlags = 0x7ffU;

	// Output flags participate in the deterministic compatibility key. Work with
	// different output obligations cannot accidentally share a renderer batch.
	enum class GpuAnimationOutputFlag : std::uint32_t {
		Pose = 0x01U,
		MorphWeights = 0x02U,
		Bounds = 0x04U,
		Palette = 0x08U
	};

	inline constexpr std::uint32_t kGpuAnimationAllOutputFlags = 0x0fU;

	// Dispatch flags describe command behavior; barrier intent has a separate
	// frozen enum because renderer back ends lower the boundaries differently.
	enum class GpuAnimationDispatchFlag : std::uint32_t {
		WritesDiagnostics = 0x01U,
		UsesBindPoseFallback = 0x02U,
		UsesReferenceFallback = UsesBindPoseFallback,
		UsesHoldLastFallback = 0x04U
	};

	inline constexpr std::uint32_t kGpuAnimationAllDispatchFlags = 0x07U;

	// Each bit names one producer-to-consumer visibility boundary. Optional
	// procedural and ragdoll stages must retain distinct boundaries because they
	// can be independently enabled by an immutable evaluation program.
	enum class GpuAnimationBarrierFlag : std::uint32_t {
		CodecToBlend = 0x01U,
		BlendToHierarchy = 0x02U,
		HierarchyToProcedural = 0x04U,
		HierarchyToRagdoll = 0x08U,
		HierarchyToMorph = 0x10U,
		ProceduralToRagdoll = 0x20U,
		ProceduralToMorph = 0x40U,
		RagdollToMorph = 0x80U,
		MorphToBounds = 0x100U,
		BoundsToPalette = 0x200U,
		PaletteToRender = 0x400U
	};

	inline constexpr std::uint32_t kGpuAnimationAllBarrierFlags = 0x7ffU;

	// GPU-visible errors are bit flags so shader lanes can use an atomic OR. The
	// shader must still write a conservative pose/bounds result after a failure.
	enum class GpuAnimationDiagnosticFlag : std::uint32_t {
		InvalidRecord = 0x001U,
		StaleGeneration = 0x002U,
		RangeOverflow = 0x004U,
		MissingResource = 0x008U,
		NonFiniteOutput = 0x010U,
		ScratchOverflow = 0x020U,
		PaletteOverflow = 0x040U,
		OutputOverflow = 0x080U,
		BindPoseFallback = 0x100U,
		HoldLastFallback = 0x200U,
		RendererLost = 0x400U
	};

	inline constexpr std::uint32_t kGpuAnimationAllDiagnosticFlags = 0x7ffU;

	[[nodiscard]] constexpr bool HasGpuAnimationFlag(
		std::uint32_t flags,
		GpuAnimationWorkFlag flag) noexcept
	{
		return (flags & static_cast<std::uint32_t>(flag)) != 0U;
	}

	[[nodiscard]] constexpr bool HasGpuAnimationDiagnostic(
		std::uint32_t flags,
		GpuAnimationDiagnosticFlag flag) noexcept
	{
		return (flags & static_cast<std::uint32_t>(flag)) != 0U;
	}

	// Generation records carry every lifetime that can invalidate a visual result.
	// The frame ID is split into two lanes so HLSL need not require uint64 support.
	struct alignas(16) GpuAnimationGenerationRecord {
		std::uint32_t instanceSlot = 0U;
		std::uint32_t instanceGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		std::uint32_t workGeneration = 0U;
		std::uint32_t programGeneration = 0U;
		std::uint32_t controllerGeneration = 0U;
		std::uint32_t skeletonGeneration = 0U;
		std::uint32_t clipGeneration = 0U;
		std::uint32_t meshGeneration = 0U;
		std::uint32_t resourceGeneration = 0U;
		std::uint32_t rendererGeneration = 0U;
		std::uint32_t sceneGeneration = 0U;
		std::uint32_t frameIdLow = 0U;
		std::uint32_t frameIdHigh = 0U;
		std::uint32_t flags = 0U;
		std::uint32_t reserved = 0U;

		auto operator<=>(const GpuAnimationGenerationRecord&) const = default;
	};

	[[nodiscard]] constexpr bool IsGpuAnimationGenerationRecordValid(
		const GpuAnimationGenerationRecord& record) noexcept
	{
		return record.instanceGeneration != 0U
			&& record.componentGeneration != 0U
			&& record.workGeneration != 0U
			&& record.programGeneration != 0U
			&& record.controllerGeneration != 0U
			&& record.skeletonGeneration != 0U
			&& record.clipGeneration != 0U
			&& record.meshGeneration != 0U
			&& record.resourceGeneration != 0U
			&& record.rendererGeneration != 0U
			&& record.sceneGeneration != 0U;
	}

	// GPU poses use three float4 lanes. Translation/scale W lanes are defined
	// padding, which keeps every element at the frozen 48-byte stride.
	struct alignas(16) GpuAnimationPoseRecord {
		std::array<float, 4U> translation{};
		std::array<float, 4U> rotation{};
		std::array<float, 4U> scale{};
	};

	// Affine and palette matrices share the same three-row representation. A
	// hierarchy result can therefore be copied into a palette without repacking.
	struct alignas(16) GpuAnimationAffineRecord {
		std::array<float, 4U> row0{};
		std::array<float, 4U> row1{};
		std::array<float, 4U> row2{};
	};

	struct alignas(16) GpuAnimationPaletteRecord {
		std::array<float, 4U> row0{};
		std::array<float, 4U> row1{};
		std::array<float, 4U> row2{};
	};

	// Bounds use float4 lanes so shaders may reserve each W lane for validity and
	// conservative-expansion metadata without changing the 32-byte ABI.
	struct alignas(16) GpuAnimationBoundsRecord {
		std::array<float, 4U> minimum{};
		std::array<float, 4U> maximum{};
	};

	// One bounded diagnostic entry is atomically claimed by the shader. flags is
	// updated with InterlockedOr; the remaining lanes identify the failed work.
	struct alignas(16) GpuAnimationDiagnosticRecord {
		std::uint32_t flags = 0U;
		std::uint32_t stableInstanceIdLow = 0U;
		std::uint32_t stableInstanceIdHigh = 0U;
		std::uint32_t stage = 0U;
		std::uint32_t workPacketIndex = 0U;
		std::uint32_t generationRecordIndex = 0U;
		std::uint32_t detail0 = 0U;
		std::uint32_t detail1 = 0U;
	};

	inline constexpr std::uint32_t kGpuAnimationPoseStride = 48U;
	inline constexpr std::uint32_t kGpuAnimationAffineStride = 48U;
	inline constexpr std::uint32_t kGpuAnimationPaletteStride = 48U;
	inline constexpr std::uint32_t kGpuAnimationBoundsStride = 32U;
	inline constexpr std::uint32_t kGpuAnimationDiagnosticStride = 32U;

	// The batch key contains content/layout values only. Pointers, graphics
	// handles, and allocation addresses are deliberately absent.
	struct alignas(16) GpuAnimationBatchKey {
		std::uint64_t programContentId = 0U;
		std::uint64_t skeletonTopologyId = 0U;
		std::uint32_t resourceLayoutId = kGpuAnimationDescriptorLayoutVersion;
		std::uint32_t stageMask = 0U;
		std::uint32_t outputFlags = 0U;
		std::uint32_t codecVersion = kAnimationCodecVersion;

		auto operator<=>(const GpuAnimationBatchKey&) const = default;
	};

	// Only content generations participate in batch compatibility. Per-instance,
	// component, work, and frame generations remain in the per-packet record.
	struct alignas(16) GpuAnimationBatchGenerationKey {
		std::uint32_t programGeneration = 0U;
		std::uint32_t controllerGeneration = 0U;
		std::uint32_t skeletonGeneration = 0U;
		std::uint32_t clipGeneration = 0U;
		std::uint32_t meshGeneration = 0U;
		std::uint32_t resourceGeneration = 0U;
		std::uint32_t rendererGeneration = 0U;
		std::uint32_t sceneGeneration = 0U;

		auto operator<=>(const GpuAnimationBatchGenerationKey&) const = default;
	};

	[[nodiscard]] constexpr GpuAnimationBatchGenerationKey MakeGpuAnimationBatchGenerationKey(
		const GpuAnimationGenerationRecord& generations) noexcept
	{
		return GpuAnimationBatchGenerationKey{
			generations.programGeneration,
			generations.controllerGeneration,
			generations.skeletonGeneration,
			generations.clipGeneration,
			generations.meshGeneration,
			generations.resourceGeneration,
			generations.rendererGeneration,
			generations.sceneGeneration};
	}

	// The immutable per-instance packet contains byte offsets into generation-
	// scoped buffers, never CPU pointers or native renderer objects.
	struct alignas(16) GpuAnimationWorkPacket {
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t stableProgramId = 0U;
		std::uint64_t inputPoseOffsetBytes = 0U;
		std::uint64_t scratchOffsetBytes = 0U;
		std::uint64_t localPoseOffsetBytes = 0U;
		std::uint64_t modelPoseOffsetBytes = 0U;
		std::uint64_t paletteOffsetBytes = 0U;
		std::uint64_t morphOutputOffsetBytes = 0U;
		std::uint32_t generationRecordIndex = 0U;
		std::uint32_t clipResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t programResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t skeletonResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t maskResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t morphResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t jointCount = 0U;
		std::uint32_t morphCount = 0U;
		std::uint32_t paletteEntryCount = 0U;
		// This is cooked mesh metadata, not a joint-count estimate. A mesh may
		// deliberately use fewer or more partitions than a contiguous skeleton.
		std::uint32_t palettePartitionCount = 0U;
		std::uint32_t scratchSizeBytes = 0U;
		std::uint32_t stageMask = 0U;
		std::uint32_t flags = 0U;
		std::uint32_t diagnosticIndex = 0U;
		std::uint32_t resourceLayoutId = kGpuAnimationDescriptorLayoutVersion;
		std::uint32_t boundsOutputIndex = 0U;
		std::uint32_t stableSubmissionOrdinal = 0U;
		std::uint32_t sampleTick = 0U;
		// Keep the final sixteen-byte lane explicit on both the C++ and HLSL
		// sides. Relying on C++ tail padding made sizeof report 144 bytes while
		// the shader used a 136-byte StructuredBuffer stride, so every packet
		// after the first was read eight bytes early.
		std::uint32_t reserved0 = 0U;
		std::uint32_t reserved1 = 0U;
	};

	// A dispatch record identifies one bounded element range in one stage. Large
	// work is split so every command remains inside the configured workgroup cap.
	struct alignas(16) GpuAnimationDispatchRecord {
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t elementOffset = 0U;
		std::uint32_t workPacketIndex = 0U;
		std::uint32_t generationRecordIndex = 0U;
		std::uint32_t stage = 0U;
		std::uint32_t elementCount = 0U;
		std::uint32_t flags = 0U;
		std::uint32_t barrierFlags = 0U;
		std::uint32_t diagnosticIndex = 0U;
		std::uint32_t resourceLayoutId = kGpuAnimationDescriptorLayoutVersion;
		std::uint32_t batchIndex = 0U;
		std::uint32_t stableSubmissionOrdinal = 0U;
		std::uint32_t reserved0 = 0U;
		std::uint32_t reserved1 = 0U;
	};

	// Push constants use eight uint lanes. Splitting the first-element offset
	// preserves checked 64-bit prefix sums even on shaders without uint64 support.
	struct alignas(16) GpuAnimationPushConstants {
		std::uint32_t stage = 0U;
		std::uint32_t firstDispatchRecord = 0U;
		std::uint32_t dispatchRecordCount = 0U;
		std::uint32_t firstElementLow = 0U;
		std::uint32_t firstElementHigh = 0U;
		std::uint32_t elementCount = 0U;
		std::uint32_t planGeneration = 0U;
		std::uint32_t flags = 0U;
	};

	// Commands contain API-neutral workgroup counts. A renderer lowers these
	// values but never stores native handles in this plan.
	struct alignas(16) GpuAnimationDispatchCommand {
		GpuAnimationPushConstants pushConstants{};
		std::uint32_t groupCountX = 0U;
		std::uint32_t groupCountY = 1U;
		std::uint32_t groupCountZ = 1U;
		std::uint32_t batchIndex = 0U;
	};

	// A batch owns a contiguous sorted packet range and freezes both content and
	// content-generation compatibility. stableBatchOrdinal is deterministic.
	struct alignas(16) GpuAnimationBatch {
		GpuAnimationBatchKey key{};
		GpuAnimationBatchGenerationKey generationKey{};
		std::uint32_t firstWorkPacket = 0U;
		std::uint32_t workPacketCount = 0U;
		std::uint32_t stableBatchOrdinal = 0U;
		std::uint32_t reserved = 0U;
	};

	// Stage spans expose explicit barrier boundaries to diagnostics and renderer
	// submission. Empty stages do not create a span or command.
	struct alignas(16) GpuAnimationStageSpan {
		std::uint64_t totalElementCount = 0U;
		std::uint32_t stage = 0U;
		std::uint32_t firstCommand = 0U;
		std::uint32_t commandCount = 0U;
		std::uint32_t firstDispatchRecord = 0U;
		std::uint32_t dispatchRecordCount = 0U;
		std::uint32_t barrierFlags = 0U;
	};

	enum class GpuAnimationArenaKind : std::uint32_t {
		Scratch = 0U,
		Palette = 1U
	};

	// Allocations retain both the renderer generation and a never-reused arena
	// epoch. The renderer generation may repeat after recovery; the epoch stops a
	// recycled byte range from validating a stale allocation in that case.
	struct alignas(16) GpuAnimationArenaAllocation {
		std::uint64_t offsetBytes = 0U;
		std::uint64_t sizeBytes = 0U;
		std::uint64_t arenaEpoch = 0U;
		std::uint32_t generation = 0U;
		std::uint32_t arenaKind = 0U;
		std::uint32_t alignmentBytes = 0U;
		std::uint32_t reserved = 0U;
	};

	inline constexpr std::uint64_t kGpuAnimationMaximumArenaBytesV1 = 1ULL << 32U;
	inline constexpr std::uint32_t kGpuAnimationArenaAlignment = 16U;
	inline constexpr std::uint32_t kGpuAnimationMaximumArenaAlignment = 256U;
	inline constexpr std::uint32_t kGpuAnimationThreadsPerGroup = 64U;
	inline constexpr std::uint32_t kGpuAnimationMaximumGroupsX = 65535U;
	// AnimationCodec.hlsli scans no more than this many records per dispatch.
	// Keep the producer-side split on the same frozen bound so no recorded work
	// is silently skipped by the shader's defensive clamp.
	inline constexpr std::uint32_t kGpuAnimationMaximumDispatchRecordsPerCommand = 1024U;
	inline constexpr std::uint64_t kGpuAnimationMaximumElementsPerDispatch =
		static_cast<std::uint64_t>(kGpuAnimationThreadsPerGroup) * kGpuAnimationMaximumGroupsX;
	inline constexpr std::uint32_t kGpuAnimationMaximumJointsV1 = 4096U;
	inline constexpr std::uint32_t kGpuAnimationMaximumMorphTargetsV1 = 4096U;
	inline constexpr std::uint32_t kGpuAnimationPaletteEntriesPerPartition = 256U;
	inline constexpr std::uint32_t kGpuAnimationMaximumPalettePartitionsV1 = 256U;
	inline constexpr std::uint32_t kGpuAnimationMaximumPaletteEntriesV1 =
		kGpuAnimationPaletteEntriesPerPartition * kGpuAnimationMaximumPalettePartitionsV1;
	inline constexpr std::uint32_t kGpuAnimationSkeletonDirectoryBytes = 16U;
	inline constexpr std::uint32_t kGpuAnimationSkeletonPaletteRecordBytes = 64U;

	enum class GpuAnimationPlanStatus : std::uint32_t {
		Ok = 0U,
		InvalidArgument = 1U,
		StaleGeneration = 2U,
		ArithmeticOverflow = 3U,
		ArenaCapacityExceeded = 4U,
		PlanLimitExceeded = 5U,
		DuplicateStableOrder = 6U,
		PalettePartitionLimitExceeded = 7U,
		AllocationFailure = 8U,
		ArenaEpochExhausted = 9U
	};

	// Defaults are bounded below uint32 indexing limits. Tests may lower limits to
	// exercise rejection without allocating unrealistic fixtures.
	struct alignas(16) GpuAnimationPlanLimits {
		std::uint32_t maximumPackets = 1U << 20U;
		std::uint32_t maximumBatches = 1U << 16U;
		std::uint32_t maximumDispatchRecords = 1U << 23U;
		std::uint32_t maximumDispatchCommands = 1U << 20U;
		std::uint64_t maximumElementsPerStage = std::numeric_limits<std::uint32_t>::max();
		std::uint32_t maximumGroupsX = kGpuAnimationMaximumGroupsX;
		std::uint32_t threadsPerGroup = kGpuAnimationThreadsPerGroup;
		std::uint32_t scratchAlignmentBytes = kGpuAnimationArenaAlignment;
		std::uint32_t maximumPalettePartitions = kGpuAnimationMaximumPalettePartitionsV1;
		std::uint32_t reserved0 = 0U;
		std::uint32_t reserved1 = 0U;

		auto operator<=>(const GpuAnimationPlanLimits&) const = default;
	};

	// Plan diagnostics are copied as one bounded 32-byte value. Build rejection
	// never publishes partial vectors or arena offsets; the returned status names
	// the failure, while a successfully published plan carries these counters.
	struct alignas(16) GpuAnimationPlanDiagnostics {
		std::uint32_t flags = 0U;
		std::uint32_t rejectedRequestCount = 0U;
		std::uint32_t fallbackRequestCount = 0U;
		std::uint32_t arenaFailureCount = 0U;
		std::uint32_t prefixSumFailureCount = 0U;
		std::uint32_t dispatchLimitFailureCount = 0U;
		std::uint32_t duplicateStableOrderCount = 0U;
		std::uint32_t palettePartitionFailureCount = 0U;
	};

	// A rejected plan cannot overwrite the caller's last-good plan, so its
	// bounded diagnostics are returned separately. The original request index
	// lets a producer identify the immutable request without relying on sort
	// order or transient container addresses.
	struct alignas(16) GpuAnimationPlanRejectionDetail {
		std::uint64_t stableInstanceId = 0U;
		std::uint32_t requestIndex = std::numeric_limits<std::uint32_t>::max();
		std::uint32_t stableSubmissionOrdinal = 0U;
		std::uint32_t status = static_cast<std::uint32_t>(GpuAnimationPlanStatus::Ok);
		std::uint32_t flags = 0U;
	};

	struct alignas(16) GpuAnimationPlanBuildDiagnostics {
		GpuAnimationPlanDiagnostics counters{};
		GpuAnimationPlanRejectionDetail rejection{};
	};

	// This identity never crosses the shader ABI. It retains the exact render-side
	// mesh and 64-bit lifetime values that accepted the cooked palette, so render
	// collection can reject a recook, scene switch, or renderer recovery without
	// comparing truncated lanes in GpuAnimationGenerationRecord.
	struct GpuAnimationRenderIdentity {
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint32_t meshHeaderVersion = 0U;
		std::uint64_t deformablePayloadGeneration = 0U;
		std::uint64_t rendererGeneration = 0U;
		std::uint64_t sceneGeneration = 0U;
		std::uint64_t skeletonCompatibilityFingerprint = 0U;

		[[nodiscard]] constexpr bool IsComplete() const noexcept
		{
			return meshId != TerraEngine::EMPTY_UUID
				&& meshHeaderVersion != 0U
				&& deformablePayloadGeneration != 0U
				&& rendererGeneration != 0U
				&& sceneGeneration != 0U
				&& skeletonCompatibilityFingerprint != 0U;
		}

		auto operator<=>(const GpuAnimationRenderIdentity&) const = default;
	};

	[[nodiscard]] constexpr bool IsGpuAnimationRenderIdentityValid(
		const GpuAnimationRenderIdentity& identity) noexcept
	{
		return identity.IsComplete();
	}

	// Requests are immutable worker-prepared values. The stable instance and
	// submission ordinal are final tie-breaks after content/generation sorting.
	struct alignas(16) GpuAnimationWorkRequest {
		GpuAnimationBatchKey batchKey{};
		GpuAnimationGenerationRecord generations{};
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t inputPoseOffsetBytes = 0U;
		std::uint32_t stableSubmissionOrdinal = 0U;
		std::uint32_t clipResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t programResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t skeletonResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t maskResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t morphResourceIndex = kGpuAnimationInvalidResourceIndex;
		std::uint32_t jointCount = 0U;
		std::uint32_t morphCount = 0U;
		std::uint32_t scratchSizeBytes = 0U;
		std::uint32_t scratchAlignmentBytes = kGpuAnimationArenaAlignment;
		std::uint32_t paletteEntryCount = 0U;
		std::uint32_t palettePartitionCount = 0U;
		std::uint32_t flags = 0U;
		std::uint32_t diagnosticIndex = 0U;
		// The producer resolves playback direction and looping before lowering.
		// Sampling therefore consumes this exact clip-local tick per instance.
		std::uint32_t sampleTick = 0U;
		GpuAnimationRenderIdentity renderIdentity{};
	};

	// This class is a checked offset allocator only. Renderer-owned buffers and
	// retirement fences stay outside the animation planner.
	class TERRAENGINE_API GpuAnimationArena {
	public:
		GpuAnimationArena() = default;
		GpuAnimationArena(GpuAnimationArenaKind kind, std::uint64_t capacityBytes) noexcept;

		[[nodiscard]] GpuAnimationPlanStatus Configure(
			GpuAnimationArenaKind kind,
			std::uint64_t capacityBytes) noexcept;
		[[nodiscard]] GpuAnimationPlanStatus BeginGeneration(std::uint32_t generation) noexcept;
		[[nodiscard]] GpuAnimationPlanStatus Allocate(
			std::uint64_t sizeBytes,
			std::uint32_t alignmentBytes,
			GpuAnimationArenaAllocation& output) noexcept;
		[[nodiscard]] bool Owns(const GpuAnimationArenaAllocation& allocation) const noexcept;

		[[nodiscard]] GpuAnimationArenaKind Kind() const noexcept { return kind_; }
		[[nodiscard]] std::uint64_t CapacityBytes() const noexcept { return capacityBytes_; }
		[[nodiscard]] std::uint64_t UsedBytes() const noexcept { return usedBytes_; }
		[[nodiscard]] std::uint32_t Generation() const noexcept { return generation_; }

	private:
		GpuAnimationArenaKind kind_ = GpuAnimationArenaKind::Scratch;
		std::uint64_t capacityBytes_ = 0U;
		std::uint64_t usedBytes_ = 0U;
		std::uint32_t generation_ = 0U;
		std::uint64_t arenaEpoch_ = 0U;
	};

	struct GpuAnimationPlan {
		std::uint32_t generation = 0U;
		// A cached topology is reusable only under the exact same safety bounds.
		// Retaining them in the CPU plan prevents a relaxed earlier build from
		// bypassing a later caller's tighter packet or dispatch limits.
		GpuAnimationPlanLimits buildLimits{};
		// These owned resident bytes match descriptor bindings three through seven.
		// A plan is published only after their producer has validated every packet.
		GpuAnimationResidentPayloads residentPayloads{};
		// When every request references the same immutable cooked payload, retain
		// that producer ownership instead of cloning clip/program/skeleton tables on
		// every animated frame. Atlas-built multi-payload plans continue to use the
		// owned value above.
		std::shared_ptr<const GpuAnimationResidentPayloads> sharedResidentPayloads;
		std::vector<GpuAnimationGenerationRecord> generationRecords;
		std::vector<GpuAnimationWorkPacket> workPackets;
		// Entries are appended beside their sorted work packet, preserving an exact
		// CPU-only render identity without expanding the frozen 144-byte packet.
		std::vector<GpuAnimationRenderIdentity> renderIdentities;
		std::vector<GpuAnimationDispatchRecord> dispatchRecords;
		std::vector<GpuAnimationDispatchCommand> dispatchCommands;
		std::vector<GpuAnimationBatch> batches;
		std::vector<GpuAnimationStageSpan> stageSpans;
		GpuAnimationPlanDiagnostics diagnostics{};

		[[nodiscard]] const GpuAnimationResidentPayloads& ResidentPayloads() const noexcept
		{
			return sharedResidentPayloads ? *sharedResidentPayloads : residentPayloads;
		}

		void Clear() noexcept;
	};

	// Prefix sums are public because scratch layouts and stage plans share the
	// same overflow rule. On failure offsets and total retain their input values.
	TERRAENGINE_API GpuAnimationPlanStatus ComputeGpuAnimationPrefixOffsets(
		std::span<const std::uint64_t> counts,
		std::uint64_t hardLimit,
		std::vector<std::uint64_t>& offsets,
		std::uint64_t& total,
		GpuAnimationPlanDiagnostics& diagnostics) noexcept;

	// Planning is transactional: output and both arenas publish only after every
	// request, allocation, prefix sum, and dispatch bound validates.
	TERRAENGINE_API GpuAnimationPlanStatus BuildGpuAnimationPlan(
		std::span<const GpuAnimationWorkRequest> requests,
		std::uint32_t planGeneration,
		GpuAnimationArena& scratchArena,
		GpuAnimationArena& paletteArena,
		GpuAnimationPlan& output,
		const GpuAnimationPlanLimits& limits = {},
		GpuAnimationPlanBuildDiagnostics* buildDiagnostics = nullptr) noexcept;

	// Stable crowds change sample ticks and per-frame generations while retaining
	// packet order, resource layout, scratch shapes, and dispatch topology. This
	// transactional fast path replays checked arena allocation and updates only the
	// dynamic lanes. It returns false without touching outputs when any topology
	// input changed, allowing callers to fall back to the complete builder.
	[[nodiscard]] TERRAENGINE_API bool TryRefreshGpuAnimationPlan(
		std::span<const GpuAnimationWorkRequest> requests,
		std::uint32_t planGeneration,
		GpuAnimationArena& scratchArena,
		GpuAnimationArena& paletteArena,
		const GpuAnimationPlan& previous,
		GpuAnimationPlan& output,
		const GpuAnimationPlanLimits& limits = {}) noexcept;

	// Shared helpers keep CPU tests, shaders, and renderer validation on one rule.
	[[nodiscard]] TERRAENGINE_API std::uint32_t GpuAnimationStageElementCount(
		GpuAnimationStage stage,
		const GpuAnimationWorkPacket& packet) noexcept;
	[[nodiscard]] TERRAENGINE_API std::uint32_t GpuAnimationBarrierAfterStage(
		GpuAnimationStage stage) noexcept;
	[[nodiscard]] TERRAENGINE_API std::uint32_t GpuAnimationBarrierBetweenStages(
		GpuAnimationStage producer,
		GpuAnimationStage consumer) noexcept;
	[[nodiscard]] TERRAENGINE_API bool GpuAnimationGenerationsMatch(
		const GpuAnimationGenerationRecord& expected,
		const GpuAnimationGenerationRecord& actual) noexcept;

	// Compile-time assertions freeze the shared codec and GPU record bytes.
	static_assert(sizeof(AnimationCodecTrackHeaderV1) == 88U);
	static_assert(sizeof(AnimationCodecVectorKeyV1) == 6U);
	static_assert(sizeof(AnimationCodecQuaternionKeyV1) == 8U);
	static_assert(offsetof(AnimationCodecTrackHeaderV1, segmentStartTick) == 16U);
	static_assert(offsetof(AnimationCodecTrackHeaderV1, rangeMinimum) == 32U);
	static_assert(offsetof(AnimationCodecTrackHeaderV1, constantRotation) == 68U);
	static_assert(offsetof(AnimationCodecTrackHeaderV1, constantScalar) == 84U);

	static_assert(sizeof(GpuAnimationGenerationRecord) == 64U);
	static_assert(sizeof(GpuAnimationPoseRecord) == kGpuAnimationPoseStride);
	static_assert(sizeof(GpuAnimationAffineRecord) == kGpuAnimationAffineStride);
	static_assert(sizeof(GpuAnimationPaletteRecord) == kGpuAnimationPaletteStride);
	static_assert(sizeof(GpuAnimationBoundsRecord) == kGpuAnimationBoundsStride);
	static_assert(sizeof(GpuAnimationDiagnosticRecord) == kGpuAnimationDiagnosticStride);
	static_assert(sizeof(GpuAnimationBatchKey) == 32U);
	static_assert(sizeof(GpuAnimationBatchGenerationKey) == 32U);
	static_assert(sizeof(GpuAnimationWorkPacket) == 144U);
	static_assert(sizeof(GpuAnimationDispatchRecord) == 64U);
	static_assert(sizeof(GpuAnimationPushConstants) == 32U);
	static_assert(sizeof(GpuAnimationDispatchCommand) == 48U);
	static_assert(sizeof(GpuAnimationBatch) == 80U);
	static_assert(sizeof(GpuAnimationStageSpan) == 32U);
	static_assert(sizeof(GpuAnimationArenaAllocation) == 48U);
	static_assert(sizeof(GpuAnimationPlanLimits) == 48U);
	static_assert(sizeof(GpuAnimationPlanDiagnostics) == 32U);
	static_assert(sizeof(GpuAnimationPlanRejectionDetail) == 32U);
	static_assert(sizeof(GpuAnimationPlanBuildDiagnostics) == 64U);
	static_assert(sizeof(GpuAnimationRenderIdentity) == 56U);
	static_assert(sizeof(GpuAnimationWorkRequest) == 240U);

	template<typename Record>
	inline constexpr bool kGpuAnimationAbiRecord = std::is_standard_layout_v<Record>
		&& std::is_trivially_copyable_v<Record>
		&& alignof(Record) == 16U;

	static_assert(kGpuAnimationAbiRecord<GpuAnimationGenerationRecord>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationPoseRecord>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationAffineRecord>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationPaletteRecord>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationBoundsRecord>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationDiagnosticRecord>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationBatchKey>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationBatchGenerationKey>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationWorkPacket>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationDispatchRecord>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationPushConstants>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationDispatchCommand>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationBatch>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationStageSpan>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationArenaAllocation>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationPlanLimits>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationPlanDiagnostics>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationPlanRejectionDetail>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationPlanBuildDiagnostics>);
	static_assert(kGpuAnimationAbiRecord<GpuAnimationWorkRequest>);
}
