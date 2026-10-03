// ResourceManagerMesh.cpp â€” Mesh Resource Management Implementation
//
// Implements the specialized logic for loading and managing mesh geometry resources.
// It handles the parsing of high-performance mesh formats, including meshlet generation and vertex data quantization.
// This module provides the geometric data required by the renderer for both standard and GPU-driven drawing paths.

#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineAnimationFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceStreaming.h"
#include "Resource/VirtualGeometryCodec.h"
#include "Runtime/ContentMount/ContentMount.h"

namespace
{
	bool ValidateReconstructedMeshletRuntime(Mesh& mesh, const std::filesystem::path& path);

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

	VirtualMeshValidationResult ValidateVirtualGeometryPayloadMetadata(const MeshletRuntimeLayout& header, const Mesh& mesh, const std::filesystem::path& resolvedPath)
	{
		VirtualMeshValidationResult result;
		const bool hasVirtualGeometryFlag = (header.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0u;
		if (!hasVirtualGeometryFlag)
		{
			return result;
		}

		// The initial validation stage defines the atomic boundary before the loader
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

	bool RejectDeformableValidation(
		AnimationResourceValidationResult& result,
		TerraEngine::Animation::AnimationStatus status,
		std::string code,
		std::string message)
	{
		result.status = status;
		result.diagnosticCode = std::move(code);
		result.message = std::move(message);
		return false;
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

	bool ValidateDirectResidentDetailLevels(
		const MeshletRuntimeLayout& layout,
		const std::vector<VirtualGeometryDetailLevelRecord>& details,
		const std::filesystem::path& resolvedPath)
	{
		// A direct cook keeps all LOD spans resident in one immutable payload. The
		// ranges must therefore be ordered, contiguous, and cover the packed meshlet
		// table exactly; otherwise a per-view selection could address another LOD or
		// silently draw several levels at once.
		if (details.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF direct-resident mesh has no LOD range table in " << resolvedPath
				<< ". Reimport the source model with the current mesh cooker.");
			return false;
		}

		std::uint32_t expectedMeshletOffset = 0u;
		for (std::size_t detailIndex = 0u; detailIndex < details.size(); ++detailIndex)
		{
			const VirtualGeometryDetailLevelRecord& detail = details[detailIndex];
			if (detail.lodIndex != detailIndex
				|| detail.meshletOffset != expectedMeshletOffset
				|| detail.meshletCount == 0u
				|| detail.primitiveCount == 0u
				|| !ValidateVirtualGeometryAddRange(
					detail.meshletOffset,
					detail.meshletCount,
					layout.meshletCount,
					resolvedPath,
					"direct-resident LOD meshlet range")
				|| !IsFiniteFloat(detail.geometricError)
				|| detail.geometricError < 0.0f
				|| !IsFiniteFloat(detail.targetRatio)
				|| detail.targetRatio <= 0.0f)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"TEMF direct-resident LOD metadata is invalid in " << resolvedPath);
				return false;
			}

			for (int component = 0; component < 3; ++component)
			{
				if (!IsFiniteFloat(detail.boundsMin[component])
					|| !IsFiniteFloat(detail.boundsMax[component])
					|| detail.boundsMin[component] > detail.boundsMax[component])
				{
					TERRAENGINE_LOG(LogLevel::Error,
						"TEMF direct-resident LOD bounds are invalid in " << resolvedPath);
					return false;
				}
			}
			expectedMeshletOffset += detail.meshletCount;
		}

		if (expectedMeshletOffset != layout.meshletCount)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMF direct-resident LOD ranges do not cover the packed meshlet table in "
				<< resolvedPath);
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

	bool ValidateVirtualGeometryPayloadTables(const MeshletRuntimeLayout& header, const Mesh& mesh, const std::filesystem::path& resolvedPath)
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

