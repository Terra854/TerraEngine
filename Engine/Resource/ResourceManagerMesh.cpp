// ResourceManagerMesh.cpp — Mesh Resource Management Implementation
//
// Implements the specialized logic for loading and managing mesh geometry resources.
// It handles the parsing of high-performance mesh formats, including meshlet generation and vertex data quantization.
// This module provides the geometric data required by the renderer for both standard and GPU-driven drawing paths.

#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineMeshFile.h"

namespace
{
	enum class VirtualMeshLoadDecision
	{
		Accepted,
		Rejected,
		Downgraded
	};

	enum class AdaptiveVirtualMeshRejectionReason
	{
		None,
		RequestedTargetOutOfRange,
		SkippedDetailOutOfRange,
		ClusterHierarchyOutOfRange,
		ClusterPageLinkOutOfRange,
		ValidationFlagStringOutOfRange
	};

	struct VirtualMeshValidationResult
	{
		VirtualMeshLoadDecision decision = VirtualMeshLoadDecision::Accepted;
		AdaptiveVirtualMeshRejectionReason adaptiveReason = AdaptiveVirtualMeshRejectionReason::None;
		std::string reason;
		std::vector<std::string> warnings;
	};

	const char* ToString(AdaptiveVirtualMeshRejectionReason reason)
	{
		switch (reason)
		{
		case AdaptiveVirtualMeshRejectionReason::None: return "none";
		case AdaptiveVirtualMeshRejectionReason::RequestedTargetOutOfRange: return "requested-target-out-of-range";
		case AdaptiveVirtualMeshRejectionReason::SkippedDetailOutOfRange: return "skipped-detail-out-of-range";
		case AdaptiveVirtualMeshRejectionReason::ClusterHierarchyOutOfRange: return "cluster-hierarchy-out-of-range";
		case AdaptiveVirtualMeshRejectionReason::ClusterPageLinkOutOfRange: return "cluster-page-link-out-of-range";
		case AdaptiveVirtualMeshRejectionReason::ValidationFlagStringOutOfRange: return "validation-flag-string-out-of-range";
		}
		return "unknown";
	}

	bool VirtualMeshValidationRejected(const VirtualMeshValidationResult& result)
	{
		return result.decision == VirtualMeshLoadDecision::Rejected;
	}

	VirtualMeshValidationResult ValidateVirtualGeometryPayloadMetadata(const MeshHeader& header, const Mesh& mesh, const std::filesystem::path& resolvedPath)
	{
		VirtualMeshValidationResult result;
		const bool hasVirtualGeometryFlag = (header.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0u;
		if (!hasVirtualGeometryFlag)
		{
			return result;
		}

		// The foundation phase defines the atomic validation boundary before US1
		// teaches the loader how to parse the full table set. Until then, a file
		// that advertises virtual geometry but does not publish valid detail
		// metadata is rejected before ResourceManager caches or GPU uploads see
		// partially initialized state.
		if (mesh.virtualGeometryDetailLevels.empty())
		{
			result.decision = VirtualMeshLoadDecision::Rejected;
			result.reason = "virtual-geometry-detail-table-missing";
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF virtual geometry payload is present but has no validated detail table in "
				<< resolvedPath << ". Rebuild the mesh after the virtual geometry cooker lands.");
			return result;
		}

		if (mesh.virtualGeometryPayloadGeneration != kVirtualGeometryPayloadGeneration)
		{
			result.decision = VirtualMeshLoadDecision::Rejected;
			result.reason = "virtual-geometry-generation-mismatch";
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF virtual geometry generation mismatch in " << resolvedPath
				<< ". Expected generation " << kVirtualGeometryPayloadGeneration << ".");
			return result;
		}

