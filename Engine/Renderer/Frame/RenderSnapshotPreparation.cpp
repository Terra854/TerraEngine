// Typed snapshot and lane preparation; inputs are immutable frame-owned records.
#include "RenderFrameState.h"
#include "RenderSnapshotPreparation.h"
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "Profiling/ProfilerScope.h"
#include "File Formats/TerraEngineMeshFile.h"
#include <cstring>

namespace TerraEngine::Renderer::Frame {
	RenderItemRecord::PublishedAnimationGeneration
	BuildPublishedAnimationGeneration(
		const TerraEngine::Renderer::AnimationRenderBinding& source) noexcept
	{
		RenderItemRecord::PublishedAnimationGeneration result{};
		result.stableInstanceId = source.gpu.IsComplete()
			? source.gpu.stableInstanceId
			: (static_cast<std::uint64_t>(source.animatorSlotIndex) << 32U)
				| source.animatorGeneration;
		result.poseGeneration = source.posePublicationGeneration;
		result.morphGeneration = source.posePublicationGeneration;
		result.deformationContentIdLow = source.deformationContentIdLow;
		result.deformationContentIdHigh = source.deformationContentIdHigh;
		result.paletteGeneration = source.gpu.IsComplete()
			? source.gpu.paletteGeneration : source.posePublicationGeneration;
		result.deformablePayloadGeneration = source.deformablePayloadGeneration;
		result.programGeneration = source.gpu.programGeneration;
		result.animationPlanGeneration = source.gpu.planGeneration;
		result.rendererGeneration = source.gpu.IsComplete()
			? source.gpu.rendererGeneration : source.invalidation.renderer;
		result.firstGpuWorkPacket = source.gpu.workPacketIndex;
		result.gpuWorkPacketCount = source.gpu.IsComplete() ? 1U : 0U;
		result.gpuStageMask = source.gpu.stageMask;
		result.source = static_cast<std::uint8_t>(source.gpu.IsComplete()
			? (source.gpu.hybridCpuAuthority
				? TerraEngine::Renderer::AnimationRenderEvaluationSource::HybridAuthoritative
				: TerraEngine::Renderer::AnimationRenderEvaluationSource::GpuVisual)
			: (source.safePose
				? TerraEngine::Renderer::AnimationRenderEvaluationSource::SafeFallback
				: TerraEngine::Renderer::AnimationRenderEvaluationSource::CpuPose));
		result.complete = source.gpu.IsComplete();
		return result;
	}

	RenderViewCommandLane MakeRenderViewCommandLane(
		const RenderItemRecord& command) noexcept
	{
		RenderViewCommandLane lane{};
		lane.materialIndex = command.materialIndex;
		lane.meshDataIndex = command.meshDataIndex;
		lane.textureIndex = command.textureIndex;
		lane.virtualMeshletOffset = command.virtualMeshletOffset;
		lane.virtualMeshletCount = command.virtualMeshletCount;
		lane.virtualGeometryFlags = command.virtualGeometryFlags;
		lane.selectedVirtualClusterOffset = command.selectedVirtualClusterOffset;
		lane.selectedVirtualClusterCount = command.selectedVirtualClusterCount;
		lane.selectedVirtualPageNeedOffset = command.selectedVirtualPageNeedOffset;
		lane.selectedVirtualPageNeedCount = command.selectedVirtualPageNeedCount;
		lane.virtualGeometryPageId = command.virtualGeometryPageId;
		lane.virtualGeometryFallbackPageId = command.virtualGeometryFallbackPageId;
		lane.virtualGeometryPayloadSource = command.virtualGeometryPayloadSource;
		lane.virtualGeometryEmittedSource = command.virtualGeometryEmittedSource;
		lane.virtualGeometryPagePayloadByteOffset =
			command.virtualGeometryPagePayloadByteOffset;
		lane.virtualGeometryPagePayloadByteSize =
			command.virtualGeometryPagePayloadByteSize;
		lane.virtualGeometryPayloadBufferIndex =
			command.virtualGeometryPayloadBufferIndex;
		lane.virtualGeometryPayloadDescriptorIndex =
			command.virtualGeometryPayloadDescriptorIndex;
		lane.virtualGeometryPacketDescriptorIndex =
			command.virtualGeometryPacketDescriptorIndex;
		lane.taskShaderCullingEnabled = command.taskShaderCullingEnabled;
		return lane;
	}

	void ApplyRenderViewCommandLane(const RenderViewCommandLane& lane,
		std::uint32_t selectedLod, TerraEngine::Renderer::GeometryRoute route,
		const glm::vec4& editorTint, RenderItemRecord& command) noexcept
	{
		command.materialIndex = lane.materialIndex;
		command.meshDataIndex = lane.meshDataIndex;
		command.textureIndex = lane.textureIndex;
		command.virtualMeshletOffset = lane.virtualMeshletOffset;
		command.virtualMeshletCount = lane.virtualMeshletCount;
		command.selectedVirtualLod = selectedLod;
		command.geometryRoute = route;
		command.editorTint = editorTint;
		command.virtualGeometryFlags = lane.virtualGeometryFlags;
		command.selectedVirtualClusterOffset = lane.selectedVirtualClusterOffset;
		command.selectedVirtualClusterCount = lane.selectedVirtualClusterCount;
		command.selectedVirtualPageNeedOffset = lane.selectedVirtualPageNeedOffset;
		command.selectedVirtualPageNeedCount = lane.selectedVirtualPageNeedCount;
		command.virtualGeometryPageId = lane.virtualGeometryPageId;
		command.virtualGeometryFallbackPageId = lane.virtualGeometryFallbackPageId;
		command.virtualGeometryPayloadSource = lane.virtualGeometryPayloadSource;
		command.virtualGeometryEmittedSource = lane.virtualGeometryEmittedSource;
		command.virtualGeometryPagePayloadByteOffset =
			lane.virtualGeometryPagePayloadByteOffset;
		command.virtualGeometryPagePayloadByteSize =
			lane.virtualGeometryPagePayloadByteSize;
		command.virtualGeometryPayloadBufferIndex =
			lane.virtualGeometryPayloadBufferIndex;
		command.virtualGeometryPayloadDescriptorIndex =
			lane.virtualGeometryPayloadDescriptorIndex;
		command.virtualGeometryPacketDescriptorIndex =
			lane.virtualGeometryPacketDescriptorIndex;
		command.taskShaderCullingEnabled = lane.taskShaderCullingEnabled;
	}
}

namespace TerraEngine::Renderer::Frame {
	std::uint64_t HashFloat(float value)
	{
		std::uint32_t bits = 0u;
		std::memcpy(&bits, &value, sizeof(bits));
		return static_cast<std::uint64_t>(bits);
	}

	void HashCombine(std::uint64_t& seed, std::uint64_t value)
	{
		seed ^= value;
		seed *= kRenderHashPrime;
	}

	void HashVec3(std::uint64_t& seed, const glm::vec3& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
	}

	void HashVec4(std::uint64_t& seed, const glm::vec4& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
		HashCombine(seed, HashFloat(value.w));
	}

	void HashMat4(std::uint64_t& seed, const glm::mat4& value)
	{
		for (int column = 0; column < 4; ++column)
		{
			HashVec4(seed, value[column]);
		}
	}