	void ResolveVirtualGeometryUploadPolicy(
		Mesh& mesh,
		std::span<const TemfPageDescriptor> canonicalPages,
		std::span<const TemfBlockDescriptor> canonicalBlocks)
	{
		mesh.virtualGeometryPageStreamingReady = false;
		mesh.virtualGeometryFullOwnerPayloadBytes = 0u;
		mesh.virtualGeometryResidentHighDetailPageBytes = 0u;
		mesh.virtualGeometryResidentFallbackPageBytes = 0u;
		mesh.virtualGeometryProtectedFallbackBytes = 0u;
		mesh.virtualGeometryOwnerFallbackReason.clear();
		mesh.virtualGeometryPhysicalPageStates.clear();
		mesh.virtualGeometryPhysicalPagePublicationGenerations.clear();
		mesh.virtualGeometryPhysicalPagePublicationGeneration = 0u;
		if (!mesh.hasVirtualGeometry || mesh.virtualGeometryPages.empty())
		{
			return;
		}

		mesh.virtualGeometryFullOwnerPayloadBytes = ResolveFullOwnerMeshPayloadBytes(mesh);
		mesh.virtualGeometryPhysicalPageStates.resize(
			mesh.virtualGeometryPages.size(),
			static_cast<std::uint32_t>(GeometryPageState::NotResident));
		mesh.virtualGeometryPhysicalPagePublicationGenerations.resize(
			mesh.virtualGeometryPages.size(), 1u);
		mesh.virtualGeometryPhysicalPagePublicationGeneration = 1u;
		for (const VirtualGeometryPageRecord& page : mesh.virtualGeometryPages)
		{
			const bool fallbackPage = page.pageId == page.fallbackPageId;
			if (fallbackPage)
			{
				// The fallback range is the only physical page payload guaranteed by
				// this loader. Pin it before render snapshots can observe the mesh so
				// resident-ancestor selection always has coverage while IO starts.
				mesh.virtualGeometryPhysicalPageStates[page.pageId] =
					static_cast<std::uint32_t>(GeometryPageState::Pinned);
				mesh.virtualGeometryResidentFallbackPageBytes = AddVirtualGeometryByteCount(
					mesh.virtualGeometryResidentFallbackPageBytes,
					page.byteSize);
				mesh.virtualGeometryProtectedFallbackBytes = AddVirtualGeometryByteCount(
					mesh.virtualGeometryProtectedFallbackBytes,
					page.byteSize);
			}
			// Non-fallback page bytes are catalogued but not published resident.
			// A later typed streaming transaction owns the state transition.
		}

		// Count equality is not an identity proof: asset-wide streams may occupy
		// additional physical pages. Every selectable hierarchy page must instead
		// map to exactly one canonical page containing its independently decodable
		// deformation payload. This is the same page ID later requested by render
		// selection, resource streaming, and Vulkan deformation publication.
		bool completePageIdentityMapping = true;
		for (const VirtualGeometryPageRecord& virtualPage : mesh.virtualGeometryPages)
		{
			const auto first = std::find_if(canonicalPages.begin(), canonicalPages.end(),
				[&virtualPage](const TemfPageDescriptor& page)
				{ return page.logicalPageId == virtualPage.pageId; });
			if (first == canonicalPages.end()
				|| std::find_if(std::next(first), canonicalPages.end(),
					[&virtualPage](const TemfPageDescriptor& page)
					{ return page.logicalPageId == virtualPage.pageId; }) != canonicalPages.end()
				|| first->firstBlockIndex > canonicalBlocks.size()
				|| first->blockCount > canonicalBlocks.size() - first->firstBlockIndex)
			{
				completePageIdentityMapping = false;
				break;
			}
			bool ownsDeformablePayload = false;
			for (std::uint32_t blockOffset = 0u; blockOffset < first->blockCount;
				++blockOffset)
			{
				const TemfBlockDescriptor& block =
					canonicalBlocks[first->firstBlockIndex + blockOffset];
				if (block.logicalPageId == virtualPage.pageId
					&& block.streamKind == static_cast<std::uint16_t>(
						TemfStreamKind::ProceduralUpdate)
					&& block.attributeMask
						== kTemfMetadataDeformableDemandedPageBlobs)
				{
					ownsDeformablePayload = true;
					break;
				}
			}
			if (!ownsDeformablePayload)
			{
				completePageIdentityMapping = false;
				break;
			}
		}
		if (!completePageIdentityMapping)
		{
			for (const VirtualGeometryPageRecord& page : mesh.virtualGeometryPages)
			{
				if (page.pageId != page.fallbackPageId)
				{
					mesh.virtualGeometryPhysicalPageStates[page.pageId] =
						static_cast<std::uint32_t>(GeometryPageState::Resident);
				}
			}
			mesh.virtualGeometryOwnerFallbackReason =
				"canonical production pages do not provide complete hierarchy page identities; using the validated owning-mesh payload";
			return;
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
		// Production loading has one fail-closed entry point. In particular, do
		// not inspect a historical extension header first: that would make a
		// stale payload appear usable before its canonical tables are proven.
		TerraEngine::Resource::TemfProductionValidationLimits productionLimits{};
		// The CPU reference path links meshoptimizer, so it advertises precisely
		// that production capability while leaving unknown requirements fail-closed.
		productionLimits.availableCapabilities = kVirtualGeometryCodecCapabilityMeshoptimizer;
		const TerraEngine::Resource::TemfProductionValidationResult validated =
			TerraEngine::Resource::ValidateTemfProductionFile(fileBytes, productionLimits);
		if (!validated.IsComplete())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMFPRD1 recook required for " << resolvedPath << ": " << validated.diagnostic);
			return nullptr;
		}

		auto productionMesh = std::make_unique<Mesh>();
		productionMesh->sourcePath = resolvedPath.string();
		productionMesh->sourceLogicalPath = ContentMount::NormalizeLogicalPath(
			resolvedPath.lexically_relative(manager.GetAssetRoot()).generic_string());
		productionMesh->storage = fileBytes;
		struct DecodedProductionBlock
		{
			TemfBlockDescriptor descriptor{};
			// Blocks decode only when a runtime stream actually consumes them. The
			// validated encoded range remains in Mesh::storage until that point, so
			// optional or future streams do not add load-time decode work.
			std::optional<std::vector<std::uint8_t>> bytes;
		};
		std::vector<DecodedProductionBlock> decodedBlocks;
		std::size_t decodedStorageBytes = 0u;
		for (const TemfBlockDescriptor& descriptor : validated.directory.blocks)
		{
			if (descriptor.decodedBytes > std::numeric_limits<std::size_t>::max() - decodedStorageBytes)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": decoded block storage exceeds process limits.");
				return nullptr;
			}
			decodedStorageBytes += static_cast<std::size_t>(descriptor.decodedBytes);
			decodedBlocks.push_back({ descriptor, std::nullopt });
		}
		productionMesh->decodedProductionStorage.reserve(decodedStorageBytes);
		const auto decodeBlock = [&](DecodedProductionBlock& block) {
			if (block.bytes.has_value()) return true;
			const auto encoded = std::span<const std::uint8_t>(productionMesh->storage).subspan(
				static_cast<std::size_t>(block.descriptor.encodedPayloadOffset),
				static_cast<std::size_t>(block.descriptor.encodedBytes));
			auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock(
				{ block.descriptor, encoded, validated.directory.quantizationDomains,
					productionLimits.availableCapabilities }, productionLimits);
			if (!decoded.IsComplete())
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 block decode failed for "
					<< resolvedPath << ": " << decoded.diagnostic);
				return false;
			}
			block.bytes = std::move(decoded.bytes);
			return true;
		};
		std::vector<std::pair<std::uint64_t, std::span<const std::uint8_t>>> decodedStreamCache;
		const auto copyStream = [&](TemfStreamKind kind, std::uint64_t metadataSubKind = 0u) {
			const std::uint64_t cacheKey = (static_cast<std::uint64_t>(kind) << 56u) | metadataSubKind;
			for (const auto& cached : decodedStreamCache)
			{
				if (cached.first == cacheKey)
				{
					return cached.second;
				}
			}
			const std::size_t begin = productionMesh->decodedProductionStorage.size();
			for (DecodedProductionBlock& block : decodedBlocks)
			{
				if (block.descriptor.streamKind == static_cast<std::uint16_t>(kind)
					&& block.descriptor.attributeMask == metadataSubKind)
				{
					if (!decodeBlock(block))
					{
						return std::span<const std::uint8_t>{};
					}
					productionMesh->decodedProductionStorage.insert(productionMesh->decodedProductionStorage.end(),
						block.bytes->begin(), block.bytes->end());
				}
			}
			const std::span<const std::uint8_t> copied = std::span<const std::uint8_t>(productionMesh->decodedProductionStorage).subspan(
				begin, productionMesh->decodedProductionStorage.size() - begin);
			decodedStreamCache.push_back({ cacheKey, copied });
			return copied;
		};
		const auto copyRecords = [&](std::uint64_t metadataSubKind, auto& destination) {
			using Record = typename std::remove_reference_t<decltype(destination)>::value_type;
			const std::span<const std::uint8_t> bytes = copyStream(TemfStreamKind::Metadata, metadataSubKind);
			if (bytes.empty() || bytes.size() % sizeof(Record) != 0u)
			{
				return false;
			}
			destination.resize(bytes.size() / sizeof(Record));
			std::memcpy(destination.data(), bytes.data(), bytes.size());
			return true;
		};
		const std::span<const std::uint8_t> runtimeBytes = copyStream(TemfStreamKind::Metadata, kTemfMetadataRuntimeLayout);
		const std::span<const std::uint8_t> positionBytes =
			copyStream(TemfStreamKind::Position);
		const std::span<const std::uint8_t> normalTangentBytes =
			copyStream(TemfStreamKind::NormalTangent);
		const std::span<const std::uint8_t> textureBytes =
			copyStream(TemfStreamKind::TextureColor);
		if (runtimeBytes.size() != sizeof(TemfRuntimeDecodeMetadata)
			|| positionBytes.empty()
			|| normalTangentBytes.empty()
			|| textureBytes.empty()
			|| copyStream(TemfStreamKind::VertexRemapStableIdentity).empty()
			|| copyStream(TemfStreamKind::Topology).empty())
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": required static runtime blocks are missing.");
			return nullptr;
		}
		TemfRuntimeDecodeMetadata runtimeMetadata{};
		std::memcpy(&runtimeMetadata, runtimeBytes.data(), sizeof(runtimeMetadata));
		productionMesh->runtimeLayout.magic[0] = 'T';
		productionMesh->runtimeLayout.magic[1] = 'E';
		productionMesh->runtimeLayout.magic[2] = 'M';
		productionMesh->runtimeLayout.magic[3] = 'F';
		productionMesh->runtimeLayout.version = runtimeMetadata.formatVersion;
		productionMesh->runtimeLayout.headerSize = sizeof(MeshletRuntimeLayout);
		productionMesh->runtimeLayout.meshletVertexCount = runtimeMetadata.meshletVertexCount;
		productionMesh->runtimeLayout.meshletVertexRemapCount = runtimeMetadata.meshletVertexRemapCount;
		productionMesh->runtimeLayout.meshletPrimitiveIndexCount = runtimeMetadata.meshletPrimitiveIndexCount;
		productionMesh->runtimeLayout.meshletCount = runtimeMetadata.meshletCount;
		productionMesh->runtimeLayout.meshletSectionCount = runtimeMetadata.meshletSectionCount;
		productionMesh->runtimeLayout.meshletBoundsCount = runtimeMetadata.meshletBoundsCount;
		productionMesh->runtimeLayout.meshletVertexRemapEncoding = runtimeMetadata.meshletVertexRemapEncoding;
		productionMesh->runtimeLayout.meshletFlags = runtimeMetadata.meshletFlags;
		productionMesh->runtimeLayout.layout = runtimeMetadata.layout;
		productionMesh->runtimeLayout.quantization = runtimeMetadata.quantization;
		const std::size_t vertexCount =
			productionMesh->runtimeLayout.meshletVertexCount;
		if (positionBytes.size() != vertexCount * 6u
			|| normalTangentBytes.size() != vertexCount * 6u
			|| textureBytes.size() != vertexCount * 4u
			|| productionMesh->runtimeLayout.layout.strideBytes
				!= kMeshletPackedVertexStrideBytes)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"TEMFPRD1 recook required for " << resolvedPath
				<< ": attribute production blocks disagree with the fixed vertex ABI.");
			return nullptr;
		}
		productionMesh->reconstructedVertexData.resize(
			vertexCount * kMeshletPackedVertexStrideBytes);
		for (std::size_t vertexIndex = 0u;
			vertexIndex < vertexCount; ++vertexIndex)
		{
			std::uint8_t* destination =
				productionMesh->reconstructedVertexData.data()
				+ vertexIndex * kMeshletPackedVertexStrideBytes;
			std::memcpy(destination, positionBytes.data() + vertexIndex * 6u, 6u);
			std::memcpy(
				destination + 6u,
				normalTangentBytes.data() + vertexIndex * 6u,
				6u);
			std::memcpy(
				destination + 12u,
				textureBytes.data() + vertexIndex * 4u,
				4u);
		}
		productionMesh->meshletVertexData =
			productionMesh->reconstructedVertexData;
		productionMesh->meshletVertexRemapData = copyStream(TemfStreamKind::VertexRemapStableIdentity);
		productionMesh->meshletPrimitiveIndexData = copyStream(TemfStreamKind::Topology);
		const MeshletRemapEncoding remapEncoding = static_cast<MeshletRemapEncoding>(productionMesh->runtimeLayout.meshletVertexRemapEncoding);
		const std::uint32_t remapStride = GetRemapElementSizeBytes(remapEncoding);
		if (productionMesh->meshletVertexRemapData.size() % remapStride != 0u)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": remap block is mis-sized.");
			return nullptr;
		}
		productionMesh->meshletVertexRemapDecoded.resize(productionMesh->meshletVertexRemapData.size() / remapStride);
		for (std::size_t index = 0u; index < productionMesh->meshletVertexRemapDecoded.size(); ++index)
		{
			if (remapEncoding == MeshletRemapEncoding::Uint16)
			{
				std::uint16_t value{};
				std::memcpy(&value, productionMesh->meshletVertexRemapData.data() + index * remapStride, sizeof(value));
				productionMesh->meshletVertexRemapDecoded[index] = value;
			}
			else
			{
				std::memcpy(&productionMesh->meshletVertexRemapDecoded[index], productionMesh->meshletVertexRemapData.data() + index * remapStride, sizeof(std::uint32_t));
			}
		}
		if (!copyRecords(kTemfMetadataMeshletRecords, productionMesh->meshletRecords)
			|| !copyRecords(kTemfMetadataMeshletSections, productionMesh->meshletSections)
			|| !copyRecords(kTemfMetadataMeshletBounds, productionMesh->meshletBounds))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": meshlet runtime records are missing.");
			return nullptr;
		}
		if (productionMesh->meshletVertexData.size() != static_cast<std::size_t>(productionMesh->runtimeLayout.meshletVertexCount) * productionMesh->runtimeLayout.layout.strideBytes
			|| productionMesh->meshletPrimitiveIndexData.size() != productionMesh->runtimeLayout.meshletPrimitiveIndexCount
			|| productionMesh->meshletVertexRemapDecoded.size() != productionMesh->runtimeLayout.meshletVertexRemapCount
			|| productionMesh->meshletRecords.size() != productionMesh->runtimeLayout.meshletCount
			|| productionMesh->meshletSections.size() != productionMesh->runtimeLayout.meshletSectionCount
			|| productionMesh->meshletBounds.size() != productionMesh->runtimeLayout.meshletBoundsCount)
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": runtime block counts disagree with decode metadata.");
			return nullptr;
		}
		if (!ValidateReconstructedMeshletRuntime(*productionMesh, resolvedPath))
		{
			return nullptr;
		}
		productionMesh->hasVirtualGeometry = (validated.directory.header.representationFlags & kTemfRepresentationVirtualGeometry) != 0u;
		productionMesh->virtualGeometryPayloadGeneration = validated.directory.header.formatVersion;
		if (productionMesh->hasVirtualGeometry)
		{
			const std::span<const std::uint8_t> virtualHeaderBytes = copyStream(TemfStreamKind::Metadata, kTemfMetadataVirtualHeader);
			if (virtualHeaderBytes.size() != sizeof(VirtualGeometryHeader))
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": virtual hierarchy root is missing.");
				return nullptr;
			}
			std::memcpy(&productionMesh->virtualGeometryHeader, virtualHeaderBytes.data(), sizeof(VirtualGeometryHeader));
			const bool hierarchyComplete = copyRecords(kTemfMetadataVirtualDetailLevels, productionMesh->virtualGeometryDetailLevels)
				&& copyRecords(kTemfMetadataVirtualMaterialSections, productionMesh->virtualGeometryMaterialSections)
				&& copyRecords(kTemfMetadataVirtualSectionRanges, productionMesh->virtualGeometrySectionRanges)
				&& copyRecords(kTemfMetadataVirtualCullableGroups, productionMesh->virtualGeometryCullableGroups)
				&& copyRecords(kTemfMetadataVirtualPages, productionMesh->virtualGeometryPages)
				&& copyRecords(kTemfMetadataVirtualClusters, productionMesh->virtualGeometryClusters)
				&& copyRecords(kTemfMetadataVirtualClusterPageLinks, productionMesh->virtualGeometryClusterPageLinks)
				&& copyRecords(kTemfMetadataVirtualConeSafety, productionMesh->virtualGeometryConeSafety)
				&& copyRecords(kTemfMetadataVirtualValidationFlags, productionMesh->virtualGeometryValidationFlags);
			if (!hierarchyComplete)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": virtual hierarchy blocks are incomplete.");
				return nullptr;
			}
			copyRecords(kTemfMetadataVirtualRequestedTargets, productionMesh->virtualGeometryRequestedTargets);
			copyRecords(kTemfMetadataVirtualSkippedDetails, productionMesh->virtualGeometrySkippedDetails);
			copyRecords(kTemfMetadataVirtualDiagnostics, productionMesh->virtualGeometryDiagnostics);
			const std::span<const std::uint8_t> diagnostics = copyStream(TemfStreamKind::Metadata, kTemfMetadataVirtualDiagnosticStrings);
			productionMesh->virtualGeometryDiagnosticStrings.assign(reinterpret_cast<const char*>(diagnostics.data()), diagnostics.size());
			// Reuse the semantic hierarchy validator after block reconstruction.
			// It validates ranges, fallback coverage, finite bounds, and ownership
			// before any partial mesh reaches the resource cache.
			if (!ValidateVirtualGeometryPayloadTables(productionMesh->runtimeLayout, *productionMesh, resolvedPath))
			{
				return nullptr;
			}
			ResolveVirtualGeometryUploadPolicy(
				*productionMesh,
				validated.directory.pages,
				validated.directory.blocks);
		}
		else
		{
			// Direct-resident meshes still retain a tiny LOD directory. It contains no
			// virtual pages or hierarchy and is consumed only to select one range from
			// the already-uploaded owner mesh payload.
			if (!copyRecords(
					kTemfMetadataVirtualDetailLevels,
					productionMesh->virtualGeometryDetailLevels)
				|| !ValidateDirectResidentDetailLevels(
					productionMesh->runtimeLayout,
					productionMesh->virtualGeometryDetailLevels,
					resolvedPath))
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"TEMFPRD1 recook required for " << resolvedPath
					<< ": direct-resident LOD ranges are missing or invalid.");
				return nullptr;
			}
		}
		if ((validated.directory.header.representationFlags
			& kTemfRepresentationDeformableStreams) != 0u)
		{
			// A deformable asset has no hidden extension.  Each collection below is
			// independently decoded from the production directory before the mesh is
			// published, which keeps corrupt metadata from becoming live state.
			const std::span<const std::uint8_t> rootBytes =
				copyStream(TemfStreamKind::Metadata, kTemfMetadataDeformableRoot);
			if (rootBytes.size() != sizeof(TemfDeformableMetadata)
				|| !copyRecords(
					kTemfMetadataDeformablePalettes,
					productionMesh->deformablePalettes))
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": deformable metadata collections are incomplete.");
				return nullptr;
			}
			TemfDeformableMetadata root{};
			std::memcpy(&root, rootBytes.data(), sizeof(root));
			const bool demandedPagesRequired =
				productionMesh->hasVirtualGeometry;
			const bool morphRecordsComplete =
				root.morphTargetCount == 0u
					? copyStream(
						TemfStreamKind::Metadata,
						kTemfMetadataDeformableMorphs).empty()
					: copyRecords(
						kTemfMetadataDeformableMorphs,
						productionMesh->deformableMorphs);
			const bool demandedRecordsComplete =
				!demandedPagesRequired
				|| (copyRecords(
						kTemfMetadataDeformableDemandedPageEntries,
						productionMesh->deformableDemandedPageEntries)
					&& copyRecords(
						kTemfMetadataDeformableDemandedPageRoots,
						productionMesh->deformableDemandedPageRoots)
					&& copyRecords(
						kTemfMetadataDeformableDemandedPageBlobs,
						productionMesh->deformableDemandedPageBlobs));
			if (!morphRecordsComplete || !demandedRecordsComplete)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"TEMFPRD1 recook required for " << resolvedPath
					<< ": deformable optional collections disagree with their declared counts.");
				return nullptr;
			}
			const std::span<const std::uint8_t> directoryBytes = copyStream(
				TemfStreamKind::Metadata, kTemfMetadataDeformableDemandedPageDirectory);
			const std::span<const std::uint8_t> dependencyBytes = copyStream(
				TemfStreamKind::Metadata, kTemfMetadataDeformableDemandedPageDependencies);
			const std::span<const std::uint8_t> paletteJointBytes = copyStream(
				TemfStreamKind::Metadata, kTemfMetadataDeformablePaletteJoints);
			const std::span<const std::uint8_t> vertexPaletteBytes = copyStream(
				TemfStreamKind::Metadata, kTemfMetadataDeformableVertexPalettes);
			const std::span<const std::uint8_t> morphPositionBytes = copyStream(
				TemfStreamKind::Metadata, kTemfMetadataDeformableMorphPositions);
			const std::span<const std::uint8_t> morphNormalBytes = copyStream(
				TemfStreamKind::Metadata, kTemfMetadataDeformableMorphNormals);
			const std::span<const std::uint8_t> morphTangentBytes = copyStream(
				TemfStreamKind::Metadata, kTemfMetadataDeformableMorphTangents);
			productionMesh->meshletSkinningData = copyStream(TemfStreamKind::Skinning);
			if ((demandedPagesRequired
					? directoryBytes.size()
						!= sizeof(TemfDeformableDemandedPageDirectory)
					: !directoryBytes.empty())
				|| dependencyBytes.size() % sizeof(TemfDeformableDemandedPageDependency) != 0u
				|| paletteJointBytes.size() % sizeof(std::uint32_t) != 0u
				|| vertexPaletteBytes.size() % sizeof(std::uint32_t) != 0u)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": deformable collection stride is invalid.");
				return nullptr;
			}
			if (demandedPagesRequired)
			{
				std::memcpy(
					&productionMesh->deformableDemandedPageDirectory,
					directoryBytes.data(),
					sizeof(productionMesh->deformableDemandedPageDirectory));
			}
			productionMesh->deformableDemandedPageDependencies.resize(dependencyBytes.size() / sizeof(TemfDeformableDemandedPageDependency));
			std::memcpy(productionMesh->deformableDemandedPageDependencies.data(), dependencyBytes.data(), dependencyBytes.size());
			productionMesh->deformablePaletteJoints.resize(paletteJointBytes.size() / sizeof(std::uint32_t));
			std::memcpy(productionMesh->deformablePaletteJoints.data(), paletteJointBytes.data(), paletteJointBytes.size());
			productionMesh->deformableVertexPaletteIndices.resize(vertexPaletteBytes.size() / sizeof(std::uint32_t));
			std::memcpy(productionMesh->deformableVertexPaletteIndices.data(), vertexPaletteBytes.data(), vertexPaletteBytes.size());
			const std::size_t expectedMorphBytes = static_cast<std::size_t>(root.morphTargetCount)
				* root.skinningVertexCount * sizeof(float) * 3u;
			if (root.skinningVertexCount != productionMesh->runtimeLayout.meshletVertexCount
				|| root.palettePartitionCount != productionMesh->deformablePalettes.size()
				|| root.morphTargetCount != productionMesh->deformableMorphs.size()
				|| productionMesh->meshletSkinningData.size() != static_cast<std::size_t>(root.skinningVertexCount) * kTemfSkinningStrideBytes
				|| productionMesh->deformableVertexPaletteIndices.size() != root.skinningVertexCount
				|| morphPositionBytes.size() != expectedMorphBytes
				|| morphNormalBytes.size() != expectedMorphBytes
				|| morphTangentBytes.size() != expectedMorphBytes
				|| root.demandedPageCount != productionMesh->deformableDemandedPageEntries.size()
				|| (demandedPagesRequired
					&& (productionMesh->deformableDemandedPageDirectory.version
							!= 1u
						|| productionMesh->deformableDemandedPageDirectory.entryCount
							!= productionMesh->deformableDemandedPageEntries.size()
						|| productionMesh->deformableDemandedPageDirectory.rootCount
							!= productionMesh->deformableDemandedPageRoots.size()
						|| productionMesh->deformableDemandedPageDirectory.dependencyCount
							!= productionMesh->deformableDemandedPageDependencies.size()
						|| productionMesh->deformableDemandedPageDirectory.blobCount
							!= productionMesh->deformableDemandedPageBlobs.size()
						|| root.skeletonCompatibilityFingerprint
							!= productionMesh->deformableDemandedPageDirectory.skeletonCompatibilityFingerprint)))
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": deformable counts or fingerprints disagree.");
				return nullptr;
			}
			std::size_t paletteJointOffset = 0u;
			for (const TemfDeformablePaletteRecord& palette : productionMesh->deformablePalettes)
			{
				if (palette.firstJointIndex != paletteJointOffset
					|| palette.jointCount == 0u
					|| palette.jointCount > kTemfMaximumPaletteJoints
					|| palette.jointCount > productionMesh->deformablePaletteJoints.size() - paletteJointOffset)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": deformable palette range is invalid.");
					return nullptr;
				}
				for (std::uint32_t joint = 0u; joint < palette.jointCount; ++joint)
				{
					const std::uint32_t sourceJoint = productionMesh->deformablePaletteJoints[paletteJointOffset + joint];
					if (sourceJoint >= root.sourceSkeletonJointCount
						|| (joint != 0u && sourceJoint <= productionMesh->deformablePaletteJoints[paletteJointOffset + joint - 1u]))
					{
						TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": deformable palette joints are not canonical.");
						return nullptr;
					}
				}
				paletteJointOffset += palette.jointCount;
			}
			if (paletteJointOffset != productionMesh->deformablePaletteJoints.size())
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": deformable palette joint coverage is incomplete.");
				return nullptr;
			}
			for (std::size_t index = 0u; index < productionMesh->deformableVertexPaletteIndices.size(); ++index)
			{
				if (productionMesh->deformableVertexPaletteIndices[index] >= productionMesh->deformablePalettes.size())
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": a deformable vertex references an invalid palette.");
					return nullptr;
				}
			}
			productionMesh->deformableMorphPositions.resize(
				morphPositionBytes.size() / sizeof(std::array<float, 3>));
			productionMesh->deformableMorphNormals.resize(
				morphNormalBytes.size() / sizeof(std::array<float, 3>));
			productionMesh->deformableMorphTangents.resize(
				morphTangentBytes.size() / sizeof(std::array<float, 3>));
			if (!morphPositionBytes.empty())
			{
				std::memcpy(productionMesh->deformableMorphPositions.data(),
					morphPositionBytes.data(), morphPositionBytes.size());
				std::memcpy(productionMesh->deformableMorphNormals.data(),
					morphNormalBytes.data(), morphNormalBytes.size());
				std::memcpy(productionMesh->deformableMorphTangents.data(),
					morphTangentBytes.data(), morphTangentBytes.size());
			}
			for (const TemfDeformableMorphRecord& morph : productionMesh->deformableMorphs)
			{
				if (morph.stableTargetId == 0u
					|| morph.deltaCount != root.skinningVertexCount
					|| morph.firstDeltaIndex
						> productionMesh->deformableMorphPositions.size()
					|| morph.deltaCount
						> productionMesh->deformableMorphPositions.size()
							- morph.firstDeltaIndex)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": deformable morph metadata is invalid.");
					return nullptr;
				}
			}
			for (std::size_t entryIndex = 0u; entryIndex < productionMesh->deformableDemandedPageEntries.size(); ++entryIndex)
			{
				const TemfDeformableDemandedPageEntry& entry = productionMesh->deformableDemandedPageEntries[entryIndex];
				if (entry.pageId == kVirtualGeometryInvalidIndex
					|| entry.dependencyOffset > productionMesh->deformableDemandedPageDependencies.size()
					|| entry.dependencyCount > productionMesh->deformableDemandedPageDependencies.size() - entry.dependencyOffset)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page dependency range is invalid.");
					return nullptr;
				}
				for (std::size_t prior = 0u; prior < entryIndex; ++prior)
				{
					if (productionMesh->deformableDemandedPageEntries[prior].pageId == entry.pageId)
					{
						TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page identifiers are not unique.");
						return nullptr;
					}
				}
				for (std::size_t dependency = entry.dependencyOffset;
					dependency < static_cast<std::size_t>(entry.dependencyOffset) + entry.dependencyCount; ++dependency)
				{
					const TemfDeformableDemandedPageDependency& record = productionMesh->deformableDemandedPageDependencies[dependency];
					if (record.pageId != entry.pageId || record.requiredPageId == entry.pageId || record.reserved != 0u)
					{
						TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page dependency is invalid.");
						return nullptr;
					}
				}
			}
			for (const TemfDeformableDemandedPageRoot& rootPage : productionMesh->deformableDemandedPageRoots)
			{
				const bool exists = std::any_of(productionMesh->deformableDemandedPageEntries.begin(),
					productionMesh->deformableDemandedPageEntries.end(), [rootPage](const TemfDeformableDemandedPageEntry& entry) {
						return entry.pageId == rootPage.pageId;
					});
				if (!exists || rootPage.reserved != 0u)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page root is invalid.");
					return nullptr;
				}
			}
			std::vector<DecodedProductionBlock*> pageBlocks;
			for (DecodedProductionBlock& block : decodedBlocks)
			{
				if (block.descriptor.streamKind == static_cast<std::uint16_t>(TemfStreamKind::ProceduralUpdate)
					&& block.descriptor.attributeMask == kTemfMetadataDeformableDemandedPageBlobs)
				{
					pageBlocks.push_back(&block);
				}
			}
			if (pageBlocks.size() != productionMesh->deformableDemandedPageBlobs.size())
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page blob count is invalid.");
				return nullptr;
			}
			productionMesh->deformableDemandedPagePayloads.reserve(pageBlocks.size());
			productionMesh->deformableDemandedPageSections.reserve(pageBlocks.size());
			for (std::size_t index = 0u; index < pageBlocks.size(); ++index)
			{
				const TemfDeformableDemandedPageBlob& record = productionMesh->deformableDemandedPageBlobs[index];
				DecodedProductionBlock& block = *pageBlocks[index];
				if (!decodeBlock(block))
				{
					return nullptr;
				}
				const std::vector<std::uint8_t>& blockBytes = *block.bytes;
				if (record.blobId != index + 1u || record.pageId != productionMesh->deformableDemandedPageEntries[index].pageId
					|| record.contentGeneration != productionMesh->deformableDemandedPageEntries[index].contentGeneration
					|| record.decodedIntegrity != block.descriptor.decodedIntegrity
					|| record.decodedBytes != blockBytes.size()
					|| blockBytes.size() < sizeof(TemfDeformableDemandedPagePayload))
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page blob integrity is invalid.");
					return nullptr;
				}
				TemfDeformableDemandedPagePayload payload{};
				std::memcpy(&payload, blockBytes.data(), sizeof(payload));
				const std::size_t sectionTableBytes = static_cast<std::size_t>(payload.sectionCount)
					* sizeof(TemfDeformablePageSection);
				if (payload.sectionCount == 0u
					|| payload.sectionTableOffset != sizeof(TemfDeformableDemandedPagePayload)
					|| payload.decodedByteSize != blockBytes.size()
					|| sectionTableBytes > blockBytes.size() - payload.sectionTableOffset)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page section table is invalid.");
					return nullptr;
				}
				std::vector<TemfDeformablePageSection> sections(payload.sectionCount);
				std::memcpy(sections.data(), blockBytes.data() + payload.sectionTableOffset, sectionTableBytes);
				std::uint32_t requiredSections = 0u;
				const std::size_t dataBegin = payload.sectionTableOffset + sectionTableBytes;
				std::vector<std::pair<std::uint32_t, std::uint32_t>> sectionRanges;
				sectionRanges.reserve(sections.size());
				for (const TemfDeformablePageSection& section : sections)
				{
					const std::uint32_t kind = static_cast<std::uint32_t>(section.kind);
					if (kind > static_cast<std::uint32_t>(TemfDeformablePageSectionKind::ErrorSamples)
						|| section.elementCount == 0u || section.elementStride == 0u
						|| section.byteSize != static_cast<std::uint64_t>(section.elementCount) * section.elementStride
						|| section.relativeByteOffset < dataBegin
						|| section.relativeByteOffset
							% kTemfDeformablePageSectionAlignment != 0u
						|| section.relativeByteOffset > blockBytes.size()
						|| section.byteSize > blockBytes.size() - section.relativeByteOffset)
					{
						TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page section range is invalid.");
						return nullptr;
					}
					requiredSections |= 1u << kind;
					sectionRanges.emplace_back(section.relativeByteOffset,
						section.relativeByteOffset + section.byteSize);
				}
				std::sort(sectionRanges.begin(), sectionRanges.end());
				const auto alignPageSection = [](std::size_t value) {
					return (value + kTemfDeformablePageSectionAlignment - 1u)
						& ~(static_cast<std::size_t>(
							kTemfDeformablePageSectionAlignment) - 1u);
				};
				std::size_t consumedOffset = dataBegin;
				for (const auto& range : sectionRanges)
				{
					const std::size_t expectedSectionOffset =
						alignPageSection(consumedOffset);
					const bool paddingIsZero =
						expectedSectionOffset <= blockBytes.size()
						&& std::all_of(
							blockBytes.begin() + static_cast<std::ptrdiff_t>(consumedOffset),
							blockBytes.begin() + static_cast<std::ptrdiff_t>(expectedSectionOffset),
							[](std::uint8_t byte) { return byte == 0u; });
					if (range.first != expectedSectionOffset
						|| range.second < range.first || !paddingIsZero)
					{
						TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page section alignment or padding is invalid.");
						return nullptr;
					}
					consumedOffset = range.second;
				}
				const std::size_t expectedBlobSize =
					alignPageSection(consumedOffset);
				const bool trailingPaddingIsZero =
					expectedBlobSize <= blockBytes.size()
					&& std::all_of(
						blockBytes.begin() + static_cast<std::ptrdiff_t>(consumedOffset),
						blockBytes.end(),
						[](std::uint8_t byte) { return byte == 0u; });
				if (expectedBlobSize != blockBytes.size()
					|| !trailingPaddingIsZero)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page trailing alignment is invalid.");
					return nullptr;
				}
				const std::uint32_t kRequiredPageSections = root.morphTargetCount == 0u
					? ((1u << 13u) - 1u) & ~((1u << 8u) | (1u << 9u))
					: (1u << 13u) - 1u;
				if (requiredSections != kRequiredPageSections)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page payload is missing a required runtime section.");
					return nullptr;
				}
				if (payload.pageId != record.pageId
					|| payload.clusterId != productionMesh->deformableDemandedPageEntries[index].clusterId
					|| payload.materialSectionId != productionMesh->deformableDemandedPageEntries[index].materialSectionId
					|| payload.contentGeneration != record.contentGeneration
					|| payload.meshPayloadGeneration != productionMesh->deformableDemandedPageDirectory.meshPayloadGeneration
					|| payload.skeletonCompatibilityFingerprint != root.skeletonCompatibilityFingerprint)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 recook required for " << resolvedPath << ": demanded-page payload identity is invalid.");
					return nullptr;
				}
				productionMesh->deformableDemandedPageSections.push_back(std::move(sections));
				productionMesh->deformableDemandedPagePayloads.push_back(blockBytes);
			}
			if (demandedPagesRequired)
			{
				// Only the canonical descriptor-owned page collections are
				// published.  Runtime consumers read these validated vectors
				// directly; no historical directory or side table is rebuilt.
				productionMesh->hasDeformableDemandedPages = true;
			}
			productionMesh->hasDeformablePayload = true;
			productionMesh->deformablePayloadGeneration =
				demandedPagesRequired
					? productionMesh->deformableDemandedPageDirectory.meshPayloadGeneration
					: (validated.directory.header.generationIdentity == 0u
						? 1u
						: validated.directory.header.generationIdentity);
			productionMesh->deformableMetadata = root;
			productionMesh->runtimeLayout.meshletSkinningDataCount =
				root.skinningVertexCount;
		}
		return productionMesh;
	}

	// Canonical blocks have already passed structural and integrity validation.
	// This second pass proves that reconstructed meshlet data is safe to publish
	// to render workers: no record can address outside its decoded stream.
	bool ValidateReconstructedMeshletRuntime(Mesh& mesh, const std::filesystem::path& path)
	{
		const MeshletRuntimeLayout& layout = mesh.runtimeLayout;
		std::vector<std::uint8_t> primitiveCoverage(layout.meshletPrimitiveIndexCount, 0u);
		for (std::size_t index = 0u; index < mesh.meshletRecords.size(); ++index)
		{
			const MeshletRecord& record = mesh.meshletRecords[index];
			std::size_t vertexEnd = 0u;
			std::size_t primitiveEnd = 0u;
			if (record.vertexCount == 0u || record.vertexCount > kMeshletMaxVertices
				|| record.primitiveIndexCount == 0u || record.primitiveIndexCount % 3u != 0u
				|| !TryAdd(record.vertexOffset, record.vertexCount, vertexEnd) || vertexEnd > layout.meshletVertexRemapCount
				|| !TryAdd(record.primitiveIndexOffset, record.primitiveIndexCount, primitiveEnd) || primitiveEnd > layout.meshletPrimitiveIndexCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 meshlet record is invalid in " << path);
				return false;
			}
			for (std::size_t primitive = record.primitiveIndexOffset; primitive < primitiveEnd; ++primitive)
			{
				if (primitiveCoverage[primitive] != 0u || mesh.meshletPrimitiveIndexData[primitive] >= record.vertexCount)
				{
					TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 meshlet primitive coverage or local index is invalid in " << path);
					return false;
				}
				primitiveCoverage[primitive] = 1u;
			}
		}
		if (std::any_of(primitiveCoverage.begin(), primitiveCoverage.end(), [](std::uint8_t value) { return value == 0u; }))
		{
			TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 meshlet primitive stream is not fully covered in " << path);
			return false;
		}
		for (const MeshletSection& section : mesh.meshletSections)
		{
			std::size_t sectionEnd = 0u;
			if (section.meshletCount == 0u || !TryAdd(section.meshletOffset, section.meshletCount, sectionEnd)
				|| sectionEnd > layout.meshletCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 meshlet section range is invalid in " << path);
				return false;
			}
		}
		mesh.hasLocalBounds = false;
		for (const MeshletBounds& bounds : mesh.meshletBounds)
		{
			const bool finite = IsFiniteFloat(bounds.center[0]) && IsFiniteFloat(bounds.center[1]) && IsFiniteFloat(bounds.center[2])
				&& IsFiniteFloat(bounds.radius) && bounds.radius >= 0.0f
				&& IsFiniteFloat(bounds.coneApex[0]) && IsFiniteFloat(bounds.coneApex[1]) && IsFiniteFloat(bounds.coneApex[2])
				&& IsFiniteFloat(bounds.coneAxis[0]) && IsFiniteFloat(bounds.coneAxis[1]) && IsFiniteFloat(bounds.coneAxis[2])
				&& IsFiniteFloat(bounds.coneCutoff) && IsFiniteFloat(bounds.coneApexOffset);
			if (!finite)
			{
				TERRAENGINE_LOG(LogLevel::Error, "TEMFPRD1 meshlet bounds or cone is non-finite in " << path);
				return false;
			}
			const float minimum[3] = { bounds.center[0] - bounds.radius, bounds.center[1] - bounds.radius, bounds.center[2] - bounds.radius };
			const float maximum[3] = { bounds.center[0] + bounds.radius, bounds.center[1] + bounds.radius, bounds.center[2] + bounds.radius };
			for (std::size_t axis = 0u; axis < 3u; ++axis)
			{
				if (!mesh.hasLocalBounds) { mesh.localBoundsMin[axis] = minimum[axis]; mesh.localBoundsMax[axis] = maximum[axis]; }
				else { mesh.localBoundsMin[axis] = std::min(mesh.localBoundsMin[axis], minimum[axis]); mesh.localBoundsMax[axis] = std::max(mesh.localBoundsMax[axis], maximum[axis]); }
			}
			mesh.hasLocalBounds = true;
		}
		return true;
	}
}