		return result;
	}

	// TryMultiply is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryMultiply(std::size_t lhs, std::size_t rhs, std::size_t& outValue)
	{
		if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max() / lhs))
		{
			return false;
		}

		outValue = lhs * rhs;
		return true;
	}

	// TryAdd is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryAdd(std::size_t lhs, std::size_t rhs, std::size_t& outValue)
	{
		if (rhs > (std::numeric_limits<std::size_t>::max() - lhs))
		{
			return false;
		}

		outValue = lhs + rhs;
		return true;
	}

	// ResolvePayloadRange is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool ResolvePayloadRange(const MeshHeader& header, std::size_t payloadSize, std::uint32_t absoluteOffset, std::size_t byteCount, const std::filesystem::path& resolvedPath, const char* rangeName, std::size_t& outRelativeOffset)
	{
		if (absoluteOffset < header.headerSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF " << rangeName << " offset underflows header in " << resolvedPath);
			return false;
		}

		const std::size_t relativeOffset = static_cast<std::size_t>(absoluteOffset - header.headerSize);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (relativeOffset > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF " << rangeName << " offset exceeds payload in " << resolvedPath);
			return false;
		}

		std::size_t rangeEnd = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryAdd(relativeOffset, byteCount, rangeEnd) || rangeEnd > payloadSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF " << rangeName << " range overruns payload in " << resolvedPath);
			return false;
		}

		outRelativeOffset = relativeOffset;
		return true;
	}

	bool DecodeRemapStream(
		const std::uint8_t* encodedData,
		std::size_t remapCount,
		MeshletRemapEncoding encoding,
		std::vector<std::uint32_t>& outDecoded)
	{
		outDecoded.resize(remapCount);
		const std::size_t elementSize = static_cast<std::size_t>(GetRemapElementSizeBytes(encoding));
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t remapIndex = 0; remapIndex < remapCount; ++remapIndex)
		{
			std::size_t byteOffset = 0;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!TryMultiply(remapIndex, elementSize, byteOffset))
			{
				return false;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (encoding == MeshletRemapEncoding::Uint16)
			{
				std::uint16_t packed = 0;
				std::memcpy(&packed, encodedData + byteOffset, sizeof(packed));
				outDecoded[remapIndex] = static_cast<std::uint32_t>(packed);
			}
			else
			{
				std::uint32_t packed = 0;
				std::memcpy(&packed, encodedData + byteOffset, sizeof(packed));
				outDecoded[remapIndex] = packed;
			}
		}

		return true;
	}

	// IsFiniteFloat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsFiniteFloat(float value)
	{
		return std::isfinite(value) != 0;
	}

	// DisableConeData is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void DisableConeData(MeshletBounds& bounds)
	{
		bounds.coneApex[0] = bounds.center[0];
		bounds.coneApex[1] = bounds.center[1];
		bounds.coneApex[2] = bounds.center[2];
		bounds.coneAxis[0] = 0.0f;
		bounds.coneAxis[1] = 0.0f;
		bounds.coneAxis[2] = 0.0f;
		// Values outside [-1, 1] are treated by the task shader as "disabled".
		bounds.coneCutoff = 2.0f;
		bounds.coneApexOffset = 0.0f;
	}

	bool ValidateVirtualGeometryStringRange(
		std::uint32_t offset,
		std::uint32_t size,
		std::size_t stringTableSize,
		const std::filesystem::path& resolvedPath,
		const char* rangeName)
	{
		if (size == 0u)
		{
			return true;
		}

		std::size_t rangeEnd = 0u;
		if (offset > stringTableSize || !TryAdd(static_cast<std::size_t>(offset), static_cast<std::size_t>(size), rangeEnd) || rangeEnd > stringTableSize)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry " << rangeName << " string range overruns string table in " << resolvedPath);
			return false;
		}
		return true;
	}

	template <typename Record>
	bool ReadVirtualGeometryTable(
		const MeshHeader& header,
		const std::uint8_t* payload,
		std::size_t payloadSize,
		std::uint32_t absoluteOffset,
		std::uint32_t count,
		const std::filesystem::path& resolvedPath,
		const char* rangeName,
		std::vector<Record>& outRecords)
	{
		outRecords.clear();
		if (count == 0u)
		{
			if (absoluteOffset != 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry " << rangeName << " offset must be zero when count is zero in " << resolvedPath);
				return false;
			}
			return true;
		}

		if (absoluteOffset == 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry " << rangeName << " offset is missing in " << resolvedPath);
			return false;
		}

		std::size_t tableBytes = 0u;
		if (!TryMultiply(static_cast<std::size_t>(count), sizeof(Record), tableBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry " << rangeName << " byte size overflow in " << resolvedPath);
			return false;
		}

		std::size_t relativeOffset = 0u;
		if (!ResolvePayloadRange(header, payloadSize, absoluteOffset, tableBytes, resolvedPath, rangeName, relativeOffset))
		{
			return false;
		}

		outRecords.resize(count);
		std::memcpy(outRecords.data(), payload + relativeOffset, tableBytes);
		return true;
	}

	bool ValidateVirtualGeometryAddRange(
		std::uint32_t offset,
		std::uint32_t count,
		std::uint32_t limit,
		const std::filesystem::path& resolvedPath,
		const char* rangeName)
	{
		std::size_t rangeEnd = 0u;
		if (!TryAdd(static_cast<std::size_t>(offset), static_cast<std::size_t>(count), rangeEnd) || rangeEnd > static_cast<std::size_t>(limit))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry " << rangeName << " references data outside its table in " << resolvedPath);
			return false;
		}
		return true;
	}

	bool ValidateAdaptiveVirtualGeometryPayloadTables(const Mesh& mesh, const std::filesystem::path& resolvedPath, AdaptiveVirtualMeshRejectionReason& outReason)
	{
		outReason = AdaptiveVirtualMeshRejectionReason::None;

		for (const VirtualGeometryRequestedTargetRecord& target : mesh.virtualGeometryRequestedTargets)
		{
			if (target.targetIndex >= mesh.virtualGeometryRequestedTargets.size()
				|| target.status > static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Rejected)
				|| !IsFiniteFloat(target.requestedRatio)
				|| target.requestedRatio <= 0.0f)
			{
				outReason = AdaptiveVirtualMeshRejectionReason::RequestedTargetOutOfRange;
				TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry requested target is invalid in " << resolvedPath);
				return false;
			}
		}

		for (const VirtualGeometrySkippedDetailRecord& skipped : mesh.virtualGeometrySkippedDetails)
		{
			const bool targetValid = mesh.virtualGeometryRequestedTargets.empty()
				|| skipped.targetIndex < mesh.virtualGeometryRequestedTargets.size();
			const bool fallbackValid = mesh.virtualGeometryDetailLevels.empty()
				|| skipped.nearestGeneratedLodIndex < mesh.virtualGeometryDetailLevels.size();
			if (!targetValid
				|| !fallbackValid
				|| skipped.status > static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Rejected)
				|| skipped.reason > static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::DuplicateOfPreviousLevel)
				|| !IsFiniteFloat(skipped.requestedRatio)
				|| !IsFiniteFloat(skipped.actualCandidateRatio)
				|| !IsFiniteFloat(skipped.actualNearestRatio)
				|| !ValidateVirtualGeometryStringRange(skipped.messageStringOffset, skipped.messageStringSize, mesh.virtualGeometryDiagnosticStrings.size(), resolvedPath, "skipped detail message"))
			{
				outReason = AdaptiveVirtualMeshRejectionReason::SkippedDetailOutOfRange;
				TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry skipped detail is invalid in " << resolvedPath);
				return false;
			}
		}

		for (std::size_t clusterIndex = 0u; clusterIndex < mesh.virtualGeometryClusters.size(); ++clusterIndex)
		{
			const VirtualGeometryClusterRecord& cluster = mesh.virtualGeometryClusters[clusterIndex];
			const bool parentValid = cluster.parentClusterId == kVirtualGeometryInvalidIndex
				|| cluster.parentClusterId < mesh.virtualGeometryClusters.size();
			const bool firstChildValid = cluster.childCount == 0u
				|| cluster.firstChildClusterId < mesh.virtualGeometryClusters.size();
			const bool childRangeValid = cluster.childCount == 0u
				|| ValidateVirtualGeometryAddRange(cluster.firstChildClusterId, cluster.childCount, static_cast<std::uint32_t>(mesh.virtualGeometryClusters.size()), resolvedPath, "cluster child range");
			const bool fallbackValid = cluster.fallbackClusterId < mesh.virtualGeometryClusters.size();
			const bool lodValid = cluster.lodIndex < mesh.virtualGeometryDetailLevels.size();
			const bool pageValid = mesh.virtualGeometryPages.empty()
				|| cluster.pageId < mesh.virtualGeometryPages.size();
			const bool materialValid = mesh.virtualGeometryMaterialSections.empty()
				|| std::any_of(mesh.virtualGeometryMaterialSections.begin(), mesh.virtualGeometryMaterialSections.end(), [&](const VirtualGeometryMaterialSectionRecord& section) {
					return section.sectionId == cluster.materialSectionId;
				});
			if (!parentValid
				|| !firstChildValid
				|| !childRangeValid
				|| !fallbackValid
				|| !lodValid
				|| !pageValid
				|| !materialValid
				|| cluster.clusterId != static_cast<std::uint32_t>(clusterIndex)
				|| !ValidateVirtualGeometryAddRange(cluster.groupOffset, cluster.groupCount, static_cast<std::uint32_t>(mesh.virtualGeometryCullableGroups.size()), resolvedPath, "cluster group range")
				|| !IsFiniteFloat(cluster.geometricError)
				|| !IsFiniteFloat(cluster.boundsCenter[0])
				|| !IsFiniteFloat(cluster.boundsCenter[1])
				|| !IsFiniteFloat(cluster.boundsCenter[2])
				|| !IsFiniteFloat(cluster.boundsRadius)
				|| cluster.boundsRadius < 0.0f)
			{
				outReason = AdaptiveVirtualMeshRejectionReason::ClusterHierarchyOutOfRange;
				TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry cluster metadata is invalid in " << resolvedPath);
				return false;
			}
			for (std::uint32_t childOffset = 0u; childOffset < cluster.childCount; ++childOffset)
			{
				const std::uint32_t childIndex = cluster.firstChildClusterId + childOffset;
				if (mesh.virtualGeometryClusters[childIndex].parentClusterId != cluster.clusterId)
				{
					outReason = AdaptiveVirtualMeshRejectionReason::ClusterHierarchyOutOfRange;
					TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry cluster child does not point back to its parent in " << resolvedPath);
					return false;
				}
			}
		}

		for (const VirtualGeometryClusterRecord& cluster : mesh.virtualGeometryClusters)
		{
			std::vector<std::uint8_t> visited(mesh.virtualGeometryClusters.size(), 0u);
			std::uint32_t parentId = cluster.parentClusterId;
			for (std::size_t guard = 0u; parentId != kVirtualGeometryInvalidIndex && guard < mesh.virtualGeometryClusters.size(); ++guard)
			{
				if (parentId >= mesh.virtualGeometryClusters.size()
					|| parentId == cluster.clusterId
					|| visited[parentId] != 0u)
				{
					outReason = AdaptiveVirtualMeshRejectionReason::ClusterHierarchyOutOfRange;
					TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry cluster hierarchy contains a cycle in " << resolvedPath);
					return false;
				}
				visited[parentId] = 1u;
				parentId = mesh.virtualGeometryClusters[parentId].parentClusterId;
			}
		}

		for (const VirtualGeometryClusterPageLinkRecord& link : mesh.virtualGeometryClusterPageLinks)
		{
			if (link.clusterId >= mesh.virtualGeometryClusters.size()
				|| link.pageId >= mesh.virtualGeometryPages.size()
				|| !ValidateVirtualGeometryAddRange(link.firstGroup, link.groupCount, static_cast<std::uint32_t>(mesh.virtualGeometryCullableGroups.size()), resolvedPath, "cluster page link group range"))
			{
				outReason = AdaptiveVirtualMeshRejectionReason::ClusterPageLinkOutOfRange;
				TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry cluster-page link is invalid in " << resolvedPath);
				return false;
			}
			const VirtualGeometryClusterRecord& cluster = mesh.virtualGeometryClusters[link.clusterId];
			const VirtualGeometryPageRecord& page = mesh.virtualGeometryPages[link.pageId];
			const std::uint64_t linkFirst = link.firstGroup;
			const std::uint64_t linkLast = linkFirst + link.groupCount;
			const std::uint64_t clusterFirst = cluster.groupOffset;
			const std::uint64_t clusterLast = clusterFirst + cluster.groupCount;
			const std::uint64_t pageFirst = page.groupOffset;
			const std::uint64_t pageLast = pageFirst + page.groupCount;
			if (link.groupCount == 0u
				|| linkFirst < clusterFirst
				|| linkLast > clusterLast
				|| linkFirst < pageFirst
				|| linkLast > pageLast)
			{
				outReason = AdaptiveVirtualMeshRejectionReason::ClusterPageLinkOutOfRange;
				TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry cluster-page link does not fit its cluster/page coverage in " << resolvedPath);
				return false;
			}
		}

		if (mesh.virtualGeometryConeSafety.size() != mesh.virtualGeometryClusters.size())
		{
			outReason = AdaptiveVirtualMeshRejectionReason::ClusterHierarchyOutOfRange;
			TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry cone safety table must publish one record per cluster in " << resolvedPath);
			return false;
		}
		for (std::size_t recordIndex = 0u; recordIndex < mesh.virtualGeometryConeSafety.size(); ++recordIndex)
		{
			const VirtualGeometryConeSafetyRecord& cone = mesh.virtualGeometryConeSafety[recordIndex];
			const bool clusterValid = cone.clusterId < mesh.virtualGeometryClusters.size()
				&& cone.clusterId == static_cast<std::uint32_t>(recordIndex);
			const bool groupRangeValid = ValidateVirtualGeometryAddRange(cone.groupOffset, cone.groupCount, static_cast<std::uint32_t>(mesh.virtualGeometryCullableGroups.size()), resolvedPath, "cone safety group range");
			const bool reasonValid = ValidateVirtualGeometryStringRange(cone.disabledReasonStringOffset, cone.disabledReasonStringSize, mesh.virtualGeometryDiagnosticStrings.size(), resolvedPath, "cone safety reason");
			const bool enumValid =
				cone.materialSidedness <= 3u
				&& cone.windingState <= 3u
				&& cone.alphaRisk <= 3u
				&& cone.thinShellRisk <= 2u;
			const bool eligible = (cone.flags & kVirtualGeometryConeSafetyFlagEligible) != 0u;
			const bool eligibleProved =
				!eligible
				|| ((cone.flags & kVirtualGeometryConeSafetyFlagConeValid) != 0u
					&& cone.materialSidedness == 0u
					&& cone.windingState == 0u
					&& cone.alphaRisk == 0u
					&& cone.thinShellRisk == 0u);
			if (!clusterValid || !groupRangeValid || !reasonValid || !enumValid || !eligibleProved)
			{
				outReason = AdaptiveVirtualMeshRejectionReason::ClusterHierarchyOutOfRange;
				TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry cone safety metadata is invalid in " << resolvedPath);
				return false;
			}
		}

		for (const VirtualGeometryValidationFlagRecord& flag : mesh.virtualGeometryValidationFlags)
		{
			if (!ValidateVirtualGeometryStringRange(flag.messageStringOffset, flag.messageStringSize, mesh.virtualGeometryDiagnosticStrings.size(), resolvedPath, "validation flag message"))
			{
				outReason = AdaptiveVirtualMeshRejectionReason::ValidationFlagStringOutOfRange;
				TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry validation flag text is invalid in " << resolvedPath);
				return false;
			}
		}

		return true;
	}

	bool ValidateVirtualGeometryPayloadTables(const MeshHeader& header, const Mesh& mesh, const std::filesystem::path& resolvedPath)
	{
		if (mesh.virtualGeometryDetailLevels.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry detail table is empty in " << resolvedPath);
			return false;
		}
		if (mesh.virtualGeometryCullableGroups.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry cullable group table is empty in " << resolvedPath);
			return false;
		}

		for (std::size_t detailIndex = 0; detailIndex < mesh.virtualGeometryDetailLevels.size(); ++detailIndex)
		{
			const VirtualGeometryDetailLevelRecord& detail = mesh.virtualGeometryDetailLevels[detailIndex];
			if (detail.lodIndex != detailIndex
				|| detail.targetStatus > static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Rejected)
				|| detail.meshletCount == 0u
				|| detail.primitiveCount == 0u
				|| !ValidateVirtualGeometryAddRange(detail.meshletOffset, detail.meshletCount, header.meshletCount, resolvedPath, "detail meshlet range")
				|| !ValidateVirtualGeometryAddRange(detail.sectionRangeOffset, detail.sectionRangeCount, static_cast<std::uint32_t>(mesh.virtualGeometrySectionRanges.size()), resolvedPath, "detail section range")
				|| !ValidateVirtualGeometryAddRange(detail.pageRangeOffset, detail.pageRangeCount, static_cast<std::uint32_t>(mesh.virtualGeometryPages.size()), resolvedPath, "detail page range"))
			{
				return false;
			}
			if (!mesh.virtualGeometryRequestedTargets.empty() && detail.requestedTargetIndex >= mesh.virtualGeometryRequestedTargets.size())
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry detail references a missing requested target in " << resolvedPath);
				return false;
			}

			if (!IsFiniteFloat(detail.geometricError)
				|| detail.geometricError < 0.0f
				|| !IsFiniteFloat(detail.targetRatio)
				|| detail.targetRatio <= 0.0f)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry detail has invalid error metadata in " << resolvedPath);
				return false;
			}

			for (int component = 0; component < 3; ++component)
			{
				if (!IsFiniteFloat(detail.boundsMin[component]) || !IsFiniteFloat(detail.boundsMax[component]) || detail.boundsMin[component] > detail.boundsMax[component])
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry detail has invalid bounds in " << resolvedPath);
					return false;
				}
			}
		}

		for (const VirtualGeometryMaterialSectionRecord& section : mesh.virtualGeometryMaterialSections)
		{
			if (section.sourceTriangleCount == 0u
				|| !ValidateVirtualGeometryAddRange(section.perLodRangeOffset, section.perLodRangeCount, static_cast<std::uint32_t>(mesh.virtualGeometrySectionRanges.size()), resolvedPath, "material section LOD range")
				|| !ValidateVirtualGeometryStringRange(section.sourceNameStringOffset, section.sourceNameStringSize, mesh.virtualGeometryDiagnosticStrings.size(), resolvedPath, "material section name"))
			{
				return false;
			}
		}

		std::vector<std::uint8_t> sectionMeshletCoverage(static_cast<std::size_t>(header.meshletCount), 0u);
		for (const VirtualGeometrySectionRangeRecord& range : mesh.virtualGeometrySectionRanges)
		{
			if (range.lodIndex >= mesh.virtualGeometryDetailLevels.size()
				|| range.meshletCount == 0u
				|| range.primitiveCount == 0u
				|| !ValidateVirtualGeometryAddRange(range.meshletOffset, range.meshletCount, header.meshletCount, resolvedPath, "section meshlet range"))
			{
				return false;
			}

			std::size_t primitiveElementCount = 0u;
			if (!TryMultiply(static_cast<std::size_t>(range.primitiveCount), 3u, primitiveElementCount)
				|| !ValidateVirtualGeometryAddRange(range.primitiveOffset, static_cast<std::uint32_t>(primitiveElementCount), header.meshletPrimitiveIndexCount, resolvedPath, "section primitive range"))
			{
				return false;
			}

			for (std::uint32_t meshlet = range.meshletOffset; meshlet < range.meshletOffset + range.meshletCount; ++meshlet)
			{
				if (sectionMeshletCoverage[meshlet] != 0u)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry section ranges overlap meshlets in " << resolvedPath);
					return false;
				}
				sectionMeshletCoverage[meshlet] = 1u;
			}
		}
		if (std::any_of(sectionMeshletCoverage.begin(), sectionMeshletCoverage.end(), [](std::uint8_t covered) { return covered == 0u; }))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry section ranges do not cover every resident meshlet in " << resolvedPath);
			return false;
		}

		for (const VirtualGeometryCullableGroupRecord& group : mesh.virtualGeometryCullableGroups)
		{
			if (group.lodIndex >= mesh.virtualGeometryDetailLevels.size()
				|| group.vertexCount == 0u
				|| group.primitiveCount == 0u
				|| !ValidateVirtualGeometryAddRange(group.vertexOffset, group.vertexCount, header.meshletVertexRemapCount, resolvedPath, "cullable group vertex range"))
			{
				return false;
			}

			std::size_t primitiveElementCount = 0u;
			if (!TryMultiply(static_cast<std::size_t>(group.primitiveCount), 3u, primitiveElementCount)
				|| !ValidateVirtualGeometryAddRange(group.primitiveOffset, static_cast<std::uint32_t>(primitiveElementCount), header.meshletPrimitiveIndexCount, resolvedPath, "cullable group primitive range"))
			{
				return false;
			}

			if (!IsFiniteFloat(group.boundsCenter[0])
				|| !IsFiniteFloat(group.boundsCenter[1])
				|| !IsFiniteFloat(group.boundsCenter[2])
				|| !IsFiniteFloat(group.boundsRadius)
				|| group.boundsRadius < 0.0f)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry cullable group has invalid bounds in " << resolvedPath);
				return false;
			}
		}

		bool hasRenderableFallbackPage = mesh.virtualGeometryPages.empty();
		if (!mesh.virtualGeometryPages.empty()
			&& (mesh.virtualGeometryClusters.empty() || mesh.virtualGeometryClusterPageLinks.empty()))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry page table is missing required cluster-page coverage metadata in " << resolvedPath);
			return false;
		}
		for (const VirtualGeometryPageRecord& page : mesh.virtualGeometryPages)
		{
			if (!ValidateVirtualGeometryAddRange(page.firstLodIndex, page.lodCount, static_cast<std::uint32_t>(mesh.virtualGeometryDetailLevels.size()), resolvedPath, "page LOD range")
				|| !ValidateVirtualGeometryAddRange(page.groupOffset, page.groupCount, static_cast<std::uint32_t>(mesh.virtualGeometryCullableGroups.size()), resolvedPath, "page group range")
				|| !ValidateVirtualGeometryAddRange(page.clusterPageLinkOffset, page.clusterPageLinkCount, static_cast<std::uint32_t>(mesh.virtualGeometryClusterPageLinks.size()), resolvedPath, "page cluster-link range"))
			{
				return false;
			}
			if (page.pageId >= mesh.virtualGeometryPages.size()
				|| mesh.virtualGeometryPages[page.pageId].pageId != page.pageId
				|| page.fallbackPageId >= mesh.virtualGeometryPages.size()
				|| page.byteSize == 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry page has invalid identity, fallback, or byte-size metadata in " << resolvedPath);
				return false;
			}
			if (page.clusterPageLinkCount == 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry page does not declare cluster-page coverage metadata in " << resolvedPath);
				return false;
			}
			const VirtualGeometryPageRecord& fallbackPage = mesh.virtualGeometryPages[page.fallbackPageId];
			if (fallbackPage.firstLodIndex < page.firstLodIndex)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry page fallback points to a higher-detail page in " << resolvedPath);
				return false;
			}
			if (page.fallbackPageId == page.pageId)
			{
				hasRenderableFallbackPage = true;
			}
		}
		if (!hasRenderableFallbackPage)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry page table does not declare a resident fallback page in " << resolvedPath);
			return false;
		}

		for (const VirtualGeometryDiagnosticRecord& diagnostic : mesh.virtualGeometryDiagnostics)
		{
			if (!ValidateVirtualGeometryStringRange(diagnostic.codeStringOffset, diagnostic.codeStringSize, mesh.virtualGeometryDiagnosticStrings.size(), resolvedPath, "diagnostic code")
				|| !ValidateVirtualGeometryStringRange(diagnostic.messageStringOffset, diagnostic.messageStringSize, mesh.virtualGeometryDiagnosticStrings.size(), resolvedPath, "diagnostic message"))
			{
				return false;
			}
		}

		AdaptiveVirtualMeshRejectionReason adaptiveReason = AdaptiveVirtualMeshRejectionReason::None;
		if (!ValidateAdaptiveVirtualGeometryPayloadTables(mesh, resolvedPath, adaptiveReason))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF adaptive virtual geometry rejected: " << ToString(adaptiveReason));
			return false;
		}

		return true;
	}

	std::uint64_t AddVirtualGeometryByteCount(std::uint64_t left, std::uint64_t right)
	{
		constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
		return left > max - right ? max : left + right;
	}

	std::uint64_t ResolveFullOwnerMeshPayloadBytes(const Mesh& mesh)
	{
		// The current mesh shader decodes the owner mesh buffers: packed
		// vertices, remap entries, optional skinning, primitive indices,
		// meshlet records, sections, and bounds. This helper keeps the exact
		// byte count beside the page-policy derivation so diagnostics do not
		// confuse page-budget metadata with the actual owner upload.
		std::uint64_t bytes = 0u;
		bytes = AddVirtualGeometryByteCount(bytes, static_cast<std::uint64_t>(mesh.meshletVertexData.size()));
		bytes = AddVirtualGeometryByteCount(bytes, static_cast<std::uint64_t>(mesh.meshletVertexRemapData.size()));
		bytes = AddVirtualGeometryByteCount(bytes, static_cast<std::uint64_t>(mesh.meshletSkinningData.size()));
		bytes = AddVirtualGeometryByteCount(bytes, static_cast<std::uint64_t>(mesh.meshletPrimitiveIndexData.size()));
		bytes = AddVirtualGeometryByteCount(bytes, static_cast<std::uint64_t>(mesh.meshletRecords.size()) * sizeof(MeshletRecord));
		bytes = AddVirtualGeometryByteCount(bytes, static_cast<std::uint64_t>(mesh.meshletSections.size()) * sizeof(MeshletSection));
		bytes = AddVirtualGeometryByteCount(bytes, static_cast<std::uint64_t>(mesh.meshletBounds.size()) * sizeof(MeshletBounds));
		return bytes;
	}

	void ResolveVirtualGeometryUploadPolicy(Mesh& mesh)
	{
		mesh.virtualGeometryPageStreamingReady = false;
		mesh.virtualGeometryFullOwnerPayloadBytes = 0u;
		mesh.virtualGeometryResidentHighDetailPageBytes = 0u;
		mesh.virtualGeometryResidentFallbackPageBytes = 0u;
		mesh.virtualGeometryProtectedFallbackBytes = 0u;
		mesh.virtualGeometryOwnerFallbackReason.clear();
		if (!mesh.hasVirtualGeometry || mesh.virtualGeometryPages.empty())
		{
			return;
		}

		mesh.virtualGeometryFullOwnerPayloadBytes = ResolveFullOwnerMeshPayloadBytes(mesh);
		for (const VirtualGeometryPageRecord& page : mesh.virtualGeometryPages)
		{
			const bool fallbackPage = page.pageId == page.fallbackPageId;
			if (fallbackPage)
			{
				mesh.virtualGeometryResidentFallbackPageBytes = AddVirtualGeometryByteCount(
					mesh.virtualGeometryResidentFallbackPageBytes,
					page.byteSize);
				mesh.virtualGeometryProtectedFallbackBytes = AddVirtualGeometryByteCount(
					mesh.virtualGeometryProtectedFallbackBytes,
					page.byteSize);
			}
			else
			{
				mesh.virtualGeometryResidentHighDetailPageBytes = AddVirtualGeometryByteCount(
					mesh.virtualGeometryResidentHighDetailPageBytes,
					page.byteSize);
			}
		}

		mesh.virtualGeometryPageStreamingReady =
			!mesh.virtualGeometryClusters.empty()
			&& !mesh.virtualGeometryClusterPageLinks.empty()
			&& mesh.virtualGeometryProtectedFallbackBytes > 0u;
		if (!mesh.virtualGeometryPageStreamingReady)
		{
			mesh.virtualGeometryOwnerFallbackReason = "virtual geometry page tables do not prove protected fallback coverage";
			return;
		}

		// A valid protected fallback page table is now enough to enter the
		// normal page-local rendering path. Any later owning-mesh upload must be
		// caused by a real page-policy failure, not by the old metadata-only
		// shader limitation.
		mesh.virtualGeometryOwnerFallbackReason.clear();
	}

	bool ParseVirtualGeometryPayload(
		const MeshHeader& header,
		const std::uint8_t* payload,
		std::size_t payloadSize,
		const std::filesystem::path& resolvedPath,
		Mesh& mesh)
	{
		const bool hasVirtualGeometryFlag = (header.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0u;
		if (!hasVirtualGeometryFlag)
		{
			return true;
		}

		if (header.virtualGeometryHeaderOffset == 0u
			|| header.virtualGeometryHeaderSize != sizeof(VirtualGeometryHeader)
			|| header.virtualGeometryPayloadGeneration != kVirtualGeometryPayloadGeneration)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF virtual geometry header metadata is stale or incomplete in " << resolvedPath
				<< ". Rebuild the mesh with the current virtual geometry cooker.");
			return false;
		}

		std::size_t virtualHeaderOffset = 0u;
		if (!ResolvePayloadRange(header, payloadSize, header.virtualGeometryHeaderOffset, sizeof(VirtualGeometryHeader), resolvedPath, "virtual geometry header", virtualHeaderOffset))
		{
			return false;
		}

		VirtualGeometryHeader virtualHeader{};
		std::memcpy(&virtualHeader, payload + virtualHeaderOffset, sizeof(VirtualGeometryHeader));
		if (virtualHeader.payloadGeneration != kVirtualGeometryPayloadGeneration
			|| virtualHeader.headerSize != sizeof(VirtualGeometryHeader)
			|| virtualHeader.detailLevelCount == 0u
			|| virtualHeader.sourceTriangleCount == 0u
			|| virtualHeader.sourceVertexCount == 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF virtual geometry payload root is invalid in " << resolvedPath
				<< ". Rebuild the mesh with the current virtual geometry cooker.");
			return false;
		}
		constexpr std::uint32_t kRequiredVirtualGeometryValidationFlags =
			kVirtualGeometryValidationFlagFallbackCoverageValidated
			| kVirtualGeometryValidationFlagClusterHierarchyValidated
			| kVirtualGeometryValidationFlagPageLinksValidated
			| kVirtualGeometryValidationFlagConeSafetyMetadataValidated;
		if ((virtualHeader.flags & kRequiredVirtualGeometryValidationFlags) != kRequiredVirtualGeometryValidationFlags)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF virtual geometry payload lacks current fallback, hierarchy, or page-link validation flags in " << resolvedPath
				<< ". Rebuild the mesh with the current virtual geometry cooker.");
			return false;
		}

		std::vector<VirtualGeometryDetailLevelRecord> detailLevels;
		std::vector<VirtualGeometryMaterialSectionRecord> materialSections;
		std::vector<VirtualGeometrySectionRangeRecord> sectionRanges;
		std::vector<VirtualGeometryCullableGroupRecord> cullableGroups;
		std::vector<VirtualGeometryPageRecord> pages;
		std::vector<VirtualGeometryRequestedTargetRecord> requestedTargets;
		std::vector<VirtualGeometrySkippedDetailRecord> skippedDetails;
		std::vector<VirtualGeometryClusterRecord> clusters;
		std::vector<VirtualGeometryClusterPageLinkRecord> clusterPageLinks;
		std::vector<VirtualGeometryConeSafetyRecord> coneSafety;
		std::vector<VirtualGeometryValidationFlagRecord> validationFlags;
		std::vector<VirtualGeometryDiagnosticRecord> diagnostics;

		if (!ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.detailLevelTableOffset, virtualHeader.detailLevelCount, resolvedPath, "virtual geometry detail table", detailLevels)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.materialSectionTableOffset, virtualHeader.materialSectionCount, resolvedPath, "virtual geometry material section table", materialSections)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.sectionRangeTableOffset, virtualHeader.sectionRangeCount, resolvedPath, "virtual geometry section range table", sectionRanges)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.cullableGroupTableOffset, virtualHeader.cullableGroupCount, resolvedPath, "virtual geometry cullable group table", cullableGroups)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.pageTableOffset, virtualHeader.pageCount, resolvedPath, "virtual geometry page table", pages)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.requestedTargetTableOffset, virtualHeader.requestedTargetCount, resolvedPath, "virtual geometry requested target table", requestedTargets)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.skippedDetailTableOffset, virtualHeader.skippedDetailCount, resolvedPath, "virtual geometry skipped detail table", skippedDetails)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.clusterTableOffset, virtualHeader.clusterCount, resolvedPath, "virtual geometry cluster table", clusters)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.clusterPageLinkTableOffset, virtualHeader.clusterPageLinkCount, resolvedPath, "virtual geometry cluster page link table", clusterPageLinks)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.coneSafetyTableOffset, virtualHeader.coneSafetyCount, resolvedPath, "virtual geometry cone safety table", coneSafety)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.validationFlagTableOffset, virtualHeader.validationFlagCount, resolvedPath, "virtual geometry validation flag table", validationFlags)
			|| !ReadVirtualGeometryTable(header, payload, payloadSize, virtualHeader.diagnosticTableOffset, virtualHeader.diagnosticRecordCount, resolvedPath, "virtual geometry diagnostic table", diagnostics))
		{
			return false;
		}

		for (const VirtualGeometryPageRecord& page : pages)
		{
			const bool offsetFits = page.byteOffset <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
			const bool sizeFits = page.byteSize <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
			std::size_t rangeEnd = 0u;
			if (!offsetFits
				|| !sizeFits
				|| page.byteOffset > static_cast<std::uint64_t>(payloadSize)
				|| !TryAdd(static_cast<std::size_t>(page.byteOffset), static_cast<std::size_t>(page.byteSize), rangeEnd)
				|| rangeEnd > payloadSize)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF virtual geometry page payload byte range overruns payload in " << resolvedPath);
				return false;
			}
		}

		std::string stringTable;
		if (virtualHeader.diagnosticStringSize != 0u)
		{
			std::size_t stringTableOffset = 0u;
			if (!ResolvePayloadRange(header, payloadSize, virtualHeader.diagnosticStringOffset, virtualHeader.diagnosticStringSize, resolvedPath, "virtual geometry string table", stringTableOffset))
			{
				return false;
			}
			stringTable.assign(reinterpret_cast<const char*>(payload + stringTableOffset), static_cast<std::size_t>(virtualHeader.diagnosticStringSize));
		}

		mesh.hasVirtualGeometry = true;
		mesh.virtualGeometryPayloadGeneration = virtualHeader.payloadGeneration;
		mesh.virtualGeometryHeader = virtualHeader;
		mesh.virtualGeometryDetailLevels = std::move(detailLevels);
		mesh.virtualGeometryMaterialSections = std::move(materialSections);
		mesh.virtualGeometrySectionRanges = std::move(sectionRanges);
		mesh.virtualGeometryCullableGroups = std::move(cullableGroups);
		mesh.virtualGeometryPages = std::move(pages);
		mesh.virtualGeometryRequestedTargets = std::move(requestedTargets);
		mesh.virtualGeometrySkippedDetails = std::move(skippedDetails);
		mesh.virtualGeometryClusters = std::move(clusters);
		mesh.virtualGeometryClusterPageLinks = std::move(clusterPageLinks);
		mesh.virtualGeometryConeSafety = std::move(coneSafety);
		mesh.virtualGeometryValidationFlags = std::move(validationFlags);
		mesh.virtualGeometryDiagnostics = std::move(diagnostics);
		mesh.virtualGeometryDiagnosticStrings = std::move(stringTable);

		const bool validPayload = ValidateVirtualGeometryPayloadTables(header, mesh, resolvedPath);
		if (validPayload)
		{
			ResolveVirtualGeometryUploadPolicy(mesh);
		}
		return validPayload;
	}

	// IsCompiledMeshPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsCompiledMeshPath(const std::filesystem::path& path)
	{
		std::string extension = path.extension().generic_string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return extension == ".temf";
	}

	// Parse a TEMF mesh file from disk and populate spans into the payload.
	// This helper avoids touching global caches so it can be shared by both
	// synchronous and asynchronous entry points.
	std::unique_ptr<Mesh> LoadMeshFile(ResourceManager& manager, const std::filesystem::path& resolvedPath)
	{
		const auto fileBytes = manager.ReadFileBytes(resolvedPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fileBytes.size() < sizeof(MeshHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF file is too small: " << resolvedPath);
			return nullptr;
		}

		MeshHeader header{};
		std::memcpy(&header, fileBytes.data(), sizeof(MeshHeader));

		std::uint32_t magic = 0;
		std::memcpy(&magic, header.magic, sizeof(magic));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (magic != kMeshMagicValue)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF magic mismatch in " << resolvedPath);
			return nullptr;
		}

		// Direct replacement intentionally rejects every non-current layout.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.version != kMeshCurrentVersion)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF version mismatch in " << resolvedPath
				<< ". Expected version " << kMeshCurrentVersion
				<< " with no backward compatibility path.");
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.layout.attributeCount > kMaxVertexAttributes)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex attribute count exceeds limit in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.layout.strideBytes != kMeshletPackedVertexStrideBytes)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF stride mismatch in " << resolvedPath
				<< ". Expected fixed " << kMeshletPackedVertexStrideBytes
				<< "-byte packed vertex layout.");
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.layout.attributeCount != 4u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF packed vertex layout must expose exactly 4 attributes in " << resolvedPath);
			return nullptr;
		}

		const VertexAttribute expectedLayout[4] = {
			{
				static_cast<std::uint32_t>(AttributeSemantic::Position),
				static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm),
				0u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::Normal),
				static_cast<std::uint32_t>(AttributeFormat::Oct32),
				4u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::Tangent),
				static_cast<std::uint32_t>(AttributeFormat::Oct32),
				8u},
			{
				static_cast<std::uint32_t>(AttributeSemantic::TexCoord0),
				static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm),
				12u}};

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t layoutIndex = 0; layoutIndex < 4u; ++layoutIndex)
		{
			const VertexAttribute& current = header.layout.attributes[layoutIndex];
			const VertexAttribute& expected = expectedLayout[layoutIndex];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (current.semantic != expected.semantic
				|| current.format != expected.format
				|| current.offsetBytes != expected.offsetBytes)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"TEMF packed attribute mismatch at index " << layoutIndex << " in " << resolvedPath);
				return nullptr;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.headerSize > fileBytes.size())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF headerSize exceeds file length for " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.headerSize < sizeof(MeshHeader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF headerSize is smaller than MeshHeader in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.meshletVertexCount == 0
			|| header.meshletVertexRemapCount == 0
			|| header.meshletPrimitiveIndexCount == 0
			|| header.meshletCount == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet counts (vertex/remap/primitive/table) must all be non-zero in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.meshletSectionCount == 0)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF requires at least one meshlet section in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((header.meshletVertexDataOffset % kMeshletVertexDataAlignmentBytes) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex stream offset must be 16-byte aligned in " << resolvedPath);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.meshletSkinningDataCount != 0)
		{
			if ((header.meshletSkinningDataOffset % kMeshletIndexDataAlignmentBytes) != 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF skinning stream offset must be aligned in " << resolvedPath);
				return nullptr;
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (header.meshletSkinningDataCount != header.meshletVertexCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF skinning count must match vertex count in " << resolvedPath);
				return nullptr;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((header.meshletVertexRemapDataOffset % kMeshletIndexDataAlignmentBytes) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex remap stream offset must be 4-byte aligned in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((header.meshletPrimitiveIndexDataOffset % kMeshletIndexDataAlignmentBytes) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF primitive stream offset must be 4-byte aligned in " << resolvedPath);
			return nullptr;
		}

		// Primitive entries are meshlet-local uint8 elements and must remain triangle-aligned.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((header.meshletPrimitiveIndexCount % 3u) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet primitive index count must be a multiple of 3 in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if ((header.meshletFlags & ~(kMeshletHeaderFlagHasBounds | kMeshletHeaderFlagHasVirtualGeometry)) != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshletFlags has unsupported bits in " << resolvedPath);
			return nullptr;
		}

		const MeshletRemapEncoding remapEncoding = static_cast<MeshletRemapEncoding>(header.meshletVertexRemapEncoding);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (remapEncoding != MeshletRemapEncoding::Uint16 && remapEncoding != MeshletRemapEncoding::Uint32)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF remap encoding is invalid in " << resolvedPath);
			return nullptr;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (remapEncoding == MeshletRemapEncoding::Uint16
			&& header.meshletVertexCount > static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF remap encoding is u16 but vertex count exceeds 65535 in " << resolvedPath);
			return nullptr;
		}

		const bool boundsPresent = (header.meshletFlags & kMeshletHeaderFlagHasBounds) != 0u;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (boundsPresent)
		{
			if (header.meshletBoundsCount != header.meshletCount || header.meshletBoundsOffset == 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF bounds flag is set but bounds count/offset is inconsistent in " << resolvedPath);
				return nullptr;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if ((header.meshletBoundsOffset % kMeshletIndexDataAlignmentBytes) != 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF bounds payload offset must be 4-byte aligned in " << resolvedPath);
				return nullptr;
			}
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (header.meshletBoundsCount != 0u || header.meshletBoundsOffset != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF bounds payload must be fully absent when bounds flag is not set in " << resolvedPath);
			return nullptr;
		}

		std::size_t vertexBytes = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryMultiply(static_cast<std::size_t>(header.layout.strideBytes), static_cast<std::size_t>(header.meshletVertexCount), vertexBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex payload size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t skinningBytes = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryMultiply(static_cast<std::size_t>(header.meshletSkinningDataCount), 8ULL, skinningBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF skinning payload size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t vertexRemapBytes = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryMultiply(
			static_cast<std::size_t>(header.meshletVertexRemapCount),
			static_cast<std::size_t>(GetRemapElementSizeBytes(remapEncoding)),
			vertexRemapBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF vertex remap payload size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t primitiveIndexBytes = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryMultiply(static_cast<std::size_t>(header.meshletPrimitiveIndexCount), static_cast<std::size_t>(kMeshletPrimitiveIndexSizeBytes), primitiveIndexBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF primitive index payload size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t meshletTableBytes = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryMultiply(static_cast<std::size_t>(header.meshletCount), sizeof(MeshletRecord), meshletTableBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t meshletSectionBytes = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryMultiply(static_cast<std::size_t>(header.meshletSectionCount), sizeof(MeshletSection), meshletSectionBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet section table size overflow in " << resolvedPath);
			return nullptr;
		}

		std::size_t meshletBoundsBytes = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryMultiply(static_cast<std::size_t>(header.meshletBoundsCount), sizeof(MeshletBounds), meshletBoundsBytes))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet bounds table size overflow in " << resolvedPath);
			return nullptr;
		}

		const std::size_t payloadOffset = header.headerSize;
		auto mesh = std::make_unique<Mesh>();
		mesh->header = header;

		mesh->storage.assign(fileBytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset), fileBytes.end());

		const std::size_t payloadSize = mesh->storage.size();
		const std::uint8_t* payload = mesh->storage.data();

		std::size_t vertexPayloadOffset = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!ResolvePayloadRange(header, payloadSize, header.meshletVertexDataOffset, vertexBytes, resolvedPath, "meshlet vertex data", vertexPayloadOffset))
		{
			return nullptr;
		}

		std::size_t vertexRemapPayloadOffset = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!ResolvePayloadRange(header, payloadSize, header.meshletVertexRemapDataOffset, vertexRemapBytes, resolvedPath, "meshlet vertex remap data", vertexRemapPayloadOffset))
		{
			return nullptr;
		}

		std::size_t primitivePayloadOffset = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!ResolvePayloadRange(header, payloadSize, header.meshletPrimitiveIndexDataOffset, primitiveIndexBytes, resolvedPath, "meshlet primitive index data", primitivePayloadOffset))
		{
			return nullptr;
		}

		std::size_t meshletTableOffset = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!ResolvePayloadRange(header, payloadSize, header.meshletTableOffset, meshletTableBytes, resolvedPath, "meshlet table", meshletTableOffset))
		{
			return nullptr;
		}

		std::size_t meshletSectionOffset = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!ResolvePayloadRange(header, payloadSize, header.meshletSectionOffset, meshletSectionBytes, resolvedPath, "meshlet section table", meshletSectionOffset))
		{
			return nullptr;
		}

		std::size_t meshletBoundsOffset = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.meshletBoundsCount != 0 && !ResolvePayloadRange(header, payloadSize, header.meshletBoundsOffset, meshletBoundsBytes, resolvedPath, "meshlet bounds table", meshletBoundsOffset))
		{
			return nullptr;
		}

		std::size_t skinningPayloadOffset = 0;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.meshletSkinningDataCount != 0 && !ResolvePayloadRange(header, payloadSize, header.meshletSkinningDataOffset, skinningBytes, resolvedPath, "meshlet skinning data", skinningPayloadOffset))
		{
			return nullptr;
		}

		mesh->meshletVertexData = std::span<const std::uint8_t>(payload + vertexPayloadOffset, vertexBytes);
		mesh->meshletVertexRemapData = std::span<const std::uint8_t>(payload + vertexRemapPayloadOffset, vertexRemapBytes);
		mesh->meshletSkinningData = (header.meshletSkinningDataCount != 0) 
			? std::span<const std::uint8_t>(payload + skinningPayloadOffset, skinningBytes)
			: std::span<const std::uint8_t>{};
		mesh->meshletPrimitiveIndexData = std::span<const std::uint8_t>(payload + primitivePayloadOffset, primitiveIndexBytes);

		mesh->meshletRecords.resize(header.meshletCount);
		std::memcpy(mesh->meshletRecords.data(), payload + meshletTableOffset, meshletTableBytes);

		mesh->meshletSections.resize(header.meshletSectionCount);
		std::memcpy(mesh->meshletSections.data(), payload + meshletSectionOffset, meshletSectionBytes);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header.meshletBoundsCount != 0)
		{
			mesh->meshletBounds.resize(header.meshletBoundsCount);
			std::memcpy(mesh->meshletBounds.data(), payload + meshletBoundsOffset, meshletBoundsBytes);

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::size_t boundsIndex = 0; boundsIndex < mesh->meshletBounds.size(); ++boundsIndex)
			{
				MeshletBounds& bounds = mesh->meshletBounds[boundsIndex];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!IsFiniteFloat(bounds.center[0])
					|| !IsFiniteFloat(bounds.center[1])
					|| !IsFiniteFloat(bounds.center[2])
					|| !IsFiniteFloat(bounds.radius)
					|| bounds.radius < 0.0f)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet bounds[" << boundsIndex << "] has invalid sphere data in " << resolvedPath);
					return nullptr;
				}

				const glm::vec3 center(bounds.center[0], bounds.center[1], bounds.center[2]);
				const glm::vec3 extent(bounds.radius);
				const auto meshletBounds = TerraEngine::Scene::MakeAABB(center - extent, center + extent, TerraEngine::Scene::SpatialBoundsSource::Mesh);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (mesh->hasLocalBounds)
				{
					mesh->localBoundsMin[0] = std::min(mesh->localBoundsMin[0], meshletBounds.min.x);
					mesh->localBoundsMin[1] = std::min(mesh->localBoundsMin[1], meshletBounds.min.y);
					mesh->localBoundsMin[2] = std::min(mesh->localBoundsMin[2], meshletBounds.min.z);
					mesh->localBoundsMax[0] = std::max(mesh->localBoundsMax[0], meshletBounds.max.x);
					mesh->localBoundsMax[1] = std::max(mesh->localBoundsMax[1], meshletBounds.max.y);
					mesh->localBoundsMax[2] = std::max(mesh->localBoundsMax[2], meshletBounds.max.z);
				}
				else
				{
					mesh->hasLocalBounds = true;
					mesh->localBoundsMin[0] = meshletBounds.min.x;
					mesh->localBoundsMin[1] = meshletBounds.min.y;
					mesh->localBoundsMin[2] = meshletBounds.min.z;
					mesh->localBoundsMax[0] = meshletBounds.max.x;
					mesh->localBoundsMax[1] = meshletBounds.max.y;
					mesh->localBoundsMax[2] = meshletBounds.max.z;
				}

				// Cone validation is fail-safe: invalid payload disables backface culling
				// for that meshlet instead of rejecting the whole asset.
				const bool coneInvalid = !IsFiniteFloat(bounds.coneApex[0])
					|| !IsFiniteFloat(bounds.coneApex[1])
					|| !IsFiniteFloat(bounds.coneApex[2])
					|| !IsFiniteFloat(bounds.coneAxis[0])
					|| !IsFiniteFloat(bounds.coneAxis[1])
					|| !IsFiniteFloat(bounds.coneAxis[2])
					|| !IsFiniteFloat(bounds.coneCutoff)
					|| !IsFiniteFloat(bounds.coneApexOffset);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (coneInvalid)
				{
					TERRAENGINE_LOG(LogLevel::Warning, "TEMF meshlet bounds[" << boundsIndex << "] has invalid cone data; disabling cone backface culling for this meshlet.");
					DisableConeData(bounds);
					continue;
				}

				const float axisLengthSq = bounds.coneAxis[0] * bounds.coneAxis[0]
					+ bounds.coneAxis[1] * bounds.coneAxis[1]
					+ bounds.coneAxis[2] * bounds.coneAxis[2];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (axisLengthSq < 1e-8f)
				{
					DisableConeData(bounds);
					continue;
				}

				const float invAxisLength = 1.0f / std::sqrt(axisLengthSq);
				bounds.coneAxis[0] *= invAxisLength;
				bounds.coneAxis[1] *= invAxisLength;
				bounds.coneAxis[2] *= invAxisLength;
			}
		}
		else
		{
			// Missing meshlet bounds should not make object-level culling unsafe.
			// The renderer will include fallback records so visibility remains
			// correct until the asset pipeline produces precise bounds.
			mesh->hasLocalBounds = false;
		}

		const std::uint8_t* primitivePayload = mesh->meshletPrimitiveIndexData.data();

		std::vector<std::uint8_t> primitiveCoverage(static_cast<std::size_t>(header.meshletPrimitiveIndexCount), 0u);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t meshletIndex = 0; meshletIndex < mesh->meshletRecords.size(); ++meshletIndex)
		{
			const MeshletRecord& meshlet = mesh->meshletRecords[meshletIndex];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (meshlet.vertexCount == 0
				|| meshlet.primitiveIndexCount == 0
				|| meshlet.vertexCount > kMeshletMaxVertices
				|| (meshlet.primitiveIndexCount % 3u) != 0u)
			{
				// Count validation is kept strict so malformed files fail before GPU upload.
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet[" << meshletIndex << "] has invalid vertex/primitive counts in " << resolvedPath);
				return nullptr;
			}

			std::size_t meshletVertexEnd = 0;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!TryAdd(static_cast<std::size_t>(meshlet.vertexOffset), static_cast<std::size_t>(meshlet.vertexCount), meshletVertexEnd)
				|| meshletVertexEnd > header.meshletVertexRemapCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet[" << meshletIndex << "] remap range overruns in " << resolvedPath);
				return nullptr;
			}

			std::size_t meshletPrimitiveEnd = 0;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!TryAdd(static_cast<std::size_t>(meshlet.primitiveIndexOffset), static_cast<std::size_t>(meshlet.primitiveIndexCount), meshletPrimitiveEnd)
				|| meshletPrimitiveEnd > header.meshletPrimitiveIndexCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet[" << meshletIndex << "] primitive range overruns in " << resolvedPath);
				return nullptr;
			}

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (std::size_t primitive = static_cast<std::size_t>(meshlet.primitiveIndexOffset); primitive < meshletPrimitiveEnd; ++primitive)
			{
				if (primitiveCoverage[primitive] != 0u)
				{
					// Overlap means one primitive element would belong to two meshlets, which is invalid.
					TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table has overlapping primitive ranges in " << resolvedPath);
					return nullptr;
				}

				const std::uint8_t localVertex = primitivePayload[primitive];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (localVertex >= meshlet.vertexCount)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMF primitive stream references out-of-range local vertex in meshlet[" << meshletIndex << "] for " << resolvedPath);
					return nullptr;
				}

				primitiveCoverage[primitive] = 1u;
			}
		}

		const bool hasPrimitiveUnderrun = std::any_of(primitiveCoverage.begin(), primitiveCoverage.end(), [](std::uint8_t value) { return value == 0u; });
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hasPrimitiveUnderrun)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet table overrun/underrun due to uncovered primitive indices in " << resolvedPath);
			return nullptr;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t sectionIndex = 0; sectionIndex < mesh->meshletSections.size(); ++sectionIndex)
		{
			const MeshletSection& section = mesh->meshletSections[sectionIndex];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (section.meshletCount == 0)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet section[" << sectionIndex << "] has zero meshlets in " << resolvedPath);
				return nullptr;
			}

			std::size_t sectionEnd = 0;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!TryAdd(static_cast<std::size_t>(section.meshletOffset), static_cast<std::size_t>(section.meshletCount), sectionEnd) || sectionEnd > header.meshletCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMF meshlet section[" << sectionIndex << "] references out-of-range meshlets in " << resolvedPath);
				return nullptr;
			}
		}

		if (!ParseVirtualGeometryPayload(header, payload, payloadSize, resolvedPath, *mesh))
		{
			return nullptr;
		}

		const VirtualMeshValidationResult virtualGeometryValidation = ValidateVirtualGeometryPayloadMetadata(header, *mesh, resolvedPath);
		if (VirtualMeshValidationRejected(virtualGeometryValidation))
		{
			return nullptr;
		}
		for (const std::string& warning : virtualGeometryValidation.warnings)
		{
			TERRAENGINE_LOG(LogLevel::Warning, warning);
		}

		return mesh;
	}
}

