// TerrainEditTransaction.h -- Typed, bounded authored terrain edit requests.

#pragma once

#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainResize.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Terrain
{
	enum class TerrainEditOrigin : std::uint8_t { VisibleEditor, Undo, Redo, Automation, Import, Repair, Test };
	enum class TerrainEditOutcome : std::uint8_t { Pending, Committed, Rejected, Cancelled, Failed, Duplicate };
	enum class TerrainPatchKind : std::uint8_t { ReplaceHeight, ReplaceHoles, ReplaceWeightGroup, ReplaceDescriptorMetadata };
	enum class TerrainInvalidationKind : std::uint8_t
	{
		GeometryAndCollision = 1u << 0u,
		ControlTextures = 1u << 1u,
		MaterialBindings = 1u << 2u
	};

	// A small typed set makes unions explicit while preventing unrelated integer
	// flags from entering terrain scheduling decisions.
	struct TerrainInvalidationSet
	{
		std::uint8_t bits = 0u;

		void Add(TerrainInvalidationKind kind) noexcept { bits |= static_cast<std::uint8_t>(kind); }
		void UnionWith(TerrainInvalidationSet other) noexcept { bits |= other.bits; }
		[[nodiscard]] bool Contains(TerrainInvalidationKind kind) const noexcept
		{
			return (bits & static_cast<std::uint8_t>(kind)) != 0u;
		}
		[[nodiscard]] bool Empty() const noexcept { return bits == 0u; }
		[[nodiscard]] constexpr bool operator==(const TerrainInvalidationSet&) const noexcept = default;
	};

	struct TerrainAreaObservation
	{
		TerrainAreaCoordinate coordinate{};
		Generation editGeneration = 0u;
		ContentHash authoredContentHash{};
		[[nodiscard]] constexpr bool operator==(const TerrainAreaObservation&) const noexcept = default;
	};

	// TerrainExactSpan preserves the exact compressed byte sequence used by a
	// brush or import operation. History replays descriptors through the normal
	// publication path, while these bytes make a pruned source provider unable
	// to silently change what an undo means.
	struct TerrainExactSpan
	{
		TerrainChannelKind channel = TerrainChannelKind::Height;
		std::uint16_t weightGroup = 0u;
		std::uint32_t firstByte = 0u;
		std::uint32_t uncompressedByteCount = 0u;
		ContentHash compressedContentHash{};
		ContentHash immutableBlobHash{};
		std::vector<std::uint8_t> compressedBytes;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct TerrainPatch
	{
		TerrainAreaDescriptor replacement{};
		TerrainPatchKind kind = TerrainPatchKind::ReplaceHeight;
		ContentHash preparedBlobHash{};
		std::vector<TerrainExactSpan> beforeSpans;
		std::vector<TerrainExactSpan> afterSpans;
		// Retained content-addressed blobs remain pinned until history pruning.
		// They can represent a complete channel when a span is not practical.
		std::vector<ContentHash> retainedBlobHashes;
	};

	struct TerrainEditBudget
	{
		std::size_t maximumAreas = 1u;
		std::size_t maximumSamples = kTerrainAreaSampleCount;
		std::size_t maximumPreparedBytes = Format::kTerrainAreaMaximumDecodedBytes;
	};

	// Kept in a committed receipt only after the root swap succeeds. Editor
	// history can construct fresh transactions from these exact value objects.
	struct TerrainHistoryPayload
	{
		std::vector<TerrainPatch> forwardPatches;
		std::vector<TerrainPatch> inversePatches;
		std::size_t retainedByteCount = 0u;
		std::optional<TerrainLayerPatch> forwardLayerStackPatch;
		std::optional<TerrainLayerPatch> inverseLayerStackPatch;
		std::optional<TerrainResizeDelta> forwardResize;
		std::optional<TerrainResizeDelta> inverseResize;
	};

	struct TerrainEditTransaction
	{
		TerraEngine::UUID transactionId = TerraEngine::EMPTY_UUID;
		TerrainEditOrigin origin = TerrainEditOrigin::VisibleEditor;
		TerraEngine::UUID targetAssetUuid = TerraEngine::EMPTY_UUID;
		std::string historyGroup;
		ContentHash inputRootHash{};
		Generation expectedLayerStackGeneration = 0u;
		TerrainSampleBounds affectedBounds{};
		TerrainEditBudget budget{};
		std::vector<TerrainAreaObservation> readSet;
		std::vector<TerrainPatch> patches;
		std::vector<TerrainPatch> inversePatches;
		// Layer metadata changes use the same protected root boundary as painted
		// channels, but carry no bulk area payload through the mutation queue.
		std::optional<TerrainLayerPatch> layerStackPatch;
		std::optional<TerrainLayerPatch> inverseLayerStackPatch;
		// Bounds and descriptor membership travel beside channel patches so resize
		// publication and history still cross one protected root boundary.
		std::optional<TerrainResizeDelta> resizeDelta;
		std::optional<TerrainResizeDelta> inverseResizeDelta;
		ContentHash operationFingerprint{};
		bool cancelled = false;
	};

	struct TerrainDerivedInvalidation
	{
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		Generation publicationGeneration = 0u;
		std::vector<TerrainAreaCoordinate> areas;
		std::vector<std::uint16_t> layerIds;
		TerrainInvalidationSet kinds{};

		[[nodiscard]] bool Contains(TerrainInvalidationKind kind) const noexcept
		{
			return kinds.Contains(kind);
		}
	};

	struct TerrainTransactionReceipt
	{
		TerraEngine::UUID transactionId = TerraEngine::EMPTY_UUID;
		TerrainEditOutcome outcome = TerrainEditOutcome::Pending;
		ContentHash previousRootHash{};
		ContentHash publishedRootHash{};
		Generation publicationGeneration = 0u;
		std::vector<TerrainAreaCoordinate> affectedAreas;
		TerrainResultCode code = TerrainResultCode::Ok;
		std::string message;
		std::optional<TerrainDerivedInvalidation> invalidation;
		std::optional<TerrainHistoryPayload> historyPayload;
		std::size_t retainedHistoryBytes = 0u;
		std::vector<ContentHash> retainedBlobHashes;
	};

	TERRAENGINE_API TerrainCodecResult ValidateTerrainEditTransaction(const TerrainEditTransaction& transaction, const TerrainAssetRoot& root);
	TERRAENGINE_API TerrainCodecResult ValidateTerrainExactSpan(const TerrainExactSpan& span);
	[[nodiscard]] TERRAENGINE_API TerrainInvalidationSet DetermineTerrainInvalidationKinds(
		const TerrainEditTransaction& transaction) noexcept;
}