	void HashUVec2(std::uint64_t& seed, const glm::uvec2& value)
	{
		HashCombine(seed, static_cast<std::uint64_t>(value.x));
		HashCombine(seed, static_cast<std::uint64_t>(value.y));
	}

	void HashQuat(std::uint64_t& seed, const glm::quat& value)
	{
		HashCombine(seed, HashFloat(value.x));
		HashCombine(seed, HashFloat(value.y));
		HashCombine(seed, HashFloat(value.z));
		HashCombine(seed, HashFloat(value.w));
	}

	std::uint64_t BuildRenderSettingsSignature(const RenderSettings& settings, QualityPreset preset, bool taskShaderCullingEnabled)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(preset));
		HashCombine(seed, taskShaderCullingEnabled ? 1u : 0u);
		HashCombine(seed, settings.shadowResolution);
		HashCombine(seed, settings.shadowFilteringSize);
		HashCombine(seed, settings.maxUploadedLights);
		HashCombine(seed, settings.maxDynamicLights);
		HashCombine(seed, settings.maxShadowLights);
		HashCombine(seed, settings.maxAreaLights);
		HashCombine(seed, settings.enableLTC);
		HashCombine(seed, settings.enableIBL);
		HashCombine(seed, HashFloat(settings.indirectStrength));
		HashCombine(seed, HashFloat(settings.ambientOcclusionStrength));
		HashCombine(seed, settings.ambientOcclusionQuality);
		HashCombine(seed, settings.anisotropicFiltering);
		HashCombine(seed, settings.variableRateShading);
		// Virtual-geometry debug and quality settings affect the selected meshlet
		// range stored in cached draw commands. Include them in the cache key so
		// editor dropdown changes are visible immediately, without waiting for a
		// camera move to invalidate the view-projection portion of the key.
		HashCombine(seed, BuildVirtualGeometryQualityProfileSignature(settings.virtualGeometry));
		return seed;
	}
}

namespace TerraEngine::Renderer::Frame {
	bool FitsCanonicalDirectoryRange(
		const std::vector<std::uint8_t>& storage,
		std::uint64_t offset,
		std::uint32_t count,
		std::size_t elementSize) noexcept
	{
		// The production loader owns full validation.  This second bounded check
		// protects snapshot preparation if a synthetic Mesh reaches this layer.
		if(elementSize == 0u || offset > storage.size()) return false;
		const std::size_t begin = static_cast<std::size_t>(offset);
		return count <= (storage.size() - begin) / elementSize;
	}

	CanonicalGeometryRouteFacts BuildCanonicalGeometryRouteFacts(const Mesh& mesh)
	{
		CanonicalGeometryRouteFacts facts{};
		const bool generatedDirect = mesh.generatedDirectMeshletRoute.IsComplete();
		bool declaresDirect = generatedDirect;
		bool declaresVirtual = false;
		std::uint32_t logicalPageSizeBytes = 0u;
		bool directResidentDescriptors = true;
		if(generatedDirect) {
			// A procedural producer has no serialized TEMF directory. Its typed
			// publication receipt supplies only immutable identity and resident-byte
			// facts; the stream/range checks below still gate route eligibility.
			facts.representationFlags = kTemfRepresentationDirectMeshlets;
			facts.storedBytes = mesh.generatedDirectMeshletRoute.residentPayloadBytes;
			facts.decodedBytes = mesh.generatedDirectMeshletRoute.residentPayloadBytes;
			facts.calibrationIdentity =
				mesh.generatedDirectMeshletRoute.sourceContentIdentity
				^ mesh.generatedDirectMeshletRoute.generationIdentity;
			if(facts.calibrationIdentity == 0u) facts.calibrationIdentity = 1u;
		}
		else {
			if(mesh.storage.size() < sizeof(TemfProductionHeader)) return facts;
			TemfProductionHeader header{};
			std::memcpy(&header, mesh.storage.data(), sizeof(header));
			if(header.magic != kMeshMagicValue
				|| header.formatVersion != kMeshCurrentVersion
				|| header.minorVersion != kMeshCurrentMinorVersion
				|| header.headerBytes != kTemfProductionHeaderBytes
				|| header.formatContractId != kTemfProductionContractId
				|| (header.representationFlags & ~kTemfRepresentationKnownMask) != 0u
				|| !FitsCanonicalDirectoryRange(mesh.storage, header.blockTableOffset,
					header.blockCount, sizeof(TemfBlockDescriptor))
				|| !FitsCanonicalDirectoryRange(mesh.storage, header.pageTableOffset,
					header.pageCount, sizeof(TemfPageDescriptor))) {
				return facts;
			}
			declaresDirect =
				(header.representationFlags & kTemfRepresentationDirectMeshlets) != 0u;
			declaresVirtual =
				(header.representationFlags & kTemfRepresentationVirtualGeometry) != 0u;
			if(!declaresDirect && !declaresVirtual) return facts;

			for(std::uint32_t index = 0u; index < header.blockCount; ++index) {
				TemfBlockDescriptor block{};
				const std::size_t offset = static_cast<std::size_t>(header.blockTableOffset)
					+ static_cast<std::size_t>(index) * sizeof(block);
				std::memcpy(&block, mesh.storage.data() + offset, sizeof(block));
				if(block.encodedBytes == 0u || block.decodedBytes == 0u
					|| block.encodedBytes > std::numeric_limits<std::uint64_t>::max()
						- facts.storedBytes
					|| block.decodedBytes > std::numeric_limits<std::uint64_t>::max()
						- facts.decodedBytes) {
					return {};
				}
				facts.storedBytes += block.encodedBytes;
				facts.decodedBytes += block.decodedBytes;
				directResidentDescriptors = directResidentDescriptors
					&& (block.flags & kTemfBlockFlagDirectResident) != 0u
					&& block.logicalPageId == kTemfDirectResidentPageId;
			}
			facts.representationFlags = header.representationFlags;
			facts.physicalPageCount = header.pageCount;
			facts.physicalBlockCount = header.blockCount;
			logicalPageSizeBytes = header.logicalPageSizeBytes;
			facts.calibrationIdentity = header.buildRecipeHash
				^ header.generationIdentity
				^ (static_cast<std::uint64_t>(header.selectedCodecProfileId) << 32u)
				^ header.selectedQuantizationProfileId;
		}

		std::uint64_t primitiveCount = 0u;
		std::uint64_t generatedVertexOffset = 0u;
		std::uint64_t generatedPrimitiveOffset = 0u;
		for(const MeshletRecord& record : mesh.meshletRecords) {
			if(record.primitiveIndexCount == 0u
				|| record.primitiveIndexCount % 3u != 0u
				|| primitiveCount > std::numeric_limits<std::uint64_t>::max()
					- record.primitiveIndexCount
				|| (generatedDirect
					&& (record.vertexOffset != generatedVertexOffset
						|| record.primitiveIndexOffset != generatedPrimitiveOffset))) {
				return {};
			}
			primitiveCount += record.primitiveIndexCount;
			generatedVertexOffset += record.vertexCount;
			generatedPrimitiveOffset += record.primitiveIndexCount;
		}
		if(mesh.runtimeLayout.meshletCount == 0u
			|| mesh.runtimeLayout.meshletCount != mesh.meshletRecords.size()
			|| mesh.runtimeLayout.meshletPrimitiveIndexCount != primitiveCount
			|| mesh.meshletPrimitiveIndexData.size() != primitiveCount) {
			return {};
		}
		if(generatedDirect) {
			const std::size_t remapStride = mesh.runtimeLayout.meshletVertexRemapEncoding
				== static_cast<std::uint32_t>(MeshletRemapEncoding::Uint16)
				? sizeof(std::uint16_t)
				: mesh.runtimeLayout.meshletVertexRemapEncoding
					== static_cast<std::uint32_t>(MeshletRemapEncoding::Uint32)
					? sizeof(std::uint32_t) : 0u;
			const std::size_t expectedVertexBytes =
				static_cast<std::size_t>(mesh.runtimeLayout.meshletVertexCount)
				* mesh.runtimeLayout.layout.strideBytes;
			const std::size_t expectedRemapBytes =
				static_cast<std::size_t>(mesh.runtimeLayout.meshletVertexRemapCount)
				* remapStride;
			if(mesh.runtimeLayout.layout.strideBytes != kMeshletPackedVertexStrideBytes
				|| remapStride == 0u
				|| mesh.meshletVertexData.size() != expectedVertexBytes
				|| mesh.meshletVertexRemapData.size() != expectedRemapBytes
				|| mesh.meshletVertexRemapDecoded.size()
					!= mesh.runtimeLayout.meshletVertexRemapCount
				|| generatedVertexOffset != mesh.runtimeLayout.meshletVertexRemapCount
				|| mesh.runtimeLayout.meshletSectionCount != mesh.meshletSections.size()
				|| mesh.runtimeLayout.meshletBoundsCount != mesh.meshletBounds.size()
				|| mesh.meshletBounds.size() != mesh.meshletRecords.size()) {
				return {};
			}
			for(const MeshletSection& section : mesh.meshletSections) {
				const std::uint64_t sectionEnd =
					static_cast<std::uint64_t>(section.meshletOffset) + section.meshletCount;
				if(section.meshletCount == 0u
					|| sectionEnd > mesh.runtimeLayout.meshletCount) return {};
			}
		}

		facts.complete = true;
		// The direct payload may contain several resident LOD ranges. Route cost and
		// draw batching describe one selected range, not the sum of every alternative
		// detail packed beside it.
		const VirtualGeometryDetailLevelRecord* baseDetail =
			mesh.virtualGeometryDetailLevels.empty()
				? nullptr
				: &mesh.virtualGeometryDetailLevels.front();
		facts.packedMeshletCount = !declaresVirtual && baseDetail != nullptr
			? baseDetail->meshletCount
			: mesh.runtimeLayout.meshletCount;
		facts.sourceTriangleCount = baseDetail != nullptr
			? baseDetail->primitiveCount
			: static_cast<std::uint32_t>(primitiveCount / 3u);
		// Direct-only TEMFPRD1 cooks prove that every descriptor is resident;
		// hybrid cooks can share decoded runtime streams without that restriction.
		// Generated meshes instead carry the typed resident receipt validated above.
		facts.directUsable = declaresDirect
			&& (generatedDirect || declaresVirtual || directResidentDescriptors);
		facts.virtualGeometryUsable = declaresVirtual
			&& logicalPageSizeBytes != 0u
			&& facts.physicalPageCount != 0u
			&& mesh.hasVirtualGeometry
			&& !mesh.virtualGeometryClusters.empty();
		return facts;
	}

