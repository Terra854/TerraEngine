// VulkanShadowPagePreparation.cpp — worker-safe authored-page cut validation.
#include "VulkanShadowPagePreparation.h"
#include "Renderer/ShadowWorkPlan.h"
#include "Renderer/RenderTypes.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace TerraEngine::Renderer::Vulkan
{
	bool ApplyShadowAuthoredPageBinding(const PreparedShadowPageBinding& binding, ShadowCasterStamp& stamp) noexcept
	{
		const auto& segment = binding.segment;
		// Only the actual complete shared word stream can bound shader reads.
		// Validate before writing any field: one failed binding must leave the
		// caller's complete owner draw available, never a partially stamped cut.
		const std::uint64_t segmentEnd = static_cast<std::uint64_t>(segment.ownerMeshletOffset) + segment.ownerMeshletCount;
		if(binding.descriptorIndex == std::numeric_limits<std::uint32_t>::max() || binding.payloadBufferIndex == std::numeric_limits<std::uint32_t>::max()
			|| binding.source != ShadowPagePayloadSource::AuthoredShadowLocal
			|| stamp.meshId == EMPTY_UUID || stamp.assetId == EMPTY_UUID
			|| !binding.cpuPublicationGeneration || binding.cpuPublicationGeneration != stamp.meshRevision
			|| binding.cpuPublicationGeneration != stamp.assetRevision
			|| !binding.payloadAllocationGeneration || !binding.payloadByteSize
			|| binding.payloadByteOffset > std::numeric_limits<std::uint64_t>::max() - binding.payloadByteSize
			|| binding.payloadByteOffset != 0 || binding.payloadByteSize % sizeof(std::uint32_t) != 0
			|| segment.pageId == std::numeric_limits<std::uint32_t>::max() || segment.clusterId == std::numeric_limits<std::uint32_t>::max()
			|| !segment.ownerMeshletCount || segmentEnd > std::numeric_limits<std::uint32_t>::max()
			|| !stamp.meshletCount || stamp.firstMeshlet > std::numeric_limits<std::uint64_t>::max() - stamp.meshletCount
			|| segment.selectedLod != stamp.selectedLod || segment.ownerMeshletOffset < stamp.firstMeshlet
			|| segmentEnd > stamp.firstMeshlet + stamp.meshletCount)
			return false;
		stamp.segmentId = (static_cast<std::uint64_t>(segment.ownerMeshletOffset) << 32U) | segment.ownerMeshletCount;
		stamp.firstMeshlet = segment.ownerMeshletOffset; stamp.meshletCount = segment.ownerMeshletCount;
		stamp.firstCluster = segment.clusterId; stamp.clusterCount = 1U;
		stamp.pageId = segment.pageId; stamp.fallbackPageId = segment.fallbackPageId;
		stamp.pageContentRevision = binding.cpuPublicationGeneration;
		stamp.payloadContentRevision = binding.cpuPublicationGeneration;
		stamp.residencyGeneration = binding.payloadAllocationGeneration;
		// This exact label identifies the authored shadow-local stream, never a
		// persistent streamed page allocation that these commands did not consume.
		stamp.payloadSource = 0x100U;
		stamp.emittedSource = static_cast<std::uint32_t>(VirtualGeometryEmittedPageSource::HighDetailPage);
		stamp.payloadByteOffset = binding.payloadByteOffset; stamp.payloadByteSize = binding.payloadByteSize;
		return true;
	}
	ShadowPageCut BuildShadowAuthoredPageCut(const ShadowPageCutRequest& request)
	{
		const auto reject = [](const char* reason) { return ShadowPageCut{false, {}, reason}; };
		if (!request.mesh || request.meshId == EMPTY_UUID || request.cpuPublicationGeneration == 0U
			|| request.mesh->publicationGeneration != request.cpuPublicationGeneration)
			return reject("Immutable authored mesh publication is unavailable.");
		const Mesh& mesh = *request.mesh;
		if (!mesh.hasVirtualGeometry || request.deformationUnsupported || request.materialUnsupported
			|| mesh.hasDeformablePayload || mesh.hasDeformableDemandedPages
			|| !mesh.meshletSkinningData.empty() || !mesh.deformableMorphs.empty())
			return reject("Authored shadow-local pages require supported static geometry and material.");
		const std::uint64_t selectedEnd = static_cast<std::uint64_t>(request.ownerMeshletOffset) + request.ownerMeshletCount;
		if (request.ownerMeshletCount == 0U || selectedEnd > mesh.meshletRecords.size()
			|| selectedEnd > std::numeric_limits<std::uint32_t>::max())
			return reject("Selected owner meshlet span is empty or out of range.");

		// Cooked groups mirror meshlets, but validate their actual topology joins
		// instead of assuming an arbitrary page ID describes the selected bytes.
		ShadowPageCut result;
		std::unordered_set<std::uint32_t> clusterIds;
		for (const auto& cluster : mesh.virtualGeometryClusters) {
			if (cluster.lodIndex != request.selectedLod || cluster.groupCount == 0U) continue;
			const std::uint64_t end = static_cast<std::uint64_t>(cluster.groupOffset) + cluster.groupCount;
			if (end <= request.ownerMeshletOffset || cluster.groupOffset >= selectedEnd) continue;
			if (cluster.groupOffset < request.ownerMeshletOffset || end > selectedEnd
				|| end > mesh.virtualGeometryCullableGroups.size() || end > mesh.meshletRecords.size()
				|| !clusterIds.insert(cluster.clusterId).second)
				return reject("Selected cluster coverage is partial, duplicated, or out of range.");
			const auto page = std::find_if(mesh.virtualGeometryPages.begin(), mesh.virtualGeometryPages.end(),
				[&](const auto& row) { return row.pageId == cluster.pageId; });
			if (page == mesh.virtualGeometryPages.end() || cluster.pageId == 0xffffffffU
				|| std::count_if(mesh.virtualGeometryPages.begin(), mesh.virtualGeometryPages.end(),
					[&](const auto& row) { return row.pageId == cluster.pageId; }) != 1
				|| cluster.groupOffset < page->groupOffset
				|| end > static_cast<std::uint64_t>(page->groupOffset) + page->groupCount
				|| request.selectedLod < page->firstLodIndex
				|| request.selectedLod >= static_cast<std::uint64_t>(page->firstLodIndex) + page->lodCount)
				return reject("Authored page does not own the selected cluster and LOD.");
			const std::uint64_t linkEnd = static_cast<std::uint64_t>(page->clusterPageLinkOffset) + page->clusterPageLinkCount;
			if (linkEnd > mesh.virtualGeometryClusterPageLinks.size()) return reject("Page cluster joins are out of range.");
			bool joined = false;
			for (std::uint32_t index = page->clusterPageLinkOffset; index < linkEnd; ++index) {
				const auto& link = mesh.virtualGeometryClusterPageLinks[index];
				if (link.clusterId == cluster.clusterId && link.pageId == cluster.pageId
					&& link.firstGroup == cluster.groupOffset && link.groupCount == cluster.groupCount) joined = true;
			}
			if (!joined) return reject("Page has no exact authored cluster coverage join.");
			for (std::uint32_t index = cluster.groupOffset; index < end; ++index) {
				const auto& group = mesh.virtualGeometryCullableGroups[index];
				const auto& record = mesh.meshletRecords[index];
				if (group.groupIndex != index || group.lodIndex != request.selectedLod
					|| group.pageId != cluster.pageId || group.sectionId != cluster.materialSectionId
					|| group.vertexOffset != record.vertexOffset || group.vertexCount != record.vertexCount
					|| group.primitiveOffset != record.primitiveIndexOffset
					|| static_cast<std::uint64_t>(group.primitiveCount) * 3U != record.primitiveIndexCount)
					return reject("Authored group does not describe the meshlet bytes to be emitted.");
			}
			result.segments.push_back({page->pageId, page->fallbackPageId, cluster.clusterId,
				cluster.groupOffset, cluster.groupCount, request.selectedLod});
		}
		std::sort(result.segments.begin(), result.segments.end(), [](const auto& left, const auto& right) {
			return left.ownerMeshletOffset < right.ownerMeshletOffset;
		});
		std::uint64_t next = request.ownerMeshletOffset;
		for (const auto& segment : result.segments) {
			if (segment.ownerMeshletOffset != next) return reject("Authored page cut has a gap or overlap.");
			next += segment.ownerMeshletCount;
		}
		if (next != selectedEnd) return reject("Authored page cut does not cover the entire light-selected owner span.");
		result.complete = true;
		return result;
	}
}