AnimationResourceValidationResult ResourceManager::ValidateDeformableMeshFile(
	const std::filesystem::path& path,
	std::uint32_t expectedSkeletonJointCount,
	TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint) const
{
	using TerraEngine::Animation::AnimationStatus;
	AnimationResourceValidationResult result;
	result.assetType = AssetType::Mesh;
	result.resolvedPath = ResolveAssetPath(path);
	result.expectedCompatibilityFingerprint = expectedCompatibilityFingerprint;
	if (!IsCompiledMeshPath(path))
	{
		RejectDeformableValidation(result, AnimationStatus::Unsupported,
			"DeformableSourceFormatRejected",
			"Runtime deformation loading accepts only cooked .temf files and never parses a glTF or FBX source package.");
		return result;
	}
	// The authoritative production decoder is also the validator: it checks
	// descriptor ranges, codec integrity, typed deformable streams, demanded
	// page dependencies, and blob identities without publishing a cache entry.
	// This intentionally gives legacy appended payloads no fallback path.
	std::unique_ptr<Mesh> decoded = LoadMeshFile(
		const_cast<ResourceManager&>(*this), result.resolvedPath);
	if (decoded == nullptr || !decoded->hasDeformablePayload)
	{
		RejectDeformableValidation(result, AnimationStatus::InvalidEnvelope,
			"CanonicalDeformableStreamsMissing",
			"The TEMFPRD1 file has no complete canonical deformable descriptor set. Recook the mesh.");
		return result;
	}
	result.detectedMajorVersion = decoded->runtimeLayout.version;
	result.detectedMinorVersion = 1u;
	result.deformableVertexCount = decoded->deformableMetadata.skinningVertexCount;
	result.palettePartitionCount = decoded->deformableMetadata.palettePartitionCount;
	result.morphTargetCount = decoded->deformableMetadata.morphTargetCount;
	result.sourceSkeletonJointCount = decoded->deformableMetadata.sourceSkeletonJointCount;
	result.actualCompatibilityFingerprint = decoded->deformableMetadata.skeletonCompatibilityFingerprint;
	result.compatibilityFingerprintPresent = result.actualCompatibilityFingerprint != 0u;
	if (expectedSkeletonJointCount != 0u
		&& result.sourceSkeletonJointCount != expectedSkeletonJointCount)
	{
		RejectDeformableValidation(result, AnimationStatus::IncompatibleAsset,
			"DeformableSkeletonJointCountMismatch",
			"The canonical deformable root was cooked for a different skeleton joint count.");
		return result;
	}
	if (expectedCompatibilityFingerprint != 0u
		&& result.actualCompatibilityFingerprint != expectedCompatibilityFingerprint)
	{
		RejectDeformableValidation(result, AnimationStatus::IncompatibleAsset,
			"DeformableSkeletonFingerprintMismatch",
			"The canonical deformable root was cooked against a different skeleton generation.");
		return result;
	}
	result.status = AnimationStatus::Ok;
	result.diagnosticCode = "CanonicalDeformableMeshValid";
	result.message = "Canonical TEMFPRD1 deformable streams, demanded-page descriptors, dependencies, and integrity records are valid.";
	return result;

}

