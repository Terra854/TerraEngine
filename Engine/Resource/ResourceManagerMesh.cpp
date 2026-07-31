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
#include "File Formats/TerraEngineAnimationFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceStreaming.h"
#include "Resource/VirtualGeometryCodec.h"

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

	bool ValidateDeformableFileRange(
		std::span<const std::uint8_t> fileBytes,
		std::uint64_t offset,
		std::uint64_t size,
		std::uint64_t extensionBegin,
		std::uint64_t extensionEnd)
	{
		std::uint64_t end = 0U;
		return TerraEngine::Animation::CheckedAdd(offset, size, end)
			&& offset >= extensionBegin
			&& end <= extensionEnd
			&& end <= fileBytes.size();
	}

	template <typename Record>
	bool ReadDeformableRecord(
		std::span<const std::uint8_t> fileBytes,
		std::uint64_t offset,
		Record& record)
	{
		if (offset > fileBytes.size() || fileBytes.size() - static_cast<std::size_t>(offset) < sizeof(Record))
		{
			return false;
		}
		std::memcpy(&record, fileBytes.data() + static_cast<std::size_t>(offset), sizeof(Record));
		return true;
	}

	bool ValidateDeformableMeshPayload(
		std::span<const std::uint8_t> fileBytes,
		const MeshHeader& meshHeader,
		const std::filesystem::path& resolvedPath,
		std::uint32_t expectedSkeletonJointCount,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		bool requireDeformablePayload,
		AnimationResourceValidationResult& result)
	{
		using TerraEngine::Animation::AnimationLimit;
		using TerraEngine::Animation::AnimationStatus;
		result = {};
		result.assetType = AssetType::Mesh;
		result.resolvedPath = resolvedPath;
		result.fileSizeBytes = fileBytes.size();
		result.detectedMajorVersion = meshHeader.version;
		result.expectedCompatibilityFingerprint = expectedCompatibilityFingerprint;

		const std::uint32_t metadataOffset = meshHeader.virtualGeometryReserved;
		if (metadataOffset == 0U)
		{
			if (meshHeader.meshletSkinningDataOffset != 0U || meshHeader.meshletSkinningDataCount != 0U)
			{
				return RejectDeformableValidation(result, AnimationStatus::InvalidEnvelope,
					"DeformableMetadataMissing",
					"TEMF advertises skinning bytes without the required checked TDEF metadata. Reimport the skinned mesh.");
			}
			if (requireDeformablePayload)
			{
				return RejectDeformableValidation(result, AnimationStatus::InvalidType,
					"StaticMeshIsNotDeformable",
					"The TEMF is a valid static-mesh layout but has no deformable TDEF extension. Import a skinned mesh artifact.");
			}
			result.status = AnimationStatus::Ok;
			result.diagnosticCode = "StaticMeshCompatible";
			result.message = "The static TEMF v1 payload remains compatible and contains no deformable extension.";
			return true;
		}

		if ((metadataOffset % kMeshletVertexDataAlignmentBytes) != 0U)
		{
			return RejectDeformableValidation(result, AnimationStatus::InvalidEnvelope,
				"DeformableMetadataMisaligned",
				"The TDEF metadata offset is not aligned to the TEMF payload boundary. Recook the mesh.");
		}

		DeformableMeshPayloadHeader metadata{};
		if (!ReadDeformableRecord(fileBytes, metadataOffset, metadata))
		{
			return RejectDeformableValidation(result, AnimationStatus::TruncatedInput,
				"DeformableHeaderTruncated",
				"The TDEF header range is outside the TEMF file. Restore or recook the mesh.");
		}
		result.detectedMinorVersion = metadata.version;
		result.deformableVertexCount = metadata.skinningVertexCount;
		result.palettePartitionCount = metadata.palettePartitionCount;
		result.morphTargetCount = metadata.morphTargetCount;
		result.sourceSkeletonJointCount = metadata.sourceSkeletonJointCount;

		if (metadata.magic != kDeformableMeshPayloadMagicValue)
		{
			return RejectDeformableValidation(result, AnimationStatus::InvalidEnvelope,
				"DeformableMagicMismatch",
				"The appended deformable payload does not begin with the TDEF magic. Recook the mesh.");
		}
		if (metadata.version != kDeformableMeshPayloadVersion)
		{
			return RejectDeformableValidation(result, AnimationStatus::UnsupportedMajorVersion,
				"DeformableVersionUnsupported",
				"The TDEF payload version is not supported by this runtime. Recook it with the current editor.");
		}
		if (metadata.headerSize != sizeof(DeformableMeshPayloadHeader)
			|| metadata.payloadSize < sizeof(DeformableMeshPayloadHeader)
			|| metadata.compatibleMeshVersion != meshHeader.version
			|| metadata.reserved != 0U)
		{
			return RejectDeformableValidation(result, AnimationStatus::InvalidEnvelope,
				"DeformableHeaderInvalid",
				"The TDEF header size, reserved field, or compatible TEMF version is invalid. Recook the mesh.");
		}
		result.actualCompatibilityFingerprint = metadata.skeletonCompatibilityFingerprint;
		result.compatibilityFingerprintPresent = metadata.skeletonCompatibilityFingerprint != 0U;
		if (expectedCompatibilityFingerprint != 0U
			&& metadata.skeletonCompatibilityFingerprint != expectedCompatibilityFingerprint)
		{
			return RejectDeformableValidation(result, AnimationStatus::IncompatibleAsset,
				"DeformableSkeletonFingerprintMismatch",
				"The TDEF payload was cooked against a different skeleton generation. Reimport the complete package atomically.");
		}

		std::uint64_t extensionEnd = 0U;
		if (!TerraEngine::Animation::CheckedAdd(metadataOffset, metadata.payloadSize, extensionEnd)
			|| extensionEnd > fileBytes.size())
		{
			return RejectDeformableValidation(result, AnimationStatus::RangeOutOfBounds,
				"DeformablePayloadRangeInvalid",
				"The declared TDEF payload extends beyond the TEMF file. Restore or recook the mesh.");
		}
		if (metadata.skinningVertexCount == 0U
			|| metadata.skinningVertexCount != meshHeader.meshletVertexCount
			|| metadata.skinningVertexCount != meshHeader.meshletSkinningDataCount
			|| metadata.vertexPaletteIndexCount != metadata.skinningVertexCount
			|| metadata.palettePartitionCount == 0U
			|| metadata.palettePartitionCount > metadata.skinningVertexCount)
		{
			return RejectDeformableValidation(result, AnimationStatus::InvalidArgument,
				"DeformableCountMismatch",
				"Skinning, vertex-palette, or palette-partition counts disagree with the base TEMF vertex count. Recook the mesh.");
		}
		if (metadata.sourceSkeletonJointCount == 0U
			|| TerraEngine::Animation::ValidateAnimationHardLimit(
				AnimationLimit::SkeletonJoints, metadata.sourceSkeletonJointCount) != AnimationStatus::Ok
			|| TerraEngine::Animation::ValidateAnimationHardLimit(
				AnimationLimit::MeshMorphTargets, metadata.morphTargetCount) != AnimationStatus::Ok)
		{
			return RejectDeformableValidation(result, AnimationStatus::HardLimitExceeded,
				"DeformableHardLimitExceeded",
				"The source skeleton or morph-target count exceeds the frozen runtime hard limit. Split the asset before import.");
		}
		if (expectedSkeletonJointCount != 0U
			&& metadata.sourceSkeletonJointCount != expectedSkeletonJointCount)
		{
			return RejectDeformableValidation(result, AnimationStatus::IncompatibleAsset,
				"DeformableSkeletonJointCountMismatch",
				"The deformable mesh was cooked against a different skeleton joint count. Reimport the package atomically.");
		}
		if (metadata.skinningStrideBytes != kDeformableMeshSkinningStrideBytes
			|| metadata.maximumInfluences != kDeformableMeshMaximumInfluences
			|| metadata.maximumPaletteJoints != kDeformableMeshMaximumPaletteJoints)
		{
			return RejectDeformableValidation(result, AnimationStatus::IncompatibleAsset,
				"DeformableLayoutIncompatible",
				"The skinning stride, retained-influence count, or palette limit does not match the runtime layout.");
		}

		constexpr std::uint32_t knownFlags = kDeformableMeshPayloadFlagConservativeMorphEnvelope
			| kDeformableMeshPayloadFlagZeroWeightMeansIdentity
			| kDeformableMeshPayloadFlagVertexPaletteIndices
			| kDeformableMeshPayloadFlagDenseMorphPositionDeltas;
		constexpr std::uint32_t requiredVirtualFlags = kDeformableMeshVirtualGeometryFlagWholeMeshEnvelope
			| kDeformableMeshVirtualGeometryFlagDisableUnprovenConeCulling
			| kDeformableMeshVirtualGeometryFlagRequireCompleteFallback;
		if (metadata.flags != knownFlags || metadata.virtualGeometryDeformationFlags != requiredVirtualFlags)
		{
			return RejectDeformableValidation(result, AnimationStatus::UnknownRequiredChunk,
				"DeformableFlagsUnsupported",
				"Required identity, morph-envelope, palette, or fail-open virtual-geometry rules are missing or unknown.");
		}
		for (std::size_t component = 0U; component < 3U; ++component)
		{
			if (!std::isfinite(metadata.deformationBoundsMin[component])
				|| !std::isfinite(metadata.deformationBoundsMax[component])
				|| metadata.deformationBoundsMin[component] > metadata.deformationBoundsMax[component])
			{
				return RejectDeformableValidation(result, AnimationStatus::InvalidArgument,
					"DeformableEnvelopeInvalid",
					"The conservative deformation envelope contains a non-finite or inverted axis. Recook the mesh.");
			}
		}

		std::uint64_t expectedSkinningBytes = 0U;
		if (!TerraEngine::Animation::CheckedMultiply(
			metadata.skinningVertexCount, kDeformableMeshSkinningStrideBytes, expectedSkinningBytes)
			|| metadata.skinningDataSize != expectedSkinningBytes
			|| metadata.skinningDataOffset != meshHeader.meshletSkinningDataOffset)
		{
			return RejectDeformableValidation(result, AnimationStatus::RangeOverflow,
				"DeformableSkinningRangeInvalid",
				"The 8-byte-per-vertex skinning stream size or base-header offset is inconsistent.");
		}
		std::uint64_t skinningEnd = 0U;
		if (!TerraEngine::Animation::CheckedAdd(metadata.skinningDataOffset, metadata.skinningDataSize, skinningEnd)
			|| skinningEnd > fileBytes.size() || skinningEnd > metadataOffset)
		{
			return RejectDeformableValidation(result, AnimationStatus::RangeOutOfBounds,
				"DeformableSkinningOutOfBounds",
				"The skinning stream is outside the file or overlaps the TDEF metadata.");
		}

		std::uint64_t paletteTableBytes = 0U;
		std::uint64_t morphTableBytes = 0U;
		if (!TerraEngine::Animation::CheckedMultiply(metadata.palettePartitionCount,
				sizeof(DeformableMeshPaletteRecord), paletteTableBytes)
			|| !TerraEngine::Animation::CheckedMultiply(metadata.morphTargetCount,
				sizeof(DeformableMeshMorphRecord), morphTableBytes))
		{
			return RejectDeformableValidation(result, AnimationStatus::RangeOverflow,
				"DeformableTableSizeOverflow",
				"A deformable table byte count overflowed checked arithmetic.");
		}
		const std::uint64_t expectedPaletteTableOffset = static_cast<std::uint64_t>(metadataOffset) + metadata.headerSize;
		const std::uint64_t expectedMorphTableOffset = expectedPaletteTableOffset + paletteTableBytes;
		if (metadata.paletteTableOffset != expectedPaletteTableOffset
			|| metadata.morphTableOffset != expectedMorphTableOffset
			|| !ValidateDeformableFileRange(fileBytes, metadata.paletteTableOffset, paletteTableBytes, metadataOffset, extensionEnd)
			|| !ValidateDeformableFileRange(fileBytes, metadata.morphTableOffset, morphTableBytes, metadataOffset, extensionEnd))
		{
			return RejectDeformableValidation(result, AnimationStatus::RangeOutOfBounds,
				"DeformableTableRangeInvalid",
				"Palette or morph records are not in their canonical checked TDEF table ranges.");
		}

		std::vector<DeformableMeshPaletteRecord> palettes(metadata.palettePartitionCount);
		if (!palettes.empty())
		{
			std::memcpy(palettes.data(), fileBytes.data() + metadata.paletteTableOffset,
				static_cast<std::size_t>(paletteTableBytes));
		}
		std::vector<DeformableMeshMorphRecord> morphs(metadata.morphTargetCount);
		if (!morphs.empty())
		{
			std::memcpy(morphs.data(), fileBytes.data() + metadata.morphTableOffset,
				static_cast<std::size_t>(morphTableBytes));
		}

		using ByteRange = std::pair<std::uint64_t, std::uint64_t>;
		std::vector<ByteRange> variableRanges;
		variableRanges.reserve(palettes.size() + morphs.size() + 1U);
		for (std::size_t paletteIndex = 0U; paletteIndex < palettes.size(); ++paletteIndex)
		{
			const DeformableMeshPaletteRecord& palette = palettes[paletteIndex];
			std::uint64_t jointBytes = 0U;
			if (palette.jointCount == 0U || palette.jointCount > kDeformableMeshMaximumPaletteJoints
				|| !TerraEngine::Animation::CheckedMultiply(palette.jointCount, sizeof(std::uint32_t), jointBytes)
				|| !ValidateDeformableFileRange(fileBytes, palette.jointTableOffset, jointBytes,
					metadataOffset, extensionEnd))
			{
				return RejectDeformableValidation(result, AnimationStatus::RangeOutOfBounds,
					"DeformablePaletteRangeInvalid",
					"A palette is empty, exceeds 256 joints, or points outside the TDEF payload.");
			}
			variableRanges.emplace_back(palette.jointTableOffset, palette.jointTableOffset + jointBytes);
			std::uint32_t previousJoint = 0U;
			for (std::uint32_t jointIndex = 0U; jointIndex < palette.jointCount; ++jointIndex)
			{
				std::uint32_t sourceJoint = 0U;
				std::memcpy(&sourceJoint,
					fileBytes.data() + palette.jointTableOffset + static_cast<std::size_t>(jointIndex) * sizeof(sourceJoint),
					sizeof(sourceJoint));
				if (sourceJoint >= metadata.sourceSkeletonJointCount
					|| (jointIndex != 0U && sourceJoint <= previousJoint))
				{
					return RejectDeformableValidation(result, AnimationStatus::IncompatibleAsset,
						"DeformablePaletteJointInvalid",
						"Palette source-joint IDs must be strictly ordered and within the source skeleton.");
				}
				previousJoint = sourceJoint;
			}
		}

		std::uint64_t vertexPaletteBytes = 0U;
		if (!TerraEngine::Animation::CheckedMultiply(metadata.vertexPaletteIndexCount,
				sizeof(std::uint32_t), vertexPaletteBytes)
			|| !ValidateDeformableFileRange(fileBytes, metadata.vertexPaletteIndexOffset,
				vertexPaletteBytes, metadataOffset, extensionEnd))
		{
			return RejectDeformableValidation(result, AnimationStatus::RangeOutOfBounds,
				"DeformableVertexPaletteRangeInvalid",
				"The per-vertex palette-index table points outside the TDEF payload.");
		}
		variableRanges.emplace_back(metadata.vertexPaletteIndexOffset,
			static_cast<std::uint64_t>(metadata.vertexPaletteIndexOffset) + vertexPaletteBytes);

		std::uint32_t previousMorphId = 0U;
		for (std::size_t morphIndex = 0U; morphIndex < morphs.size(); ++morphIndex)
		{
			const DeformableMeshMorphRecord& morph = morphs[morphIndex];
			std::uint64_t morphBytes = 0U;
			if (morph.stableTargetId == 0U
				|| (morphIndex != 0U && morph.stableTargetId <= previousMorphId)
				|| morph.positionDeltaCount != metadata.skinningVertexCount
				|| morph.positionDeltaStrideBytes != sizeof(float) * 3U
				|| morph.flags != kDeformableMeshPayloadFlagDenseMorphPositionDeltas
				|| !TerraEngine::Animation::CheckedMultiply(
					morph.positionDeltaCount, morph.positionDeltaStrideBytes, morphBytes)
				|| !ValidateDeformableFileRange(fileBytes, morph.positionDeltaOffset,
					morphBytes, metadataOffset, extensionEnd))
			{
				return RejectDeformableValidation(result, AnimationStatus::RangeOutOfBounds,
					"DeformableMorphRangeInvalid",
					"A morph target has an unstable ID, incompatible dense layout, or out-of-range delta stream.");
			}
			previousMorphId = morph.stableTargetId;
			variableRanges.emplace_back(morph.positionDeltaOffset,
				static_cast<std::uint64_t>(morph.positionDeltaOffset) + morphBytes);
			for (std::uint32_t vertexIndex = 0U; vertexIndex < morph.positionDeltaCount; ++vertexIndex)
			{
				float delta[3]{};
				std::memcpy(delta,
					fileBytes.data() + morph.positionDeltaOffset
						+ static_cast<std::size_t>(vertexIndex) * morph.positionDeltaStrideBytes,
					sizeof(delta));
				if (!std::isfinite(delta[0]) || !std::isfinite(delta[1]) || !std::isfinite(delta[2]))
				{
					return RejectDeformableValidation(result, AnimationStatus::InvalidArgument,
						"DeformableMorphDeltaInvalid",
						"A morph delta contains a non-finite component. Recook the source mesh.");
				}
			}
		}

		std::sort(variableRanges.begin(), variableRanges.end());
		std::uint64_t expectedVariableOffset = expectedMorphTableOffset + morphTableBytes;
		for (const ByteRange& range : variableRanges)
		{
			if (range.first != expectedVariableOffset || range.second < range.first)
			{
				return RejectDeformableValidation(result, AnimationStatus::InvalidChunk,
					"DeformableRangesOverlapOrGap",
					"TDEF variable tables overlap or contain an unrecorded gap. Recook the mesh deterministically.");
			}
			expectedVariableOffset = range.second;
		}
		if (expectedVariableOffset != extensionEnd)
		{
			return RejectDeformableValidation(result, AnimationStatus::InvalidChunk,
				"DeformablePayloadSizeMismatch",
				"The TDEF payload size includes unclaimed bytes or truncates a recorded stream.");
		}

		for (std::uint32_t vertexIndex = 0U; vertexIndex < metadata.skinningVertexCount; ++vertexIndex)
		{
			std::uint32_t paletteIndex = 0U;
			std::memcpy(&paletteIndex,
				fileBytes.data() + metadata.vertexPaletteIndexOffset
					+ static_cast<std::size_t>(vertexIndex) * sizeof(paletteIndex),
				sizeof(paletteIndex));
			if (paletteIndex >= palettes.size())
			{
				return RejectDeformableValidation(result, AnimationStatus::RangeOutOfBounds,
					"DeformableVertexPaletteInvalid",
					"A vertex references a palette partition outside the validated palette table.");
			}

			const std::uint8_t* skinning = fileBytes.data() + metadata.skinningDataOffset
				+ static_cast<std::size_t>(vertexIndex) * kDeformableMeshSkinningStrideBytes;
			std::uint32_t weightSum = 0U;
			for (std::size_t influence = 0U; influence < kDeformableMeshMaximumInfluences; ++influence)
			{
				const std::uint8_t localJoint = skinning[influence];
				const std::uint8_t weight = skinning[4U + influence];
				weightSum += weight;
				if ((weight != 0U && localJoint >= palettes[paletteIndex].jointCount)
					|| (weight == 0U && localJoint != 0U))
				{
					return RejectDeformableValidation(result, AnimationStatus::RangeOutOfBounds,
						"DeformableInfluenceInvalid",
						"A retained influence references a joint outside its palette or uses a non-canonical zero-weight joint.");
				}
			}
			if (weightSum == 0U)
			{
				++result.zeroWeightIdentityVertexCount;
				continue;
			}
			if (weightSum != 255U)
			{
				return RejectDeformableValidation(result, AnimationStatus::InvalidArgument,
					"DeformableWeightNormalizationInvalid",
					"Non-identity UNORM8 weights must sum exactly to 255. Recook the mesh to restore deterministic normalization.");
			}
		}
		if (result.zeroWeightIdentityVertexCount != metadata.skinningVertexCount
			&& metadata.skeletonCompatibilityFingerprint == 0U)
		{
			return RejectDeformableValidation(result, AnimationStatus::IncompatibleAsset,
				"DeformableSkeletonFingerprintMissing",
				"A TDEF payload with nonzero skin weights must identify its compatible cooked skeleton generation. Reimport the package.");
		}

		result.status = AnimationStatus::Ok;
		result.diagnosticCode = "DeformableMeshValid";
		result.message = "The TEMF/TDEF magic, versions, counts, checked ranges, palette joints, morphs, conservative envelope, and zero-weight identity records are valid.";
		return true;
	}

	bool ValidateDeformableVirtualGeometrySidecar(
		std::span<const std::uint8_t> fileBytes,
		const MeshHeader& meshHeader,
		const std::filesystem::path& resolvedPath,
		AnimationResourceValidationResult& result)
	{
		// TDFV is intentionally outside TDEF v1's payloadSize. A TDEF-only file
		// therefore remains loadable, while a file that advertises a sidecar must
		// validate every byte before any runtime Mesh state receives its records.
		const std::uint32_t tdefOffset = meshHeader.virtualGeometryReserved;
		if (tdefOffset == 0U)
		{
			// Ordinary static TEMF files have no TDEF boundary and therefore cannot
			// advertise TDFV. Preserve their historical load path unchanged.
			return true;
		}
		DeformableMeshPayloadHeader tdef{};
		if (!ReadDeformableRecord(fileBytes, tdefOffset, tdef))
		{
			return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::TruncatedInput,
				"DeformableSidecarBaseHeaderMissing",
				"The checked TDEF header disappeared before virtual-geometry metadata could be validated.");
		}
		std::uint64_t sidecarOffset = 0U;
		if (!TerraEngine::Animation::CheckedAdd(tdefOffset, tdef.payloadSize, sidecarOffset)
			|| sidecarOffset > fileBytes.size())
		{
			return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::RangeOutOfBounds,
				"DeformableSidecarOffsetInvalid",
				"The TDEF payload does not leave a valid boundary for optional cluster metadata.");
		}
		if (sidecarOffset == fileBytes.size())
		{
			// Legacy TDEF v1 remains safe; it simply has no cluster-local proof and
			// downstream deformation code must retain its conservative fallback.
			return true;
		}

		DeformableVirtualGeometrySidecarHeader sidecar{};
		if (!ReadDeformableRecord(fileBytes, sidecarOffset, sidecar)
			|| sidecar.magic != kDeformableVirtualGeometrySidecarMagicValue
			|| sidecar.version != kDeformableVirtualGeometrySidecarVersion
			|| sidecar.headerSize != sizeof(DeformableVirtualGeometrySidecarHeader)
			|| sidecar.flags != kDeformableVirtualGeometrySidecarFlagDerivedFromClusters
			|| sidecar.payloadSize < sizeof(DeformableVirtualGeometrySidecarHeader)
			|| sidecar.clusterCount == 0U || sidecar.paletteIndexCount == 0U
			|| sidecar.morphTargetCount != tdef.morphTargetCount
			|| sidecar.morphVertexStrideBytes != sizeof(DeformableVirtualGeometryMorphVertex))
		{
			return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidEnvelope,
				"DeformableVirtualGeometrySidecarInvalid",
				"The optional TDFV header is malformed, unsupported, or omits required cluster coverage.");
		}
		std::uint64_t sidecarEnd = 0U;
		if (!TerraEngine::Animation::CheckedAdd(sidecarOffset, sidecar.payloadSize, sidecarEnd)
			|| sidecarEnd > fileBytes.size())
		{
			return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::RangeOutOfBounds,
				"DeformableVirtualGeometrySidecarRangeInvalid",
				"The TDFV range is truncated.");
		}
		if (sidecarEnd < fileBytes.size())
		{
			// AVPD/AVPB is the only legal extension after frozen TDFV v1. The
			// directory starts at a 16-byte boundary and claims every physical
			// blob through checked ranges, so unrelated trailing bytes still fail.
			const std::uint64_t directoryOffset =
				(sidecarEnd + 15u) & ~std::uint64_t{15u};
			const bool paddingValid =
				directoryOffset <= fileBytes.size()
				&& std::all_of(
					fileBytes.begin()
						+ static_cast<std::ptrdiff_t>(sidecarEnd),
					fileBytes.begin()
						+ static_cast<std::ptrdiff_t>(directoryOffset),
					[](std::uint8_t value) { return value == 0u; });
			const TerraEngine::Resource::AnimatedVirtualPageCatalog catalog =
				paddingValid
				? TerraEngine::Resource::
					ParseAnimatedVirtualPageDirectory(
						fileBytes, directoryOffset)
				: TerraEngine::Resource::
					AnimatedVirtualPageCatalog{};
			if (!paddingValid || !catalog.valid)
			{
				return RejectDeformableValidation(
					result,
					TerraEngine::Animation::AnimationStatus::
						RangeOutOfBounds,
					"DeformableVirtualGeometryExtensionInvalid",
					"Bytes after TDFV must be one aligned, fully "
					"validated AVPD/AVPB extension.");
			}
		}
		std::uint64_t clusterBytes = 0U;
		std::uint64_t paletteBytes = 0U;
		std::uint64_t morphTableBytes = 0U;
		std::uint64_t morphVertexBytes = 0U;
		if (!TerraEngine::Animation::CheckedMultiply(sidecar.clusterCount,
				sizeof(DeformableVirtualGeometryClusterRecord), clusterBytes)
			|| !TerraEngine::Animation::CheckedMultiply(sidecar.paletteIndexCount,
				sizeof(std::uint32_t), paletteBytes)
			|| !TerraEngine::Animation::CheckedMultiply(sidecar.morphTargetCount,
				sizeof(DeformableVirtualGeometryMorphRecord), morphTableBytes)
			|| !TerraEngine::Animation::CheckedMultiply(
				static_cast<std::uint64_t>(sidecar.morphTargetCount) * tdef.skinningVertexCount,
				sizeof(DeformableVirtualGeometryMorphVertex), morphVertexBytes))
		{
			return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::RangeOverflow,
				"DeformableVirtualGeometrySidecarSizeOverflow",
				"A TDFV table count overflowed checked arithmetic.");
		}
		const std::uint64_t expectedClusterOffset = sidecarOffset + sidecar.headerSize;
		const std::uint64_t expectedPaletteOffset = expectedClusterOffset + clusterBytes;
		const std::uint64_t expectedMorphTableOffset = expectedPaletteOffset + paletteBytes;
		const std::uint64_t expectedMorphVertexDataOffset = expectedMorphTableOffset + morphTableBytes;
		if (sidecar.clusterTableOffset != expectedClusterOffset
			|| sidecar.paletteIndexTableOffset != expectedPaletteOffset
			|| sidecar.morphTableOffset != (sidecar.morphTargetCount == 0U ? 0U : expectedMorphTableOffset)
			|| sidecar.morphVertexDataOffset != (sidecar.morphTargetCount == 0U ? 0U : expectedMorphVertexDataOffset)
			|| !ValidateDeformableFileRange(fileBytes, sidecar.clusterTableOffset, clusterBytes, sidecarOffset, sidecarEnd)
			|| !ValidateDeformableFileRange(fileBytes, sidecar.paletteIndexTableOffset, paletteBytes, sidecarOffset, sidecarEnd)
			|| (sidecar.morphTargetCount != 0U
				&& (!ValidateDeformableFileRange(fileBytes, sidecar.morphTableOffset, morphTableBytes, sidecarOffset, sidecarEnd)
					|| !ValidateDeformableFileRange(fileBytes, sidecar.morphVertexDataOffset, morphVertexBytes, sidecarOffset, sidecarEnd)))
			|| expectedMorphVertexDataOffset + morphVertexBytes != sidecarEnd)
		{
			return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::RangeOutOfBounds,
				"DeformableVirtualGeometrySidecarTableInvalid",
				"TDFV cluster or palette ranges are not the canonical contiguous payload tables.");
		}

		std::vector<DeformableVirtualGeometryClusterRecord> clusters(sidecar.clusterCount);
		std::vector<std::uint32_t> paletteIndices(sidecar.paletteIndexCount);
		std::vector<DeformableVirtualGeometryMorphRecord> morphs(sidecar.morphTargetCount);
		std::memcpy(clusters.data(), fileBytes.data() + sidecar.clusterTableOffset, static_cast<std::size_t>(clusterBytes));
		std::memcpy(paletteIndices.data(), fileBytes.data() + sidecar.paletteIndexTableOffset, static_cast<std::size_t>(paletteBytes));
		if (!morphs.empty())
		{
			std::memcpy(morphs.data(), fileBytes.data() + sidecar.morphTableOffset, static_cast<std::size_t>(morphTableBytes));
		}
		std::uint32_t expectedClusterPaletteOffset = sidecar.paletteIndexTableOffset;
		constexpr std::uint32_t requiredClusterFlags = kDeformableVirtualGeometryClusterFlagActive
			| kDeformableVirtualGeometryClusterFlagFallbackCoverageComplete;
		for (std::uint32_t clusterIndex = 0U; clusterIndex < sidecar.clusterCount; ++clusterIndex)
		{
			const DeformableVirtualGeometryClusterRecord& cluster = clusters[clusterIndex];
			std::uint64_t clusterPaletteBytes = 0U;
			if (cluster.clusterId != clusterIndex
				|| cluster.paletteIndexCount == 0U
				|| cluster.morphTargetCount != sidecar.morphTargetCount
				|| (cluster.flags & ~kDeformableVirtualGeometryClusterFlagAll) != 0U
				|| (cluster.flags & requiredClusterFlags) != requiredClusterFlags
				|| (cluster.flags & (kDeformableVirtualGeometryClusterFlagDynamicBoundsValid
					| kDeformableVirtualGeometryClusterFlagConeCullingSafe
					| kDeformableVirtualGeometryClusterFlagHiZCullingSafe)) != 0U
				|| ((cluster.flags & kDeformableVirtualGeometryClusterFlagFrustumCullingSafe) != 0U
					&& (cluster.flags & kDeformableVirtualGeometryClusterFlagConservativeEnvelopeValid) == 0U)
				|| !std::isfinite(cluster.geometricError) || cluster.geometricError < 0.0F
				|| !std::isfinite(cluster.deformationError) || cluster.deformationError < 0.0F
				|| !TerraEngine::Animation::CheckedMultiply(cluster.paletteIndexCount, sizeof(std::uint32_t), clusterPaletteBytes)
				|| cluster.paletteIndexOffset != expectedClusterPaletteOffset
				|| !ValidateDeformableFileRange(fileBytes, cluster.paletteIndexOffset, clusterPaletteBytes, sidecarOffset, sidecarEnd))
			{
				return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidChunk,
					"DeformableVirtualGeometryClusterInvalid",
					"A TDFV cluster has invalid proof flags, palette coverage, or deformation error metadata.");
			}
			for (std::size_t axis = 0U; axis < 3U; ++axis)
			{
				if (!std::isfinite(cluster.deformationBoundsMin[axis])
					|| !std::isfinite(cluster.deformationBoundsMax[axis])
					|| cluster.deformationBoundsMin[axis] > cluster.deformationBoundsMax[axis])
				{
					return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidArgument,
						"DeformableVirtualGeometryEnvelopeInvalid",
						"A TDFV cluster deformation envelope contains a non-finite or inverted axis.");
				}
			}
			const std::size_t paletteBegin = static_cast<std::size_t>(cluster.paletteIndexOffset - sidecar.paletteIndexTableOffset) / sizeof(std::uint32_t);
			std::uint32_t previousPalette = 0U;
			for (std::uint32_t paletteOffset = 0U; paletteOffset < cluster.paletteIndexCount; ++paletteOffset)
			{
				const std::uint32_t palette = paletteIndices[paletteBegin + paletteOffset];
				if (palette >= tdef.palettePartitionCount || (paletteOffset != 0U && palette <= previousPalette))
				{
					return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::RangeOutOfBounds,
						"DeformableVirtualGeometryPaletteInvalid",
						"A TDFV cluster references an unknown or non-deterministically ordered TDEF palette.");
				}
				previousPalette = palette;
			}
			expectedClusterPaletteOffset += static_cast<std::uint32_t>(clusterPaletteBytes);
		}
		for (std::uint32_t morphIndex = 0U; morphIndex < sidecar.morphTargetCount; ++morphIndex)
		{
			const DeformableVirtualGeometryMorphRecord& morph = morphs[morphIndex];
			const std::uint64_t expectedOffset = expectedMorphVertexDataOffset
				+ static_cast<std::uint64_t>(morphIndex) * tdef.skinningVertexCount * sizeof(DeformableVirtualGeometryMorphVertex);
			if (morph.stableTargetId == 0U
				|| morph.vertexDataOffset != expectedOffset
				|| morph.vertexCount != tdef.skinningVertexCount
				|| morph.vertexStrideBytes != sizeof(DeformableVirtualGeometryMorphVertex)
				|| (morph.flags & ~(
					kDeformableVirtualGeometryMorphFlagPosition
					| kDeformableVirtualGeometryMorphFlagNormal
					| kDeformableVirtualGeometryMorphFlagTangent)) != 0U
				|| (morph.flags & kDeformableVirtualGeometryMorphFlagPosition) == 0U)
			{
				return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidChunk,
					"DeformableVirtualGeometryMorphRecordInvalid",
					"A TDFV morph target does not use the required target-major 48-byte GPU record layout.");
			}
			for (std::uint32_t vertexIndex = 0U; vertexIndex < morph.vertexCount; ++vertexIndex)
			{
				DeformableVirtualGeometryMorphVertex vertex{};
				std::memcpy(&vertex, fileBytes.data() + morph.vertexDataOffset
					+ static_cast<std::size_t>(vertexIndex) * sizeof(vertex), sizeof(vertex));
				for (float value : vertex.positionDelta)
				{
					if (!std::isfinite(value)) return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidArgument, "DeformableVirtualGeometryMorphValueInvalid", "A TDFV position delta is non-finite.");
				}
				if (vertex.positionDelta[3] != 0.0F)
				{
					return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidArgument, "DeformableVirtualGeometryMorphPaddingInvalid", "A TDFV position float4 must keep its padding component at zero.");
				}
				for (float value : vertex.normalDelta)
				{
					if (!std::isfinite(value)) return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidArgument, "DeformableVirtualGeometryMorphValueInvalid", "A TDFV normal delta is non-finite.");
				}
				for (float value : vertex.tangentDelta)
				{
					if (!std::isfinite(value)) return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidArgument, "DeformableVirtualGeometryMorphValueInvalid", "A TDFV tangent delta is non-finite.");
				}
				if (vertex.normalDelta[3] != 0.0F || vertex.tangentDelta[3] != 0.0F
					|| ((morph.flags & kDeformableVirtualGeometryMorphFlagNormal) == 0U
						&& (vertex.normalDelta[0] != 0.0F || vertex.normalDelta[1] != 0.0F || vertex.normalDelta[2] != 0.0F))
					|| ((morph.flags & kDeformableVirtualGeometryMorphFlagTangent) == 0U
						&& (vertex.tangentDelta[0] != 0.0F || vertex.tangentDelta[1] != 0.0F || vertex.tangentDelta[2] != 0.0F)))
				{
					return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidArgument, "DeformableVirtualGeometryMorphPaddingInvalid", "Absent morph channels and float4 padding must be encoded as zero.");
				}
			}
		}

		if ((meshHeader.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0U)
		{
			VirtualGeometryHeader virtualHeader{};
			if (!ReadDeformableRecord(fileBytes, meshHeader.virtualGeometryHeaderOffset, virtualHeader)
				|| virtualHeader.headerSize != sizeof(VirtualGeometryHeader)
				|| virtualHeader.clusterCount != sidecar.clusterCount)
			{
				return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::IncompatibleAsset,
					"DeformableVirtualGeometryClusterCountMismatch",
					"TDFV cluster coverage does not match the immutable virtual-geometry cluster table.");
			}
			std::uint64_t sourceClusterBytes = 0U;
			if (!TerraEngine::Animation::CheckedMultiply(virtualHeader.clusterCount, sizeof(VirtualGeometryClusterRecord), sourceClusterBytes)
				|| !ValidateDeformableFileRange(fileBytes, virtualHeader.clusterTableOffset, sourceClusterBytes, 0U, fileBytes.size()))
			{
				return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::RangeOutOfBounds,
					"DeformableVirtualGeometrySourceClusterRangeInvalid",
					"The virtual-geometry cluster table cannot be checked against TDFV metadata.");
			}
			for (std::uint32_t index = 0U; index < sidecar.clusterCount; ++index)
			{
				VirtualGeometryClusterRecord sourceCluster{};
				std::memcpy(&sourceCluster, fileBytes.data() + virtualHeader.clusterTableOffset
					+ static_cast<std::size_t>(index) * sizeof(sourceCluster), sizeof(sourceCluster));
				if (sourceCluster.clusterId != clusters[index].clusterId
					|| sourceCluster.fallbackClusterId != clusters[index].fallbackClusterId
					|| sourceCluster.parentClusterId != clusters[index].parentClusterId
					|| sourceCluster.firstChildClusterId != clusters[index].firstChildClusterId
					|| sourceCluster.childCount != clusters[index].childCount
					|| sourceCluster.geometricError != clusters[index].geometricError)
				{
					return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::IncompatibleAsset,
						"DeformableVirtualGeometryClusterBindingMismatch",
						"TDFV records no longer match the cluster hierarchy or geometric errors they were cooked against.");
				}
			}
		}
		else if (sidecar.clusterCount != 1U || clusters.front().fallbackClusterId != 0U
			|| clusters.front().parentClusterId != kVirtualGeometryInvalidIndex)
		{
			return RejectDeformableValidation(result, TerraEngine::Animation::AnimationStatus::InvalidChunk,
				"DeformableVirtualGeometryFallbackRecordInvalid",
				"A non-virtual mesh must use one deterministic complete-coverage TDFV fallback record.");
		}
		return true;
	}

	bool CopyValidatedDeformableRuntimeMetadata(
		std::span<const std::uint8_t> fileBytes,
		const MeshHeader& meshHeader,
		Mesh& mesh)
	{
		// ValidateDeformableMeshPayload has already proven every count and byte
		// range before this helper runs. Copying the records makes those facts
		// available to renderer jobs without letting them reopen a file or retain a
		// pointer into the loader's temporary byte vector.
		const std::uint32_t metadataOffset = meshHeader.virtualGeometryReserved;
		if(metadataOffset == 0U) {
			return true;
		}

		DeformableMeshPayloadHeader metadata{};
		if(!ReadDeformableRecord(fileBytes, metadataOffset, metadata)) {
			return false;
		}

		// Hash the checked skinning stream and complete validated extension, not
		// only its header. This makes palette, influence, morph, or envelope recooks
		// invalidate renderer bindings while remaining deterministic across loads.
		std::uint64_t payloadGeneration = 14695981039346656037ULL;
		const auto hashBytes = [&payloadGeneration](std::span<const std::uint8_t> bytes) {
			for(const std::uint8_t byte : bytes) {
				payloadGeneration ^= static_cast<std::uint64_t>(byte);
				payloadGeneration *= 1099511628211ULL;
			}
		};
		hashBytes(fileBytes.subspan(metadata.skinningDataOffset, metadata.skinningDataSize));
		hashBytes(fileBytes.subspan(metadataOffset, metadata.payloadSize));
		mesh.hasDeformablePayload = true;
		mesh.deformablePayloadGeneration = payloadGeneration == 0U ? 1U : payloadGeneration;
		mesh.deformableMetadata = metadata;
		mesh.deformablePalettes.resize(metadata.palettePartitionCount);
		mesh.deformableMorphs.resize(metadata.morphTargetCount);
		if(!mesh.deformablePalettes.empty()) {
			std::memcpy(mesh.deformablePalettes.data(),
				fileBytes.data() + metadata.paletteTableOffset,
				mesh.deformablePalettes.size() * sizeof(DeformableMeshPaletteRecord));
		}
		if(!mesh.deformableMorphs.empty()) {
			std::memcpy(mesh.deformableMorphs.data(),
				fileBytes.data() + metadata.morphTableOffset,
				mesh.deformableMorphs.size() * sizeof(DeformableMeshMorphRecord));
		}

		std::size_t totalPaletteJoints = 0U;
		for(const DeformableMeshPaletteRecord& palette : mesh.deformablePalettes) {
			totalPaletteJoints += palette.jointCount;
		}
		mesh.deformablePaletteJoints.clear();
		mesh.deformablePaletteJoints.reserve(totalPaletteJoints);
		for(const DeformableMeshPaletteRecord& palette : mesh.deformablePalettes) {
			const std::size_t oldSize = mesh.deformablePaletteJoints.size();
			mesh.deformablePaletteJoints.resize(oldSize + palette.jointCount);
			std::memcpy(mesh.deformablePaletteJoints.data() + oldSize,
				fileBytes.data() + palette.jointTableOffset,
				static_cast<std::size_t>(palette.jointCount) * sizeof(std::uint32_t));
		}

		mesh.deformableVertexPaletteIndices.resize(metadata.vertexPaletteIndexCount);
		if(!mesh.deformableVertexPaletteIndices.empty()) {
			std::memcpy(mesh.deformableVertexPaletteIndices.data(),
				fileBytes.data() + metadata.vertexPaletteIndexOffset,
				mesh.deformableVertexPaletteIndices.size() * sizeof(std::uint32_t));
		}

		const std::uint64_t sidecarOffset = static_cast<std::uint64_t>(metadataOffset) + metadata.payloadSize;
		if (sidecarOffset < fileBytes.size())
		{
			DeformableVirtualGeometrySidecarHeader sidecar{};
			if (!ReadDeformableRecord(fileBytes, sidecarOffset, sidecar))
			{
				return false;
			}
			std::vector<DeformableVirtualGeometryClusterRecord> clusters(sidecar.clusterCount);
			std::vector<std::uint32_t> paletteIndices(sidecar.paletteIndexCount);
			std::vector<DeformableVirtualGeometryMorphRecord> morphs(sidecar.morphTargetCount);
			std::uint64_t morphVertexCount = 0U;
			if (!TerraEngine::Animation::CheckedMultiply(
				sidecar.morphTargetCount, metadata.skinningVertexCount, morphVertexCount))
			{
				return false;
			}
			if (morphVertexCount > std::numeric_limits<std::size_t>::max())
			{
				return false;
			}
			std::vector<DeformableVirtualGeometryMorphVertex> morphVertices(static_cast<std::size_t>(morphVertexCount));
			std::memcpy(clusters.data(), fileBytes.data() + sidecar.clusterTableOffset,
				clusters.size() * sizeof(DeformableVirtualGeometryClusterRecord));
			std::memcpy(paletteIndices.data(), fileBytes.data() + sidecar.paletteIndexTableOffset,
				paletteIndices.size() * sizeof(std::uint32_t));
			if (!morphs.empty())
			{
				std::memcpy(morphs.data(), fileBytes.data() + sidecar.morphTableOffset,
					morphs.size() * sizeof(DeformableVirtualGeometryMorphRecord));
				std::memcpy(morphVertices.data(), fileBytes.data() + sidecar.morphVertexDataOffset,
					morphVertices.size() * sizeof(DeformableVirtualGeometryMorphVertex));
			}
			mesh.hasDeformableVirtualGeometrySidecar = true;
			mesh.deformableVirtualGeometrySidecar = sidecar;
			mesh.deformableVirtualGeometryClusters = std::move(clusters);
			mesh.deformableVirtualGeometryPaletteIndices = std::move(paletteIndices);
			mesh.deformableVirtualGeometryMorphs = std::move(morphs);
			mesh.deformableVirtualGeometryMorphVertices = std::move(morphVertices);
		}
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

	// Publish aggregate profile data only after the ordinary cluster/page tables
	// have passed atomic validation. Invalid optional sidecars fall back to the
	// existing validated cluster chain and request a targeted editor recook.
	void ResolveVirtualGeometryAggregateProfile(
		const std::vector<std::uint8_t>& fileBytes,
		const MeshHeader& header,
		Mesh& mesh)
	{
		mesh.hasVirtualGeometryAggregateProfile = false;
		mesh.virtualGeometryAggregateProfileProvenance = VirtualGeometryAggregateProfileProvenance::None;
		mesh.virtualGeometryAggregateProfiles.clear();
		mesh.virtualGeometryAggregateProfileOffset = 0u;
		mesh.virtualGeometryAggregateProfileDiagnostic.clear();
		if (!mesh.hasVirtualGeometry || mesh.virtualGeometryClusters.empty())
		{
			return;
		}

		for (std::uint64_t offset = header.headerSize;
			offset + sizeof(VirtualGeometryAggregateProfileHeader) <= fileBytes.size(); offset += 4u)
		{
			VirtualGeometryAggregateProfileHeader profileHeader{};
			std::memcpy(&profileHeader, fileBytes.data() + offset, sizeof(profileHeader));
			if (profileHeader.magic != kVirtualGeometryAggregateProfileMagicValue)
			{
				continue;
			}
			const std::uint64_t recordBytes = static_cast<std::uint64_t>(profileHeader.profileCount)
				* sizeof(VirtualGeometryAggregateProfileRecord);
			if (profileHeader.version != kVirtualGeometryAggregateProfileVersion
				|| profileHeader.headerSize != sizeof(profileHeader)
				|| profileHeader.profileCount == 0u
				|| profileHeader.byteSize != sizeof(profileHeader) + recordBytes
				|| profileHeader.byteSize > fileBytes.size() - offset)
			{
				mesh.virtualGeometryAggregateProfileDiagnostic = "VGAP header is incompatible or out of range.";
				// Geometry payload bytes may coincidentally contain the four-byte
				// magic. Keep scanning for the appended sidecar instead of letting
				// that false positive hide a later valid aggregate publication.
				continue;
			}
			const std::span<const std::uint8_t> rawProfiles(
				fileBytes.data() + offset + sizeof(profileHeader), static_cast<std::size_t>(recordBytes));
			std::vector<VirtualGeometryAggregateProfileRecord> profiles(profileHeader.profileCount);
			std::memcpy(profiles.data(), rawProfiles.data(), rawProfiles.size());
			const bool valid = profileHeader.contentHash == CalculateVirtualGeometryCompactHash(rawProfiles)
				&& std::all_of(profiles.begin(), profiles.end(), [&](const auto& profile) {
					return profile.kind <= static_cast<std::uint32_t>(VirtualGeometryAggregateProfileKind::Fallback)
						&& (profile.flags & kVirtualGeometryAggregateProfileFlagCompleteFallbackCoverage) != 0u
						&& profile.stableId != 0u && profile.fallbackStableId == profile.stableId
						&& std::isfinite(profile.canonicalError) && profile.canonicalError >= 0.0f
						&& std::isfinite(profile.coverageRatio) && profile.coverageRatio >= 0.0f
						&& profile.coverageRatio <= 1.0f && profile.sourceElementCount != 0u
						&& profile.fallbackClusterId < mesh.virtualGeometryClusters.size()
						&& profile.reserved == 0u;
				});
			if (valid)
			{
				mesh.hasVirtualGeometryAggregateProfile = true;
				mesh.virtualGeometryAggregateProfileProvenance = VirtualGeometryAggregateProfileProvenance::CookedSidecar;
				mesh.virtualGeometryAggregateProfiles = std::move(profiles);
				mesh.virtualGeometryAggregateProfileOffset = offset;
				return;
			}
			mesh.virtualGeometryAggregateProfileDiagnostic = "VGAP aggregate profile validation failed.";
			// A false-positive magic match must not prevent the validated sidecar
			// appended by the current cooker from being discovered later.
			continue;
		}

		const auto rootIt = std::find_if(mesh.virtualGeometryClusters.begin(), mesh.virtualGeometryClusters.end(),
			[](const VirtualGeometryClusterRecord& cluster) { return cluster.parentClusterId == kVirtualGeometryInvalidIndex; });
		if (rootIt == mesh.virtualGeometryClusters.end()
			|| std::count_if(mesh.virtualGeometryClusters.begin(), mesh.virtualGeometryClusters.end(),
				[](const VirtualGeometryClusterRecord& cluster) { return cluster.parentClusterId == kVirtualGeometryInvalidIndex; }) != 1)
		{
			mesh.virtualGeometryAggregateProfileDiagnostic = "No valid VGAP sidecar exists; multiple roots require a regional aggregate-profile recook.";
			return;
		}
		std::uint32_t fallbackClusterId = rootIt->clusterId;
		for (std::size_t guard = 0u; guard < mesh.virtualGeometryClusters.size(); ++guard)
		{
			const VirtualGeometryClusterRecord& cluster = mesh.virtualGeometryClusters[fallbackClusterId];
			if (cluster.fallbackClusterId == fallbackClusterId)
			{
				break;
			}
			fallbackClusterId = cluster.fallbackClusterId;
		}
		float canonicalError = 0.0f;
		for (const VirtualGeometryClusterRecord& cluster : mesh.virtualGeometryClusters)
		{
			canonicalError = std::max(canonicalError, cluster.geometricError);
		}
		const std::span<const std::uint8_t> clusterBytes(
			reinterpret_cast<const std::uint8_t*>(mesh.virtualGeometryClusters.data()),
			mesh.virtualGeometryClusters.size() * sizeof(VirtualGeometryClusterRecord));
		std::uint64_t stableId = CalculateVirtualGeometryCompactHash(clusterBytes);
		mesh.virtualGeometryAggregateProfiles.push_back({
			static_cast<std::uint32_t>(VirtualGeometryAggregateProfileKind::Fallback),
			kVirtualGeometryAggregateProfileFlagCompleteFallbackCoverage,
			stableId == 0u ? 1u : stableId, canonicalError,
			static_cast<std::uint32_t>(mesh.virtualGeometryClusters.size()), 1.0f,
			stableId == 0u ? 1u : stableId, fallbackClusterId, 0u });
		mesh.hasVirtualGeometryAggregateProfile = true;
		mesh.virtualGeometryAggregateProfileProvenance = VirtualGeometryAggregateProfileProvenance::LegacyDerived;
		if (mesh.virtualGeometryAggregateProfileDiagnostic.empty())
		{
			mesh.virtualGeometryAggregateProfileDiagnostic = "No VGAP sidecar was found; a validated legacy fallback profile was derived.";
		}
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
		if (!TryMultiply(static_cast<std::size_t>(header.meshletSkinningDataCount),
			static_cast<std::size_t>(kDeformableMeshSkinningStrideBytes), skinningBytes))
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

		AnimationResourceValidationResult deformableValidation;
		if (!ValidateDeformableMeshPayload(
			fileBytes, header, resolvedPath, 0U, 0U, false, deformableValidation)
			|| !ValidateDeformableVirtualGeometrySidecar(fileBytes, header, resolvedPath, deformableValidation))
		{
			TERRAENGINE_LOG(LogLevel::Error,
				deformableValidation.diagnosticCode << ": " << deformableValidation.message
				<< " path=" << resolvedPath);
			return nullptr;
		}

		const std::size_t payloadOffset = header.headerSize;
		auto mesh = std::make_unique<Mesh>();
		mesh->header = header;
		mesh->sourcePath = resolvedPath.string();
		if(!CopyValidatedDeformableRuntimeMetadata(fileBytes, header, *mesh)) {
			TERRAENGINE_LOG(LogLevel::Error,
				"Validated TDEF metadata could not be copied into runtime mesh state: " << resolvedPath);
			return nullptr;
		}

		// AVPD has no pointer in the frozen TEMF/TDEF headers. Current cookers
		// append it after TDFV, so scan aligned extension starts and accept only a
		// fully validated directory whose full-width mesh generation matches.
		if(mesh->hasDeformablePayload)
		{
			for(std::uint64_t offset = header.headerSize;
				offset + sizeof(AnimatedVirtualPageDirectoryHeader)
					<= fileBytes.size();
				offset += alignof(std::uint32_t))
			{
				std::uint32_t magic = 0u;
				std::memcpy(&magic, fileBytes.data() + offset, sizeof(magic));
				if(magic != kAnimatedVirtualPageDirectoryMagicValue)
				{
					continue;
				}
				TerraEngine::Resource::AnimatedVirtualPageCatalog catalog =
					TerraEngine::Resource::ParseAnimatedVirtualPageDirectory(
						fileBytes, offset);
				if(!catalog.valid
					|| catalog.header.meshPayloadGeneration
						!= mesh->deformablePayloadGeneration)
				{
					continue;
				}
				std::vector<std::vector<AnimatedVirtualPageSectionRecord>>
					parsedSections;
				parsedSections.reserve(catalog.entries.size());
				bool everyPageValid = true;
				for(const AnimatedVirtualPageDirectoryEntry& entry :
					catalog.entries)
				{
					const std::span<const std::uint8_t> pageBytes(
						fileBytes.data() + entry.blobOffset,
						static_cast<std::size_t>(entry.blobSize));
					TerraEngine::Resource::AnimatedVirtualPageBlob blob =
						TerraEngine::Resource::ParseAnimatedVirtualPageBlob(
							pageBytes,
							entry,
							mesh->deformablePayloadGeneration);
					if(!blob.valid)
					{
						everyPageValid = false;
						break;
					}
					parsedSections.push_back(std::move(blob.sections));
				}
				if(!everyPageValid)
				{
					continue;
				}
				mesh->hasAnimatedVirtualPageDirectory = true;
				mesh->animatedVirtualPageDirectory = catalog.header;
				mesh->animatedVirtualPageEntries = std::move(catalog.entries);
				mesh->animatedVirtualPageRoots = std::move(catalog.rootPageIds);
				mesh->animatedVirtualPageSections =
					std::move(parsedSections);
				mesh->animatedVirtualPageDirectoryOffset = offset;
				break;
			}
			if(!mesh->hasAnimatedVirtualPageDirectory)
			{
				mesh->animatedVirtualPageDirectoryDiagnostic =
					"No valid AVPD directory matched the deformable payload generation.";
			}
		}
		// VGXD is an optional static compact footer. Clear every candidate-facing
		// vector before scanning so a reload can never retain metadata from a
		// rejected footer; malformed compact data still leaves expanded TEMF usable.
		mesh->hasVirtualGeometryCompactDirectory = false;
		mesh->virtualGeometryCompactDirectory = {};
		mesh->virtualGeometryCompactEntries.clear();
		mesh->virtualGeometryCompactPages.clear();
		mesh->virtualGeometryCompactBlocks.clear();
		mesh->virtualGeometryCompactDirectoryOffset = 0u;
		mesh->virtualGeometryCompactDiagnostic.clear();
		std::string compactCandidateDiagnostic;
		for(std::uint64_t offset = header.headerSize; offset + sizeof(VirtualGeometryExtensionDirectoryHeader) <= fileBytes.size(); offset += 4u)
		{
			VirtualGeometryExtensionDirectoryHeader directory{};
			std::memcpy(&directory, fileBytes.data() + offset, sizeof(directory));
			if(directory.magic != kVirtualGeometryExtensionDirectoryMagicValue) continue;
			if(directory.version != kVirtualGeometryExtensionDirectoryVersion
				|| directory.headerSize != sizeof(directory) || directory.entryCount == 0u)
			{
				compactCandidateDiagnostic =
					"VGXD header identity, version, or entry count is invalid.";
				continue;
			}
			const std::uint64_t entriesBytes = static_cast<std::uint64_t>(directory.entryCount) * sizeof(VirtualGeometryExtensionDirectoryEntry);
			if(directory.byteSize < sizeof(directory) || directory.entryTableOffset < sizeof(directory)
				|| directory.byteSize > fileBytes.size() - offset
				|| entriesBytes > directory.byteSize - directory.entryTableOffset)
			{
				compactCandidateDiagnostic =
					"VGXD header or entry table exceeds the cooked file range.";
				continue;
			}
			const std::span<const std::uint8_t> footer(fileBytes.data() + offset, directory.byteSize);
			std::string directoryError;
			if(!TerraEngine::Resource::ValidateVirtualGeometryExtensionDirectory(footer, {}, &directoryError))
			{
				compactCandidateDiagnostic = std::move(directoryError);
				continue;
			}
			std::vector<VirtualGeometryExtensionDirectoryEntry> entries(directory.entryCount);
			std::memcpy(entries.data(), footer.data() + directory.entryTableOffset, static_cast<std::size_t>(entriesBytes));
			bool valid = true;
			for(const auto& entry : entries)
				if(entry.byteOffset < directory.headerSize || entry.byteSize == 0u
					|| entry.byteOffset > directory.byteSize
					|| entry.byteSize > directory.byteSize - entry.byteOffset)
				{
					compactCandidateDiagnostic =
						"VGXD contains an entry outside its validated footer range.";
					valid = false;
					break;
				}
			if(valid)
			{
				std::vector<VirtualGeometryCompactPageDescriptor> pages;
				std::vector<VirtualGeometryCompactBlockDescriptor> blocks;
				std::vector<std::pair<std::uint64_t, std::uint64_t>> blockTableRanges;
				for(const auto& entry : entries)
				{
					if(entry.type == static_cast<std::uint32_t>(VirtualGeometryExtensionType::CompactPage)
						&& entry.byteSize >= sizeof(VirtualGeometryCompactPageDescriptor))
					{
						VirtualGeometryCompactPageDescriptor page{}; std::memcpy(&page, footer.data() + entry.byteOffset, sizeof(page));
						if(entry.byteSize != sizeof(page)
							|| entry.contentHash != CalculateVirtualGeometryCompactHash(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&page), sizeof(page)))
							|| page.profile != 64u || page.blockCount == 0u || page.requiredCapabilities == 0u
							|| page.decodedByteSize != entry.decodedByteSize)
						{
							compactCandidateDiagnostic =
								"VGXD compact page metadata or integrity is invalid.";
							valid = false;
							break;
						}
						pages.push_back(page);
					}
					else if(entry.type == static_cast<std::uint32_t>(VirtualGeometryExtensionType::TerraBlock)
						&& entry.byteSize % sizeof(VirtualGeometryCompactBlockDescriptor) == 0u)
					{
						if(entry.contentHash != CalculateVirtualGeometryCompactHash(footer.subspan(entry.byteOffset, entry.byteSize)))
						{
							compactCandidateDiagnostic =
								"VGXD Terra block descriptor table failed integrity validation.";
							valid = false;
							break;
						}
						blockTableRanges.emplace_back(entry.byteOffset, entry.byteSize);
						const auto count = entry.byteSize / sizeof(VirtualGeometryCompactBlockDescriptor);
						for(std::uint32_t i = 0u; i < count; ++i)
						{
							VirtualGeometryCompactBlockDescriptor block{}; std::memcpy(&block, footer.data() + entry.byteOffset + i * sizeof(block), sizeof(block));
							if(block.byteSize == 0u || block.requiredCapabilities == 0u || block.byteOffset > directory.byteSize
								|| block.byteSize > directory.byteSize - block.byteOffset
								|| block.contentHash != CalculateVirtualGeometryCompactHash(footer.subspan(block.byteOffset, block.byteSize)))
							{
								compactCandidateDiagnostic =
									"VGXD Terra block range or stored integrity is invalid.";
								valid = false;
								break;
							}
							const auto decoded = TerraEngine::Resource::DecodeVirtualGeometryCompactBlock(footer.subspan(block.byteOffset, block.byteSize), {});
							if(!decoded.IsComplete() || decoded.header.codec != block.codec || decoded.header.decodedByteSize != block.decodedByteSize
								|| decoded.bytes.size() != block.decodedByteSize)
							{
								compactCandidateDiagnostic = decoded.error.empty()
									? "VGXD decoded Terra block metadata does not match its descriptor."
									: decoded.error;
								valid = false;
								break;
							}
							blocks.push_back(block);
						}
					}
					else
					{
						compactCandidateDiagnostic =
							"VGXD contains an unsupported required entry or malformed Terra block table.";
						valid = false;
						break;
					}
					if(!valid) break;
				}
				for(const VirtualGeometryCompactPageDescriptor& page : pages)
				{
					const std::uint64_t pageBlockBytes = static_cast<std::uint64_t>(page.blockCount) * sizeof(VirtualGeometryCompactBlockDescriptor);
					const bool pageTableIsKnown = std::any_of(blockTableRanges.begin(), blockTableRanges.end(), [&page, pageBlockBytes](const auto& range) {
						return page.blockTableOffset >= range.first && pageBlockBytes <= range.second - (page.blockTableOffset - range.first);
					});
					if(!pageTableIsKnown)
					{
						compactCandidateDiagnostic =
							"VGXD compact page points outside every validated block descriptor table.";
						valid = false;
						break;
					}
				}
				if(valid && (pages.empty() || blocks.empty()))
				{
					compactCandidateDiagnostic =
						"VGXD has no usable compact pages or Terra blocks.";
					valid = false;
				}
				if(valid)
				{
					mesh->virtualGeometryCompactPages = std::move(pages);
					mesh->virtualGeometryCompactBlocks = std::move(blocks);
				}
			}
			if(!valid) continue;
			mesh->hasVirtualGeometryCompactDirectory = true;
			mesh->virtualGeometryCompactDirectory = directory;
			mesh->virtualGeometryCompactEntries = std::move(entries);
			mesh->virtualGeometryCompactDirectoryOffset = offset;
			break;
		}
		if(!mesh->hasVirtualGeometryCompactDirectory)
		{
			// Preserve one bounded actionable reason on the mesh. Renderer and
			// automation copy this value without reopening the cooked file.
			mesh->virtualGeometryCompactDiagnostic =
				compactCandidateDiagnostic.empty()
					? "No VGXD compact directory was found in the cooked mesh."
					: compactCandidateDiagnostic;
		}

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
		// True page-streaming readiness requires independently verifiable AVPB
		// ranges. Legacy embedded virtual geometry remains renderable only through
		// the existing compatibility/owner fallback path.
		mesh->virtualGeometryPageStreamingReady =
			mesh->hasAnimatedVirtualPageDirectory;

		const VirtualMeshValidationResult virtualGeometryValidation = ValidateVirtualGeometryPayloadMetadata(header, *mesh, resolvedPath);
		if (VirtualMeshValidationRejected(virtualGeometryValidation))
		{
			return nullptr;
		}
		for (const std::string& warning : virtualGeometryValidation.warnings)
		{
			TERRAENGINE_LOG(LogLevel::Warning, warning);
		}
		// This publication is derived from fully validated immutable tables and
		// therefore stays safe for render-worker snapshots. Legacy profiles are
		// explicitly marked so editor repair can recook only this mesh later.
		ResolveVirtualGeometryAggregateProfile(fileBytes, header, *mesh);

		return mesh;
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

	const std::vector<std::uint8_t> fileBytes = ReadFileBytes(result.resolvedPath);
	result.fileSizeBytes = fileBytes.size();
	if (fileBytes.size() < sizeof(MeshHeader))
	{
		RejectDeformableValidation(result,
			fileBytes.empty() ? AnimationStatus::MissingAsset : AnimationStatus::TruncatedInput,
			fileBytes.empty() ? "DeformableMeshMissing" : "DeformableTemfHeaderTruncated",
			fileBytes.empty()
				? "The cooked deformable TEMF could not be read from the project or mounted runtime content."
				: "The TEMF is too small to contain its fixed mesh header.");
		return result;
	}

	MeshHeader header{};
	std::memcpy(&header, fileBytes.data(), sizeof(header));
	result.detectedMajorVersion = header.version;
	std::uint32_t magic = 0U;
	std::memcpy(&magic, header.magic, sizeof(magic));
	if (magic != kMeshMagicValue || header.headerSize != sizeof(MeshHeader))
	{
		RejectDeformableValidation(result, AnimationStatus::InvalidEnvelope,
			"DeformableTemfHeaderInvalid",
			"The TEMF magic or fixed header size is invalid. Restore or recook the deformable mesh.");
		return result;
	}
	if (header.version != kMeshCurrentVersion)
	{
		RejectDeformableValidation(result, AnimationStatus::UnsupportedMajorVersion,
			"DeformableTemfVersionUnsupported",
			"The TEMF version is unsupported by this runtime. Recook the mesh with the current editor.");
		return result;
	}

	if (ValidateDeformableMeshPayload(fileBytes, header, result.resolvedPath,
		expectedSkeletonJointCount, expectedCompatibilityFingerprint, true, result))
	{
		ValidateDeformableVirtualGeometrySidecar(fileBytes, header, result.resolvedPath, result);
	}
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

	if (mesh->hasDeformablePayload)
	{
		// The static mesh box is insufficient for CPU broadphase queries: a skin
		// or morph can cross a view plane before compute publishes current page
		// bounds. TDEF validation proves this whole-animation envelope is finite;
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
