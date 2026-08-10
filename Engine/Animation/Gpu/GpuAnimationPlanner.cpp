// GpuAnimationPlanner.cpp - Deterministic immutable GPU animation lowering.

#include "Animation/Gpu/GpuAnimationPlanner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>

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

		void MixTopologyIdentity(std::uint64_t& hash, std::uint64_t value) noexcept
		{
			// FNV-1a over typed scalar lanes is deterministic inside one runtime
			// session and cheap enough to compute while presentation inputs are hot.
			hash ^= value;
			hash *= 1099511628211ULL;
		}

		void MixLoweringTopologyInput(
			std::uint64_t& hash,
			std::uint64_t programContentId,
			std::uint32_t programResourceIndex,
			const GpuAnimationVisualInput& visual,
			std::uint32_t prevalidatedStageMask,
			AnimationEffectiveExecutionMode effectiveMode,
			bool allowSharedVisualEvaluation) noexcept
		{
				const GpuAnimationGenerationRecord& generation = visual.generations;
				// Shared-pointer addresses describe temporary ownership, not executable
				// topology. Stable content IDs, resource indices, compatibility, and
				// generations below are the authoritative invalidation proof.
				MixTopologyIdentity(hash, programContentId);
				MixTopologyIdentity(hash, programResourceIndex);
				MixTopologyIdentity(hash, visual.stableInstanceId);
				MixTopologyIdentity(hash, visual.skeletonTopologyId);
				MixTopologyIdentity(hash, visual.compatibilityFingerprint);
				MixTopologyIdentity(hash, visual.inputPoseOffsetBytes);
				// Raw parameters and sample ticks change every frame. AnimationSystem
				// separately proves that every retained evaluation-sharing group preserves
				// equality; hashing raw values here would invalidate an unchanged partition.
				MixTopologyIdentity(hash, visual.stableSubmissionOrdinal);
				MixTopologyIdentity(hash, visual.clipResourceIndex);
				MixTopologyIdentity(hash, visual.skeletonResourceIndex);
				MixTopologyIdentity(hash, visual.maskResourceIndex);
				MixTopologyIdentity(hash, visual.morphResourceIndex);
				MixTopologyIdentity(hash, visual.jointCount);
				MixTopologyIdentity(hash, visual.morphCount);
				MixTopologyIdentity(hash, visual.paletteEntryCount);
				MixTopologyIdentity(hash, visual.palettePartitionCount);
				MixTopologyIdentity(hash, visual.diagnosticIndex);
				MixTopologyIdentity(hash, generation.instanceSlot);
				MixTopologyIdentity(hash, generation.instanceGeneration);
				// Component publication advances with the authored playback state and is
				// consumed from the dynamic generation record. It does not change packet
				// layout, batching, resource selectors, or stable owner identity.
				MixTopologyIdentity(hash, generation.programGeneration);
				MixTopologyIdentity(hash, generation.controllerGeneration);
				MixTopologyIdentity(hash, generation.skeletonGeneration);
				MixTopologyIdentity(hash, generation.clipGeneration);
				MixTopologyIdentity(hash, generation.meshGeneration);
				// The shader-visible residency/output epoch advances as dynamic buffers
				// rotate. Immutable clip/skeleton selectors and their asset generations
				// already invalidate the retained payload topology when content changes.
				MixTopologyIdentity(hash, generation.rendererGeneration);
				// Scene publication advances independently of packet layout. The current
				// value travels in the dynamic generation lane and is validated again by
				// renderer publication; it is not an immutable animation resource epoch.
				MixTopologyIdentity(hash, generation.flags
					& ~kGpuAnimationGenerationFlagSuspendVisualEvaluation);
				// Render identity contains the current deformation payload generation.
				// It advances with animation output and belongs in the dynamic sidecar,
				// All other identity lanes remain part of the immutable proof.
				MixTopologyIdentity(hash, static_cast<std::uint64_t>(
					std::hash<TerraEngine::UUID>{}(visual.renderIdentity.meshId)));
				MixTopologyIdentity(hash, visual.renderIdentity.meshHeaderVersion);
				MixTopologyIdentity(hash, visual.renderIdentity.rendererGeneration);
				// The render identity's scene publication is the matching dynamic half of
				// the generation record above, not a mesh/topology selector.
				MixTopologyIdentity(hash,
					visual.renderIdentity.skeletonCompatibilityFingerprint);
				// The retained plan owns the validated resident payload. Reconstructed
				// shared_ptr control blocks with identical resource generations must not
				// force packet topology to rebuild.
				MixTopologyIdentity(hash, prevalidatedStageMask);
				MixTopologyIdentity(hash, static_cast<std::uint32_t>(effectiveMode));
				// Validation booleans are transient proof state, not packet identity.
				// Warm adoption checks them directly before accepting the retained root.
				std::uint64_t flags = allowSharedVisualEvaluation ? 1ULL : 0ULL;
				flags |= visual.looping ? 1ULL << 1U : 0ULL;
				flags |= visual.reverse ? 1ULL << 2U : 0ULL;
				flags |= visual.useBindPoseFallback ? 1ULL << 3U : 0ULL;
				flags |= visual.useHoldLastFallback ? 1ULL << 4U : 0ULL;
				flags |= visual.hasRootMotion ? 1ULL << 5U : 0ULL;
				flags |= visual.allowVisualProcedural ? 1ULL << 6U : 0ULL;
				flags |= visual.allowVisualRagdoll ? 1ULL << 7U : 0ULL;
				MixTopologyIdentity(hash, flags);
				MixTopologyIdentity(hash, visual.ragdollTableOffsetBytes);
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

	void GpuAnimationLoweringTopologySignatureBuilder::Include(
		const GpuAnimationLoweringInput& input) noexcept
	{
		IncludePrepared(
			input.program.programContentId,
			input.program.programResourceIndex,
			input.visual,
			input.prevalidatedStageMask,
			input.effectiveMode,
			input.allowSharedVisualEvaluation);
	}

	void GpuAnimationLoweringTopologySignatureBuilder::IncludePrepared(
		std::uint64_t programContentId,
		std::uint32_t programResourceIndex,
		const GpuAnimationVisualInput& visual,
		std::uint32_t prevalidatedStageMask,
		AnimationEffectiveExecutionMode effectiveMode,
		bool allowSharedVisualEvaluation) noexcept
	{
		std::uint64_t inputHash = 1469598103934665603ULL;
		MixLoweringTopologyInput(
			inputHash,
			programContentId,
			programResourceIndex,
			visual,
			prevalidatedStageMask,
			effectiveMode,
			allowSharedVisualEvaluation);
		// Finalize each member before the commutative reduction so permutations of
		// the same proven inputs produce one signature, while duplicate/replaced
		// members perturb all three accumulators.
		inputHash ^= inputHash >> 30U;
		inputHash *= 0xbf58476d1ce4e5b9ULL;
		inputHash ^= inputHash >> 27U;
		inputHash *= 0x94d049bb133111ebULL;
		inputHash ^= inputHash >> 31U;
		xor_ ^= inputHash;
		sum_ += inputHash;
		productSum_ += inputHash * (inputHash | 1ULL);
		++count_;
	}

	std::uint64_t GpuAnimationLoweringTopologySignatureBuilder::Complete() const noexcept
	{
		std::uint64_t completed = 1469598103934665603ULL;
		// Include cardinality and every commutative lane at the boundary so an empty,
		// truncated, reordered, or duplicate source cannot inherit another topology.
		MixTopologyIdentity(completed, count_);
		MixTopologyIdentity(completed, xor_);
		MixTopologyIdentity(completed, sum_);
		MixTopologyIdentity(completed, productSum_);
		return completed == 0U ? 1U : completed;
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
		if(!IsGpuExecutionMode(input.effectiveMode)) {
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
		if(!input.programValidated && !IsProgramUsable(program)) {
			return GpuAnimationLoweringStatus::InvalidArgument;
		}
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
		// AnimationSystem may carry forward the exact result from its mandatory
		// preflight. Only a validated immutable program may authorize this shortcut;
		// ordinary callers retain the fully checked derivation path.
		staged.batchKey.stageMask = input.programValidated
			&& input.prevalidatedStageMask != 0U
			? input.prevalidatedStageMask
			: DeriveStageMask(program, input.visual);
		staged.batchKey.outputFlags = static_cast<std::uint32_t>(GpuAnimationOutputFlag::Pose)
			| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Bounds)
			| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Palette);
		if(input.visual.morphCount != 0U) staged.batchKey.outputFlags |= static_cast<std::uint32_t>(GpuAnimationOutputFlag::MorphWeights);
		staged.generations = input.visual.generations;
		if(input.visual.suspendVisualEvaluation) {
			staged.generations.flags |=
				kGpuAnimationGenerationFlagSuspendVisualEvaluation;
		} else {
			staged.generations.flags &=
				~kGpuAnimationGenerationFlagSuspendVisualEvaluation;
		}
		// The final reserved generation lane is a bounded byte offset only for the
		// ragdoll stage. Clear caller data in every other mode so it cannot become a
		// hidden identity or accidental address-like shader input.
		staged.generations.reserved = input.visual.allowVisualRagdoll
			? input.visual.ragdollTableOffsetBytes : 0U;
		staged.stableInstanceId = input.visual.stableInstanceId;
		staged.inputPoseOffsetBytes = input.visual.inputPoseOffsetBytes;
		staged.evaluationParameterFingerprint = input.visual.evaluationParameterFingerprint;
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
		// diagnosticIndex is bounded below the high bit by plan limits. Retain the
		// opt-in in that private request lane, then strip it before shader packets.
		staged.diagnosticIndex = input.visual.diagnosticIndex
			| (input.allowSharedVisualEvaluation ? 0x80000000U : 0U);
		staged.sampleTick = input.visual.sampleTick;
		staged.renderIdentity = input.visual.renderIdentity;
		if(input.visual.looping) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::Looping);
		if(input.visual.reverse) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::Reverse);
		if(input.visual.useBindPoseFallback) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::UseBindPoseFallback);
		if(input.visual.useHoldLastFallback) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::UseHoldLastFallback);
		if(input.visual.hasRootMotion) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::HasRootMotion);
		if(input.visual.morphCount != 0U) staged.flags |= static_cast<std::uint32_t>(GpuAnimationWorkFlag::HasMorph);
		if(input.effectiveMode == AnimationEffectiveExecutionMode::HybridAuthoritative) {
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
			payloadMappings.reserve(entries.size());
			struct AtlasPayloadIdentity {
				const GpuAnimationResidentPayloads* payload = nullptr;
				std::uint32_t programResourceIndex = 0U;
				std::uint32_t skeletonResourceIndex = 0U;
				std::uint32_t morphResourceIndex = 0U;
				std::uint32_t jointCount = 0U;
				std::uint32_t morphCount = 0U;
				std::uint32_t paletteEntryCount = 0U;

				bool operator==(const AtlasPayloadIdentity&) const noexcept = default;
			};
			struct AtlasPayloadIdentityHash {
				std::size_t operator()(const AtlasPayloadIdentity& value) const noexcept {
					std::size_t hash = std::hash<const void*>{}(value.payload);
					const std::array<std::uint32_t, 6U> lanes{
						value.programResourceIndex, value.skeletonResourceIndex,
						value.morphResourceIndex, value.jointCount,
						value.morphCount, value.paletteEntryCount};
					for(const std::uint32_t lane : lanes) {
						hash ^= std::hash<std::uint32_t>{}(lane) + 0x9e3779b9U
							+ (hash << 6U) + (hash >> 2U);
					}
					return hash;
				}
			};
			std::unordered_map<AtlasPayloadIdentity, std::size_t,
				AtlasPayloadIdentityHash> payloadIdentityMappings;
			payloadIdentityMappings.reserve(entries.size());
			// Pointer identity handles the common crowd case. A content bucket retains
			// exact deduplication for independently loaded immutable copies without a
			// full scan through every distinct asset already appended to the atlas.
			std::unordered_multimap<std::size_t, std::size_t> payloadContentMappings;
			payloadContentMappings.reserve(entries.size());
			struct CachedPayloadContentHash {
				std::weak_ptr<const GpuAnimationResidentPayloads> owner;
				std::size_t hash = 0U;
			};
			// Cooked payload objects are immutable and normally survive for the whole
			// scene. Cache their byte hash on the presentation thread; weak ownership
			// prevents this diagnostic/dedup aid from extending asset lifetime.
			thread_local std::unordered_map<const GpuAnimationResidentPayloads*,
				CachedPayloadContentHash> payloadHashCache;
			const auto buildContentHash = [&](const AtlasPayloadIdentity& identity,
				const std::shared_ptr<const GpuAnimationResidentPayloads>& payloadOwner) {
				const GpuAnimationResidentPayloads& payload = *payloadOwner;
				std::size_t payloadHash = 0U;
				const auto cached = payloadHashCache.find(payloadOwner.get());
				if(cached != payloadHashCache.end()) {
					const std::shared_ptr<const GpuAnimationResidentPayloads> retained =
						cached->second.owner.lock();
					if(retained == payloadOwner) payloadHash = cached->second.hash;
				}
				if(payloadHash == 0U) {
					if(payloadHashCache.size() >= 4096U) {
						std::erase_if(payloadHashCache, [](const auto& entry) {
							return entry.second.owner.expired();
						});
						if(payloadHashCache.size() >= 4096U) payloadHashCache.clear();
					}
					payloadHash = 1469598103934665603ULL;
					const auto mixPayloadBytes = [&payloadHash](
						const void* data, std::size_t byteCount) {
						const auto* bytes = static_cast<const std::uint8_t*>(data);
						for(std::size_t index = 0U; index < byteCount; ++index) {
							payloadHash ^= bytes[index];
							payloadHash *= 1099511628211ULL;
						}
					};
					mixPayloadBytes(payload.programWords.data(),
						payload.programWords.size() * sizeof(std::uint32_t));
					mixPayloadBytes(payload.packedClipBytes.data(),
						payload.packedClipBytes.size());
					mixPayloadBytes(payload.skeletonTables.data(),
						payload.skeletonTables.size());
					mixPayloadBytes(payload.morphInputs.data(), payload.morphInputs.size());
					// FNV can theoretically produce zero; reserve it as the cache-miss
					// sentinel without weakening exact byte comparison after bucket lookup.
					if(payloadHash == 0U) payloadHash = 1U;
					payloadHashCache[payloadOwner.get()] = {payloadOwner, payloadHash};
				}
				std::size_t hash = payloadHash;
				const auto mixBytes = [&hash](const void* data, std::size_t byteCount) {
					const auto* bytes = static_cast<const std::uint8_t*>(data);
					for(std::size_t index = 0U; index < byteCount; ++index) {
						hash ^= bytes[index];
						hash *= 1099511628211ULL;
					}
				};
				const std::array<std::uint32_t, 6U> shape{
					identity.programResourceIndex, identity.skeletonResourceIndex,
					identity.morphResourceIndex, identity.jointCount,
					identity.morphCount, identity.paletteEntryCount};
				mixBytes(shape.data(), sizeof(shape));
				return hash;
			};

			for(const GpuAnimationFrameAtlasEntry& entry : entries) {
				if(!entry.residentPayloads
					|| !entry.request.renderIdentity.IsComplete()
					|| (!entry.residentPayloadsValidated
						&& !ValidateGpuAnimationResidentPayloads(
							entry.request, *entry.residentPayloads))
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
				const AtlasPayloadIdentity payloadIdentity{
					entry.residentPayloads.get(), entry.request.programResourceIndex,
					entry.request.skeletonResourceIndex,
					entry.request.morphResourceIndex, entry.request.jointCount,
					entry.request.morphCount, entry.request.paletteEntryCount};
				auto identityMapping = payloadIdentityMappings.find(payloadIdentity);
				const std::size_t payloadContentHash = identityMapping
					== payloadIdentityMappings.end()
					? buildContentHash(payloadIdentity, entry.residentPayloads) : 0U;
				auto existingMapping = payloadMappings.end();
				if(identityMapping != payloadIdentityMappings.end()) {
					existingMapping = payloadMappings.begin()
						+ static_cast<std::ptrdiff_t>(identityMapping->second);
				} else {
					const auto candidates = payloadContentMappings.equal_range(payloadContentHash);
					for(auto candidate = candidates.first;
						candidate != candidates.second; ++candidate) {
						AtlasPayloadMapping& mapping = payloadMappings[candidate->second];
						// Content hashes only select a small bucket. Exact bytes and the
						// selected resource shape remain the proof used for aliasing.
						if(mapping.sourceProgramResourceIndex
								== entry.request.programResourceIndex
							&& mapping.sourceSkeletonResourceIndex
								== entry.request.skeletonResourceIndex
							&& mapping.sourceMorphResourceIndex
								== entry.request.morphResourceIndex
							&& mapping.jointCount == entry.request.jointCount
							&& mapping.morphCount == entry.request.morphCount
							&& mapping.paletteEntryCount == entry.request.paletteEntryCount
							&& mapping.payload->programWords == source.programWords
							&& mapping.payload->packedClipBytes == source.packedClipBytes
							&& mapping.payload->skeletonTables == source.skeletonTables
							&& mapping.payload->morphInputs == source.morphInputs) {
							existingMapping = payloadMappings.begin()
								+ static_cast<std::ptrdiff_t>(candidate->second);
							break;
						}
					}
				}
				if(existingMapping != payloadMappings.end()) {
					// A distinct immutable owner may contain identical bytes. Remember the
					// proven mapping so every later crowd member is an O(1) pointer lookup.
					payloadIdentityMappings.try_emplace(
						payloadIdentity,
						static_cast<std::size_t>(
							std::distance(payloadMappings.begin(), existingMapping)));
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
				payloadIdentityMappings.emplace(
					payloadIdentity, payloadMappings.size() - 1U);
				payloadContentMappings.emplace(
					payloadContentHash, payloadMappings.size() - 1U);
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

	namespace {
		[[nodiscard]] bool TryRefreshValidatedSharedPayloadPlan(
			std::span<const GpuAnimationLoweringInput> inputs,
			std::uint64_t topologyInputSignature,
			std::uint32_t planGeneration,
			GpuAnimationArena& scratchArena,
			GpuAnimationArena& paletteArena,
			const GpuAnimationPlan& previous,
			GpuAnimationPlan& output,
			const GpuAnimationPlanLimits& limits,
			std::shared_ptr<const GpuAnimationPlan> previousOwner,
			const std::shared_ptr<GpuAnimationPlanDynamicLanes>&
				dynamicLaneScratch) noexcept
		{
			const auto& packets = previous.WorkPackets();
			const std::size_t inputCount = inputs.size();
			if(topologyInputSignature == 0U
				|| previous.topologyInputSignature != topologyInputSignature
				|| planGeneration == 0U || previous.generation == 0U
				|| previous.buildLimits != limits
				|| scratchArena.Kind() != GpuAnimationArenaKind::Scratch
				|| paletteArena.Kind() != GpuAnimationArenaKind::Palette
				|| inputCount != packets.size()
				|| inputCount != previous.GenerationRecordCount()
				|| inputCount != previous.RenderIdentityCount()
				|| (previousOwner && previousOwner.get() != &previous)
				|| inputCount > limits.maximumPackets) {
				return false;
			}

			try {
				GpuAnimationArena refreshedScratch = scratchArena;
				GpuAnimationArena refreshedPalette = paletteArena;
				if(refreshedScratch.BeginGeneration(planGeneration)
						!= GpuAnimationPlanStatus::Ok
					|| refreshedPalette.BeginGeneration(planGeneration)
						!= GpuAnimationPlanStatus::Ok) {
					return false;
				}

				GpuAnimationPlan refreshed;
				refreshed.generation = planGeneration;
				refreshed.buildLimits = previous.buildLimits;
				refreshed.diagnostics = previous.diagnostics;
				refreshed.topologyInputSignature = topologyInputSignature;
				refreshed.topologyMembershipGeneration = previous.topologyMembershipGeneration;
				refreshed.topologyResourceGeneration = previous.topologyResourceGeneration;
				refreshed.topologyProgramGeneration = previous.topologyProgramGeneration;
				refreshed.topologySceneGeneration = previous.topologySceneGeneration;
				refreshed.topologyRendererGeneration = previous.topologyRendererGeneration;
				refreshed.retainedStaticPlan = previous.retainedStaticPlan
					? previous.retainedStaticPlan
					: (previousOwner ? std::move(previousOwner)
						: std::make_shared<GpuAnimationPlan>(previous));
				std::shared_ptr<GpuAnimationPlanDynamicLanes> dynamicLanes;
				if(dynamicLaneScratch && dynamicLaneScratch.use_count() == 1U) {
					dynamicLanes = dynamicLaneScratch;
					dynamicLanes->ResetForReuse();
				}
				auto& generationRecords = dynamicLanes
					? dynamicLanes->generationRecords
					: refreshed.dynamicGenerationRecords;
				auto& sampleTicks = dynamicLanes
					? dynamicLanes->sampleTicks
					: refreshed.dynamicSampleTicks;
				auto& renderIdentities = dynamicLanes
					? dynamicLanes->renderIdentities
					: refreshed.dynamicRenderIdentities;
				generationRecords.resize(inputCount);
				sampleTicks.resize(inputCount);
				renderIdentities.resize(inputCount);

				for(std::size_t packetIndex = 0U; packetIndex < inputs.size(); ++packetIndex) {
					const GpuAnimationLoweringInput& input = inputs[packetIndex];
					const GpuAnimationWorkPacket& packet = packets[packetIndex];
					// topologyInputSignature was produced while these exact immutable
					// inputs were captured and includes payload owner, stable identity,
					// submission order, resource selectors, shape, execution mode, and
					// every batch generation. Rechecking those fields here would turn the
					// proof back into a second 4,000-row topology scan. Only changing lanes
					// and the compact destination ordinal still require per-frame checks.
					if(!IsGpuExecutionMode(input.effectiveMode)
						|| !IsGpuAnimationGenerationRecordValid(input.visual.generations)
						|| !IsGpuAnimationRenderIdentityValid(input.visual.renderIdentity)
						|| input.residentPayloads
							!= previous.sharedResidentPayloads
						|| packet.generationRecordIndex >= inputs.size()) {
						return false;
					}

					GpuAnimationGenerationRecord publishedGeneration =
						input.visual.generations;
					if(input.visual.suspendVisualEvaluation) {
						publishedGeneration.flags |=
							kGpuAnimationGenerationFlagSuspendVisualEvaluation;
					} else {
						publishedGeneration.flags &=
							~kGpuAnimationGenerationFlagSuspendVisualEvaluation;
					}
					publishedGeneration.reserved = input.visual.allowVisualRagdoll
						? input.visual.ragdollTableOffsetBytes : 0U;
					generationRecords[packet.generationRecordIndex] =
						publishedGeneration;
					sampleTicks[packetIndex] = input.visual.sampleTick;
					renderIdentities[packetIndex] =
						input.visual.renderIdentity;
				}
				if(dynamicLanes) {
					refreshed.retainedDynamicLanes = std::move(dynamicLanes);
				}

				output = std::move(refreshed);
				scratchArena = std::move(refreshedScratch);
				paletteArena = std::move(refreshedPalette);
				return true;
			} catch(const std::bad_alloc&) {
				return false;
			} catch(...) {
				return false;
			}
		}
	}

	bool TryRefreshLoweredGpuAnimationDynamicPlan(
		std::span<const GpuAnimationLoweringInput> inputs,
		std::uint64_t topologyInputSignature,
		std::uint32_t planGeneration,
		GpuAnimationArena& scratchArena,
		GpuAnimationArena& paletteArena,
		const GpuAnimationPlan& previous,
		GpuAnimationPlan& output,
		const GpuAnimationPlanLimits& limits,
		std::shared_ptr<const GpuAnimationPlan> previousOwner,
		const std::shared_ptr<GpuAnimationPlanDynamicLanes>&
			dynamicLaneScratch) noexcept
	{
		if(!previous.sharedResidentPayloads
			|| previous.WorkPackets().size() != inputs.size()
			|| !TryRefreshValidatedSharedPayloadPlan(
				inputs, topologyInputSignature, planGeneration, scratchArena,
				paletteArena, previous, output, limits,
				std::move(previousOwner), dynamicLaneScratch)) {
			return false;
		}
		output.sharedResidentPayloads = previous.sharedResidentPayloads;
		return true;
	}

	GpuAnimationLoweringStatus BuildLoweredGpuAnimationPlan(
		std::span<const GpuAnimationLoweringInput> inputs,
		std::uint32_t planGeneration,
		GpuAnimationArena& scratchArena,
		GpuAnimationArena& paletteArena,
		GpuAnimationPlan& output,
		GpuAnimationLoweredPlanResult& result,
		const GpuAnimationPlanLimits& limits,
		GpuAnimationFrameAtlasCache* atlasCache,
		const GpuAnimationPlan* previousPlan,
		std::shared_ptr<const GpuAnimationPlan> previousPlanOwner,
		std::uint64_t precomputedTopologyInputSignature,
		const std::shared_ptr<GpuAnimationPlanDynamicLanes>&
			dynamicLaneScratch) noexcept
	{
		result = {};
		try {
			std::uint64_t topologyInputSignature =
				precomputedTopologyInputSignature;
			if(topologyInputSignature == 0U) {
				GpuAnimationLoweringTopologySignatureBuilder signatureBuilder;
				for(const GpuAnimationLoweringInput& input : inputs) {
					signatureBuilder.Include(input);
				}
				topologyInputSignature = signatureBuilder.Complete();
			}

			// The normal warmed crowd keeps both topology and one immutable cooked
			// payload unchanged. Validate those cheap identities first, then update
			// only work generations and sample ticks. Any mismatch leaves the caller's
			// plan and arenas untouched and falls through to the complete checked path.
			if(previousPlan != nullptr && !inputs.empty()
				&& previousPlan->sharedResidentPayloads
				&& previousPlan->topologyInputSignature == topologyInputSignature
				&& previousPlan->WorkPackets().size() == inputs.size()
				&& TryRefreshLoweredGpuAnimationDynamicPlan(
					inputs, topologyInputSignature, planGeneration, scratchArena,
					paletteArena, *previousPlan, output, limits,
					previousPlanOwner, dynamicLaneScratch)) {
				result.planStatus = GpuAnimationPlanStatus::Ok;
				result.topologyReused = true;
				result.status = GpuAnimationLoweringStatus::Ok;
				return result.status;
			}

			// A warmed heterogeneous crowd already owns one validated atlas entry per
			// input. Lower directly into relocated requests and validate them against
			// the immutable prior plan instead of first materializing an intermediate
			// atlas-entry vector, rescanning that vector, and then moving every request
			// into a second allocation. Any ownership, resource-shape, or topology
			// mismatch leaves every output untouched and falls through to the complete
			// transactional builder below.
			if(previousPlan != nullptr && atlasCache != nullptr
				&& atlasCache->residentPayloads
				&& previousPlan->sharedResidentPayloads == atlasCache->residentPayloads
				&& atlasCache->entries.size() == inputs.size()
				&& previousPlan->WorkPackets().size() == inputs.size()) {
				struct CachedAtlasRefreshContext {
					std::span<const GpuAnimationLoweringInput> inputs;
					const GpuAnimationFrameAtlasCache* cache = nullptr;
				};
				CachedAtlasRefreshContext refreshContext{inputs, atlasCache};
				const auto requestProducer = +[](
					std::size_t index,
					GpuAnimationWorkRequest& request,
					void* context) noexcept -> bool
				{
					const auto* refresh =
						static_cast<const CachedAtlasRefreshContext*>(context);
					if(refresh == nullptr || refresh->cache == nullptr
						|| index >= refresh->inputs.size()
						|| index >= refresh->cache->entries.size()) {
						return false;
					}

					const GpuAnimationLoweringInput& input = refresh->inputs[index];
					if(LowerGpuAnimationWorkRequest(input, request)
							!= GpuAnimationLoweringStatus::Ok
						|| !input.residentPayloads
						|| (!input.residentPayloadsValidated
							&& !ValidateGpuAnimationResidentPayloads(
								request, *input.residentPayloads))) {
						return false;
					}

					const GpuAnimationFrameAtlasCacheEntry& cached =
						refresh->cache->entries[index];
					if(cached.sourcePayload.lock() != input.residentPayloads
						|| !IsRelocatableSingleClipRequest(request, *input.residentPayloads)
						|| cached.sourceProgramResourceIndex != request.programResourceIndex
						|| cached.sourceSkeletonResourceIndex != request.skeletonResourceIndex
						|| cached.sourceMorphResourceIndex != request.morphResourceIndex
						|| cached.jointCount != request.jointCount
						|| cached.morphCount != request.morphCount
						|| cached.paletteEntryCount != request.paletteEntryCount) {
						return false;
					}

					// The retained atlas owns immutable payload storage. Only these absolute
					// resource offsets are rewritten; per-frame ticks and generations remain
					// those produced from the current presentation input.
					request.clipResourceIndex = cached.relocatedClipResourceIndex;
					request.programResourceIndex = cached.relocatedProgramResourceIndex;
					request.skeletonResourceIndex = cached.relocatedSkeletonResourceIndex;
					request.morphResourceIndex = cached.relocatedMorphResourceIndex;
					return true;
				};

				if(TryRefreshGeneratedGpuAnimationPlan(
					inputs.size(), requestProducer, &refreshContext, planGeneration,
					scratchArena, paletteArena, *previousPlan, output, limits,
					previousPlanOwner, dynamicLaneScratch)) {
					output.sharedResidentPayloads = atlasCache->residentPayloads;
					output.topologyInputSignature = topologyInputSignature;
					result.planStatus = GpuAnimationPlanStatus::Ok;
					result.topologyReused = true;
					result.status = GpuAnimationLoweringStatus::Ok;
					return result.status;
				}
			}

			// The usual crowd case already shares one immutable cooked payload.  Do
			// the same one-pass refresh as the retained atlas path above instead of
			// first allocating an atlas-entry vector and then allocating a second
			// request vector only to prove that every entry names that same payload.
			// The generated refresh still compares every lowered request with the
			// prior immutable packet, so a changed clip, program, or resource layout
			// rejects this candidate and falls through to the complete builder below.
			if(previousPlan != nullptr && !inputs.empty()
				&& previousPlan->sharedResidentPayloads
				&& previousPlan->WorkPackets().size() == inputs.size()
				&& std::all_of(inputs.begin(), inputs.end(), [previousPlan](
					const GpuAnimationLoweringInput& input) {
					return input.residentPayloads == previousPlan->sharedResidentPayloads;
				})) {
				struct SharedPayloadRefreshContext {
					std::span<const GpuAnimationLoweringInput> inputs;
					const std::shared_ptr<const GpuAnimationResidentPayloads>* payload = nullptr;
				};
				const std::shared_ptr<const GpuAnimationResidentPayloads>& sharedPayload =
					previousPlan->sharedResidentPayloads;
				SharedPayloadRefreshContext refreshContext{inputs, &sharedPayload};
				const auto requestProducer = +[](
					std::size_t index,
					GpuAnimationWorkRequest& request,
					void* context) noexcept -> bool
				{
					const auto* refresh =
						static_cast<const SharedPayloadRefreshContext*>(context);
					if(refresh == nullptr || refresh->payload == nullptr
						|| index >= refresh->inputs.size()) {
						return false;
					}

					const GpuAnimationLoweringInput& input = refresh->inputs[index];
					if(input.residentPayloads != *refresh->payload
						|| LowerGpuAnimationWorkRequest(input, request)
							!= GpuAnimationLoweringStatus::Ok) {
						return false;
					}
					// AnimationSystem normally supplies this proof. Direct callers retain
					// the complete payload validation before their request can reuse the
					// prior plan.
					return input.residentPayloadsValidated
						|| ValidateGpuAnimationResidentPayloads(
							request, **refresh->payload);
				};

				if(TryRefreshGeneratedGpuAnimationPlan(
					inputs.size(), requestProducer, &refreshContext, planGeneration,
					scratchArena, paletteArena, *previousPlan, output, limits,
					previousPlanOwner, dynamicLaneScratch)) {
					output.sharedResidentPayloads = sharedPayload;
					output.topologyInputSignature = topologyInputSignature;
					result.planStatus = GpuAnimationPlanStatus::Ok;
					result.topologyReused = true;
					result.status = GpuAnimationLoweringStatus::Ok;
					return result.status;
				}
			}

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
					|| (!input.residentPayloadsValidated
						&& !ValidateGpuAnimationResidentPayloads(
							request, *input.residentPayloads))) {
					// Incomplete GPU residency is an expected capability failure. The CPU
					// evaluator remains authoritative and no malformed GPU packet is emitted.
					continue;
				}
				atlasEntries.push_back({
					std::move(request),
					input.residentPayloads,
					input.residentPayloadsValidated});
			}
			std::vector<GpuAnimationWorkRequest> stagedRequests;
			GpuAnimationResidentPayloads payloadCopy{};
			std::shared_ptr<const GpuAnimationResidentPayloads> sharedPayload;
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
					// Cooked payload bytes are immutable and already shared by every
					// lowering input. Preserve that ownership rather than duplicating
					// the same clip/program/skeleton vectors once per rendered frame.
					sharedPayload = firstPayload;
				} else {
					bool cacheMatches = atlasCache != nullptr
						&& atlasCache->residentPayloads
						&& atlasCache->entries.size() == atlasEntries.size();
					if(cacheMatches) {
						for(std::size_t index = 0U; index < atlasEntries.size(); ++index) {
							const GpuAnimationFrameAtlasEntry& current = atlasEntries[index];
							const GpuAnimationFrameAtlasCacheEntry& cached =
								atlasCache->entries[index];
							// Payload objects are immutable. Exact shared ownership plus the selected
							// descriptor shape proves every cached relocation remains valid, while
							// sample ticks and generation records stay frame-local below.
							cacheMatches = cached.sourcePayload.lock() == current.residentPayloads
								&& IsRelocatableSingleClipRequest(
									current.request, *current.residentPayloads)
								&& cached.sourceProgramResourceIndex
									== current.request.programResourceIndex
								&& cached.sourceSkeletonResourceIndex
									== current.request.skeletonResourceIndex
								&& cached.sourceMorphResourceIndex
									== current.request.morphResourceIndex
								&& cached.jointCount == current.request.jointCount
								&& cached.morphCount == current.request.morphCount
								&& cached.paletteEntryCount == current.request.paletteEntryCount;
							if(!cacheMatches) break;
						}
					}
					if(cacheMatches) {
						stagedRequests.reserve(atlasEntries.size());
						for(std::size_t index = 0U; index < atlasEntries.size(); ++index) {
							GpuAnimationWorkRequest relocated = std::move(atlasEntries[index].request);
							const GpuAnimationFrameAtlasCacheEntry& cached =
								atlasCache->entries[index];
							relocated.clipResourceIndex = cached.relocatedClipResourceIndex;
							relocated.programResourceIndex = cached.relocatedProgramResourceIndex;
							relocated.skeletonResourceIndex = cached.relocatedSkeletonResourceIndex;
							relocated.morphResourceIndex = cached.relocatedMorphResourceIndex;
							stagedRequests.push_back(std::move(relocated));
						}
						sharedPayload = atlasCache->residentPayloads;
					} else {
						GpuAnimationFrameAtlas atlas;
						const GpuAnimationLoweringStatus atlasStatus =
							BuildGpuAnimationFrameAtlas(atlasEntries, atlas);
						if(atlasStatus != GpuAnimationLoweringStatus::Ok) {
							result.status = atlasStatus;
							return atlasStatus;
						}
						stagedRequests = std::move(atlas.requests);
						if(atlasCache != nullptr) {
							auto immutablePayload =
								std::make_shared<const GpuAnimationResidentPayloads>(
									std::move(atlas.residentPayloads));
							std::vector<GpuAnimationFrameAtlasCacheEntry> refreshedEntries;
							refreshedEntries.reserve(atlasEntries.size());
							for(std::size_t index = 0U; index < atlasEntries.size(); ++index) {
								const GpuAnimationFrameAtlasEntry& sourceEntry = atlasEntries[index];
								const GpuAnimationWorkRequest& relocated = stagedRequests[index];
								refreshedEntries.push_back({
									sourceEntry.residentPayloads,
									sourceEntry.request.programResourceIndex,
									sourceEntry.request.skeletonResourceIndex,
									sourceEntry.request.morphResourceIndex,
									sourceEntry.request.jointCount,
									sourceEntry.request.morphCount,
									sourceEntry.request.paletteEntryCount,
									relocated.clipResourceIndex,
									relocated.programResourceIndex,
									relocated.skeletonResourceIndex,
									relocated.morphResourceIndex});
							}
							atlasCache->entries = std::move(refreshedEntries);
							atlasCache->residentPayloads = immutablePayload;
							sharedPayload = std::move(immutablePayload);
						} else {
							payloadCopy = std::move(atlas.residentPayloads);
						}
					}
				}
			}
			GpuAnimationPlanBuildDiagnostics diagnostics{};
			const bool topologyReused = previousPlan != nullptr
				&& sharedPayload
				&& previousPlan->sharedResidentPayloads == sharedPayload
				&& TryRefreshGpuAnimationPlan(
					stagedRequests,
					planGeneration,
					scratchArena,
					paletteArena,
					*previousPlan,
					output,
					limits,
					previousPlanOwner,
					dynamicLaneScratch);
			const GpuAnimationPlanStatus planStatus = topologyReused
				? GpuAnimationPlanStatus::Ok
				: BuildGpuAnimationPlan(
					stagedRequests, planGeneration, scratchArena, paletteArena, output, limits, &diagnostics);
			result.planStatus = planStatus;
			result.diagnostics = diagnostics;
			result.topologyReused = topologyReused;
			if(planStatus != GpuAnimationPlanStatus::Ok) {
				result.status = GpuAnimationLoweringStatus::PlanBuildFailed;
				return result.status;
			}
			output.residentPayloads = std::move(payloadCopy);
			output.sharedResidentPayloads = std::move(sharedPayload);
			output.topologyInputSignature = topologyInputSignature;
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