TerraEngine::UUID ResourceManager::LoadDeformableMesh(
	const std::filesystem::path& path,
	std::uint32_t expectedSkeletonJointCount,
	TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint)
{
	const AnimationResourceValidationResult validation = ValidateDeformableMeshFile(
		path, expectedSkeletonJointCount, expectedCompatibilityFingerprint);
	if (!validation.Passed())
	{
		TERRAENGINE_LOG(LogLevel::Error,
			validation.diagnosticCode << ": " << validation.message
			<< " path=" << validation.resolvedPath);
		return TerraEngine::EMPTY_UUID;
	}

	const TerraEngine::UUID id = LoadMesh(path);
	if (id == TerraEngine::EMPTY_UUID)
	{
		return TerraEngine::EMPTY_UUID;
	}
	std::shared_lock lock(meshCache.mutex);
	return meshCache.resources.contains(id) ? id : TerraEngine::EMPTY_UUID;
}

std::optional<TerraEngine::Scene::SpatialAABB> ResourceManager::ComputeLoadedMeshBounds(const Mesh* mesh)
{
	if (!mesh) return {};
	TerraEngine::Scene::SpatialAABB bounds = TerraEngine::Scene::MakeFallbackBounds(glm::vec3(0.0f), 1.0f);
	if (mesh->hasLocalBounds)
	{
		bounds = TerraEngine::Scene::MakeAABB(
			glm::vec3(mesh->localBoundsMin[0], mesh->localBoundsMin[1], mesh->localBoundsMin[2]),
			glm::vec3(mesh->localBoundsMax[0], mesh->localBoundsMax[1], mesh->localBoundsMax[2]),
			TerraEngine::Scene::SpatialBoundsSource::Mesh);
	}

	if (mesh->hasDeformablePayload)
	{
		// The static mesh box is insufficient for CPU broadphase queries: a skin
		// or morph can cross a view plane before compute publishes current page
		// bounds. Canonical deformable validation proves this envelope is finite;
		// union it with bind pose so editor selection and inactive-pose frames
		// remain conservative as well.
		const glm::vec3 deformationMinimum(
			mesh->deformableMetadata.deformationBoundsMin[0],
			mesh->deformableMetadata.deformationBoundsMin[1],
			mesh->deformableMetadata.deformationBoundsMin[2]);
		const glm::vec3 deformationMaximum(
			mesh->deformableMetadata.deformationBoundsMax[0],
			mesh->deformableMetadata.deformationBoundsMax[1],
			mesh->deformableMetadata.deformationBoundsMax[2]);
		const TerraEngine::Scene::SpatialAABB deformationBounds =
			TerraEngine::Scene::MakeAABB(
				deformationMinimum,
				deformationMaximum,
				TerraEngine::Scene::SpatialBoundsSource::Mesh);
		if (deformationBounds.IsUsable())
		{
			bounds = bounds.IsUsable()
				? TerraEngine::Scene::MakeAABB(
					glm::min(bounds.min, deformationBounds.min),
					glm::max(bounds.max, deformationBounds.max),
					TerraEngine::Scene::SpatialBoundsSource::Mesh)
				: deformationBounds;
		}
	}

	return bounds;
}

