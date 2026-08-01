// GpuAnimationPlan.cpp - Checked deterministic GPU animation work planning.

#include "Animation/Gpu/GpuAnimationPlan.h"

#include <algorithm>
#include <limits>
#include <new>
#include <numeric>
#include <tuple>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		struct ScratchLayout {
			std::uint64_t totalSizeBytes = 0U;
			std::uint64_t localPoseOffsetBytes = 0U;
			std::uint64_t modelPoseOffsetBytes = 0U;
			std::uint64_t morphOutputOffsetBytes = 0U;
		};

		[[nodiscard]] constexpr bool IsPowerOfTwo(std::uint32_t value) noexcept
		{
			return value != 0U && (value & (value - 1U)) == 0U;
		}

		[[nodiscard]] constexpr bool CheckedGpuAdd(
			std::uint64_t left,
			std::uint64_t right,
			std::uint64_t& output) noexcept
		{
			if(left > std::numeric_limits<std::uint64_t>::max() - right) {
				return false;
			}
			output = left + right;
			return true;
		}

		[[nodiscard]] constexpr bool CheckedGpuMultiply(
			std::uint64_t left,
			std::uint64_t right,
			std::uint64_t& output) noexcept
		{
			if(left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
				return false;
			}
			output = left * right;
			return true;
		}

		[[nodiscard]] constexpr bool CheckedGpuAlignUp(
			std::uint64_t value,
			std::uint32_t alignment,
			std::uint64_t& output) noexcept
		{
			if(!IsPowerOfTwo(alignment)) {
				return false;
			}
			const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1U);
			if(value > std::numeric_limits<std::uint64_t>::max() - mask) {
				return false;
			}
			output = (value + mask) & ~mask;
			return true;
		}

		void SaturatingIncrement(std::uint32_t& value) noexcept
		{
			if(value != std::numeric_limits<std::uint32_t>::max()) {
				++value;
			}
		}

		void RecordDiagnostic(
			GpuAnimationPlanDiagnostics& diagnostics,
			GpuAnimationDiagnosticFlag flag,
			std::uint32_t& counter) noexcept
		{
			diagnostics.flags |= static_cast<std::uint32_t>(flag);
			SaturatingIncrement(counter);
		}

		void RecordPlanFailure(
			GpuAnimationPlanBuildDiagnostics* buildDiagnostics,
			GpuAnimationPlanStatus status,
			GpuAnimationDiagnosticFlag flag,
			std::uint32_t requestIndex,
			const GpuAnimationWorkRequest* request) noexcept
		{
			if(buildDiagnostics == nullptr) {
				return;
			}

			GpuAnimationPlanDiagnostics& counters = buildDiagnostics->counters;
			counters.flags |= static_cast<std::uint32_t>(flag);
			SaturatingIncrement(counters.rejectedRequestCount);
			switch(status) {
			case GpuAnimationPlanStatus::ArenaCapacityExceeded:
			case GpuAnimationPlanStatus::ArenaEpochExhausted:
				SaturatingIncrement(counters.arenaFailureCount);
				break;
			case GpuAnimationPlanStatus::ArithmeticOverflow:
				SaturatingIncrement(counters.prefixSumFailureCount);
				break;
			case GpuAnimationPlanStatus::PlanLimitExceeded:
				SaturatingIncrement(counters.dispatchLimitFailureCount);
				break;
			case GpuAnimationPlanStatus::DuplicateStableOrder:
				SaturatingIncrement(counters.duplicateStableOrderCount);
				break;
			case GpuAnimationPlanStatus::PalettePartitionLimitExceeded:
				SaturatingIncrement(counters.palettePartitionFailureCount);
				break;
			default:
				break;
			}

			GpuAnimationPlanRejectionDetail& rejection = buildDiagnostics->rejection;
			rejection.requestIndex = requestIndex;
			rejection.status = static_cast<std::uint32_t>(status);
			rejection.flags = static_cast<std::uint32_t>(flag);
			if(request != nullptr) {
				rejection.stableInstanceId = request->stableInstanceId;
				rejection.stableSubmissionOrdinal = request->stableSubmissionOrdinal;
			}
		}

		[[nodiscard]] constexpr bool IsArenaKindValid(GpuAnimationArenaKind kind) noexcept
		{
			return kind == GpuAnimationArenaKind::Scratch || kind == GpuAnimationArenaKind::Palette;
		}

		[[nodiscard]] bool IsPlanLimitConfigurationValid(const GpuAnimationPlanLimits& limits) noexcept
		{
			const std::uint32_t scratchAlignment = std::max(
				kGpuAnimationArenaAlignment,
				limits.scratchAlignmentBytes);
			return limits.maximumPackets != 0U
				&& limits.maximumBatches != 0U
				&& limits.maximumDispatchRecords != 0U
				&& limits.maximumDispatchCommands != 0U
				&& limits.maximumElementsPerStage != 0U
				&& limits.maximumGroupsX != 0U
				&& limits.maximumGroupsX <= kGpuAnimationMaximumGroupsX
				&& limits.threadsPerGroup == kGpuAnimationThreadsPerGroup
				&& IsPowerOfTwo(scratchAlignment)
				&& scratchAlignment <= kGpuAnimationMaximumArenaAlignment
				&& limits.maximumPalettePartitions != 0U
				&& limits.maximumPalettePartitions <= kGpuAnimationMaximumPalettePartitionsV1;
		}

		[[nodiscard]] bool IsRequestValid(
			const GpuAnimationWorkRequest& request,
			const GpuAnimationPlanLimits& limits) noexcept
		{
			if(request.stableInstanceId == 0U
				|| request.batchKey.programContentId == 0U
				|| request.batchKey.skeletonTopologyId == 0U
				|| request.batchKey.resourceLayoutId != kGpuAnimationDescriptorLayoutVersion
				|| request.batchKey.codecVersion != kAnimationCodecVersion
				|| request.batchKey.stageMask == 0U
				|| (request.batchKey.stageMask & ~kGpuAnimationAllStageMask) != 0U
				|| request.batchKey.outputFlags == 0U
				|| (request.batchKey.outputFlags & ~kGpuAnimationAllOutputFlags) != 0U
				|| !IsGpuAnimationGenerationRecordValid(request.generations)
				|| request.jointCount == 0U
				|| request.jointCount > kGpuAnimationMaximumJointsV1
				|| request.morphCount > kGpuAnimationMaximumMorphTargetsV1
				|| request.paletteEntryCount > kGpuAnimationMaximumPaletteEntriesV1
				|| (request.flags & ~kGpuAnimationAllWorkFlags) != 0U
				|| request.clipResourceIndex == kGpuAnimationInvalidResourceIndex
				|| request.programResourceIndex == kGpuAnimationInvalidResourceIndex
				|| request.skeletonResourceIndex == kGpuAnimationInvalidResourceIndex
				|| (request.inputPoseOffsetBytes % kGpuAnimationArenaAlignment) != 0U) {
				return false;
			}

			const bool referenceFallback = HasGpuAnimationFlag(
				request.flags,
				GpuAnimationWorkFlag::UseReferenceFallback);
			const bool holdLastFallback = HasGpuAnimationFlag(
				request.flags,
				GpuAnimationWorkFlag::UseHoldLastFallback);
			if(referenceFallback && holdLastFallback) {
				return false;
			}
			if(HasGpuAnimationFlag(request.flags, GpuAnimationWorkFlag::HybridCpuAuthority)
				&& HasGpuAnimationFlag(request.flags, GpuAnimationWorkFlag::DeterministicReference)) {
				return false;
			}

			const std::uint32_t poseStageMask = GpuAnimationStageMask(GpuAnimationStage::Sample)
				| GpuAnimationStageMask(GpuAnimationStage::Blend)
				| GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Procedural)
				| GpuAnimationStageMask(GpuAnimationStage::Ragdoll);
			const bool hasPoseStage = (request.batchKey.stageMask & poseStageMask) != 0U;
			const bool hasPoseOutput = (request.batchKey.outputFlags
				& static_cast<std::uint32_t>(GpuAnimationOutputFlag::Pose)) != 0U;
			if(hasPoseStage != hasPoseOutput) {
				return false;
			}
			if(HasGpuAnimationFlag(request.flags, GpuAnimationWorkFlag::AllowVisualProcedural)
				&& (request.batchKey.stageMask & GpuAnimationStageMask(GpuAnimationStage::Procedural)) == 0U) {
				return false;
			}
			if(HasGpuAnimationFlag(request.flags, GpuAnimationWorkFlag::AllowRagdollVisual)
				&& (request.batchKey.stageMask & GpuAnimationStageMask(GpuAnimationStage::Ragdoll)) == 0U) {
				return false;
			}

			const bool hasMorphFlag = HasGpuAnimationFlag(request.flags, GpuAnimationWorkFlag::HasMorph);
			const bool hasMorphStage = (request.batchKey.stageMask
				& GpuAnimationStageMask(GpuAnimationStage::Morph)) != 0U;
			const bool hasMorphOutput = (request.batchKey.outputFlags
				& static_cast<std::uint32_t>(GpuAnimationOutputFlag::MorphWeights)) != 0U;
			if((request.morphCount != 0U) != hasMorphFlag
				|| (request.morphCount != 0U) != hasMorphStage
				|| (request.morphCount != 0U) != hasMorphOutput
				|| (request.morphCount != 0U
					&& request.morphResourceIndex == kGpuAnimationInvalidResourceIndex)) {
				return false;
			}

			const bool hasPaletteStage = (request.batchKey.stageMask
				& GpuAnimationStageMask(GpuAnimationStage::Palette)) != 0U;
			const bool hasPaletteOutput = (request.batchKey.outputFlags
				& static_cast<std::uint32_t>(GpuAnimationOutputFlag::Palette)) != 0U;
			if((request.paletteEntryCount != 0U) != hasPaletteStage
				|| (request.paletteEntryCount != 0U) != hasPaletteOutput) {
				return false;
			}

			const bool hasBoundsStage = (request.batchKey.stageMask
				& GpuAnimationStageMask(GpuAnimationStage::Bounds)) != 0U;
			const bool hasBoundsOutput = (request.batchKey.outputFlags
				& static_cast<std::uint32_t>(GpuAnimationOutputFlag::Bounds)) != 0U;
			if(hasBoundsStage != hasBoundsOutput) {
				return false;
			}

			const std::uint32_t scratchAlignment = std::max({
				kGpuAnimationArenaAlignment,
				limits.scratchAlignmentBytes,
				request.scratchAlignmentBytes});
			if(!IsPowerOfTwo(scratchAlignment)
				|| scratchAlignment > kGpuAnimationMaximumArenaAlignment) {
				return false;
			}

			// The cooker owns partitioning: it considers meshlet influence sets, not
			// merely a contiguous skeleton range. Planning must consume that exact
			// metadata rather than synthesizing a potentially incompatible ceil.
			return (request.paletteEntryCount == 0U && request.palettePartitionCount == 0U)
				|| (request.paletteEntryCount != 0U
					&& request.palettePartitionCount != 0U
					&& request.palettePartitionCount <= kGpuAnimationMaximumPalettePartitionsV1
					&& request.paletteEntryCount
						<= request.palettePartitionCount
							* kGpuAnimationPaletteEntriesPerPartition);
		}

		[[nodiscard]] bool BuildScratchLayout(
			const GpuAnimationWorkRequest& request,
			ScratchLayout& output) noexcept
		{
			ScratchLayout temporary{};
			std::uint64_t cursor = request.scratchSizeBytes;
			if(!CheckedGpuAlignUp(cursor, kGpuAnimationArenaAlignment, cursor)) {
				return false;
			}
			temporary.localPoseOffsetBytes = cursor;

			std::uint64_t poseBytes = 0U;
			if(!CheckedGpuMultiply(request.jointCount, kGpuAnimationPoseStride, poseBytes)
				|| !CheckedGpuAdd(cursor, poseBytes, cursor)
				|| !CheckedGpuAlignUp(cursor, kGpuAnimationArenaAlignment, cursor)) {
				return false;
			}
			temporary.modelPoseOffsetBytes = cursor;

			if(!CheckedGpuMultiply(request.jointCount, kGpuAnimationAffineStride, poseBytes)
				|| !CheckedGpuAdd(cursor, poseBytes, cursor)
				|| !CheckedGpuAlignUp(cursor, kGpuAnimationArenaAlignment, cursor)) {
				return false;
			}
			temporary.morphOutputOffsetBytes = cursor;

			std::uint64_t morphBytes = 0U;
			if(!CheckedGpuMultiply(request.morphCount, sizeof(float), morphBytes)
				|| !CheckedGpuAdd(cursor, morphBytes, cursor)
				|| !CheckedGpuAlignUp(cursor, kGpuAnimationArenaAlignment, cursor)
				|| cursor > std::numeric_limits<std::uint32_t>::max()) {
				return false;
			}
			temporary.totalSizeBytes = cursor;
			output = temporary;
			return true;
		}

		[[nodiscard]] bool SameBatchCompatibility(
			const GpuAnimationWorkRequest& left,
			const GpuAnimationWorkRequest& right) noexcept
		{
			return left.batchKey == right.batchKey
				&& MakeGpuAnimationBatchGenerationKey(left.generations)
					== MakeGpuAnimationBatchGenerationKey(right.generations);
		}

		[[nodiscard]] bool RequestLess(
			const GpuAnimationWorkRequest& left,
			const GpuAnimationWorkRequest& right) noexcept
		{
			const GpuAnimationBatchGenerationKey leftGenerations =
				MakeGpuAnimationBatchGenerationKey(left.generations);
			const GpuAnimationBatchGenerationKey rightGenerations =
				MakeGpuAnimationBatchGenerationKey(right.generations);
			return std::tie(
				left.batchKey,
				leftGenerations,
				left.stableInstanceId,
				left.stableSubmissionOrdinal)
				< std::tie(
					right.batchKey,
					rightGenerations,
					right.stableInstanceId,
					right.stableSubmissionOrdinal);
		}

		[[nodiscard]] std::uint32_t DispatchFlagsForPacket(
			const GpuAnimationWorkPacket& packet) noexcept
		{
			std::uint32_t flags = static_cast<std::uint32_t>(
				GpuAnimationDispatchFlag::WritesDiagnostics);
			if(HasGpuAnimationFlag(packet.flags, GpuAnimationWorkFlag::UseBindPoseFallback)) {
				flags |= static_cast<std::uint32_t>(GpuAnimationDispatchFlag::UsesBindPoseFallback);
			}
			if(HasGpuAnimationFlag(packet.flags, GpuAnimationWorkFlag::UseHoldLastFallback)) {
				flags |= static_cast<std::uint32_t>(GpuAnimationDispatchFlag::UsesHoldLastFallback);
			}
			return flags;
		}
	}

	GpuAnimationArena::GpuAnimationArena(
		GpuAnimationArenaKind kind,
		std::uint64_t capacityBytes) noexcept
	{
		static_cast<void>(Configure(kind, capacityBytes));
	}

	GpuAnimationPlanStatus GpuAnimationArena::Configure(
		GpuAnimationArenaKind kind,
		std::uint64_t capacityBytes) noexcept
	{
		if(!IsArenaKindValid(kind) || capacityBytes > kGpuAnimationMaximumArenaBytesV1) {
			return GpuAnimationPlanStatus::InvalidArgument;
		}
		kind_ = kind;
		capacityBytes_ = capacityBytes;
		usedBytes_ = 0U;
		generation_ = 0U;
		arenaEpoch_ = 0U;
		return GpuAnimationPlanStatus::Ok;
	}

	GpuAnimationPlanStatus GpuAnimationArena::BeginGeneration(std::uint32_t generation) noexcept
	{
		if(generation == 0U) {
			return GpuAnimationPlanStatus::InvalidArgument;
		}
		// Do not wrap this value. The epoch is copied into every allocation and
		// is the proof that a byte range belongs to this exact arena lifetime.
		if(arenaEpoch_ == std::numeric_limits<std::uint64_t>::max()) {
			return GpuAnimationPlanStatus::ArenaEpochExhausted;
		}
		++arenaEpoch_;
		generation_ = generation;
		usedBytes_ = 0U;
		return GpuAnimationPlanStatus::Ok;
	}

	GpuAnimationPlanStatus GpuAnimationArena::Allocate(
		std::uint64_t sizeBytes,
		std::uint32_t alignmentBytes,
		GpuAnimationArenaAllocation& output) noexcept
	{
		const std::uint32_t effectiveAlignment = std::max(
			kGpuAnimationArenaAlignment,
			alignmentBytes);
		if(generation_ == 0U
			|| sizeBytes == 0U
			|| !IsPowerOfTwo(effectiveAlignment)
			|| effectiveAlignment > kGpuAnimationMaximumArenaAlignment) {
			return GpuAnimationPlanStatus::InvalidArgument;
		}

		std::uint64_t alignedOffset = 0U;
		std::uint64_t endOffset = 0U;
		if(!CheckedGpuAlignUp(usedBytes_, effectiveAlignment, alignedOffset)
			|| !CheckedGpuAdd(alignedOffset, sizeBytes, endOffset)) {
			return GpuAnimationPlanStatus::ArithmeticOverflow;
		}
		if(endOffset > capacityBytes_) {
			return GpuAnimationPlanStatus::ArenaCapacityExceeded;
		}

		GpuAnimationArenaAllocation temporary{};
		temporary.offsetBytes = alignedOffset;
		temporary.sizeBytes = sizeBytes;
		temporary.arenaEpoch = arenaEpoch_;
		temporary.generation = generation_;
		temporary.arenaKind = static_cast<std::uint32_t>(kind_);
		temporary.alignmentBytes = effectiveAlignment;
		usedBytes_ = endOffset;
		output = temporary;
		return GpuAnimationPlanStatus::Ok;
	}

	bool GpuAnimationArena::Owns(const GpuAnimationArenaAllocation& allocation) const noexcept
	{
		std::uint64_t endOffset = 0U;
		return allocation.arenaEpoch != 0U
			&& allocation.arenaEpoch == arenaEpoch_
			&& allocation.generation != 0U
			&& allocation.generation == generation_
			&& allocation.arenaKind == static_cast<std::uint32_t>(kind_)
			&& allocation.sizeBytes != 0U
			&& IsPowerOfTwo(allocation.alignmentBytes)
			&& allocation.alignmentBytes >= kGpuAnimationArenaAlignment
			&& allocation.alignmentBytes <= kGpuAnimationMaximumArenaAlignment
			&& (allocation.offsetBytes % allocation.alignmentBytes) == 0U
			&& CheckedGpuAdd(allocation.offsetBytes, allocation.sizeBytes, endOffset)
			&& endOffset <= usedBytes_
			&& endOffset <= capacityBytes_;
	}

	void GpuAnimationPlan::Clear() noexcept
	{
		generation = 0U;
		residentPayloads = {};
		sharedResidentPayloads.reset();
		generationRecords.clear();
		workPackets.clear();
		renderIdentities.clear();
		dispatchRecords.clear();
		dispatchCommands.clear();
		batches.clear();
		stageSpans.clear();
		diagnostics = {};
	}

	GpuAnimationPlanStatus ComputeGpuAnimationPrefixOffsets(
		std::span<const std::uint64_t> counts,
		std::uint64_t hardLimit,
		std::vector<std::uint64_t>& offsets,
		std::uint64_t& total,
		GpuAnimationPlanDiagnostics& diagnostics) noexcept
	{
		try {
			std::vector<std::uint64_t> temporary;
			temporary.reserve(counts.size());
			std::uint64_t running = 0U;
			for(const std::uint64_t count : counts) {
				temporary.push_back(running);
				if(count > hardLimit || running > hardLimit - count) {
					RecordDiagnostic(
						diagnostics,
						GpuAnimationDiagnosticFlag::RangeOverflow,
						diagnostics.prefixSumFailureCount);
					return GpuAnimationPlanStatus::ArithmeticOverflow;
				}
				running += count;
			}
			offsets = std::move(temporary);
			total = running;
			return GpuAnimationPlanStatus::Ok;
		} catch(const std::bad_alloc&) {
			RecordDiagnostic(
				diagnostics,
				GpuAnimationDiagnosticFlag::OutputOverflow,
				diagnostics.prefixSumFailureCount);
			return GpuAnimationPlanStatus::AllocationFailure;
		} catch(...) {
			RecordDiagnostic(
				diagnostics,
				GpuAnimationDiagnosticFlag::OutputOverflow,
				diagnostics.prefixSumFailureCount);
			return GpuAnimationPlanStatus::AllocationFailure;
		}
	}

	std::uint32_t GpuAnimationStageElementCount(
		GpuAnimationStage stage,
		const GpuAnimationWorkPacket& packet) noexcept
	{
		switch(stage) {
		case GpuAnimationStage::Sample:
		case GpuAnimationStage::Blend:
		case GpuAnimationStage::Hierarchy:
		case GpuAnimationStage::Procedural:
		case GpuAnimationStage::Ragdoll:
			return packet.jointCount;
		case GpuAnimationStage::Morph:
			return packet.morphCount;
		case GpuAnimationStage::Bounds:
			return 1U;
		case GpuAnimationStage::Palette:
			return packet.paletteEntryCount;
		}
		return 0U;
	}

	std::uint32_t GpuAnimationBarrierAfterStage(GpuAnimationStage stage) noexcept
	{
		switch(stage) {
		case GpuAnimationStage::Sample:
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::CodecToBlend);
		case GpuAnimationStage::Blend:
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::BlendToHierarchy);
		case GpuAnimationStage::Hierarchy:
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::HierarchyToProcedural);
		case GpuAnimationStage::Procedural:
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::ProceduralToRagdoll);
		case GpuAnimationStage::Ragdoll:
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::RagdollToMorph);
		case GpuAnimationStage::Morph:
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::MorphToBounds);
		case GpuAnimationStage::Bounds:
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::BoundsToPalette);
		case GpuAnimationStage::Palette:
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::PaletteToRender);
		}
		return 0U;
	}

	std::uint32_t GpuAnimationBarrierBetweenStages(
		GpuAnimationStage producer,
		GpuAnimationStage consumer) noexcept
	{
		if(producer == GpuAnimationStage::Sample && consumer == GpuAnimationStage::Blend) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::CodecToBlend);
		}
		if(producer == GpuAnimationStage::Blend && consumer == GpuAnimationStage::Hierarchy) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::BlendToHierarchy);
		}
		if(producer == GpuAnimationStage::Hierarchy && consumer == GpuAnimationStage::Procedural) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::HierarchyToProcedural);
		}
		if(producer == GpuAnimationStage::Hierarchy && consumer == GpuAnimationStage::Ragdoll) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::HierarchyToRagdoll);
		}
		if(producer == GpuAnimationStage::Hierarchy && consumer == GpuAnimationStage::Morph) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::HierarchyToMorph);
		}
		if(producer == GpuAnimationStage::Procedural && consumer == GpuAnimationStage::Ragdoll) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::ProceduralToRagdoll);
		}
		if(producer == GpuAnimationStage::Procedural && consumer == GpuAnimationStage::Morph) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::ProceduralToMorph);
		}
		if(producer == GpuAnimationStage::Ragdoll && consumer == GpuAnimationStage::Morph) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::RagdollToMorph);
		}
		if(producer == GpuAnimationStage::Morph && consumer == GpuAnimationStage::Bounds) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::MorphToBounds);
		}
		if(producer == GpuAnimationStage::Bounds && consumer == GpuAnimationStage::Palette) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::BoundsToPalette);
		}
		if(producer == GpuAnimationStage::Palette) {
			return static_cast<std::uint32_t>(GpuAnimationBarrierFlag::PaletteToRender);
		}
		// Optional stages may be absent from a valid program. The next active
		// stage still consumes writes made by the producer: for example, a simple
		// clip runs Sample -> Hierarchy without a Blend stage. Preserve the
		// producer's ordinary visibility boundary so Vulkan cannot let the
		// consumer observe the frame buffer before those writes are available.
		return GpuAnimationBarrierAfterStage(producer);
	}

	bool GpuAnimationGenerationsMatch(
		const GpuAnimationGenerationRecord& expected,
		const GpuAnimationGenerationRecord& actual) noexcept
	{
		return expected.instanceSlot == actual.instanceSlot
			&& expected.instanceGeneration == actual.instanceGeneration
			&& expected.componentGeneration == actual.componentGeneration
			&& expected.workGeneration == actual.workGeneration
			&& expected.programGeneration == actual.programGeneration
			&& expected.controllerGeneration == actual.controllerGeneration
			&& expected.skeletonGeneration == actual.skeletonGeneration
			&& expected.clipGeneration == actual.clipGeneration
			&& expected.meshGeneration == actual.meshGeneration
			&& expected.resourceGeneration == actual.resourceGeneration
			&& expected.rendererGeneration == actual.rendererGeneration
			&& expected.sceneGeneration == actual.sceneGeneration
			&& expected.frameIdLow == actual.frameIdLow
			&& expected.frameIdHigh == actual.frameIdHigh
			&& expected.flags == actual.flags
			// Reserved is the frame-owned ragdoll-table byte offset when that visual
			// stage is enabled, so it participates in late-result rejection as well.
			&& expected.reserved == actual.reserved;
	}

	GpuAnimationPlanStatus BuildGpuAnimationPlan(
		std::span<const GpuAnimationWorkRequest> requests,
		std::uint32_t planGeneration,
		GpuAnimationArena& scratchArena,
		GpuAnimationArena& paletteArena,
		GpuAnimationPlan& output,
		const GpuAnimationPlanLimits& limits,
		GpuAnimationPlanBuildDiagnostics* buildDiagnostics) noexcept
	{
		if(buildDiagnostics != nullptr) {
			*buildDiagnostics = {};
		}
		auto reject = [buildDiagnostics](
			GpuAnimationPlanStatus status,
			GpuAnimationDiagnosticFlag flag,
			std::uint32_t requestIndex = std::numeric_limits<std::uint32_t>::max(),
			const GpuAnimationWorkRequest* request = nullptr) noexcept {
			RecordPlanFailure(buildDiagnostics, status, flag, requestIndex, request);
			return status;
		};
		if(planGeneration == 0U
			|| scratchArena.Kind() != GpuAnimationArenaKind::Scratch
			|| paletteArena.Kind() != GpuAnimationArenaKind::Palette
			|| !IsPlanLimitConfigurationValid(limits)
			|| requests.size() > limits.maximumPackets) {
			return reject(GpuAnimationPlanStatus::InvalidArgument, GpuAnimationDiagnosticFlag::InvalidRecord);
		}

		try {
			GpuAnimationPlan temporaryPlan;
			temporaryPlan.generation = planGeneration;
			GpuAnimationArena temporaryScratchArena = scratchArena;
			GpuAnimationArena temporaryPaletteArena = paletteArena;
			const GpuAnimationPlanStatus scratchGenerationStatus = temporaryScratchArena.BeginGeneration(planGeneration);
			const GpuAnimationPlanStatus paletteGenerationStatus = temporaryPaletteArena.BeginGeneration(planGeneration);
			if(scratchGenerationStatus != GpuAnimationPlanStatus::Ok
				|| paletteGenerationStatus != GpuAnimationPlanStatus::Ok) {
				const GpuAnimationPlanStatus status = scratchGenerationStatus != GpuAnimationPlanStatus::Ok
					? scratchGenerationStatus
					: paletteGenerationStatus;
				return reject(status, GpuAnimationDiagnosticFlag::ScratchOverflow);
			}

			std::vector<std::size_t> sortedRequestIndices(requests.size());
			std::iota(sortedRequestIndices.begin(), sortedRequestIndices.end(), 0U);
			for(std::size_t requestIndex = 0U; requestIndex < requests.size(); ++requestIndex) {
				const GpuAnimationWorkRequest& request = requests[requestIndex];
				if(!IsGpuAnimationGenerationRecordValid(request.generations)) {
					return reject(GpuAnimationPlanStatus::StaleGeneration,
						GpuAnimationDiagnosticFlag::StaleGeneration,
						static_cast<std::uint32_t>(requestIndex), &request);
				}
				if(!IsRequestValid(request, limits)) {
					return reject(GpuAnimationPlanStatus::InvalidArgument,
						GpuAnimationDiagnosticFlag::InvalidRecord,
						static_cast<std::uint32_t>(requestIndex), &request);
				}
				if(request.palettePartitionCount > limits.maximumPalettePartitions) {
					return reject(GpuAnimationPlanStatus::PalettePartitionLimitExceeded,
						GpuAnimationDiagnosticFlag::PaletteOverflow,
						static_cast<std::uint32_t>(requestIndex), &request);
				}
			}
			std::sort(
				sortedRequestIndices.begin(),
				sortedRequestIndices.end(),
				[requests](std::size_t left, std::size_t right) noexcept {
					return RequestLess(requests[left], requests[right]);
				});

			// Batch sorting groups compatibility first, so two equal stable pairs can
			// be far apart. Sort a separate identity view to reject duplicates without
			// accidentally depending on adjacency in the dispatch ordering.
			std::vector<std::size_t> stableOrderIndices = sortedRequestIndices;
			std::sort(
				stableOrderIndices.begin(),
				stableOrderIndices.end(),
				[requests](std::size_t left, std::size_t right) noexcept {
					return std::tie(
						requests[left].stableInstanceId,
						requests[left].stableSubmissionOrdinal)
						< std::tie(
							requests[right].stableInstanceId,
							requests[right].stableSubmissionOrdinal);
				});
			for(std::size_t stableIndex = 1U; stableIndex < stableOrderIndices.size(); ++stableIndex) {
				const std::size_t previousIndex = stableOrderIndices[stableIndex - 1U];
				const std::size_t currentIndex = stableOrderIndices[stableIndex];
				const GpuAnimationWorkRequest& previous = requests[previousIndex];
				const GpuAnimationWorkRequest& current = requests[currentIndex];
				if(previous.stableInstanceId == current.stableInstanceId
					&& previous.stableSubmissionOrdinal == current.stableSubmissionOrdinal) {
					return reject(GpuAnimationPlanStatus::DuplicateStableOrder,
						GpuAnimationDiagnosticFlag::InvalidRecord,
						static_cast<std::uint32_t>(currentIndex), &current);
				}
			}

			temporaryPlan.generationRecords.reserve(requests.size());
			temporaryPlan.workPackets.reserve(requests.size());
			temporaryPlan.renderIdentities.reserve(requests.size());
			temporaryPlan.batches.reserve(std::min<std::size_t>(requests.size(), limits.maximumBatches));

			for(std::size_t sortedIndex = 0U; sortedIndex < sortedRequestIndices.size(); ++sortedIndex) {
				const GpuAnimationWorkRequest& request = requests[sortedRequestIndices[sortedIndex]];
				ScratchLayout scratchLayout{};
				if(!BuildScratchLayout(request, scratchLayout)) {
					return reject(GpuAnimationPlanStatus::ArithmeticOverflow,
						GpuAnimationDiagnosticFlag::RangeOverflow,
						static_cast<std::uint32_t>(sortedRequestIndices[sortedIndex]), &request);
				}

				const std::uint32_t scratchAlignment = std::max({
					kGpuAnimationArenaAlignment,
					limits.scratchAlignmentBytes,
					request.scratchAlignmentBytes});
				GpuAnimationArenaAllocation scratchAllocation{};
				const GpuAnimationPlanStatus scratchStatus = temporaryScratchArena.Allocate(
					scratchLayout.totalSizeBytes,
					scratchAlignment,
					scratchAllocation);
				if(scratchStatus != GpuAnimationPlanStatus::Ok) {
					return reject(scratchStatus, GpuAnimationDiagnosticFlag::ScratchOverflow,
						static_cast<std::uint32_t>(sortedRequestIndices[sortedIndex]), &request);
				}

				GpuAnimationArenaAllocation paletteAllocation{};
				if(request.paletteEntryCount != 0U) {
					std::uint64_t paletteSizeBytes = 0U;
					if(!CheckedGpuMultiply(
						request.paletteEntryCount,
						kGpuAnimationPaletteStride,
						paletteSizeBytes)) {
						return reject(GpuAnimationPlanStatus::ArithmeticOverflow,
							GpuAnimationDiagnosticFlag::PaletteOverflow,
							static_cast<std::uint32_t>(sortedRequestIndices[sortedIndex]), &request);
					}
					const GpuAnimationPlanStatus paletteStatus = temporaryPaletteArena.Allocate(
						paletteSizeBytes,
						kGpuAnimationArenaAlignment,
						paletteAllocation);
					if(paletteStatus != GpuAnimationPlanStatus::Ok) {
						return reject(paletteStatus, GpuAnimationDiagnosticFlag::PaletteOverflow,
							static_cast<std::uint32_t>(sortedRequestIndices[sortedIndex]), &request);
					}
				}

				const std::uint32_t workPacketIndex = static_cast<std::uint32_t>(
					temporaryPlan.workPackets.size());
				GpuAnimationWorkPacket packet{};
				packet.stableInstanceId = request.stableInstanceId;
				packet.stableProgramId = request.batchKey.programContentId;
				packet.inputPoseOffsetBytes = request.inputPoseOffsetBytes;
				packet.scratchOffsetBytes = scratchAllocation.offsetBytes;
				packet.localPoseOffsetBytes = scratchAllocation.offsetBytes
					+ scratchLayout.localPoseOffsetBytes;
				packet.modelPoseOffsetBytes = scratchAllocation.offsetBytes
					+ scratchLayout.modelPoseOffsetBytes;
				packet.paletteOffsetBytes = paletteAllocation.offsetBytes;
				packet.morphOutputOffsetBytes = scratchAllocation.offsetBytes
					+ scratchLayout.morphOutputOffsetBytes;
				packet.generationRecordIndex = workPacketIndex;
				packet.clipResourceIndex = request.clipResourceIndex;
				packet.programResourceIndex = request.programResourceIndex;
				packet.skeletonResourceIndex = request.skeletonResourceIndex;
				packet.maskResourceIndex = request.maskResourceIndex;
				packet.morphResourceIndex = request.morphResourceIndex;
				packet.jointCount = request.jointCount;
				packet.morphCount = request.morphCount;
				packet.paletteEntryCount = request.paletteEntryCount;
				packet.palettePartitionCount = request.palettePartitionCount;
				packet.scratchSizeBytes = static_cast<std::uint32_t>(scratchLayout.totalSizeBytes);
				packet.stageMask = request.batchKey.stageMask;
				packet.flags = request.flags;
				packet.diagnosticIndex = request.diagnosticIndex;
				packet.resourceLayoutId = request.batchKey.resourceLayoutId;
				packet.boundsOutputIndex = workPacketIndex;
				packet.stableSubmissionOrdinal = request.stableSubmissionOrdinal;
				packet.sampleTick = request.sampleTick;
				temporaryPlan.generationRecords.push_back(request.generations);
				temporaryPlan.workPackets.push_back(packet);
				temporaryPlan.renderIdentities.push_back(request.renderIdentity);

				if(HasGpuAnimationFlag(packet.flags, GpuAnimationWorkFlag::UseBindPoseFallback)
					|| HasGpuAnimationFlag(packet.flags, GpuAnimationWorkFlag::UseHoldLastFallback)) {
					SaturatingIncrement(temporaryPlan.diagnostics.fallbackRequestCount);
					temporaryPlan.diagnostics.flags |= HasGpuAnimationFlag(
						packet.flags,
						GpuAnimationWorkFlag::UseBindPoseFallback)
						? static_cast<std::uint32_t>(GpuAnimationDiagnosticFlag::BindPoseFallback)
						: static_cast<std::uint32_t>(GpuAnimationDiagnosticFlag::HoldLastFallback);
				}

				const bool beginsBatch = sortedIndex == 0U
					|| !SameBatchCompatibility(
						requests[sortedRequestIndices[sortedIndex - 1U]],
						request);
				if(beginsBatch) {
					if(temporaryPlan.batches.size() >= limits.maximumBatches) {
						return reject(GpuAnimationPlanStatus::PlanLimitExceeded,
							GpuAnimationDiagnosticFlag::OutputOverflow,
							static_cast<std::uint32_t>(sortedRequestIndices[sortedIndex]), &request);
					}
					GpuAnimationBatch batch{};
					batch.key = request.batchKey;
					batch.generationKey = MakeGpuAnimationBatchGenerationKey(request.generations);
					batch.firstWorkPacket = workPacketIndex;
					batch.workPacketCount = 1U;
					batch.stableBatchOrdinal = static_cast<std::uint32_t>(temporaryPlan.batches.size());
					temporaryPlan.batches.push_back(batch);
				} else {
					++temporaryPlan.batches.back().workPacketCount;
				}
			}

			std::uint64_t maximumElementsPerDispatch = 0U;
			if(!CheckedGpuMultiply(
				limits.maximumGroupsX,
				limits.threadsPerGroup,
				maximumElementsPerDispatch)
				|| maximumElementsPerDispatch == 0U
				|| maximumElementsPerDispatch > std::numeric_limits<std::uint32_t>::max()) {
				return reject(GpuAnimationPlanStatus::InvalidArgument, GpuAnimationDiagnosticFlag::RangeOverflow);
			}

			std::vector<std::uint64_t> counts;
			std::vector<std::uint64_t> offsets;
			counts.resize(temporaryPlan.workPackets.size());
			offsets.reserve(temporaryPlan.workPackets.size());
			for(std::uint32_t stageValue = 0U; stageValue < kGpuAnimationStageCount; ++stageValue) {
				const GpuAnimationStage stage = static_cast<GpuAnimationStage>(stageValue);
				for(std::size_t packetIndex = 0U; packetIndex < temporaryPlan.workPackets.size(); ++packetIndex) {
					const GpuAnimationWorkPacket& packet = temporaryPlan.workPackets[packetIndex];
					counts[packetIndex] = (packet.stageMask & GpuAnimationStageMask(stage)) == 0U
						? 0U
						: GpuAnimationStageElementCount(stage, packet);
				}

				std::uint64_t stageTotal = 0U;
				const GpuAnimationPlanStatus prefixStatus = ComputeGpuAnimationPrefixOffsets(
					counts,
					limits.maximumElementsPerStage,
					offsets,
					stageTotal,
					temporaryPlan.diagnostics);
				if(prefixStatus != GpuAnimationPlanStatus::Ok) {
					return reject(prefixStatus, GpuAnimationDiagnosticFlag::RangeOverflow);
				}
				if(stageTotal == 0U) {
					continue;
				}

				GpuAnimationStageSpan span{};
				span.totalElementCount = stageTotal;
				span.stage = stageValue;
				span.firstCommand = static_cast<std::uint32_t>(temporaryPlan.dispatchCommands.size());
				span.firstDispatchRecord = static_cast<std::uint32_t>(temporaryPlan.dispatchRecords.size());
				span.barrierFlags = 0U;

				for(std::uint32_t batchIndex = 0U;
					batchIndex < temporaryPlan.batches.size();
					++batchIndex) {
					const GpuAnimationBatch& batch = temporaryPlan.batches[batchIndex];
					const std::uint32_t packetEnd = batch.firstWorkPacket + batch.workPacketCount;
					// One command can cover several records when their shader policy and
					// batch match. This removes per-character submission while retaining
					// a bounded command whenever the group limit would be exceeded.
					bool hasPendingCommand = false;
					std::uint32_t pendingFirstDispatch = 0U;
					std::uint32_t pendingDispatchCount = 0U;
					std::uint32_t pendingGroupCount = 0U;
					std::uint32_t pendingElementCount = 0U;
					std::uint32_t pendingFlags = 0U;
					std::uint64_t pendingFirstElement = 0U;
					auto flushPendingCommand = [&]() noexcept -> bool {
						if(!hasPendingCommand) {
							return true;
						}
						if(temporaryPlan.dispatchCommands.size() >= limits.maximumDispatchCommands) {
							return false;
						}
						GpuAnimationDispatchCommand command{};
						command.pushConstants.stage = stageValue;
						command.pushConstants.firstDispatchRecord = pendingFirstDispatch;
						command.pushConstants.dispatchRecordCount = pendingDispatchCount;
						command.pushConstants.firstElementLow = static_cast<std::uint32_t>(pendingFirstElement);
						command.pushConstants.firstElementHigh = static_cast<std::uint32_t>(pendingFirstElement >> 32U);
						command.pushConstants.elementCount = pendingElementCount;
						command.pushConstants.planGeneration = planGeneration;
						command.pushConstants.flags = pendingFlags;
						command.groupCountX = pendingGroupCount;
						command.batchIndex = batchIndex;
						temporaryPlan.dispatchCommands.push_back(command);
						hasPendingCommand = false;
						return true;
					};
					for(std::uint32_t packetIndex = batch.firstWorkPacket;
						packetIndex < packetEnd;
						++packetIndex) {
						const GpuAnimationWorkPacket& packet = temporaryPlan.workPackets[packetIndex];
						std::uint64_t remaining = counts[packetIndex];
						std::uint64_t consumed = 0U;
						while(remaining != 0U) {
							if(temporaryPlan.dispatchRecords.size() >= limits.maximumDispatchRecords) {
								return reject(GpuAnimationPlanStatus::PlanLimitExceeded,
									GpuAnimationDiagnosticFlag::OutputOverflow);
							}

							const std::uint32_t elementCount = static_cast<std::uint32_t>(
								std::min(remaining, maximumElementsPerDispatch));
							std::uint64_t elementOffset = 0U;
							if(!CheckedGpuAdd(offsets[packetIndex], consumed, elementOffset)) {
								return reject(GpuAnimationPlanStatus::ArithmeticOverflow,
									GpuAnimationDiagnosticFlag::RangeOverflow);
							}

							GpuAnimationDispatchRecord dispatch{};
							dispatch.stableInstanceId = packet.stableInstanceId;
							dispatch.elementOffset = elementOffset;
							dispatch.workPacketIndex = packetIndex;
							dispatch.generationRecordIndex = packet.generationRecordIndex;
							dispatch.stage = stageValue;
							dispatch.elementCount = elementCount;
							dispatch.flags = DispatchFlagsForPacket(packet);
							dispatch.barrierFlags = span.barrierFlags;
							dispatch.diagnosticIndex = packet.diagnosticIndex;
							dispatch.resourceLayoutId = packet.resourceLayoutId;
							dispatch.batchIndex = batchIndex;
							dispatch.stableSubmissionOrdinal = packet.stableSubmissionOrdinal;
							const std::uint32_t dispatchIndex = static_cast<std::uint32_t>(
								temporaryPlan.dispatchRecords.size());
							temporaryPlan.dispatchRecords.push_back(dispatch);

							const std::uint32_t groupCount = static_cast<std::uint32_t>(
								(static_cast<std::uint64_t>(elementCount) + limits.threadsPerGroup - 1U)
								/ limits.threadsPerGroup);
							if(!hasPendingCommand
								|| pendingFlags != dispatch.flags
								|| pendingDispatchCount >= kGpuAnimationMaximumDispatchRecordsPerCommand
								|| pendingGroupCount > limits.maximumGroupsX - groupCount) {
								if(!flushPendingCommand()) {
									return reject(GpuAnimationPlanStatus::PlanLimitExceeded,
										GpuAnimationDiagnosticFlag::OutputOverflow);
								}
								hasPendingCommand = true;
								pendingFirstDispatch = dispatchIndex;
								pendingDispatchCount = 0U;
								pendingGroupCount = 0U;
								pendingElementCount = 0U;
								pendingFlags = dispatch.flags;
								pendingFirstElement = elementOffset;
							}
							++pendingDispatchCount;
							pendingGroupCount += groupCount;
							pendingElementCount += elementCount;

							remaining -= elementCount;
							consumed += elementCount;
						}
					}
					if(!flushPendingCommand()) {
						return reject(GpuAnimationPlanStatus::PlanLimitExceeded,
							GpuAnimationDiagnosticFlag::OutputOverflow);
					}
				}

				span.commandCount = static_cast<std::uint32_t>(temporaryPlan.dispatchCommands.size())
					- span.firstCommand;
				span.dispatchRecordCount = static_cast<std::uint32_t>(temporaryPlan.dispatchRecords.size())
					- span.firstDispatchRecord;
				temporaryPlan.stageSpans.push_back(span);
			}

			// Resolve barriers after finding every non-empty stage. This keeps skipped
			// optional stages explicit (for example Hierarchy -> Ragdoll) instead of
			// relabelling their writes as one generic hierarchy-to-morph boundary.
			for(std::size_t spanIndex = 0U; spanIndex < temporaryPlan.stageSpans.size(); ++spanIndex) {
				GpuAnimationStageSpan& span = temporaryPlan.stageSpans[spanIndex];
				const GpuAnimationStage producer = static_cast<GpuAnimationStage>(span.stage);
				span.barrierFlags = spanIndex + 1U < temporaryPlan.stageSpans.size()
					? GpuAnimationBarrierBetweenStages(
						producer,
						static_cast<GpuAnimationStage>(temporaryPlan.stageSpans[spanIndex + 1U].stage))
					: GpuAnimationBarrierAfterStage(producer);
				for(std::uint32_t recordIndex = span.firstDispatchRecord;
					recordIndex < span.firstDispatchRecord + span.dispatchRecordCount;
					++recordIndex) {
					temporaryPlan.dispatchRecords[recordIndex].barrierFlags = span.barrierFlags;
				}
			}

			// Publish all related state together only after the complete plan passes.
			output = std::move(temporaryPlan);
			scratchArena = temporaryScratchArena;
			paletteArena = temporaryPaletteArena;
			return GpuAnimationPlanStatus::Ok;
		} catch(const std::bad_alloc&) {
			return reject(GpuAnimationPlanStatus::AllocationFailure, GpuAnimationDiagnosticFlag::OutputOverflow);
		} catch(...) {
			return reject(GpuAnimationPlanStatus::AllocationFailure, GpuAnimationDiagnosticFlag::OutputOverflow);
		}
	}
}