void ResourceManager::CacheLoadedMeshBounds(const TerraEngine::UUID& id, const Mesh* mesh)
{
	if (id == TerraEngine::EMPTY_UUID || mesh == nullptr)
	{
		return;
	}

	TerraEngine::Scene::SpatialAABB bounds = TerraEngine::Scene::MakeFallbackBounds(glm::vec3(0.0f), 1.0f);
	if (mesh->hasLocalBounds)
	{
		bounds = TerraEngine::Scene::MakeAABB(
			glm::vec3(mesh->localBoundsMin[0], mesh->localBoundsMin[1], mesh->localBoundsMin[2]),
			glm::vec3(mesh->localBoundsMax[0], mesh->localBoundsMax[1], mesh->localBoundsMax[2]),
			TerraEngine::Scene::SpatialBoundsSource::Mesh);
	}

	std::unique_lock lock(m_MeshLocalBoundsMutex);
	m_MeshLocalBoundsCache[id] = bounds;
}

void ResourceManager::CacheLoadedMeshBounds(const TerraEngine::UUID&, const void*)
{
	// Non-mesh resources have no spatial bounds metadata. This overload keeps
	// generic load finalization simple without forcing materials, textures, or
	// shaders to know about mesh-specific residency behavior.
}

// ResourceManager::LoadMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::LoadMesh(const std::filesystem::path& path)
{
	if (!IsCompiledMeshPath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping source mesh load until compiled TEMF is available: " << path);
		return TerraEngine::EMPTY_UUID;
	}

	return LoadOrGetCached<Mesh>(path, meshCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMeshFile(*this, resolvedPath);
	});
}

