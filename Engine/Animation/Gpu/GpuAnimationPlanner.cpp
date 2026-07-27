// GpuAnimationPlanner.cpp - Deterministic immutable GPU animation lowering.

#include "Animation/Gpu/GpuAnimationPlanner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		[[nodiscard]] bool ReadPayloadWord(
			const GpuAnimationResidentPayloads& payloads,
			std::size_t byteOffset,
			std::uint32_t& output) noexcept
		{
			const std::size_t byteCount = payloads.programWords.size() * sizeof(std::uint32_t);
			if(byteOffset > byteCount || sizeof(output) > byteCount - byteOffset) return false;
			std::memcpy(&output,
				reinterpret_cast<const std::uint8_t*>(payloads.programWords.data()) + byteOffset,
				sizeof(output));
			return true;
		}

		[[nodiscard]] bool ReadPayloadFloat(
			const GpuAnimationResidentPayloads& payloads,
			std::size_t byteOffset,
			float& output) noexcept
		{
			std::uint32_t bits = 0U;
			if(!ReadPayloadWord(payloads, byteOffset, bits)) return false;
			std::memcpy(&output, &bits, sizeof(output));
			return std::isfinite(output);
		}

		[[nodiscard]] bool HasValidProceduralPayload(
			const GpuAnimationWorkRequest& request,
			const GpuAnimationResidentPayloads& payloads) noexcept
		{
			if(!HasGpuAnimationFlag(request.flags, GpuAnimationWorkFlag::AllowVisualProcedural)) {
				return true;
			}
			const std::size_t programBytes = payloads.programWords.size() * sizeof(std::uint32_t);
			if(request.programResourceIndex > std::numeric_limits<std::size_t>::max() / 64U) return false;
			const std::size_t directoryOffset = static_cast<std::size_t>(request.programResourceIndex) * 64U;
			std::uint32_t tableOffset = 0U;
			std::uint32_t recordCount = 0U;
			std::uint32_t versionAndStride = 0U;
			if(!ReadPayloadWord(payloads, directoryOffset + 4U, tableOffset)
				|| !ReadPayloadWord(payloads, directoryOffset + 8U, recordCount)
				|| !ReadPayloadWord(payloads, directoryOffset + 12U, versionAndStride)) {
				return false;
			}
			const std::uint32_t version = versionAndStride & 0xffffU;
			const std::uint32_t stride = versionAndStride >> 16U;
			return version == kGpuAnimationProceduralVersion
				&& stride == kGpuAnimationProceduralRecordBytes
				&& recordCount != 0U
				&& recordCount <= kGpuAnimationMaximumProceduralRecordsV1
				&& (tableOffset & 15U) == 0U
				&& tableOffset <= programBytes
				&& static_cast<std::size_t>(recordCount)
					<= (programBytes - tableOffset) / kGpuAnimationProceduralRecordBytes;
		}

		[[nodiscard]] bool HasValidRagdollPayload(
			const GpuAnimationWorkRequest& request,
			const GpuAnimationResidentPayloads& payloads) noexcept
		{
			if(!HasGpuAnimationFlag(request.flags, GpuAnimationWorkFlag::AllowRagdollVisual)) {
				return request.generations.reserved == 0U;
			}
			const std::size_t programBytes = payloads.programWords.size() * sizeof(std::uint32_t);
			const std::size_t headerOffset = request.generations.reserved;
			if(headerOffset == 0U || (headerOffset & 15U) != 0U
				|| headerOffset > programBytes
				|| sizeof(GpuAnimationRagdollTableHeader) > programBytes - headerOffset) {
				return false;
			}
			GpuAnimationRagdollTableHeader header{};
			std::memcpy(&header,
				reinterpret_cast<const std::uint8_t*>(payloads.programWords.data()) + headerOffset,
				sizeof(header));
			const std::uint32_t version = header.versionAndStride & 0xffffU;
			const std::uint32_t stride = header.versionAndStride >> 16U;
			if(header.magic != kGpuAnimationRagdollTableMagic
				|| version != kGpuAnimationRagdollVersion
				|| stride != kGpuAnimationRagdollRecordBytes
				|| header.recordCount == 0U
				|| header.recordCount > kGpuAnimationMaximumRagdollRecordsV1
				|| header.flags != kGpuAnimationRagdollTableEnabled
				|| header.physicsGeneration != request.generations.resourceGeneration
				|| header.skeletonGeneration != request.generations.skeletonGeneration
				|| header.componentGeneration != request.generations.componentGeneration) {
				return false;
			}
			const std::size_t recordsOffset = headerOffset + sizeof(header);
			if(recordsOffset > programBytes
				|| static_cast<std::size_t>(header.recordCount)
					> (programBytes - recordsOffset) / sizeof(GpuAnimationRagdollRecord)) {
				return false;
			}
			std::uint32_t previousJoint = 0U;
			for(std::uint32_t index = 0U; index < header.recordCount; ++index) {
				const std::size_t recordOffset = recordsOffset
					+ static_cast<std::size_t>(index) * sizeof(GpuAnimationRagdollRecord);
				GpuAnimationRagdollRecord record{};
				std::memcpy(&record,
					reinterpret_cast<const std::uint8_t*>(payloads.programWords.data()) + recordOffset,
					sizeof(record));
				if(record.jointIndex >= request.jointCount
					|| (index != 0U && record.jointIndex <= previousJoint)
					|| record.bodyGeneration == 0U
					|| record.physicsGeneration != header.physicsGeneration
					|| record.skeletonGeneration != header.skeletonGeneration
					|| record.componentGeneration != header.componentGeneration
					|| record.flags != 0U
					|| !std::isfinite(record.profileWeight)
					|| !std::isfinite(record.activationWeight)
					|| !std::isfinite(record.bodyWeight)
					|| record.profileWeight < 0.0F || record.profileWeight > 1.0F
					|| record.activationWeight < 0.0F || record.activationWeight > 1.0F
					|| record.bodyWeight < 0.0F || record.bodyWeight > 1.0F) {
					return false;
				}
				for(std::size_t byte = 40U; byte < 88U; byte += sizeof(float)) {
					float value = 0.0F;
					if(!ReadPayloadFloat(payloads, recordOffset + byte, value)
						|| std::abs(value) > 1.0e8F) return false;
				}
				previousJoint = record.jointIndex;
			}
			return true;
		}

		[[nodiscard]] bool IsGpuExecutionMode(AnimationEffectiveExecutionMode mode) noexcept
		{
			return mode == AnimationEffectiveExecutionMode::GpuVisual
				|| mode == AnimationEffectiveExecutionMode::HybridAuthoritative;
		}

		[[nodiscard]] bool AppendAlignedPayloadBytes(
			std::vector<std::uint8_t>& destination,
			const std::uint8_t* source,
			std::size_t sourceSize,
			std::size_t alignment,
			std::uint32_t& baseOffset) noexcept
		{
			if(source == nullptr || sourceSize == 0U || alignment == 0U
				|| (alignment & (alignment - 1U)) != 0U) {
				return false;
			}
			const std::size_t padding =
				(alignment - (destination.size() & (alignment - 1U)))
					& (alignment - 1U);
			if(destination.size() > std::numeric_limits<std::uint32_t>::max()
				|| padding > std::numeric_limits<std::uint32_t>::max()
					- destination.size()
				|| sourceSize > std::numeric_limits<std::uint32_t>::max()
					- destination.size() - padding) {
				return false;
			}
			try {
				destination.insert(destination.end(), padding, 0U);
				baseOffset = static_cast<std::uint32_t>(destination.size());
				destination.insert(destination.end(), source, source + sourceSize);
				return true;
			} catch(...) {
				return false;
			}
		}

		[[nodiscard]] bool ReadByteWord(
			std::span<const std::uint8_t> bytes,
			std::size_t offset,
			std::uint32_t& output) noexcept
		{
			if(offset > bytes.size() || sizeof(output) > bytes.size() - offset) {
				return false;
			}
			std::memcpy(&output, bytes.data() + offset, sizeof(output));
			return true;
		}

		[[nodiscard]] bool WriteByteWord(
			std::vector<std::uint8_t>& bytes,
			std::size_t offset,
			std::uint32_t value) noexcept
		{
			if(offset > bytes.size() || sizeof(value) > bytes.size() - offset) {
				return false;
			}
			std::memcpy(bytes.data() + offset, &value, sizeof(value));
			return true;
		}

		[[nodiscard]] bool HasValidMorphPayload(
			const GpuAnimationWorkRequest& request,
			const GpuAnimationResidentPayloads& payloads) noexcept
		{
			if(request.morphResourceIndex
					== kGpuAnimationInvalidResourceIndex
				|| request.morphResourceIndex
					> std::numeric_limits<std::size_t>::max() / 16U) {
				return false;
			}
			const std::size_t tableOffset =
				static_cast<std::size_t>(
					request.morphResourceIndex) * 16U;
			const std::size_t fixedBytes =
				sizeof(GpuAnimationMorphTableHeader)
					+ sizeof(GpuAnimationMorphBoundsRecord);
			if(tableOffset > payloads.morphInputs.size()
				|| fixedBytes
					> payloads.morphInputs.size() - tableOffset) {
				return false;
			}
			GpuAnimationMorphTableHeader header{};
			GpuAnimationMorphBoundsRecord bounds{};
			std::memcpy(
				&header,
				payloads.morphInputs.data() + tableOffset,
				sizeof(header));
			std::memcpy(
				&bounds,
				payloads.morphInputs.data() + tableOffset
					+ sizeof(header),
				sizeof(bounds));
			const std::uint32_t version =
				header.versionAndStride & 0xffffU;
			const std::uint32_t stride =
				header.versionAndStride >> 16U;
			if(header.magic != kGpuAnimationMorphTableMagic
				|| version != kGpuAnimationMorphTableVersion
				|| stride != kGpuAnimationMorphInputRecordBytes
				|| header.morphCount != request.morphCount
				|| header.morphCount
					> kGpuAnimationMaximumMorphTargetsV1
				|| header.reserved != 0U) {
				return false;
			}
			for(std::size_t lane = 0U; lane < 3U; ++lane) {
				if(!std::isfinite(bounds.minimum[lane])
					|| !std::isfinite(bounds.maximum[lane])
					|| bounds.minimum[lane] > bounds.maximum[lane]) {
					return false;
				}
			}
			if(bounds.minimum[3U] != 0.0F
				|| bounds.maximum[3U] != 0.0F) {
				return false;
			}
			const std::size_t recordsOffset =
				tableOffset + fixedBytes;
			if(recordsOffset > payloads.morphInputs.size()
				|| static_cast<std::size_t>(header.morphCount)
					> (payloads.morphInputs.size() - recordsOffset)
						/ sizeof(GpuAnimationMorphInputRecord)) {
				return false;
			}
			const std::span<const std::uint8_t> clipBytes(
				payloads.packedClipBytes.data(),
				payloads.packedClipBytes.size());
			for(std::uint32_t index = 0U;
				index < header.morphCount; ++index) {
				GpuAnimationMorphInputRecord record{};
				std::memcpy(
					&record,
					payloads.morphInputs.data() + recordsOffset
						+ static_cast<std::size_t>(index)
							* sizeof(record),
					sizeof(record));
				std::uint32_t locatorMagic = 0U;
				if(std::any_of(
						record.reserved.begin(),
						record.reserved.end(),
						[](std::uint32_t value) {
							return value != 0U;
						})
					|| !ReadByteWord(
						clipBytes,
						record.trackLocatorOffsetBytes,
						locatorMagic)
					|| (locatorMagic != kAnimationCodecTrackMagic
						&& locatorMagic
							!= kGpuAnimationTrackDirectoryMagic)) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool IsRelocatableSingleClipRequest(
			const GpuAnimationWorkRequest& request,
			const GpuAnimationResidentPayloads& payloads) noexcept
		{
			constexpr std::uint32_t supportedStages =
				GpuAnimationStageMask(GpuAnimationStage::Sample)
				| GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Morph)
				| GpuAnimationStageMask(GpuAnimationStage::Bounds)
				| GpuAnimationStageMask(GpuAnimationStage::Palette);
			return request.jointCount != 0U
				&& request.paletteEntryCount != 0U
				&& request.palettePartitionCount == 1U
				&& request.maskResourceIndex == kGpuAnimationInvalidResourceIndex
				&& request.generations.reserved == 0U
				&& (request.batchKey.stageMask & ~supportedStages) == 0U
				&& payloads.maskTables.empty()
				&& HasValidMorphPayload(request, payloads);
		}

		[[nodiscard]] bool RelocateTrackLocator(
			std::span<const std::uint8_t> clipBytes,
			std::vector<std::uint8_t>& destinationClipBytes,
			std::uint32_t clipBase,
			std::uint32_t offset,
			std::vector<std::uint32_t>& relocatedDirectories) noexcept
		{
			if(offset > clipBytes.size()
				|| sizeof(std::uint32_t) > clipBytes.size() - offset) {
				return false;
			}
			std::uint32_t magic = 0U;
			if(!ReadByteWord(clipBytes, offset, magic)) return false;
			if(magic == kAnimationCodecTrackMagic) {
				if(sizeof(AnimationCodecTrackHeaderV1)
					> clipBytes.size() - offset) return false;
				AnimationCodecTrackHeaderV1 header{};
				std::memcpy(
					&header, clipBytes.data() + offset, sizeof(header));
				return header.version == kAnimationCodecVersion
					&& header.byteSize >= sizeof(header)
					&& header.byteSize <= clipBytes.size() - offset;
			}
			if(magic != kGpuAnimationTrackDirectoryMagic
				|| sizeof(GpuAnimationTrackDirectoryHeader)
					> clipBytes.size() - offset) {
				return false;
			}
			GpuAnimationTrackDirectoryHeader header{};
			std::memcpy(
				&header, clipBytes.data() + offset, sizeof(header));
			const std::uint32_t version = header.versionAndStride & 0xffffU;
			const std::uint32_t stride = header.versionAndStride >> 16U;
			if(version != kGpuAnimationTrackDirectoryVersion
				|| stride != kGpuAnimationTrackDirectoryEntryBytes
				|| header.entryCount == 0U
				|| header.entryCount > kGpuAnimationMaximumTrackSegmentsV1
				|| header.reserved != 0U
				|| static_cast<std::size_t>(header.entryCount)
					> (clipBytes.size() - offset - sizeof(header))
						/ sizeof(GpuAnimationTrackDirectoryEntry)) {
				return false;
			}
			const bool alreadyRelocated = std::find(
				relocatedDirectories.begin(),
				relocatedDirectories.end(),
				offset) != relocatedDirectories.end();
			std::uint32_t previousEnd = 0U;
			for(std::uint32_t index = 0U;
				index < header.entryCount; ++index) {
				const std::size_t entryOffset = static_cast<std::size_t>(offset)
					+ sizeof(header)
					+ static_cast<std::size_t>(index)
						* sizeof(GpuAnimationTrackDirectoryEntry);
				GpuAnimationTrackDirectoryEntry entry{};
				std::memcpy(
					&entry, clipBytes.data() + entryOffset, sizeof(entry));
				if(entry.endTick < entry.startTick
					|| (index != 0U && entry.startTick < previousEnd)
					|| entry.reserved != 0U
					|| entry.trackHeaderOffsetBytes > clipBytes.size()
					|| sizeof(AnimationCodecTrackHeaderV1)
						> clipBytes.size()
							- entry.trackHeaderOffsetBytes
					|| entry.trackHeaderOffsetBytes
						> std::numeric_limits<std::uint32_t>::max()
							- clipBase) {
					return false;
				}
				AnimationCodecTrackHeaderV1 track{};
				std::memcpy(
					&track,
					clipBytes.data() + entry.trackHeaderOffsetBytes,
					sizeof(track));
				if(track.magic != kAnimationCodecTrackMagic
					|| track.version != kAnimationCodecVersion
					|| track.byteSize < sizeof(track)
					|| track.byteSize > clipBytes.size()
						- entry.trackHeaderOffsetBytes) {
					return false;
				}
				if(!alreadyRelocated
					&& !WriteByteWord(
						destinationClipBytes,
						static_cast<std::size_t>(clipBase)
							+ entryOffset
							+ offsetof(
								GpuAnimationTrackDirectoryEntry,
								trackHeaderOffsetBytes),
						entry.trackHeaderOffsetBytes + clipBase)) {
					return false;
				}
				previousEnd = entry.endTick;
			}
			if(!alreadyRelocated) relocatedDirectories.push_back(offset);
			return true;
		}

		// Sampling and hierarchy cannot recover from absent resident bytes. Optional
		// mask and morph tables use the invalid-resource sentinel only when their
		// corresponding packet input/stage is absent.
		[[nodiscard]] bool HasCompleteResidentPayloads(
			const GpuAnimationWorkRequest& request,
			const GpuAnimationResidentPayloads& payloads) noexcept
		{
			return payloads.HasRequiredSampleAndHierarchyInputs()
				&& (request.maskResourceIndex == kGpuAnimationInvalidResourceIndex
					|| !payloads.maskTables.empty())
				&& HasValidMorphPayload(request, payloads)
				&& HasValidProceduralPayload(request, payloads)
				&& HasValidRagdollPayload(request, payloads);
		}

		[[nodiscard]] bool IsProgramUsable(const EvaluationProgram& program) noexcept
		{
			// The CPU dispatcher is the current shared immutable-program validator.
			// It writes only a local count and does not evaluate a pose or touch scene data.
			EvaluationDispatchState validation{};
			return DispatchEvaluationProgramCpu(program, validation) == AnimationStatus::Ok;
		}

		[[nodiscard]] std::uint32_t DeriveStageMask(
			const EvaluationProgram& program,
			const GpuAnimationVisualInput& visual) noexcept
		{
			std::uint32_t mask = GpuAnimationStageMask(GpuAnimationStage::Sample)
				| GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Bounds)
				| GpuAnimationStageMask(GpuAnimationStage::Palette);
			// A compiled controller always has a root layer. One unmasked,
			// non-additive, full-weight root is only a graph ownership record: the
			// Sample stage already produced its exact result, so dispatching Blend
			// would read an absent layer table and duplicate work.
			bool hasBlend = program.layers.size() > 1U;
			if(program.layers.size() == 1U) {
				const EvaluationLayer& layer = program.layers.front();
				hasBlend = layer.additive || layer.maskAssetId.IsValid()
					|| layer.maskGeneration != 0U
					|| layer.maskFingerprint != 0U
					|| !std::isfinite(layer.defaultWeight)
					|| std::abs(layer.defaultWeight - 1.0F) > 1.0e-6F;
			}
			bool hasProcedural = false;
			for(const EvaluationInstruction& instruction : program.instructions) {
				hasBlend = hasBlend || instruction.opcode == EvaluationOpcode::BlendTree1D
					|| instruction.opcode == EvaluationOpcode::BlendTree2D;
				hasProcedural = hasProcedural || instruction.opcode == EvaluationOpcode::ProceduralPose;
			}
			if(hasBlend) mask |= GpuAnimationStageMask(GpuAnimationStage::Blend);
			if(hasProcedural && visual.allowVisualProcedural) {
				mask |= GpuAnimationStageMask(GpuAnimationStage::Procedural);
			}
			if(visual.allowVisualRagdoll) mask |= GpuAnimationStageMask(GpuAnimationStage::Ragdoll);
			if(visual.morphCount != 0U) mask |= GpuAnimationStageMask(GpuAnimationStage::Morph);
			return mask;
		}
	}

	bool ValidateGpuAnimationResidentPayloads(
		const GpuAnimationWorkRequest& request,
		const GpuAnimationResidentPayloads& payloads) noexcept
	{
		// Keep AnimationSystem's GPU-only preflight and presentation's later
		// immutable-plan validation on one implementation. A shallow byte-presence
		// check could otherwise publish no CPU pose and then silently drop the GPU
		// request when a checked table header or track locator is malformed.
		return HasCompleteResidentPayloads(request, payloads);
	}

	GpuAnimationLoweringStatus LowerGpuAnimationWorkRequest(
		const GpuAnimationLoweringInput& input,
		GpuAnimationWorkRequest& output) noexcept
	{
		if(!IsGpuExecutionMode(input.execution.effectiveMode)) {
			// CPU fallback may still produce authoritative output, but it must never
			// create visual GPU work or force a readback to reconstruct a pose.
			return GpuAnimationLoweringStatus::CpuFallbackNoGpuWork;
		}
		if(!input.program.program || input.program.programContentId == 0U
			|| input.program.programResourceIndex == kGpuAnimationInvalidResourceIndex
			|| input.visual.stableInstanceId == 0U || input.visual.skeletonTopologyId == 0U
			|| input.visual.compatibilityFingerprint == 0U
			|| !IsGpuAnimationRenderIdentityValid(input.visual.renderIdentity)) {
			return GpuAnimationLoweringStatus::InvalidArgument;
		}
		const EvaluationProgram& program = *input.program.program;
		if(!IsProgramUsable(program)) return GpuAnimationLoweringStatus::InvalidArgument;
		// The compiled stable hash is the immutable program-byte identity used by
		// batching. Accepting a caller-selected value could group a recompiled
		// program with stale descriptor contents under the same compatibility key.
		if(input.program.programContentId != program.stableHash) {
			return GpuAnimationLoweringStatus::StaleGeneration;
		}
		if(program.compatibilityFingerprint != input.visual.compatibilityFingerprint) {
			return GpuAnimationLoweringStatus::IncompatibleProgram;
		}
		if(!IsGpuAnimationGenerationRecordValid(input.visual.generations)) {
			return GpuAnimationLoweringStatus::StaleGeneration;
		}
		if(input.visual.useBindPoseFallback && input.visual.useHoldLastFallback) {
			return GpuAnimationLoweringStatus::InvalidArgument;
		}

		GpuAnimationWorkRequest staged{};
		staged.batchKey.programContentId = input.program.programContentId;
		staged.batchKey.skeletonTopologyId = input.visual.skeletonTopologyId;
		staged.batchKey.stageMask = DeriveStageMask(program, input.visual);
		staged.batchKey.outputFlags = static_cast<std::uint32_t>(GpuAnimationOutputFlag::Pose)
			| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Bounds)
			| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Palette);
		if(input.visual.morphCount != 0U) staged.batchKey.outputFlags |= static_cast<std::uint32_t>(GpuAnimationOutputFlag::MorphWeights);
		staged.generations = input.visual.generations;
		// The final reserved generation lane is a bounded byte offset only for the
		// ragdoll stage. Clear caller data in every other mode so it cannot become a
		// hidden identity or accidental address-like shader input.
		staged.generations.reserved = input.visual.allowVisualRagdoll
			? input.visual.ragdollTableOffsetBytes : 0U;
		staged.stableInstanceId = input.visual.stableInstanceId;
		staged.inputPoseOffsetBytes = input.visual.inputPoseOffsetBytes;
		staged.stableSubmissionOrdinal = input.visual.stableSubmissionOrdinal;
		staged.clipResourceIndex = input.visual.clipResourceIndex;
		staged.programResourceIndex = input.program.programResourceIndex;
		staged.skeletonResourceIndex = input.visual.skeletonResourceIndex;
		staged.maskResourceIndex = input.visual.maskResourceIndex;
		staged.morphResourceIndex = input.visual.morphResourceIndex;
		staged.jointCount = input.visual.jointCount;
		staged.morphCount = input.visual.morphCount;
		staged.scratchSizeBytes = program.scratchPlan.byteCount;
		staged.scratchAlignmentBytes = program.scratchPlan.alignmentBytes;
		staged.paletteEntryCount = input.visual.paletteEntryCount;
		staged.palettePartitionCount = input.visual.palettePartitionCount;
		staged.diagnosticIndex = input.visual.diagnosticIndex;
		staged.sampleTick = input.visual.sampleTick;
		staged.renderIdentity = input.visual.renderIdentity;
		if(input.visual.looping) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::Looping);
		if(input.visual.reverse) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::Reverse);
		if(input.visual.useBindPoseFallback) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::UseBindPoseFallback);
		if(input.visual.useHoldLastFallback) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::UseHoldLastFallback);
		if(input.visual.hasRootMotion) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::HasRootMotion);
		if(input.visual.morphCount != 0U) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::HasMorph);
		if(input.execution.effectiveMode == AnimationEffectiveExecutionMode::HybridAuthoritative) {
			staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::HybridCpuAuthority);
		}
		if((staged.batchKey.stageMask & GpuAnimationStageMask(GpuAnimationStage::Procedural)) != 0U) {
			staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::AllowVisualProcedural);
		}
		if(input.visual.allowVisualRagdoll) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::AllowRagdollVisual);
		output = staged;
		return GpuAnimationLoweringStatus::Ok;
	}

	GpuAnimationLoweringStatus BuildGpuAnimationFrameAtlas(
		std::span<const GpuAnimationFrameAtlasEntry> entries,
		GpuAnimationFrameAtlas& output) noexcept
	{
		if(entries.empty()) {
			GpuAnimationFrameAtlas empty;
			output = std::move(empty);
			return GpuAnimationLoweringStatus::Ok;
		}
		try {
			GpuAnimationFrameAtlas staged;
			std::vector<std::uint8_t> programBytes;
			staged.requests.reserve(entries.size());
			struct AtlasPayloadMapping {
				std::shared_ptr<const GpuAnimationResidentPayloads> payload;
				std::uint32_t sourceProgramResourceIndex = 0U;
				std::uint32_t sourceSkeletonResourceIndex = 0U;
				std::uint32_t sourceMorphResourceIndex = 0U;
				std::uint32_t jointCount = 0U;
				std::uint32_t morphCount = 0U;
				std::uint32_t paletteEntryCount = 0U;
				std::uint32_t clipBase = 0U;
				std::uint32_t programBase = 0U;
				std::uint32_t skeletonBase = 0U;
				std::uint32_t morphBase = 0U;
			};
			std::vector<AtlasPayloadMapping> payloadMappings;

			for(const GpuAnimationFrameAtlasEntry& entry : entries) {
				if(!entry.residentPayloads
					|| !entry.request.renderIdentity.IsComplete()
					|| !ValidateGpuAnimationResidentPayloads(
						entry.request, *entry.residentPayloads)
					|| !IsRelocatableSingleClipRequest(
						entry.request, *entry.residentPayloads)) {
					return GpuAnimationLoweringStatus::InvalidArgument;
				}
				const GpuAnimationResidentPayloads& source =
					*entry.residentPayloads;
				const std::size_t sourceProgramByteCount =
					source.programWords.size() * sizeof(std::uint32_t);
				const auto* sourceProgramBytes = reinterpret_cast<
					const std::uint8_t*>(source.programWords.data());
				const std::span<const std::uint8_t> sourceClip(
					source.packedClipBytes.data(), source.packedClipBytes.size());
				const std::span<const std::uint8_t> sourceSkeleton(
					source.skeletonTables.data(), source.skeletonTables.size());
				const std::span<const std::uint8_t> sourceMorph(
					source.morphInputs.data(), source.morphInputs.size());

				if(entry.request.programResourceIndex
						> std::numeric_limits<std::size_t>::max() / 64U
					|| entry.request.skeletonResourceIndex
						> std::numeric_limits<std::size_t>::max() / 16U
					|| entry.request.morphResourceIndex
						> std::numeric_limits<std::size_t>::max() / 16U) {
					return GpuAnimationLoweringStatus::InvalidArgument;
				}
				const std::size_t sourceProgramDirectory =
					static_cast<std::size_t>(
						entry.request.programResourceIndex) * 64U;
				const std::size_t sourceSkeletonDirectory =
					static_cast<std::size_t>(
						entry.request.skeletonResourceIndex) * 16U;
				const std::size_t sourceMorphDirectory =
					static_cast<std::size_t>(
						entry.request.morphResourceIndex) * 16U;
				const std::size_t sourceJointRecords =
					sourceProgramDirectory + 64U;
				const std::size_t sourceMorphRecords =
					sourceMorphDirectory
						+ sizeof(GpuAnimationMorphTableHeader)
						+ sizeof(GpuAnimationMorphBoundsRecord);
				if(sourceProgramDirectory > sourceProgramByteCount
					|| 64U > sourceProgramByteCount - sourceProgramDirectory
					|| sourceJointRecords > sourceProgramByteCount
					|| static_cast<std::size_t>(entry.request.jointCount)
						> (sourceProgramByteCount - sourceJointRecords) / 16U
					|| sourceSkeletonDirectory > sourceSkeleton.size()
					|| 16U > sourceSkeleton.size()
						- sourceSkeletonDirectory
					|| sourceMorphRecords > sourceMorph.size()
					|| static_cast<std::size_t>(
							entry.request.morphCount)
						> (sourceMorph.size() - sourceMorphRecords)
							/ sizeof(GpuAnimationMorphInputRecord)) {
					return GpuAnimationLoweringStatus::InvalidArgument;
				}

				std::uint32_t clipBase = 0U;
				std::uint32_t programBase = 0U;
				std::uint32_t skeletonBase = 0U;
				std::uint32_t morphBase = 0U;
				const auto existingMapping = std::find_if(
					payloadMappings.begin(),
					payloadMappings.end(),
					[&](const AtlasPayloadMapping& candidate) {
						// Hash-only deduplication could alias corrupt or colliding
						// content. Exact byte equality and selected resource shape
						// are required before two animators share atlas ranges.
						return candidate.sourceProgramResourceIndex
								== entry.request.programResourceIndex
							&& candidate.sourceSkeletonResourceIndex
								== entry.request.skeletonResourceIndex
							&& candidate.sourceMorphResourceIndex
								== entry.request.morphResourceIndex
							&& candidate.jointCount == entry.request.jointCount
							&& candidate.morphCount
								== entry.request.morphCount
							&& candidate.paletteEntryCount
								== entry.request.paletteEntryCount
							&& candidate.payload->programWords
								== source.programWords
							&& candidate.payload->packedClipBytes
								== source.packedClipBytes
							&& candidate.payload->skeletonTables
								== source.skeletonTables
							&& candidate.payload->morphInputs
								== source.morphInputs;
					});
				if(existingMapping != payloadMappings.end()) {
					clipBase = existingMapping->clipBase;
					programBase = existingMapping->programBase;
					skeletonBase = existingMapping->skeletonBase;
					morphBase = existingMapping->morphBase;
				} else {
				if(!AppendAlignedPayloadBytes(
					staged.residentPayloads.packedClipBytes,
					source.packedClipBytes.data(),
					source.packedClipBytes.size(),
					alignof(std::uint32_t),
					clipBase)) {
					return GpuAnimationLoweringStatus::AllocationFailure;
				}
				if(!AppendAlignedPayloadBytes(
					programBytes,
					sourceProgramBytes,
					sourceProgramByteCount,
					64U,
					programBase)) {
					return GpuAnimationLoweringStatus::AllocationFailure;
				}
				if(!AppendAlignedPayloadBytes(
					staged.residentPayloads.skeletonTables,
					source.skeletonTables.data(),
					source.skeletonTables.size(),
					16U,
					skeletonBase)) {
					return GpuAnimationLoweringStatus::AllocationFailure;
				}
				if(!AppendAlignedPayloadBytes(
					staged.residentPayloads.morphInputs,
					source.morphInputs.data(),
					source.morphInputs.size(),
					16U,
					morphBase)) {
					return GpuAnimationLoweringStatus::AllocationFailure;
				}

				// The single-clip program stores absolute track offsets. Relocate all
				// T/R/S lanes into the frame clip atlas before any packet is published.
				std::vector<std::uint32_t> relocatedTrackDirectories;
				for(std::uint32_t joint = 0U;
					joint < entry.request.jointCount; ++joint) {
					for(std::uint32_t lane = 0U; lane < 3U; ++lane) {
						const std::size_t sourceOffset =
							sourceJointRecords
								+ static_cast<std::size_t>(joint) * 16U
								+ static_cast<std::size_t>(lane) * 4U;
						std::uint32_t trackOffset = 0U;
						if(!ReadByteWord(
								std::span<const std::uint8_t>(
									sourceProgramBytes,
									sourceProgramByteCount),
								sourceOffset,
								trackOffset)
							|| !RelocateTrackLocator(
								sourceClip,
								staged.residentPayloads.packedClipBytes,
								clipBase,
								trackOffset,
								relocatedTrackDirectories)
							|| trackOffset
								> std::numeric_limits<std::uint32_t>::max()
									- clipBase) {
							return GpuAnimationLoweringStatus::InvalidArgument;
						}
						const std::size_t destinationOffset =
							static_cast<std::size_t>(programBase)
								+ sourceOffset;
						if(!WriteByteWord(
							programBytes,
							destinationOffset,
							trackOffset + clipBase)) {
							return GpuAnimationLoweringStatus::InvalidArgument;
						}
					}
				}

				// Morph records use the same ATD1 locator contract as joint lanes.
				// Relocate each selected table record into the clip atlas before
				// the request can expose its new morph resource index.
				for(std::uint32_t morph = 0U;
					morph < entry.request.morphCount; ++morph) {
					const std::size_t sourceOffset =
						sourceMorphRecords
							+ static_cast<std::size_t>(morph)
								* sizeof(GpuAnimationMorphInputRecord);
					std::uint32_t trackOffset = 0U;
					if(!ReadByteWord(
							sourceMorph, sourceOffset, trackOffset)
						|| !RelocateTrackLocator(
							sourceClip,
							staged.residentPayloads.packedClipBytes,
							clipBase,
							trackOffset,
							relocatedTrackDirectories)
						|| trackOffset
							> std::numeric_limits<std::uint32_t>::max()
								- clipBase
						|| !WriteByteWord(
							staged.residentPayloads.morphInputs,
							static_cast<std::size_t>(morphBase)
								+ sourceOffset,
							trackOffset + clipBase)) {
						return GpuAnimationLoweringStatus::InvalidArgument;
					}
				}

				// Skeleton directories store absolute byte offsets for parent,
				// topology-level, and palette tables. Validate the selected resource
				// against this request, then relocate all three lanes atomically.
				std::uint32_t parentTable = 0U;
				std::uint32_t levelTable = 0U;
				std::uint32_t levelCount = 0U;
				std::uint32_t paletteTable = 0U;
				if(!ReadByteWord(
						sourceSkeleton,
						sourceSkeletonDirectory,
						parentTable)
					|| !ReadByteWord(
						sourceSkeleton,
						sourceSkeletonDirectory + 4U,
						levelTable)
					|| !ReadByteWord(
						sourceSkeleton,
						sourceSkeletonDirectory + 8U,
						levelCount)
					|| !ReadByteWord(
						sourceSkeleton,
						sourceSkeletonDirectory + 12U,
						paletteTable)
					|| levelCount == 0U
					|| parentTable > sourceSkeleton.size()
					|| static_cast<std::size_t>(entry.request.jointCount)
						> (sourceSkeleton.size() - parentTable)
							/ sizeof(std::int32_t)
					|| levelTable > sourceSkeleton.size()
					|| static_cast<std::size_t>(entry.request.jointCount)
						> (sourceSkeleton.size() - levelTable)
							/ sizeof(std::uint32_t)
					|| paletteTable > sourceSkeleton.size()
					|| static_cast<std::size_t>(
							entry.request.paletteEntryCount)
						> (sourceSkeleton.size() - paletteTable)
							/ kGpuAnimationSkeletonPaletteRecordBytes
					|| parentTable
						> std::numeric_limits<std::uint32_t>::max()
							- skeletonBase
					|| levelTable
						> std::numeric_limits<std::uint32_t>::max()
							- skeletonBase
					|| paletteTable
						> std::numeric_limits<std::uint32_t>::max()
							- skeletonBase) {
					return GpuAnimationLoweringStatus::InvalidArgument;
				}
				const std::size_t destinationSkeletonDirectory =
					static_cast<std::size_t>(skeletonBase)
						+ sourceSkeletonDirectory;
				if(!WriteByteWord(
						staged.residentPayloads.skeletonTables,
						destinationSkeletonDirectory,
						parentTable + skeletonBase)
					|| !WriteByteWord(
						staged.residentPayloads.skeletonTables,
						destinationSkeletonDirectory + 4U,
						levelTable + skeletonBase)
					|| !WriteByteWord(
						staged.residentPayloads.skeletonTables,
						destinationSkeletonDirectory + 12U,
						paletteTable + skeletonBase)) {
					return GpuAnimationLoweringStatus::InvalidArgument;
				}
				payloadMappings.push_back({
					entry.residentPayloads,
					entry.request.programResourceIndex,
					entry.request.skeletonResourceIndex,
					entry.request.morphResourceIndex,
					entry.request.jointCount,
					entry.request.morphCount,
					entry.request.paletteEntryCount,
					clipBase,
					programBase,
					skeletonBase,
					morphBase});
				}

				GpuAnimationWorkRequest relocated = entry.request;
				const std::uint64_t relocatedProgramDirectory =
					static_cast<std::uint64_t>(programBase)
						+ sourceProgramDirectory;
				const std::uint64_t relocatedSkeletonDirectory =
					static_cast<std::uint64_t>(skeletonBase)
						+ sourceSkeletonDirectory;
				const std::uint64_t relocatedMorphDirectory =
					static_cast<std::uint64_t>(morphBase)
						+ sourceMorphDirectory;
				if((relocatedProgramDirectory & 63U) != 0U
					|| relocatedProgramDirectory / 64U
						>= kGpuAnimationInvalidResourceIndex
					|| (relocatedSkeletonDirectory & 15U) != 0U
					|| relocatedSkeletonDirectory / 16U
						>= kGpuAnimationInvalidResourceIndex
					|| (relocatedMorphDirectory & 15U) != 0U
					|| relocatedMorphDirectory / 16U
						>= kGpuAnimationInvalidResourceIndex) {
					return GpuAnimationLoweringStatus::InvalidArgument;
				}
				relocated.clipResourceIndex = clipBase;
				relocated.programResourceIndex =
					static_cast<std::uint32_t>(
						relocatedProgramDirectory / 64U);
				relocated.skeletonResourceIndex =
					static_cast<std::uint32_t>(
						relocatedSkeletonDirectory / 16U);
				relocated.morphResourceIndex =
					static_cast<std::uint32_t>(
						relocatedMorphDirectory / 16U);
				staged.requests.push_back(std::move(relocated));
			}

			if((programBytes.size() % sizeof(std::uint32_t)) != 0U) {
				return GpuAnimationLoweringStatus::InvalidArgument;
			}
			staged.residentPayloads.programWords.resize(
				programBytes.size() / sizeof(std::uint32_t));
			std::memcpy(
				staged.residentPayloads.programWords.data(),
				programBytes.data(),
				programBytes.size());
			output = std::move(staged);
			return GpuAnimationLoweringStatus::Ok;
		} catch(const std::bad_alloc&) {
			return GpuAnimationLoweringStatus::AllocationFailure;
		} catch(...) {
			return GpuAnimationLoweringStatus::AllocationFailure;
		}
	}

	GpuAnimationLoweringStatus BuildLoweredGpuAnimationPlan(
		std::span<const GpuAnimationLoweringInput> inputs,
		std::uint32_t planGeneration,
		GpuAnimationArena& scratchArena,
		GpuAnimationArena& paletteArena,
		GpuAnimationPlan& output,
		GpuAnimationLoweredPlanResult& result,
		const GpuAnimationPlanLimits& limits) noexcept
	{
		result = {};
		try {
			std::vector<GpuAnimationFrameAtlasEntry> atlasEntries;
			atlasEntries.reserve(inputs.size());
			for(const GpuAnimationLoweringInput& input : inputs) {
				GpuAnimationWorkRequest request{};
				const GpuAnimationLoweringStatus status = LowerGpuAnimationWorkRequest(input, request);
				if(status == GpuAnimationLoweringStatus::CpuFallbackNoGpuWork) continue;
				if(status != GpuAnimationLoweringStatus::Ok) {
					result.status = status;
					return status;
				}
				if(!input.residentPayloads
					|| !ValidateGpuAnimationResidentPayloads(
						request, *input.residentPayloads)) {
					// Incomplete GPU residency is an expected capability failure. The CPU
					// evaluator remains authoritative and no malformed GPU packet is emitted.
					continue;
				}
				atlasEntries.push_back({
					std::move(request),
					input.residentPayloads});
			}
			std::vector<GpuAnimationWorkRequest> stagedRequests;
			GpuAnimationResidentPayloads payloadCopy{};
			if(!atlasEntries.empty()) {
				const std::shared_ptr<const GpuAnimationResidentPayloads> firstPayload =
					atlasEntries.front().residentPayloads;
				const bool oneSharedPayload = std::all_of(
					atlasEntries.begin(),
					atlasEntries.end(),
					[&](const GpuAnimationFrameAtlasEntry& entry) {
						return entry.residentPayloads == firstPayload;
					});
				if(oneSharedPayload) {
					stagedRequests.reserve(atlasEntries.size());
					for(GpuAnimationFrameAtlasEntry& entry : atlasEntries) {
						stagedRequests.push_back(std::move(entry.request));
					}
					payloadCopy = *firstPayload;
				} else {
					GpuAnimationFrameAtlas atlas;
					const GpuAnimationLoweringStatus atlasStatus =
						BuildGpuAnimationFrameAtlas(atlasEntries, atlas);
					if(atlasStatus != GpuAnimationLoweringStatus::Ok) {
						result.status = atlasStatus;
						return atlasStatus;
					}
					stagedRequests = std::move(atlas.requests);
					payloadCopy = std::move(atlas.residentPayloads);
				}
			}
			GpuAnimationPlanBuildDiagnostics diagnostics{};
			const GpuAnimationPlanStatus planStatus = BuildGpuAnimationPlan(
				stagedRequests, planGeneration, scratchArena, paletteArena, output, limits, &diagnostics);
			result.planStatus = planStatus;
			result.diagnostics = diagnostics;
			if(planStatus != GpuAnimationPlanStatus::Ok) {
				result.status = GpuAnimationLoweringStatus::PlanBuildFailed;
				return result.status;
			}
			output.residentPayloads = std::move(payloadCopy);
			result.workRequests = std::move(stagedRequests);
			result.status = GpuAnimationLoweringStatus::Ok;
			return result.status;
		} catch(const std::bad_alloc&) {
			result.status = GpuAnimationLoweringStatus::AllocationFailure;
			return result.status;
		} catch(...) {
			result.status = GpuAnimationLoweringStatus::AllocationFailure;
			return result.status;
		}
	}
}