	VirtualGeometryAssetRuntime BuildVirtualGeometryRuntimeFromMesh(const TerraEngine::UUID& meshId, const Mesh& mesh)
	{
		VirtualGeometryAssetRuntime runtime{};
		const TerraEngine::Resource::VirtualGeometryPhysicalPageResidencySnapshot residency =
			TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance().ResidencySnapshot();
		const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication> publications =
			TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance().PublishedPages();
		runtime.assetId = meshId;
		runtime.cookStatus = mesh.hasVirtualGeometry ? VirtualGeometryCookStatus::Cooked : VirtualGeometryCookStatus::NeedsRebuild;
		runtime.sourceTriangleCount = mesh.hasVirtualGeometry
			? mesh.virtualGeometryHeader.sourceTriangleCount
			: (mesh.virtualGeometryDetailLevels.empty()
				? 0u : mesh.virtualGeometryDetailLevels.front().primitiveCount);
		runtime.sourceVertexCount = mesh.hasVirtualGeometry
			? mesh.virtualGeometryHeader.sourceVertexCount
			: (mesh.virtualGeometryDetailLevels.empty()
				? 0u : mesh.virtualGeometryDetailLevels.front().vertexCount);
		runtime.clusterCoverageValidated =
			(mesh.virtualGeometryHeader.flags & kVirtualGeometryValidationFlagClusterCoverageValidated) != 0u;

		// Convert the loader-owned TEMF tables into a compact runtime mirror.
		// Render preparation copies this mirror into immutable frame snapshots,
		// so workers can choose LOD/page ranges without touching ResourceManager
		// caches or live scene/component storage.
		runtime.requestedTargets.reserve(mesh.virtualGeometryRequestedTargets.size());
		for (const VirtualGeometryRequestedTargetRecord& target : mesh.virtualGeometryRequestedTargets)
		{
			VirtualGeometryRequestedTargetRuntime record{};
			record.targetIndex = target.targetIndex;
			record.requestedRatio = target.requestedRatio;
			record.desiredTriangleCount = target.desiredTriangleCount;
			record.desiredMeshletCount = target.desiredMeshletCount;
			record.status = static_cast<VirtualGeometryDetailTargetStatus>(target.status);
			runtime.requestedTargets.push_back(record);
		}
		runtime.detailLevels.reserve(mesh.virtualGeometryDetailLevels.size());
		for (const VirtualGeometryDetailLevelRecord& detail : mesh.virtualGeometryDetailLevels)
		{
			VirtualGeometryDetailRuntime record{};
			record.lodIndex = detail.lodIndex;
			record.meshletOffset = detail.meshletOffset;
			record.meshletCount = detail.meshletCount;
			record.triangleCount = detail.primitiveCount;
			record.geometricError = detail.geometricError;
			record.targetRatio = detail.targetRatio;
			record.boundsMin = glm::vec3(detail.boundsMin[0], detail.boundsMin[1], detail.boundsMin[2]);
			record.boundsMax = glm::vec3(detail.boundsMax[0], detail.boundsMax[1], detail.boundsMax[2]);
			record.requestedTargetIndex = detail.requestedTargetIndex;
			record.targetStatus = static_cast<VirtualGeometryDetailTargetStatus>(detail.targetStatus);
			runtime.detailLevels.push_back(record);

			VirtualGeometryGeneratedLevelRuntime generated{};
			generated.generatedLevelIndex = static_cast<std::uint32_t>(runtime.generatedLevels.size());
			generated.requestedTargetIndex = detail.requestedTargetIndex;
			generated.lodIndex = detail.lodIndex;
			generated.actualRatio = detail.targetRatio;
			generated.triangleCount = detail.primitiveCount;
			generated.meshletCount = detail.meshletCount;
			generated.geometricError = detail.geometricError;
			runtime.generatedLevels.push_back(generated);
		}
		runtime.skippedDetails.reserve(mesh.virtualGeometrySkippedDetails.size());
		for (const VirtualGeometrySkippedDetailRecord& skipped : mesh.virtualGeometrySkippedDetails)
		{
			VirtualGeometrySkippedDetailRuntime record{};
			record.requestedTargetIndex = skipped.targetIndex;
			record.requestedRatio = skipped.requestedRatio;
			record.status = static_cast<VirtualGeometryDetailTargetStatus>(skipped.status);
			record.reason = static_cast<VirtualGeometrySkippedDetailReason>(skipped.reason);
			record.nearestGeneratedLevelIndex = skipped.nearestGeneratedLodIndex;
			record.candidateTriangleCount = skipped.candidateTriangleCount;
			record.candidateMeshletCount = skipped.candidateMeshletCount;
			record.actualCandidateRatio = skipped.actualCandidateRatio;
			if (skipped.messageStringSize > 0u
				&& skipped.messageStringOffset + skipped.messageStringSize <= mesh.virtualGeometryDiagnosticStrings.size())
			{
				record.message = mesh.virtualGeometryDiagnosticStrings.substr(skipped.messageStringOffset, skipped.messageStringSize);
			}
			runtime.skippedDetails.push_back(std::move(record));
		}
		runtime.cullableGroups.reserve(mesh.virtualGeometryCullableGroups.size());
		for (const VirtualGeometryCullableGroupRecord& group : mesh.virtualGeometryCullableGroups)
		{
			const std::uint32_t groupOrdinal = static_cast<std::uint32_t>(runtime.cullableGroups.size());
			VirtualGeometryCullableGroupRuntime record{};
			record.groupIndex = group.groupIndex;
			record.lodIndex = group.lodIndex;
			// TEMF v1 stores one cullable group per meshlet. The group index is
			// therefore the meshlet table coordinate the task shader needs; the
			// vertex offset points into the remap stream and must never be used as
			// a meshlet dispatch offset.
			record.meshletIndex = group.groupIndex < mesh.meshletRecords.size() ? group.groupIndex : groupOrdinal;
			record.sectionId = group.sectionId;
			record.pageId = group.pageId;
			record.boundsCenter = glm::vec3(group.boundsCenter[0], group.boundsCenter[1], group.boundsCenter[2]);
			record.boundsRadius = group.boundsRadius;
			runtime.cullableGroups.push_back(record);
		}
		runtime.clusters.reserve(mesh.virtualGeometryClusters.size());
		for (const VirtualGeometryClusterRecord& cluster : mesh.virtualGeometryClusters)
		{
			VirtualGeometryClusterRuntime record{};
			record.clusterId = cluster.clusterId;
			record.parentClusterId = cluster.parentClusterId;
			record.firstChildClusterId = cluster.firstChildClusterId;
			record.childCount = cluster.childCount;
			record.lodIndex = cluster.lodIndex;
			record.firstGroupIndex = cluster.groupOffset;
			record.groupCount = cluster.groupCount;
			record.materialSectionId = cluster.materialSectionId;
			record.pageId = cluster.pageId;
			record.fallbackClusterId = cluster.fallbackClusterId;
			record.geometricError = cluster.geometricError;
			record.boundsCenter = glm::vec3(cluster.boundsCenter[0], cluster.boundsCenter[1], cluster.boundsCenter[2]);
			record.boundsRadius = cluster.boundsRadius;
			runtime.clusters.push_back(record);
		}
		// TEMFPRD1 has no aggregate-profile sidecar. Fallback coverage is proven
		// directly by the canonical cluster/page tables copied below; leaving this
		// vector empty deliberately routes missing pages through that typed chain
		// rather than reviving retired VGAP provenance records.
		runtime.coneSafety.reserve(mesh.virtualGeometryConeSafety.size());
		for (const VirtualGeometryConeSafetyRecord& cone : mesh.virtualGeometryConeSafety)
		{
			ConeSafetyMetadata metadata{};
			metadata.meshId = meshId;
			metadata.clusterId = cone.clusterId;
			metadata.materialSidedness = static_cast<VirtualGeometryConeMaterialSidedness>(cone.materialSidedness);
			metadata.windingState = static_cast<VirtualGeometryConeWindingState>(cone.windingState);
			metadata.alphaRisk = static_cast<VirtualGeometryConeAlphaRisk>(cone.alphaRisk);
			metadata.thinShellRisk = static_cast<VirtualGeometryConeThinShellRisk>(cone.thinShellRisk);
			metadata.coneValid = (cone.flags & kVirtualGeometryConeSafetyFlagConeValid) != 0u;
			std::string cookedDisabledReason;
			if (cone.disabledReasonStringSize > 0u
				&& cone.disabledReasonStringOffset + cone.disabledReasonStringSize <= mesh.virtualGeometryDiagnosticStrings.size())
			{
				cookedDisabledReason = mesh.virtualGeometryDiagnosticStrings.substr(cone.disabledReasonStringOffset, cone.disabledReasonStringSize);
			}
			metadata = NormalizeVirtualGeometryConeSafetyMetadata(std::move(metadata));
			if (!metadata.eligible && !cookedDisabledReason.empty())
			{
				metadata.disabledReason = std::move(cookedDisabledReason);
			}
			runtime.coneSafety.push_back(std::move(metadata));
		}
		runtime.pages.reserve(mesh.virtualGeometryPages.size());
		for (const VirtualGeometryPageRecord& page : mesh.virtualGeometryPages)
		{
			GeometryPageRuntime record{};
			record.pageId = page.pageId;
			record.firstLodIndex = page.firstLodIndex;
			record.lodCount = page.lodCount;
			record.groupOffset = page.groupOffset;
			record.groupCount = page.groupCount;
			record.byteSize = page.byteSize;
			record.priority = static_cast<float>(page.priority);
			record.fallbackPageId = page.fallbackPageId;
			record.fallbackPage = page.pageId == page.fallbackPageId;
			record.clusterOffset = page.clusterPageLinkOffset;
			record.clusterCount = page.clusterPageLinkCount;
				record.payloadByteOffset = page.byteOffset;
				record.payloadByteSize = page.byteSize;
				record.payloadBufferIndex = 0xffffffffu;
				record.payloadDescriptorIndex = 0xffffffffu;
				record.publicationGeneration = mesh.virtualGeometryPhysicalPagePublicationGeneration;
				if (page.pageId < mesh.virtualGeometryPhysicalPageStates.size()
					&& page.pageId < mesh.virtualGeometryPhysicalPagePublicationGenerations.size())
				{
					record.state = static_cast<GeometryPageState>(
						mesh.virtualGeometryPhysicalPageStates[page.pageId]);
					record.publicationGeneration =
						mesh.virtualGeometryPhysicalPagePublicationGenerations[page.pageId];
				}
				const auto streamed = std::find_if(residency.pages.begin(), residency.pages.end(),
					[meshId, pageId = page.pageId](const auto& row) { return row.assetId == meshId && row.pageId == pageId; });
				if(streamed != residency.pages.end()) {
					switch(streamed->state) {
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::Resident: record.state = GeometryPageState::Resident; break;
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::PendingIO: record.state = GeometryPageState::PendingIO; break;
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::PendingDecode: record.state = GeometryPageState::PendingIO; break;
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::PendingUpload: record.state = GeometryPageState::PendingUpload; break;
					case TerraEngine::Resource::VirtualGeometryPagePublicationState::Failed: record.state = GeometryPageState::Failed; break;
					default: break;
					}
					record.publicationGeneration = streamed->residencyGeneration;
				}
				const auto publication = std::find_if(
					publications.begin(),
					publications.end(),
					[meshId, pageId = page.pageId](const auto& value)
					{
						return value.key.asset.assetId == meshId
							&& value.key.pageId == pageId
							&& value.IsVisible();
					});
				if (publication != publications.end())
				{
					record.state = GeometryPageState::Resident;
					record.payloadByteOffset = 0u;
					record.payloadByteSize = publication->payload.decodedByteCount;
					record.byteSize = publication->payload.decodedByteCount;
					record.payloadBufferIndex = publication->descriptor.bufferIndex;
					record.payloadDescriptorIndex = publication->descriptor.descriptorIndex;
					record.allocationId = publication->descriptor.allocationId;
					record.meshPayloadGeneration =
						publication->key.generations.geometryFormatGeneration;
					record.pageContentGeneration =
						publication->key.pageContentGeneration;
					record.residencyGeneration =
						publication->key.generations.residencyGeneration;
					record.publicationGeneration =
						publication->publicationGeneration;
				}
				// Missing publication data is intentionally fail-closed: the cluster
				// resolver will request this page and walk its resident ancestors.
				record.pinnedFallback = record.fallbackPage
					&& record.state == GeometryPageState::Pinned;
				record.protectedFallback = record.pinnedFallback;
				record.eligibleForNewWork = record.state == GeometryPageState::NotResident
					|| record.state == GeometryPageState::Requested
					|| record.state == GeometryPageState::PendingIO
					|| record.state == GeometryPageState::PendingUpload;
			runtime.pages.push_back(record);
		}
		return runtime;
	}
	// Keep derived-table capacity proportional to resource diversity, never instance count.
	FrameGeometryInputs::FrameGeometryInputs(std::size_t uniqueResourceCount)
		: expectedUniqueMeshCount_(std::min<std::size_t>(64U, std::max<std::size_t>(1U, uniqueResourceCount)))
	{
		virtualGeometryRuntimeByMesh.reserve(expectedUniqueMeshCount_);
		geometryRouteFactsByMesh.reserve(expectedUniqueMeshCount_);
	}
	const VirtualGeometryAssetRuntime& FrameGeometryInputs::Runtime(const TerraEngine::UUID& meshId, const Mesh& mesh)
	{
		const auto existing = virtualGeometryRuntimeByMesh.find(meshId);
		if(existing != virtualGeometryRuntimeByMesh.end()) return existing->second;
		auto [inserted, wasInserted] = virtualGeometryRuntimeByMesh.emplace(meshId, BuildVirtualGeometryRuntimeFromMesh(meshId, mesh));
		(void)wasInserted;
		return inserted->second;
	}
	const CanonicalGeometryRouteFacts& FrameGeometryInputs::Facts(const TerraEngine::UUID& meshId, const Mesh& mesh)
	{
		const auto existing = geometryRouteFactsByMesh.find(meshId);
		if(existing != geometryRouteFactsByMesh.end()) return existing->second;
		auto [inserted, wasInserted] = geometryRouteFactsByMesh.emplace(meshId, BuildCanonicalGeometryRouteFacts(mesh));
		(void)wasInserted;
		return inserted->second;
	}
}

