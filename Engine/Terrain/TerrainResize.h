// TerrainResize.h -- Deterministic, detached terrain resize preparation.

#pragma once

#include "Terrain/TerrainAssetStore.h"

#include <functional>
#include <optional>
#include <string_view>

namespace TerraEngine::Terrain
{
	enum class TerrainResizeKind : std::uint8_t { Expand, Crop };
	enum class TerrainExpansionPolicy : std::uint8_t { EdgeBlend, ExtendEdge, FlatDefault };
	enum class TerrainBoundaryHolePolicy : std::uint8_t { CloseAtFormerEdge, ExtendConnectedBoundaryHoles };
	enum class TerrainResizeSide : std::uint8_t { MinimumX, MaximumX, MinimumZ, MaximumZ };
	enum class TerrainResizePreviewRegionKind : std::uint8_t { Retained, Changed };

	// Resize previews describe a small set of non-overlapping rectangles instead
	// of asking editor presentation code to infer an outer-minus-inner footprint.
	// Bounds remain in exact sample coordinates until a viewport localizes them.
	struct TerrainResizePreviewRegion
	{
		TerrainSampleBounds bounds{};
		TerrainResizePreviewRegionKind kind = TerrainResizePreviewRegionKind::Retained;
		[[nodiscard]] constexpr bool operator==(const TerrainResizePreviewRegion&) const noexcept = default;
	};

	struct TerrainResizePreviewGrid
	{
		std::vector<std::int64_t> sampleX;
		std::vector<std::int64_t> sampleZ;
	};

	struct TerrainResizeBudget
	{
		std::size_t maximumScannedAreas = 64u;
		std::size_t maximumGeneratedAreas = 64u;
		std::size_t maximumGeneratedSamples = 64u * kTerrainAreaSampleCount;
		std::size_t maximumPreparedBytes = 256u * 1024u * 1024u;
		std::size_t maximumHistoryBytes = 256u * 1024u * 1024u;
		std::size_t maximumResidentAreas = 32u;
	};

	struct TerrainResizeEdgeRisk
	{
		TerrainResizeSide side = TerrainResizeSide::MinimumX;
		std::int32_t minimumDeltaMm = 0;
		std::int32_t maximumDeltaMm = 0;
		std::int64_t averageDeltaMm = 0;
		std::uint32_t maximumSlopePerMille = 0u;
		std::size_t touchingHoleCount = 0u;
		std::string diagnosticCode;
	};

	struct TerrainCropLossSummary
	{
		std::uint64_t discardedSamples = 0u;
		std::uint64_t nonDefaultHeightSamples = 0u;
		std::uint64_t holeSamples = 0u;
		std::uint64_t paintedSamples = 0u;
		std::size_t removedDescriptors = 0u;
		std::size_t partialDescriptors = 0u;
		std::size_t retainedHistoryBytes = 0u;
	};

	struct TerrainResizeWorkEstimate
	{
		std::size_t scannedAreas = 0u;
		std::size_t generatedAreas = 0u;
		std::uint64_t generatedSamples = 0u;
		std::size_t preparedBytes = 0u;
	};

	struct TerrainResizeObservation
	{
		TerrainAreaCoordinate coordinate{};
		Generation editGeneration = 0u;
		ContentHash authoredContentHash{};
		[[nodiscard]] constexpr bool operator==(const TerrainResizeObservation&) const noexcept = default;
	};

	struct TerrainResizeDelta
	{
		TerrainResizeKind kind = TerrainResizeKind::Expand;
		TerrainSampleBounds previousBounds{};
		TerrainSampleBounds proposedBounds{};
		TerrainExpansionPolicy expansionPolicy = TerrainExpansionPolicy::EdgeBlend;
		TerrainBoundaryHolePolicy holePolicy = TerrainBoundaryHolePolicy::CloseAtFormerEdge;
		std::uint32_t transitionSamples = 64u;
		std::vector<TerrainAreaDescriptor> descriptorUpserts;
		std::vector<TerrainAreaCoordinate> descriptorRemovals;
		std::map<TerrainAreaCoordinate, TerrainAreaActiveRect> partialAreaBounds;
		std::vector<TerrainAreaCoordinate> forcedRetirements;
		std::vector<TerrainAreaCoordinate> affectedOneRing;
		std::vector<ContentHash> retainedBlobHashes;
	};

	struct TerrainResizeRequest
	{
		TerraEngine::UUID transactionId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID targetAssetUuid = TerraEngine::EMPTY_UUID;
		ContentHash expectedRootHash{};
		Generation expectedRootGeneration = 0u;
		TerrainResizeDelta delta{};
		TerrainResizeBudget budget{};
		bool holePolicyExplicit = false;
		bool confirmDiscard = false;
		ContentHash approvedProposalFingerprint{};
	};

