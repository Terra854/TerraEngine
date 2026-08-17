// TerrainSelectionQuery.cpp -- Worker-safe terrain selection evaluation.

#include "Terrain/TerrainSelectionQuery.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace TerraEngine::Terrain
{
	namespace
	{
		[[nodiscard]] TerrainSelectionResult Fail(TerrainResultCode code, std::string message)
		{
			TerrainSelectionResult result;
			result.code = code;
			result.message = std::move(message);
			return result;
		}

		[[nodiscard]] bool CheckedAdd(std::int64_t value, std::uint32_t extent, std::int64_t& outEnd) noexcept
		{
			if(value > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(extent)) return false;
			outEnd = value + static_cast<std::int64_t>(extent);
			return true;
		}

		[[nodiscard]] std::uint64_t AbsoluteDifference(std::int32_t left, std::int32_t right) noexcept
		{
			const std::int64_t difference = static_cast<std::int64_t>(left) - static_cast<std::int64_t>(right);
			return static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
		}

		[[nodiscard]] std::uint64_t MaximumNeighbourRise(
			const TerrainSelectionGrid& grid,
			std::uint32_t localX,
			std::uint32_t localZ) noexcept
		{
			const std::size_t centreIndex = static_cast<std::size_t>(localZ) * grid.storageWidth + localX;
			const std::int32_t centre = grid.heightMm[centreIndex];
			std::uint64_t rise = 0u;
			auto compare = [&](std::uint32_t neighbourX, std::uint32_t neighbourZ)
			{
				const std::size_t neighbourIndex = static_cast<std::size_t>(neighbourZ) * grid.storageWidth + neighbourX;
				rise = std::max(rise, AbsoluteDifference(centre, grid.heightMm[neighbourIndex]));
			};
			if(localX > 0u) compare(localX - 1u, localZ);
			if(localX + 1u < grid.storageWidth) compare(localX + 1u, localZ);
			if(localZ > 0u) compare(localX, localZ - 1u);
			if(localZ + 1u < grid.storageHeight) compare(localX, localZ + 1u);
			return rise;
		}

		[[nodiscard]] bool MatchesLayer(const TerrainWeightSample& sample, std::uint16_t layerId, std::uint8_t minimumWeight) noexcept
		{
			return std::any_of(sample.slots.begin(), sample.slots.end(), [&](const TerrainWeightSlot& slot)
			{
				return slot.layerId == layerId && slot.weight >= minimumWeight;
			});
		}
	}

	TerrainSelectionResult EvaluateTerrainSelection(const TerrainSelectionGrid& grid, const TerrainSelectionCriteria& criteria)
	{
		if(grid.storageWidth == 0u || grid.storageHeight == 0u || !grid.evaluationBounds.IsValid())
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection requires non-empty storage and valid evaluation bounds.");
		if(criteria.sampleSpacingMm == 0u)
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection sample spacing must be non-zero.");
		if(criteria.minimumHeightMm && criteria.maximumHeightMm && *criteria.minimumHeightMm > *criteria.maximumHeightMm)
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection height range is reversed.");
		if(criteria.minimumSlopePermille && criteria.maximumSlopePermille && *criteria.minimumSlopePermille > *criteria.maximumSlopePermille)
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection slope range is reversed.");
		if(criteria.layerStableId && *criteria.layerStableId == 0u)
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection layer identity zero is reserved.");

		const std::uint64_t storageCount64 = static_cast<std::uint64_t>(grid.storageWidth) * grid.storageHeight;
		if(storageCount64 > std::numeric_limits<std::size_t>::max() || grid.heightMm.size() != static_cast<std::size_t>(storageCount64))
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection height storage size does not match its dimensions.");
		if(!grid.paintedMask.empty() && grid.paintedMask.size() != grid.heightMm.size())
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection painted-mask size does not match height storage.");
		if(!grid.weights.empty() && grid.weights.size() != grid.heightMm.size())
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection weight size does not match height storage.");
		if(criteria.minimumPaintedMask && grid.paintedMask.empty())
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection requested a painted mask but none was supplied.");
		if(criteria.layerStableId && grid.weights.empty())
			return Fail(TerrainResultCode::InvalidArgument, "Terrain selection requested layer influence but no weights were supplied.");

		std::int64_t storageEndX = 0;
		std::int64_t storageEndZ = 0;
		if(!CheckedAdd(grid.storageOrigin.x, grid.storageWidth, storageEndX) || !CheckedAdd(grid.storageOrigin.z, grid.storageHeight, storageEndZ))
			return Fail(TerrainResultCode::Overflow, "Terrain selection storage bounds overflow canonical coordinates.");
		if(grid.evaluationBounds.minX < grid.storageOrigin.x || grid.evaluationBounds.minZ < grid.storageOrigin.z
			|| grid.evaluationBounds.maxXExclusive > storageEndX || grid.evaluationBounds.maxZExclusive > storageEndZ)
			return Fail(TerrainResultCode::OutOfBounds, "Terrain selection evaluation bounds are outside supplied storage.");

		const std::uint64_t evaluationWidth = static_cast<std::uint64_t>(grid.evaluationBounds.maxXExclusive - grid.evaluationBounds.minX);
		const std::uint64_t evaluationHeight = static_cast<std::uint64_t>(grid.evaluationBounds.maxZExclusive - grid.evaluationBounds.minZ);
		const std::uint64_t evaluationCount64 = evaluationWidth * evaluationHeight;
		if(evaluationCount64 > criteria.maximumEvaluatedSamples || evaluationCount64 > std::numeric_limits<std::size_t>::max())
			return Fail(TerrainResultCode::BudgetExceeded, "Terrain selection exceeds its declared sample budget.");

		TerrainSelectionResult result;
		result.mask.assign(static_cast<std::size_t>(evaluationCount64), 0u);
		std::int64_t selectedMinX = std::numeric_limits<std::int64_t>::max();
		std::int64_t selectedMinZ = std::numeric_limits<std::int64_t>::max();
		std::int64_t selectedMaxX = std::numeric_limits<std::int64_t>::min();
		std::int64_t selectedMaxZ = std::numeric_limits<std::int64_t>::min();

		for(std::int64_t sampleZ = grid.evaluationBounds.minZ; sampleZ < grid.evaluationBounds.maxZExclusive; ++sampleZ)
		{
			for(std::int64_t sampleX = grid.evaluationBounds.minX; sampleX < grid.evaluationBounds.maxXExclusive; ++sampleX)
			{
				const std::uint32_t localX = static_cast<std::uint32_t>(sampleX - grid.storageOrigin.x);
				const std::uint32_t localZ = static_cast<std::uint32_t>(sampleZ - grid.storageOrigin.z);
				const std::size_t storageIndex = static_cast<std::size_t>(localZ) * grid.storageWidth + localX;
				const std::size_t outputIndex = static_cast<std::size_t>(sampleZ - grid.evaluationBounds.minZ) * static_cast<std::size_t>(evaluationWidth)
					+ static_cast<std::size_t>(sampleX - grid.evaluationBounds.minX);
				const TerrainSampleCoordinate sample{sampleX, sampleZ};
				bool selected = !criteria.worldBounds || criteria.worldBounds->Contains(sample);
				const std::int32_t height = grid.heightMm[storageIndex];
				if(criteria.minimumHeightMm) selected = selected && height >= *criteria.minimumHeightMm;
				if(criteria.maximumHeightMm) selected = selected && height <= *criteria.maximumHeightMm;
				if(criteria.minimumPaintedMask) selected = selected && grid.paintedMask[storageIndex] >= *criteria.minimumPaintedMask;
				if(criteria.layerStableId) selected = selected && MatchesLayer(grid.weights[storageIndex], *criteria.layerStableId, criteria.minimumLayerWeight);

				if(selected && (criteria.minimumSlopePermille || criteria.maximumSlopePermille))
				{
					const std::uint64_t scaledRise = MaximumNeighbourRise(grid, localX, localZ) * 1000u;
					if(criteria.minimumSlopePermille)
						selected = scaledRise >= static_cast<std::uint64_t>(*criteria.minimumSlopePermille) * criteria.sampleSpacingMm;
					if(selected && criteria.maximumSlopePermille)
						selected = scaledRise <= static_cast<std::uint64_t>(*criteria.maximumSlopePermille) * criteria.sampleSpacingMm;
				}

				if(!selected) continue;
				result.mask[outputIndex] = 255u;
				++result.selectedSampleCount;
				selectedMinX = std::min(selectedMinX, sampleX);
				selectedMinZ = std::min(selectedMinZ, sampleZ);
				selectedMaxX = std::max(selectedMaxX, sampleX);
				selectedMaxZ = std::max(selectedMaxZ, sampleZ);
			}
		}

		if(result.selectedSampleCount != 0u)
			result.selectedBounds = TerrainSampleBounds{selectedMinX, selectedMinZ, selectedMaxX + 1, selectedMaxZ + 1};
		return result;
	}
}