void ResourceManager::CacheLoadedMeshBounds(const TerraEngine::UUID& id, const Mesh* mesh)
{
	// Ordinary bounds updates share the pure calculation with staged publication.
	if (id == TerraEngine::EMPTY_UUID) return;
	const auto bounds = ComputeLoadedMeshBounds(mesh);
	if (!bounds) return;
	std::unique_lock lock(m_MeshLocalBoundsMutex);
	m_MeshLocalBoundsCache[id] = *bounds;
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

// ResourceManager::RegisterMesh publishes one already-validated generated mesh.
// Terrain preview workers use this after completing canonical meshlet packing;
// no half-built payload is ever visible to renderer collection.
TerraEngine::UUID ResourceManager::RegisterMesh(const std::string& cacheKey, std::unique_ptr<Mesh> mesh)
{
	// Bounds and accounting now publish inside the same accepted transaction.
	return RegisterGenerated(cacheKey, meshCache, std::move(mesh));
}

void ResourceManager::ReloadMesh(const TerraEngine::UUID& id)
{
	ReloadCached<Mesh>(id, meshCache, [this](const std::filesystem::path& path) {
		return IsCompiledMeshPath(path) ? LoadMeshFile(*this, path) : std::unique_ptr<Mesh>{};
	}, true).get();
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