namespace TerraEngine::Renderer::Frame {
	// Copy detailed route evidence only for an admitted diagnostics consumer.
	GeometryRouteDecision CopyRouteDecisionForFrame(const GeometryRouteDecision& source, bool retainDetailedRouteCandidates)
	{
		TerraEngine::Renderer::GeometryRouteDecision destination;
		destination.decisionIdentity = source.decisionIdentity;
		destination.frameId = source.frameId;
		destination.viewFamilyId = source.viewFamilyId;
		destination.objectIdentity = source.objectIdentity;
		destination.meshIdentity = source.meshIdentity;
		destination.assetGeneration = source.assetGeneration;
		destination.selected = source.selected;
		if(retainDetailedRouteCandidates) {
			// Candidate rejection strings are valuable only to an active detailed
			// consumer. Avoid two nested-vector copies per instance in ordinary play.
			destination.candidates = source.candidates;
		}
		destination.policyIdentity = source.policyIdentity;
		destination.calibrationIdentity = source.calibrationIdentity;
		destination.source = source.source;
		destination.estimatedCost = source.estimatedCost;
		destination.measuredCost = source.measuredCost;
		destination.confidenceMargin = source.confidenceMargin;
		destination.stableFrameCount = source.stableFrameCount;
		destination.fallbackReason = source.fallbackReason;
		destination.fallbackDiagnostic = source.fallbackDiagnostic;
		destination.routeGeneration = source.routeGeneration;
		destination.deterministicHash = source.deterministicHash;
		destination.nextState = source.nextState;
		return destination;

}
}

