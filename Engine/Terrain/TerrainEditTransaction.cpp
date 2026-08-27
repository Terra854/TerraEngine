// TerrainEditTransaction.cpp -- Edit validation independent of mutable stores.

#include "Terrain/TerrainEditTransaction.h"

#include <algorithm>

namespace TerraEngine::Terrain
{
	TerrainInvalidationSet DetermineTerrainInvalidationKinds(
		const TerrainEditTransaction& transaction) noexcept
	{
		TerrainInvalidationSet result{};
		if(transaction.layerStackPatch.has_value()) result.Add(TerrainInvalidationKind::MaterialBindings);
		for(const TerrainPatch& patch : transaction.patches)
		{
			switch(patch.kind)
			{
			case TerrainPatchKind::ReplaceHeight:
			case TerrainPatchKind::ReplaceHoles:
			case TerrainPatchKind::ReplaceDescriptorMetadata:
				result.Add(TerrainInvalidationKind::GeometryAndCollision);
				break;
			case TerrainPatchKind::ReplaceWeightGroup:
				result.Add(TerrainInvalidationKind::ControlTextures);
				break;
			}
		}
		return result;
	}

	bool TerrainExactSpan::IsComplete() const noexcept
	{
		return uncompressedByteCount != 0u
			&& compressedContentHash.IsValid()
			&& !compressedBytes.empty()
			&& firstByte <= uncompressedByteCount;
	}

	TerrainCodecResult ValidateTerrainExactSpan(const TerrainExactSpan& span)
	{
		if (!span.IsComplete())
			return {TerrainResultCode::InvalidArgument, "Terrain history span is missing its exact compressed representation."};
		if (HashTerrainBytes(span.compressedBytes) != span.compressedContentHash)
			return {TerrainResultCode::HashMismatch, "Terrain history span compressed bytes do not match their retained identity."};
		if ((span.channel == TerrainChannelKind::WeightGroup && span.weightGroup == 0u)
			|| (span.channel != TerrainChannelKind::WeightGroup && span.weightGroup != 0u))
			return {TerrainResultCode::InvalidArgument, "Terrain history weight-group identity does not match its channel."};
		return {};
	}

	TerrainCodecResult ValidateTerrainEditTransaction(const TerrainEditTransaction& transaction, const TerrainAssetRoot& root)
	{
		if (transaction.cancelled) return {TerrainResultCode::Cancelled, "Terrain edit was cancelled before publication."};
		if (transaction.targetAssetUuid != root.assetUuid || !transaction.inputRootHash.IsValid() || transaction.inputRootHash != root.rootContentHash) return {TerrainResultCode::Conflict, "Terrain edit observed a different authored root."};
		if (!transaction.affectedBounds.IsValid() || (transaction.patches.empty() && !transaction.layerStackPatch.has_value()) || transaction.patches.size() > transaction.budget.maximumAreas || transaction.readSet.size() > transaction.budget.maximumAreas) return {TerrainResultCode::BudgetExceeded, "Terrain edit exceeds its declared bounded area budget."};
		if (transaction.expectedLayerStackGeneration != 0u && transaction.expectedLayerStackGeneration != root.layerStackGeneration) return {TerrainResultCode::Conflict, "Terrain layer stack changed while an edit was preparing."};
		if (transaction.layerStackPatch.has_value() && !transaction.patches.empty()) return {TerrainResultCode::InvalidArgument, "Terrain metadata and bulk area patches must prepare separate atomic roots."};
		for (std::size_t index = 0; index < transaction.patches.size(); ++index)
		{
			const TerrainPatch& patch = transaction.patches[index];
			if (patch.replacement.editGeneration == 0u || !patch.replacement.authoredContentHash.IsValid() || (index != 0u && !(transaction.patches[index - 1u].replacement.coordinate < patch.replacement.coordinate))) return {TerrainResultCode::InvalidArgument, "Terrain patches must be non-empty and sorted by unique area coordinate."};
			if (patch.kind == TerrainPatchKind::ReplaceWeightGroup)
			{
				if (patch.afterSpans.empty() || std::any_of(patch.afterSpans.begin(), patch.afterSpans.end(), [](const TerrainExactSpan& span) { return span.channel != TerrainChannelKind::WeightGroup || span.weightGroup == 0u; }))
					return {TerrainResultCode::InvalidArgument, "Terrain weight replacement requires exact after spans with stable non-zero group identities."};
				if (patch.beforeSpans.size() != patch.afterSpans.size())
					return {TerrainResultCode::InvalidArgument, "Terrain weight replacement requires one exact inverse span for every forward span."};
			}
			if (patch.kind == TerrainPatchKind::ReplaceHoles)
			{
				// Hole masks decide geometry, picking, and collision coverage. Require
				// immutable before/after bytes so undo cannot regenerate a different
				// cell mask after a material or derived-data change.
				if(patch.afterSpans.empty() || std::any_of(patch.afterSpans.begin(), patch.afterSpans.end(), [](const TerrainExactSpan& span) {
					return span.channel != TerrainChannelKind::Holes || span.weightGroup != 0u;
				}) || patch.beforeSpans.size() != patch.afterSpans.size())
					return {TerrainResultCode::InvalidArgument, "Terrain hole replacement requires matching exact canonical before/after masks."};
			}
			if (patch.kind == TerrainPatchKind::ReplaceDescriptorMetadata && patch.replacement.editGeneration <= root.publicationGeneration)
				return {TerrainResultCode::Conflict, "Terrain metadata patches must prepare a new descriptor generation."};
			for (const TerrainExactSpan& span : patch.beforeSpans)
				if (const TerrainCodecResult validation = ValidateTerrainExactSpan(span); !validation.Succeeded()) return validation;
			for (const TerrainExactSpan& span : patch.afterSpans)
				if (const TerrainCodecResult validation = ValidateTerrainExactSpan(span); !validation.Succeeded()) return validation;
		}
		return {};
	}
}
