#include "VulkanDescriptorPlanning.h"

#include <algorithm>
#include <limits>

namespace TerraEngine::Renderer::Vulkan {
	namespace {
		[[nodiscard]] bool AddBytes(std::uint64_t offset, std::uint64_t count, std::uint64_t stride, std::uint64_t& output)
		{
			if(count != 0U && stride > (std::numeric_limits<std::uint64_t>::max() - offset) / count) return false;
			output = offset + count * stride;
			return true;
		}

		[[nodiscard]] bool ValidatePlanRecords(const Animation::GpuAnimationPlan& plan)
		{
			if(plan.generation == 0U) return false;
			for(std::size_t commandIndex = 0U;
				commandIndex < plan.DispatchCommands().size(); ++commandIndex) {
				const Animation::GpuAnimationDispatchCommand command =
					plan.ResolvedDispatchCommand(commandIndex);
				if(command.groupCountX == 0U || command.groupCountX > Animation::kGpuAnimationMaximumGroupsX
					|| command.pushConstants.planGeneration != plan.generation
					|| !Animation::IsGpuAnimationStageValueValid(command.pushConstants.stage)) return false;
			}
			return true;
		}
	}

	DescriptorBindingPlan BuildDescriptorBindingPlan(
		const RenderFramePublication& publication, std::uint32_t viewId)
	{
		DescriptorBindingPlan plan;
		plan.frameId = publication.frameId;
		plan.viewId = viewId;
		plan.records.reserve(publication.resourcePreparation.records.size());
		for (const RenderResourcePreparationRecord& resource :
			publication.resourcePreparation.records)
		{
			// Type-specific descriptor layouts belong to the persistent packet
			// template. This common table only establishes one stable resource row.
			plan.records.push_back("resource:" + TerraEngine::ToHexString(
				resource.resourceId));
		}
		plan.fallbackReason = plan.coordinatorOwnedAllocator ? "Descriptor allocator mutation remains coordinator-owned." : "";
		return plan;
	}