// Assemble deterministic snapshot rows from captured inputs and uniquely cached geometry facts.
void RenderSystem::AssembleFrameSnapshotRows(std::span<RenderableEntry> renderables,
	TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	TerraEngine::Renderer::Frame::FrameGeometryInputs& geometryInputs,
	const FrameSnapshotRouteView& inputs, const FrameSnapshotShadowView& snapshotShadowInputs,
	const FrameSceneRevisions& sceneRevisions, const FrameWorldTransforms& worldCapture,
	TerraEngine::Renderer::Frame::GeometryRouteStatsAggregate& geometryRouteStats,
	std::uint64_t& geometryRouteSelectionSignature, std::size_t expectedUniqueMeshCount)
{
	const auto snapshotRendererGeneration = inputs.snapshotRendererGeneration;
	const auto hardwareVirtualGeometryRouteAvailable = inputs.hardwareVirtualGeometryRouteAvailable;
	const auto computeVirtualGeometryRouteAvailable = inputs.computeVirtualGeometryRouteAvailable;
	const auto& validationRouteOverride = inputs.validationRouteOverride;
	const auto& immutableGpuAnimationPlan = inputs.immutableGpuAnimationPlan;

	TERRAENGINE_PROFILE_SCOPE("RenderSystem: Assemble Render Snapshot",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	std::unordered_map<TerraEngine::UUID,
		std::array<std::optional<TerraEngine::Renderer::VirtualGeometryDrawRange>, 2U>>
		virtualDrawRangeByMesh;
	virtualDrawRangeByMesh.reserve(expectedUniqueMeshCount);
	// Route scoring depends on immutable mesh/capability/calibration facts, not on
	// an instance transform. Evaluate hysteresis once per unique mesh per frame,
	// then stamp the copied decision with each object's diagnostic identity.
	struct CachedGeometryRouteDecision
	{
		// Route scoring and mesh hashing depend only on the immutable mesh facts.
		// Keeping both values together lets every repeated instance copy the result
		// without rebuilding costs or rehashing the shared mesh UUID.
		TerraEngine::Renderer::GeometryRouteDecision decision{};
		std::uint64_t meshIdentity = 0U;
	};
	std::unordered_map<TerraEngine::UUID, CachedGeometryRouteDecision>
		geometryRouteDecisionByMesh;
	geometryRouteDecisionByMesh.reserve(expectedUniqueMeshCount);
	renderFrameSnapshot.renderables.reserve(renderables.size());
	// Meshes and materials are resource tables, not draw tables. Track their exact
	// identities once per frame so instancing does not create, sort, hash, and copy
	// thousands of duplicate backend resource records.
	std::unordered_set<TerraEngine::UUID> snapshotMeshIds;
	std::unordered_set<TerraEngine::UUID> snapshotMaterialIds;
	snapshotMeshIds.reserve(expectedUniqueMeshCount);
	snapshotMaterialIds.reserve(expectedUniqueMeshCount);
	renderFrameSnapshot.meshes.reserve(expectedUniqueMeshCount);
	renderFrameSnapshot.materials.reserve(expectedUniqueMeshCount);
	const bool retainDetailedRouteCandidates =
		TerraEngine::Renderer::RenderDiagnosticsRequested();
	if(retainDetailedRouteCandidates)
	{
		// Full route decisions contain candidate vectors and diagnostic strings.
		// Keep them only while an explicit diagnostics consumer is active; ordinary
		// rendering stores the compact per-instance selection record below.
		renderFrameSnapshot.geometryRouteDecisions.reserve(renderables.size());
	}

	const CanonicalGeometryRouteFacts emptyRouteFacts{};
	TerraEngine::UUID preparedMeshId = TerraEngine::EMPTY_UUID;
	const Mesh* preparedMesh = nullptr;
	const CanonicalGeometryRouteFacts* preparedRouteFacts = &emptyRouteFacts;
	std::array<std::optional<TerraEngine::Renderer::VirtualGeometryDrawRange>, 2U>*
		preparedVirtualRanges = nullptr;
	CachedGeometryRouteDecision* preparedRouteDecision = nullptr;
	TerraEngine::UUID lastSnapshotMeshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID lastSnapshotMaterialId = TerraEngine::EMPTY_UUID;
	RefreshFrameRenderablePoses(std::span<const RenderableEntry>(renderables), sceneRevisions, worldCapture);
	for (std::size_t renderableIndex = 0U;
		renderableIndex < renderables.size(); ++renderableIndex)
	{
		RenderableEntry& entry = renderables[renderableIndex];
		if(preparedMeshId != entry.mesh->meshId) {
			// Most crowds contain long runs of one mesh. Retain only the immediately
			// preceding lookup results so identical adjacent instances avoid four
			// unordered-map probes while mixed scenes preserve the general cache path.
			preparedMeshId = entry.mesh->meshId;
			preparedMesh = entry.runtimeMesh;
			preparedVirtualRanges = &virtualDrawRangeByMesh[entry.mesh->meshId];
			preparedRouteFacts = preparedMesh != nullptr
				? &geometryInputs.Facts(entry.mesh->meshId, *preparedMesh)
				: &emptyRouteFacts;
			const auto existingRoute = geometryRouteDecisionByMesh.find(
				entry.mesh->meshId);
			preparedRouteDecision = existingRoute != geometryRouteDecisionByMesh.end()
				? &existingRoute->second : nullptr;
		}
		TerraEngine::Renderer::VirtualGeometryDrawRange virtualRange{};
		const Mesh* mesh = preparedMesh;
		if (mesh != nullptr && mesh->hasVirtualGeometry)
		{
			const std::size_t cullingIndex = entry.mesh->taskShaderCullingEnabled ? 1U : 0U;
			auto& cachedRanges = *preparedVirtualRanges;
			if(cachedRanges[cullingIndex].has_value()) {
				virtualRange = *cachedRanges[cullingIndex];
			} else {
				const VirtualGeometryAssetRuntime& runtime =
					geometryInputs.Runtime(entry.mesh->meshId, *mesh);
				virtualRange = SelectVirtualGeometryResidentLodRange(
					runtime,
					renderFrameSnapshot.renderSettings.virtualGeometry,
					512.0f,
					entry.mesh->taskShaderCullingEnabled,
					!mesh->meshletBounds.empty());
				cachedRanges[cullingIndex] = virtualRange;
			}
		}
		renderFrameSnapshot.renderables.push_back({
			entry.objectId,
			entry.mesh->meshId,
			entry.mesh->materialId,
			glm::mat4(1.0F),
			virtualRange,
			entry.mesh->taskShaderCullingEnabled,
			false
		});
		TerraEngine::Renderer::RenderFrameRenderableRecord& snapshotRenderable =
			renderFrameSnapshot.renderables.back();
		snapshotRenderable.canonicalWorldPose =
			renderableWorldPoseCache[renderableIndex];
		snapshotRenderable.transformGeneration = entry.transformGeneration;
		// Static meshes do not carry an animation binding, but they still need a
		// complete immutable resource lifetime before persistent topology can publish
		// their direct draw selectors. Animated bindings below replace these broad
		// frame baselines with their more specific mesh/palette generations.
		snapshotRenderable.invalidation.asset = std::max<std::uint64_t>(1u,
			entry.meshIdentity);
		snapshotRenderable.invalidation.material = std::max<std::uint64_t>(1u,
			entry.materialIdentity);
		snapshotRenderable.invalidation.renderer = std::max<std::uint64_t>(1u,
			snapshotRendererGeneration);
		snapshotRenderable.invalidation.residency = std::max<std::uint64_t>(1u,
			renderFrameSnapshot.resourceRevision);
		// Copy representation facts into the frame before view-specific work. A
		// direct route clears its VG span, preventing hierarchy/page/executor work
		// while leaving conventional mesh-task submission and resource uploads intact.
		const CanonicalGeometryRouteFacts& routeFacts = *preparedRouteFacts;
		const std::uint64_t routeObjectIdentity = entry.objectIdentity;
		if(preparedRouteDecision == nullptr) {
			TerraEngine::Renderer::GeometryRoutePolicyInput routeInput{};
			// Route costs are calibrated from canonical topology, physical directory,
			// and decoded-byte facts. Optional meshlet bounds deliberately contribute
			// nothing: they are a culling acceleration structure, not geometry truth.
			// Repeated instances share these immutable facts, so calculate them once
			// for the mesh instead of once for every object in a crowd.
			const float sourceTriangleWork =
				static_cast<float>(routeFacts.sourceTriangleCount);
			const float meshletWork = static_cast<float>(routeFacts.packedMeshletCount);
			const float decodedKiB = static_cast<float>(routeFacts.decodedBytes) / 1024.0F;
			const float directCost = sourceTriangleWork + meshletWork * 8.0F
				+ decodedKiB * 0.125F;
			const float virtualDirectoryWork =
				static_cast<float>(routeFacts.physicalPageCount) * 24.0F
				+ static_cast<float>(routeFacts.physicalBlockCount) * 3.0F;
			routeInput.direct.valid = routeFacts.directUsable;
			routeInput.direct.estimatedCost = routeFacts.directUsable
				? std::max(1.0F, directCost) : 0.0F;
			routeInput.virtualGeometryHardware.valid = routeFacts.virtualGeometryUsable;
			routeInput.virtualGeometryHardware.capabilitiesAvailable =
				hardwareVirtualGeometryRouteAvailable;
			routeInput.virtualGeometryHardware.estimatedCost = routeFacts.virtualGeometryUsable
				? std::max(1.0F, sourceTriangleWork * 0.45F + meshletWork * 2.0F
					+ decodedKiB * 0.050F + virtualDirectoryWork)
				: 0.0F;
			routeInput.virtualGeometryCompute.valid = routeFacts.virtualGeometryUsable;
			routeInput.virtualGeometryCompute.capabilitiesAvailable =
				computeVirtualGeometryRouteAvailable;
			routeInput.virtualGeometryCompute.estimatedCost = routeFacts.virtualGeometryUsable
				? std::max(1.0F, sourceTriangleWork * 0.58F + meshletWork * 3.0F
					+ decodedKiB * 0.075F + virtualDirectoryWork)
				: 0.0F;
			routeInput.calibrationIdentity = routeFacts.calibrationIdentity == 0u
				? 1u : routeFacts.calibrationIdentity;
			if(validationRouteOverride.has_value()) {
				switch(*validationRouteOverride) {
				case TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed:
					routeInput.authorOverride =
						TerraEngine::Renderer::GeometryRoute::DirectMeshlet;
					break;
				case TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware:
					routeInput.authorOverride =
						TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware;
					break;
				case TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute:
					routeInput.authorOverride =
						TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute;
					break;
				case TerraEngine::Renderer::VirtualGeometryRasterRoute::Fallback:
					// The setter rejects this retired validation choice. Keep the
					// snapshot automatic if external code somehow supplies it.
					break;
				}
			}
			const std::uint64_t routeMeshIdentity = entry.meshIdentity;
			// Calibration identity distinguishes recooked topology while the mesh
			// identity keeps unrelated resources from sharing hysteresis history.
			const std::uint64_t routeStateKey = routeMeshIdentity
				^ (routeInput.calibrationIdentity << 1U);
			const auto priorRouteState = geometryRoutePolicyStates.find(routeStateKey);
			TerraEngine::Renderer::GeometryRouteDecision preparedRoute =
				priorRouteState != geometryRoutePolicyStates.end()
				? TerraEngine::Renderer::SelectGeometryRouteCompact(
					routeInput, priorRouteState->second)
				: TerraEngine::Renderer::SelectGeometryRouteCompact(routeInput);
			geometryRoutePolicyStates[routeStateKey] = preparedRoute.nextState;
			const auto cachedRoute = geometryRouteDecisionByMesh.emplace(
				entry.mesh->meshId,
				CachedGeometryRouteDecision{
					std::move(preparedRoute), routeMeshIdentity}).first;
			preparedRouteDecision = &cachedRoute->second;
		}
		const TerraEngine::Renderer::GeometryRouteDecision& preparedRoute =
			preparedRouteDecision->decision;
		const std::uint64_t routeMeshIdentity =
			preparedRouteDecision->meshIdentity;
		TerraEngine::Renderer::GeometryRouteSelectionRecord routeSelection{};
		routeSelection.objectIdentity = routeObjectIdentity;
		routeSelection.meshIdentity = routeMeshIdentity;
		routeSelection.assetGeneration = snapshotRendererGeneration;
		routeSelection.decisionIdentity = routeObjectIdentity
			^ (routeMeshIdentity << 1u) ^ activeRenderFrameId;
		routeSelection.selected = preparedRoute.selected;
		routeSelection.policyIdentity = preparedRoute.policyIdentity;
		routeSelection.calibrationIdentity = preparedRoute.calibrationIdentity;
		routeSelection.source = preparedRoute.source;
		routeSelection.stableFrameCount = preparedRoute.stableFrameCount;
		routeSelection.fallbackReason = preparedRoute.fallbackReason;
		routeSelection.routeGeneration = snapshotRendererGeneration;
		geometryRouteStats.directCount += routeSelection.selected
			== TerraEngine::Renderer::GeometryRoute::DirectMeshlet ? 1U : 0U;
		geometryRouteStats.virtualCount += (routeSelection.selected
			== TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware
			|| routeSelection.selected
				== TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute) ? 1U : 0U;
		geometryRouteStats.policyIdentity = routeSelection.policyIdentity;
		geometryRouteStats.calibrationIdentity = routeSelection.calibrationIdentity;
		geometryRouteStats.hysteresisFrames = std::max(
			geometryRouteStats.hysteresisFrames, routeSelection.stableFrameCount);
		geometryRouteStats.source = routeSelection.source;
		geometryRouteSelectionSignature ^=
			static_cast<std::uint64_t>(routeSelection.selected) + 1U;
		geometryRouteSelectionSignature *= 1099511628211ULL;
		snapshotRenderable.geometryRouteInput.sourceTriangleCount =
			routeFacts.sourceTriangleCount;
		snapshotRenderable.geometryRouteInput.packedMeshletCount =
			routeFacts.packedMeshletCount;
		snapshotRenderable.geometryRouteInput.baseStoredBytes = routeFacts.storedBytes;
		snapshotRenderable.geometryRouteInput.baseDecodedBytes = routeFacts.decodedBytes;
		snapshotRenderable.geometryRouteInput.availableRepresentationMask =
			routeFacts.representationFlags;
		snapshotRenderable.geometryRouteInput.calibrationClassIdentity =
			preparedRoute.calibrationIdentity;
		snapshotRenderable.geometryRouteInput.calibrationVersion =
			mesh != nullptr ? mesh->runtimeLayout.version : 0u;
		snapshotRenderable.geometryRouteInput.directBatchEligible =
			routeSelection.selected == TerraEngine::Renderer::GeometryRoute::DirectMeshlet;
		snapshotRenderable.geometryRouteInput.repeatedInstanceStorageCompatible = true;
		snapshotRenderable.geometryRouteInput.deterministicSummaryHash =
			routeSelection.decisionIdentity;
		snapshotRenderable.geometryRouteDecision = routeSelection;
		if(retainDetailedRouteCandidates)
		{
			TerraEngine::Renderer::GeometryRouteDecision routeDecision =
				TerraEngine::Renderer::Frame::CopyRouteDecisionForFrame(preparedRoute, retainDetailedRouteCandidates);
			routeDecision.frameId = activeRenderFrameId;
			routeDecision.objectIdentity = routeSelection.objectIdentity;
			routeDecision.meshIdentity = routeSelection.meshIdentity;
			routeDecision.assetGeneration = routeSelection.assetGeneration;
			routeDecision.decisionIdentity = routeSelection.decisionIdentity;
			routeDecision.routeGeneration = routeSelection.routeGeneration;
			routeDecision.deterministicHash = routeSelection.decisionIdentity;
			renderFrameSnapshot.geometryRouteDecisions.push_back(
				std::move(routeDecision));
		}
		if (routeSelection.selected == TerraEngine::Renderer::GeometryRoute::DirectMeshlet)
		{
			snapshotRenderable.virtualGeometry = {};
		}
		const TerraEngine::Renderer::AnimationRenderBinding& binding = entry.animationBinding;
		// Worker selection owns the complete binding once per renderable. Protected
		// publication later strips worker-only ownership and duplicated GPU fields,
		// while every view continues to reuse this resolved source value.
		if(binding.meshId != TerraEngine::EMPTY_UUID) {
			TerraEngine::Renderer::AnimationRenderGenerationRecord& animation =
				snapshotRenderable.animation;
			animation.stableInstanceId = BuildStableAnimationInstanceId(binding, entry.objectId);
			animation.deformablePayloadGeneration = binding.deformablePayloadGeneration;
			animation.rendererGeneration = snapshotRendererGeneration;
			animation.deformationContentIdLow =
				binding.deformationContentIdLow;
			animation.deformationContentIdHigh =
				binding.deformationContentIdHigh;
			if(binding.gpu.IsComplete() && immutableGpuAnimationPlan != nullptr
				&& binding.gpu.workPacketIndex < immutableGpuAnimationPlan->WorkPackets().size()) {
				const TerraEngine::Animation::GpuAnimationWorkPacket& gpuPacket =
					immutableGpuAnimationPlan->WorkPackets()[binding.gpu.workPacketIndex];
				const TerraEngine::Animation::GpuAnimationGenerationRecord& gpuGeneration =
					immutableGpuAnimationPlan->GenerationRecord(gpuPacket.generationRecordIndex);
				animation.poseGeneration = gpuGeneration.workGeneration;
				animation.morphGeneration = gpuGeneration.workGeneration;
				animation.paletteGeneration = binding.gpu.paletteGeneration;
				animation.programGeneration = gpuGeneration.programGeneration;
				animation.animationPlanGeneration = binding.gpu.planGeneration;
				animation.firstGpuWorkPacket = binding.gpu.workPacketIndex;
				animation.gpuWorkPacketCount = 1U;
				animation.gpuStageMask = binding.gpu.stageMask;
				animation.source = binding.gpu.hybridCpuAuthority
					? TerraEngine::Renderer::AnimationRenderEvaluationSource::HybridAuthoritative
					: TerraEngine::Renderer::AnimationRenderEvaluationSource::GpuVisual;
				animation.complete = true;
			} else {
				animation.poseGeneration = binding.posePublicationGeneration;
				// CPU snapshots publish pose and morph arrays atomically. Their one
				// publication identity is shared by every render view.
				animation.morphGeneration = binding.posePublicationGeneration;
				animation.paletteGeneration = binding.posePublicationGeneration;
				if(binding.authorityRow != nullptr && binding.authorityRow->snapshot != nullptr
					&& binding.authorityRow->snapshot->pose != nullptr) {
					animation.programGeneration =
						binding.authorityRow->snapshot->pose->Generation().programGeneration;
				}
				animation.safeFallback = binding.safePose
					|| binding.status != TerraEngine::Renderer::AnimationRenderBindingStatus::Ready;
				animation.source = animation.safeFallback
					? TerraEngine::Renderer::AnimationRenderEvaluationSource::SafeFallback
					: TerraEngine::Renderer::AnimationRenderEvaluationSource::CpuPose;
				animation.complete = !animation.safeFallback && binding.HasCpuPose();
			}

			snapshotRenderable.invalidation = binding.invalidation;
			snapshotRenderable.invalidation.view = activeRenderFrameId;
		}
		// Snapshot assembly is the final owner of the complete worker binding. Move
		// its shared publication handles instead of incrementing and later releasing
		// three reference counts for every member of a large animated crowd. All
		// downstream consumers read the binding from the immutable snapshot record.
		// The renderer-owned capture cache retains GPU/static binding topology across
		// warm frames. Copy only on a rebuilt snapshot; compatible frames skip this
		// entire loop, so the shared presentation owner is not churned per frame.
		snapshotRenderable.animationBinding = entry.animationBinding;
		CaptureSnapshotShadowEvidence(entry, renderableIndex, mesh, routeFacts, snapshotRenderable, snapshotShadowInputs);
		if(lastSnapshotMeshId != entry.mesh->meshId) {
			lastSnapshotMeshId = entry.mesh->meshId;
			if(snapshotMeshIds.emplace(entry.mesh->meshId).second) {
				renderFrameSnapshot.meshes.push_back({
					entry.mesh->meshId, "mesh", true, false });
			}
		}
		if(lastSnapshotMaterialId != entry.mesh->materialId) {
			lastSnapshotMaterialId = entry.mesh->materialId;
			if(snapshotMaterialIds.emplace(entry.mesh->materialId).second) {
				renderFrameSnapshot.materials.push_back({
					entry.mesh->materialId, "material", true, false });
			}
		}
	}
	}

namespace TerraEngine::Renderer::Frame {
	// Warm snapshots retain exact rows; only bounded route diagnostics are rebuilt.
	void ReuseFrameRouteDiagnostics(const RenderFrameSnapshot& renderFrameSnapshot,
		GeometryRouteStatsAggregate& geometryRouteStats, std::uint64_t& geometryRouteSelectionSignature)
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reuse Render Snapshot Records",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Route policy and batch membership are immutable under the source signature.
		// Rebuild only bounded scalar diagnostics; no mesh, material, resource, route,
		// transform, or animation-binding record is reconstructed on this path.
		for(const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable :
			renderFrameSnapshot.renderables)
		{
			const TerraEngine::Renderer::GeometryRouteSelectionRecord& route =
				renderable.geometryRouteDecision;
			geometryRouteStats.directCount += route.selected
				== TerraEngine::Renderer::GeometryRoute::DirectMeshlet ? 1u : 0u;
			geometryRouteStats.virtualCount += route.selected
				== TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware
				|| route.selected
					== TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute
				? 1u : 0u;
			geometryRouteStats.sharedInstances += route.selected
				== TerraEngine::Renderer::GeometryRoute::DirectMeshlet ? 1u : 0u;
			geometryRouteStats.policyIdentity = route.policyIdentity;
			geometryRouteStats.calibrationIdentity = route.calibrationIdentity;
			geometryRouteStats.hysteresisFrames = std::max(
				geometryRouteStats.hysteresisFrames, route.stableFrameCount);
			geometryRouteStats.source = route.source;
			geometryRouteSelectionSignature ^=
				static_cast<std::uint64_t>(route.selected) + 1u;
			geometryRouteSelectionSignature *= 1099511628211ULL;
		}

	}
	// Preserve per-view canonical identity and previous-origin matching in output order.
	void BuildFrameViewTransforms(RenderFrameSnapshot& renderFrameSnapshot,
		std::span<const RenderFrameViewRequest> previousViewRequests)
	{
	renderFrameSnapshot.viewTransforms.clear();
	renderFrameSnapshot.viewTransforms.reserve(
		renderFrameSnapshot.viewRequests.size()
		* renderFrameSnapshot.renderables.size());
	for (const TerraEngine::Renderer::RenderFrameViewRequest& viewRequest :
		renderFrameSnapshot.viewRequests)
	{
		const TerraEngine::Renderer::RenderReferenceFrame* previousFrame = nullptr;
		for (const TerraEngine::Renderer::RenderFrameViewRequest& previousRequest :
			previousViewRequests)
		{
			if (previousRequest.referenceFrame.viewId
					== viewRequest.referenceFrame.viewId
				&& previousRequest.referenceFrame.outputId
					== viewRequest.referenceFrame.outputId)
			{
				previousFrame = &previousRequest.referenceFrame;
				break;
			}
		}
		for (const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable :
			renderFrameSnapshot.renderables)
		{
			const TerraEngine::Renderer::RenderLocalizedPoseMatrices localized =
				TerraEngine::Renderer::BuildLocalizedRenderPoseMatrices(
					renderable.canonicalWorldPose,
					viewRequest.referenceFrame, previousFrame);
			TerraEngine::Renderer::RenderViewTransformRecord record{};
			record.objectId = renderable.objectId;
			record.viewId = viewRequest.referenceFrame.viewId;
			record.originGeneration =
				viewRequest.referenceFrame.originGeneration;
			record.currentMatrix = localized.currentMatrix;
			record.previousMatrix = localized.previousMatrix;
			record.valid = localized.valid && !localized.staleReferenceFrame;
			renderFrameSnapshot.viewTransforms.push_back(record);
		}
	}

	}
}