	struct TerrainResizePreview
	{
		TerrainResultCode code = TerrainResultCode::Ok;
		TerrainResizeDelta delta{};
		std::size_t previousAreaCount = 0u;
		std::size_t proposedAreaCount = 0u;
		std::size_t addedAreaCount = 0u;
		std::size_t removedAreaCount = 0u;
		double physicalWidthMetres = 0.0;
		double physicalDepthMetres = 0.0;
		double previousPhysicalWidthMetres = 0.0;
		double previousPhysicalDepthMetres = 0.0;
		std::uint64_t retainedSampleCount = 0u;
		std::uint64_t addedSampleCount = 0u;
		std::uint64_t discardedSampleCount = 0u;
		std::size_t fullAreaCount = 0u;
		std::size_t partialAreaCount = 0u;
		bool requiresDataLossConfirmation = false;
		bool boundaryHoleChoiceRequired = false;
		bool discontinuityWarning = false;
		bool canCommit = false;
		ContentHash proposalFingerprint{};
		TerrainResizeBudget budget{};
		TerrainCropLossSummary cropLoss{};
		TerrainResizeWorkEstimate estimatedWork{};
		std::vector<TerrainResizeEdgeRisk> edgeRiskBySide;
		std::vector<TerrainResizeObservation> observations;
		std::vector<std::string> warnings;
		std::string message;
		[[nodiscard]] bool Succeeded() const noexcept { return code == TerrainResultCode::Ok; }
	};

	using TerrainHeightSampler = std::function<std::optional<std::int32_t>(TerrainSampleCoordinate)>;
	using TerrainSurfaceSampler = std::function<std::optional<TerrainWeightSample>(TerrainSampleCoordinate)>;
	using TerrainHoleSampler = std::function<bool(TerrainSampleCoordinate)>;

	[[nodiscard]] TERRAENGINE_API TerrainCodecResult ValidateTerrainResizeDelta(
		const TerrainResizeDelta& delta, const TerrainAssetRoot& root);
	[[nodiscard]] TERRAENGINE_API TerrainResizePreview PreviewTerrainResize(
		const TerrainAssetRoot& root, TerrainResizeDelta delta, std::size_t maximumAreas);
	[[nodiscard]] TERRAENGINE_API TerrainResizePreview PreviewTerrainResize(
		const TerrainAssetRoot& root, const TerrainResizeRequest& request);
	[[nodiscard]] TERRAENGINE_API ContentHash FingerprintTerrainResizeProposal(
		const TerrainAssetRoot& root, const TerrainResizeRequest& request,
		const TerrainResizePreview& preview);
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(TerrainResizeKind value) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(TerrainExpansionPolicy value) noexcept;
	[[nodiscard]] TERRAENGINE_API std::string_view ToString(TerrainBoundaryHolePolicy value) noexcept;
	[[nodiscard]] TERRAENGINE_API std::vector<TerrainResizePreviewRegion> BuildTerrainResizePreviewRegions(
		const TerrainResizeDelta& delta);
	[[nodiscard]] TERRAENGINE_API TerrainResizePreviewGrid BuildTerrainResizePreviewGrid(
		const TerrainResizeDelta& delta, std::size_t maximumSegmentsPerAxis = 64u);
	[[nodiscard]] TERRAENGINE_API std::int32_t InitializeExpandedTerrainHeight(
		const TerrainAssetRoot& root, const TerrainResizeDelta& delta,
		TerrainSampleCoordinate sample, const TerrainHeightSampler& source) noexcept;
	[[nodiscard]] TERRAENGINE_API TerrainWeightSample InitializeExpandedTerrainSurface(
		const TerrainAssetRoot& root, const TerrainResizeDelta& delta,
		TerrainSampleCoordinate sample, const TerrainSurfaceSampler& source) noexcept;
	[[nodiscard]] TERRAENGINE_API bool InitializeExpandedTerrainHole(
		const TerrainResizeDelta& delta, TerrainSampleCoordinate sample,
		const TerrainHoleSampler& source) noexcept;
	[[nodiscard]] TERRAENGINE_API std::vector<std::int32_t> CanonicalizeCroppedTerrainHeights(
		std::span<const std::int32_t> source, TerrainAreaActiveRect active);
	[[nodiscard]] TERRAENGINE_API std::vector<std::uint8_t> CanonicalizeCroppedTerrainHoles(
		std::span<const std::uint8_t> source, TerrainAreaActiveRect active);
	[[nodiscard]] TERRAENGINE_API std::vector<TerrainWeightSample> CanonicalizeCroppedTerrainSurfaces(
		std::span<const TerrainWeightSample> source, TerrainAreaActiveRect active,
		std::uint16_t defaultLayerId);
}
