// TerrainEditTransaction.h -- Typed, bounded authored terrain edit requests.

#pragma once

#include "Terrain/TerrainAssetStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Terrain
{
	enum class TerrainEditOrigin : std::uint8_t { VisibleEditor, Undo, Redo, Automation, Import, Repair, Test };
	enum class TerrainEditOutcome : std::uint8_t { Pending, Committed, Rejected, Cancelled, Failed, Duplicate };
	enum class TerrainPatchKind : std::uint8_t { ReplaceHeight, ReplaceHoles, ReplaceWeightGroup, ReplaceDescriptorMetadata };

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
		ContentHash operationFingerprint{};
		bool cancelled = false;
	};

	struct TerrainDerivedInvalidation
	{
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		Generation publicationGeneration = 0u;
		std::vector<TerrainAreaCoordinate> areas;
		// Hole edits need collision, picking, and visibility products rebuilt from
		// one authored root; consumers use this without decoding authoring blobs.
		bool holesChanged = false;
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
}