	VulkanAnimationDescriptorSetPlan BuildVulkanAnimationDescriptorSetPlan(const Animation::GpuAnimationPlan& plan)
	{
		VulkanAnimationDescriptorSetPlan output;
		output.planGeneration = plan.generation;
		output.hasWork = !plan.DispatchCommands().empty();
		if(!output.hasWork) {
			// A zero-work animation frame intentionally changes no Vulkan state.
			output.valid = true;
			return output;
		}
		if(!ValidatePlanRecords(plan)) {
			output.rejectionReason = "Animation plan generation, stage, or dispatch bounds are invalid.";
			return output;
		}

		using Binding = Animation::GpuAnimationDescriptorBinding;
		auto configure = [&output](Binding binding, VulkanAnimationDescriptorAccess access,
			VulkanAnimationDescriptorSource source, std::uint64_t minimumBytes) {
			VulkanAnimationDescriptorBindingPlan& entry = output.bindings[static_cast<std::size_t>(binding)];
			entry.binding = binding;
			entry.access = access;
			entry.source = source;
			entry.minimumByteCount = minimumBytes;
		};

		std::uint64_t generationBytes = 0U;
		std::uint64_t packetBytes = 0U;
		std::uint64_t dispatchBytes = 0U;
		std::uint64_t sampleTickBytes = 0U;
		if(!AddBytes(0U, plan.GenerationRecordCount(), sizeof(Animation::GpuAnimationGenerationRecord), generationBytes)
			|| !AddBytes(0U, plan.WorkPackets().size(), sizeof(Animation::GpuAnimationWorkPacket), packetBytes)
			|| !AddBytes(0U, plan.DispatchRecords().size(), sizeof(Animation::GpuAnimationDispatchRecord), dispatchBytes)
			|| !AddBytes(0U, plan.WorkPackets().size(), sizeof(std::uint32_t), sampleTickBytes)) {
			output.rejectionReason = "Animation frame-upload range overflows uint64.";
			return output;
		}
		configure(Binding::GenerationRecords, VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::FrameUpload, generationBytes);
		configure(Binding::WorkPackets, VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::FrameUpload, packetBytes);
		configure(Binding::DispatchRecords, VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::FrameUpload, dispatchBytes);
		configure(Binding::SampleTicks, VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::FrameUpload, sampleTickBytes);
		configure(Binding::ProgramWords, VulkanAnimationDescriptorAccess::ReadOnly, VulkanAnimationDescriptorSource::ResidentInput, 0U);
		configure(Binding::PackedClipBytes, VulkanAnimationDescriptorAccess::ReadOnly, VulkanAnimationDescriptorSource::ResidentInput, 0U);
		configure(Binding::SkeletonTables, VulkanAnimationDescriptorAccess::ReadOnly, VulkanAnimationDescriptorSource::ResidentInput, 0U);
		configure(Binding::MaskTables, VulkanAnimationDescriptorAccess::ReadOnly, VulkanAnimationDescriptorSource::ResidentInput, 0U);
		configure(Binding::MorphInputs, VulkanAnimationDescriptorAccess::ReadOnly, VulkanAnimationDescriptorSource::ResidentInput, 0U);

		std::uint64_t scratchBytes = 0U;
		std::uint64_t localPoseBytes = 0U;
		std::uint64_t modelPoseBytes = 0U;
		std::uint64_t paletteBytes = 0U;
		std::uint64_t morphBytes = 0U;
		std::uint64_t boundsBytes = 0U;
		std::uint64_t diagnosticBytes = 0U;
		for(const Animation::GpuAnimationWorkPacket& packet : plan.WorkPackets()) {
			auto accumulate = [](std::uint64_t offset, std::uint64_t count, std::uint64_t stride, std::uint64_t& total) {
				std::uint64_t candidate = 0U;
				return AddBytes(offset, count, stride, candidate) && (total = std::max(total, candidate), true);
			};
			if(!accumulate(packet.scratchOffsetBytes, packet.scratchSizeBytes, 1U, scratchBytes)
				|| !accumulate(packet.localPoseOffsetBytes, packet.jointCount, Animation::kGpuAnimationPoseStride, localPoseBytes)
				|| !accumulate(packet.modelPoseOffsetBytes, packet.jointCount, Animation::kGpuAnimationAffineStride, modelPoseBytes)
				|| !accumulate(packet.paletteOffsetBytes, packet.paletteEntryCount, Animation::kGpuAnimationPaletteStride, paletteBytes)
				|| !accumulate(packet.morphOutputOffsetBytes, packet.morphCount, sizeof(float), morphBytes)
				|| !accumulate(0U, static_cast<std::uint64_t>(packet.boundsOutputIndex) + 1U, Animation::kGpuAnimationBoundsStride, boundsBytes)
				|| !accumulate(0U, static_cast<std::uint64_t>(packet.diagnosticIndex) + 1U, Animation::kGpuAnimationDiagnosticStride, diagnosticBytes)) {
				output.rejectionReason = "Animation descriptor byte range overflows uint64.";
				return output;
			}
		}
		configure(Binding::Scratch, VulkanAnimationDescriptorAccess::ReadWrite, VulkanAnimationDescriptorSource::ComputeOutput, scratchBytes);
		configure(Binding::LocalPoses, VulkanAnimationDescriptorAccess::ReadWrite, VulkanAnimationDescriptorSource::ComputeOutput, localPoseBytes);
		configure(Binding::ModelPoses, VulkanAnimationDescriptorAccess::ReadWrite, VulkanAnimationDescriptorSource::ComputeOutput, modelPoseBytes);
		configure(Binding::PaletteMatrices, VulkanAnimationDescriptorAccess::ReadWrite, VulkanAnimationDescriptorSource::ComputeOutput, paletteBytes);
		configure(Binding::MorphOutputs, VulkanAnimationDescriptorAccess::ReadWrite, VulkanAnimationDescriptorSource::ComputeOutput, morphBytes);
		configure(Binding::BoundsOutputs, VulkanAnimationDescriptorAccess::ReadWrite, VulkanAnimationDescriptorSource::ComputeOutput, boundsBytes);
		configure(Binding::Diagnostics, VulkanAnimationDescriptorAccess::ReadWrite, VulkanAnimationDescriptorSource::ComputeOutput, diagnosticBytes);
		output.valid = true;
		return output;
	}