// ResourceManager::LoadMeshAsync is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::shared_future<TerraEngine::UUID> ResourceManager::LoadMeshAsync(const std::filesystem::path& path)
{
	if (!IsCompiledMeshPath(path))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping async source mesh load until compiled TEMF is available: " << path);
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}

	return LoadOrSchedule<Mesh>(path, meshCache, [this](const std::filesystem::path& resolvedPath) {
		return LoadMeshFile(*this, resolvedPath);
	});
}

// ResourceManager::ReloadMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::ReloadMesh(const TerraEngine::UUID& id)
{
	std::string key;
	{
		std::shared_lock lock(meshCache.mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (const auto pathIt = meshCache.idToPath.find(id); pathIt != meshCache.idToPath.end())
		{
			key = pathIt->second;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (key.empty())
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Cannot reload mesh yet: UUID " << ToHexString(id) << " has no compiled TEMF path registered.");
		return;
	}

	const std::filesystem::path resolvedPath(key);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!IsCompiledMeshPath(resolvedPath))
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Skipping mesh reload from non-TEMF path: " << resolvedPath);
		return;
	}

	TERRAENGINE_LOG(LogLevel::Debug, "Reloading mesh from " << resolvedPath);
	auto mesh = LoadMeshFile(*this, resolvedPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (mesh)
	{
		CacheLoadedMeshBounds(id, mesh.get());
		std::unique_lock lock(meshCache.mutex);
		meshCache.resources[id] = std::move(mesh);
	}
}

// ResourceManager::GetMeshLocalBounds is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::Scene::SpatialAABB ResourceManager::GetMeshLocalBounds(const TerraEngine::UUID& id)
{
	{
		std::shared_lock lock(m_MeshLocalBoundsMutex);
		if (const auto boundsIt = m_MeshLocalBoundsCache.find(id); boundsIt != m_MeshLocalBoundsCache.end())
		{
			return boundsIt->second;
		}
	}

	// Spatial-index refresh must not load the full mesh payload just to find a
	// broadphase box. Until a mesh has been demanded by rendering/import, use a
	// deterministic fallback bound and let visibility streaming decide whether
	// the heavy RAM/VRAM payload is needed.
	return TerraEngine::Scene::MakeFallbackBounds(glm::vec3(0.0f), 1.0f);
}
