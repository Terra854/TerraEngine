// TerrainHydraulicErosion.cpp -- Bounded integer sediment transfer preparation.

#include "Editor/TerraEngine-Editor/Terrain/TerrainHydraulicErosion.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace TerraEngine::Editor::Terrain
{
	namespace
	{
		using namespace ::TerraEngine::Terrain;
		TerrainProceduralResult Fail(TerrainResultCode code, std::string message) { TerrainProceduralResult result; result.code = code; result.message = std::move(message); return result; }
		bool Selected(const TerrainProceduralArea& area, std::size_t index) noexcept { return area.selectionMask.empty() || area.selectionMask[index] != 0u; }
	}

	TerrainProceduralResult ApplyTerrainHydraulicErosion(const TerrainProceduralRequest& request, const TerrainHydraulicErosionSettings& settings)
	{
		if(settings.iterations == 0u || settings.iterations > settings.maximumIterations || settings.erosionPerIterationMm <= 0)
			return Fail(TerrainResultCode::InvalidArgument, "Hydraulic erosion iteration or erosion limits are invalid.");
		if(request.areas.size() > std::numeric_limits<std::size_t>::max() / (kTerrainAreaSampleCount * sizeof(std::int32_t) * 2u)) return Fail(TerrainResultCode::Overflow, "Hydraulic erosion memory calculation overflowed.");
		const std::size_t requiredBytes = request.areas.size() * kTerrainAreaSampleCount * sizeof(std::int32_t) * 2u;
		if(requiredBytes > settings.maximumWorkingBytes || requiredBytes > request.budget.maximumWorkingBytes)
			return Fail(TerrainResultCode::BudgetExceeded, "Hydraulic erosion exceeds its declared double-buffer memory budget.");
		// The identity remap performs shared immutable input validation and sorting.
		TerrainProceduralResult result = ApplyTerrainHeightRemap(request, {}); if(!result.Succeeded()) return result;
		for(std::uint32_t iteration = 0; iteration < settings.iterations; ++iteration)
		{
			if(request.cancellation && request.cancellation->load(std::memory_order_relaxed)) return Fail(TerrainResultCode::Cancelled, "Hydraulic erosion was cancelled before publication.");
			for(TerrainProceduralArea& area : result.areas)
			{
				std::vector<std::int32_t> next = area.heightMm;
				for(std::uint32_t z = 1u; z + 1u < static_cast<std::uint32_t>(kTerrainAreaExtentSamples); ++z)
				{
					if(request.cancellation && request.cancellation->load(std::memory_order_relaxed)) return Fail(TerrainResultCode::Cancelled, "Hydraulic erosion was cancelled before publication.");
					for(std::uint32_t x = 1u; x + 1u < static_cast<std::uint32_t>(kTerrainAreaExtentSamples); ++x)
					{
						const std::size_t index = static_cast<std::size_t>(z) * kTerrainAreaExtentSamples + x; if(!Selected(area, index)) continue;
						const std::array<std::size_t, 4u> neighbours = {index - 1u, index + 1u, index - kTerrainAreaExtentSamples, index + kTerrainAreaExtentSamples};
						std::optional<std::size_t> lowest; for(const std::size_t candidate : neighbours) if(Selected(area, candidate) && (!lowest || area.heightMm[candidate] < area.heightMm[*lowest])) lowest = candidate;
						if(lowest && area.heightMm[index] > area.heightMm[*lowest] && next[index] >= std::numeric_limits<std::int32_t>::min() + settings.erosionPerIterationMm && next[*lowest] <= std::numeric_limits<std::int32_t>::max() - settings.erosionPerIterationMm) { next[index] -= settings.erosionPerIterationMm; next[*lowest] += settings.erosionPerIterationMm; }
					}
				}
				area.heightMm = std::move(next);
			}
		}
		TerrainProceduralRequest finalRequest = request; finalRequest.areas = std::move(result.areas); return ApplyTerrainHeightRemap(finalRequest, {});
	}
}