	bool BuildVulkanAnimationComputeUploadPlan(
		const Animation::GpuAnimationPlan& plan,
		VulkanAnimationComputeUploadPlan& output) noexcept
	{
		output.topologyReused = false;
		const bool canReuseTopology = output.valid
			&& plan.retainedStaticPlan
			&& output.retainedStaticPlan == plan.retainedStaticPlan
			&& output.compactSourcePacketIndices.size() == output.workPackets.size()
			&& output.generationRecords.size() == output.workPackets.size()
			&& output.sampleTicks.size() == output.workPackets.size()
			&& output.dispatchRecords.size() == plan.DispatchRecords().size();
		if(canReuseTopology) {
			// Validate every dynamic source before modifying the retained slot. Pointer
			// identity proves the packet/dispatch layout, while these checks prevent a
			// malformed generation publication from partially replacing the last-good
			// upload plan.
			for(const std::uint32_t sourcePacketIndex
				: output.compactSourcePacketIndices) {
				if(sourcePacketIndex >= plan.WorkPackets().size()) return false;
				const Animation::GpuAnimationWorkPacket& sourcePacket =
					plan.WorkPackets()[sourcePacketIndex];
				if(sourcePacket.generationRecordIndex
					>= plan.GenerationRecordCount()) return false;
			}
			for(std::size_t compactIndex = 0U;
				compactIndex < output.compactSourcePacketIndices.size(); ++compactIndex) {
				const std::uint32_t sourcePacketIndex =
					output.compactSourcePacketIndices[compactIndex];
				const Animation::GpuAnimationWorkPacket& sourcePacket =
					plan.WorkPackets()[sourcePacketIndex];
				// Packet bytes are topology-owned and already resident in this frame
				// slot. Refresh only the generation and sample lanes that can change.
				output.generationRecords[compactIndex] =
					plan.GenerationRecord(sourcePacket.generationRecordIndex);
				output.sampleTicks[compactIndex] = plan.SampleTick(sourcePacketIndex);
			}
			output.topologyReused = true;
			return true;
		}

		output.valid = false;
		output.generationRecords.clear();
		output.sampleTicks.clear();
		output.workPackets.clear();
		output.dispatchRecords.clear();
		output.packetRemapScratch.clear();
		output.compactSourcePacketIndices.clear();
		output.retainedStaticPlan.reset();
		if(plan.generation == 0U || plan.DispatchRecords().empty()
			|| plan.WorkPackets().empty() || plan.GenerationRecordCount() == 0U) return false;

		try {
			constexpr std::uint32_t invalidIndex =
				std::numeric_limits<std::uint32_t>::max();
			output.packetRemapScratch.assign(plan.WorkPackets().size(), invalidIndex);
			const std::size_t maximumSourceCount = std::min(
				plan.WorkPackets().size(), plan.DispatchRecords().size());
			output.generationRecords.reserve(maximumSourceCount);
			output.sampleTicks.reserve(maximumSourceCount);
			output.workPackets.reserve(maximumSourceCount);
			output.dispatchRecords.reserve(plan.DispatchRecords().size());
			output.compactSourcePacketIndices.reserve(maximumSourceCount);

			for(const Animation::GpuAnimationDispatchRecord& sourceDispatch
				: plan.DispatchRecords()) {
				if(sourceDispatch.workPacketIndex >= plan.WorkPackets().size()) return false;
				const Animation::GpuAnimationWorkPacket& sourcePacket =
					plan.WorkPackets()[sourceDispatch.workPacketIndex];
				if(sourcePacket.generationRecordIndex >= plan.GenerationRecordCount()
					|| sourceDispatch.generationRecordIndex
						!= sourcePacket.generationRecordIndex
					|| sourceDispatch.stableInstanceId != sourcePacket.stableInstanceId) {
					return false;
				}

				std::uint32_t compactPacketIndex =
					output.packetRemapScratch[sourceDispatch.workPacketIndex];
				if(compactPacketIndex == invalidIndex) {
					if(output.workPackets.size()
						>= static_cast<std::size_t>(invalidIndex)) return false;
					compactPacketIndex = static_cast<std::uint32_t>(
						output.workPackets.size());
					output.packetRemapScratch[sourceDispatch.workPacketIndex] =
						compactPacketIndex;
					Animation::GpuAnimationWorkPacket compactPacket =
						plan.ResolvedWorkPacket(sourceDispatch.workPacketIndex);
					compactPacket.generationRecordIndex = compactPacketIndex;
					output.generationRecords.push_back(
						plan.GenerationRecord(sourcePacket.generationRecordIndex));
					output.sampleTicks.push_back(
						plan.SampleTick(sourceDispatch.workPacketIndex));
					output.workPackets.push_back(compactPacket);
					output.compactSourcePacketIndices.push_back(
						sourceDispatch.workPacketIndex);
				}

				Animation::GpuAnimationDispatchRecord compactDispatch = sourceDispatch;
				compactDispatch.workPacketIndex = compactPacketIndex;
				compactDispatch.generationRecordIndex = compactPacketIndex;
				output.dispatchRecords.push_back(compactDispatch);
			}
		} catch(...) {
			// Scratch allocation failure rejects this backend candidate before any
			// mapped buffer is changed; the prior frame-slot contents remain private.
			return false;
		}
		output.valid = !output.workPackets.empty()
			&& output.generationRecords.size() == output.workPackets.size()
			&& output.sampleTicks.size() == output.workPackets.size()
			&& output.compactSourcePacketIndices.size() == output.workPackets.size()
			&& output.dispatchRecords.size() == plan.DispatchRecords().size();
		if(output.valid) output.retainedStaticPlan = plan.retainedStaticPlan;
		return output.valid;
	}

	bool CanReuseVulkanAnimationResidentPayloadUpload(
		const std::shared_ptr<const Animation::GpuAnimationResidentPayloads>& previousOwner,
		const std::shared_ptr<const Animation::GpuAnimationResidentPayloads>& currentOwner,
		bool residentBuffersRetained) noexcept
	{
		// Pointer identity is an immutable-content identity here: animation asset
		// publication and atlas caches create a new shared object whenever any cooked
		// byte changes. Retaining the prior owner also prevents allocator address reuse.
		return residentBuffersRetained
			&& currentOwner
			&& previousOwner == currentOwner;
	}

	bool ShouldRefreshVulkanAnimationBindlessBuffer(
		bool hasPublishedIndex,
		bool bufferReallocated) noexcept
	{
		return !hasPublishedIndex || bufferReallocated;
	}

	VulkanDeformationDescriptorSetPlan BuildVulkanDeformationDescriptorSetPlan(
		const GpuDeformationPlan& plan,
		std::uint64_t workingSetLimitBytes)
	{
		VulkanDeformationDescriptorSetPlan output;
		output.hasWork = !plan.work.empty();
		if(!output.hasWork)
		{
			// Static or dormant frames must not allocate deformation resources.
			output.valid = plan.valid;
			if(!plan.valid)
			{
				output.rejectionReason = plan.rejectionReason;
			}
			return output;
		}

		output.workingSet = PlanDeformationWorkingSet(plan, workingSetLimitBytes);
		if(!output.workingSet.accepted)
		{
			output.rejectionReason = output.workingSet.rejectionReason;
			return output;
		}

		auto configure = [&output](
			VulkanDeformationDescriptorBinding binding,
			VulkanAnimationDescriptorAccess access,
			VulkanAnimationDescriptorSource source,
			std::uint64_t byteCount)
		{
			auto& record = output.bindings[static_cast<std::size_t>(binding)];
			record.binding = binding;
			record.access = access;
			record.source = source;
			record.minimumByteCount = byteCount;
		};

		std::uint64_t generationBytes = 0u;
		std::uint64_t workBytes = 0u;
		if(!AddBytes(
				0u,
				plan.work.size(),
				sizeof(GpuDeformationGenerationRecord),
				generationBytes)
			|| !AddBytes(
				0u,
				plan.work.size(),
				sizeof(GpuDeformationShaderWorkRecord),
				workBytes))
		{
			output.rejectionReason = "Deformation descriptor upload range overflowed.";
			return output;
		}
		configure(VulkanDeformationDescriptorBinding::Generations,
			VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::FrameUpload, generationBytes);
		configure(VulkanDeformationDescriptorBinding::Work,
			VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::FrameUpload, workBytes);
		configure(VulkanDeformationDescriptorBinding::StaticPageBytes,
			VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::ResidentInput, 16u);
		configure(VulkanDeformationDescriptorBinding::Palettes,
			VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::ResidentInput, 16u);
		configure(VulkanDeformationDescriptorBinding::MorphWeights,
			VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::ResidentInput, 16u);
		configure(VulkanDeformationDescriptorBinding::CurrentVertices,
			VulkanAnimationDescriptorAccess::ReadWrite,
			VulkanAnimationDescriptorSource::ComputeOutput,
			output.workingSet.committed.currentBytes);
		configure(VulkanDeformationDescriptorBinding::PreviousVertices,
			VulkanAnimationDescriptorAccess::ReadOnly,
			VulkanAnimationDescriptorSource::ComputeOutput,
			output.workingSet.committed.historyBytes);
		configure(VulkanDeformationDescriptorBinding::MeshletBounds,
			VulkanAnimationDescriptorAccess::ReadWrite,
			VulkanAnimationDescriptorSource::ComputeOutput,
			static_cast<std::uint64_t>(plan.totalMeshletBoundsCount)
				* sizeof(GpuDynamicBoundsRecord));
		configure(VulkanDeformationDescriptorBinding::ClusterBounds,
			VulkanAnimationDescriptorAccess::ReadWrite,
			VulkanAnimationDescriptorSource::ComputeOutput,
			static_cast<std::uint64_t>(plan.totalClusterBoundsCount)
				* sizeof(GpuDynamicBoundsRecord));
		configure(VulkanDeformationDescriptorBinding::Refinement,
			VulkanAnimationDescriptorAccess::ReadWrite,
			VulkanAnimationDescriptorSource::ComputeOutput,
			std::max<std::uint64_t>(16u,
				static_cast<std::uint64_t>(plan.totalClusterBoundsCount) * 16u));
		configure(VulkanDeformationDescriptorBinding::Diagnostics,
			VulkanAnimationDescriptorAccess::ReadWrite,
			VulkanAnimationDescriptorSource::ComputeOutput, 16u);
		output.valid = true;
		return output;
	}

}
