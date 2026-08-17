// TerrainProceduralOperators.cpp -- Pure worker-side deterministic height maps.

#include "Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.h"

#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace TerraEngine::Editor::Terrain
{
	namespace
	{
		using namespace ::TerraEngine::Terrain;

		TerrainProceduralResult Fail(TerrainResultCode code, std::string message)
		{
			TerrainProceduralResult result; result.code = code; result.message = std::move(message); return result;
		}

		bool Cancelled(const TerrainProceduralRequest& request) noexcept { return request.cancellation && request.cancellation->load(std::memory_order_relaxed); }

		bool IsSelected(const TerrainProceduralArea& area, std::size_t index) noexcept
		{
			return area.selectionMask.empty() || area.selectionMask[index] != 0u;
		}

		bool AreaOrigin(const TerrainAreaCoordinate& area, TerrainSampleCoordinate& out) noexcept
		{
			if(area.x > std::numeric_limits<std::int64_t>::max() / kTerrainAreaExtentSamples || area.x < std::numeric_limits<std::int64_t>::min() / kTerrainAreaExtentSamples
				|| area.z > std::numeric_limits<std::int64_t>::max() / kTerrainAreaExtentSamples || area.z < std::numeric_limits<std::int64_t>::min() / kTerrainAreaExtentSamples) return false;
			out = {area.x * kTerrainAreaExtentSamples, area.z * kTerrainAreaExtentSamples}; return true;
		}

		TerrainProceduralResult Prepare(const TerrainProceduralRequest& request)
		{
			if(request.areas.empty() || request.areas.size() > request.budget.maximumAreas)
				return Fail(TerrainResultCode::BudgetExceeded, "Procedural terrain request exceeds its area budget.");
			if(request.areas.size() > std::numeric_limits<std::size_t>::max() / kTerrainAreaSampleCount)
				return Fail(TerrainResultCode::Overflow, "Procedural terrain sample count overflowed its size budget.");
			const std::size_t sampleCount = request.areas.size() * kTerrainAreaSampleCount;
			if(sampleCount > request.budget.maximumSamples || sampleCount > request.budget.maximumWorkingBytes / sizeof(std::int32_t))
				return Fail(TerrainResultCode::BudgetExceeded, "Procedural terrain request exceeds its sample or memory budget.");
			TerrainProceduralResult result; result.areas = request.areas;
			std::sort(result.areas.begin(), result.areas.end(), [](const TerrainProceduralArea& left, const TerrainProceduralArea& right) { return left.coordinate < right.coordinate; });
			for(std::size_t index = 0; index < result.areas.size(); ++index)
			{
				const TerrainProceduralArea& area = result.areas[index]; TerrainSampleCoordinate ignored{};
				if(area.heightMm.size() != kTerrainAreaSampleCount || (!area.selectionMask.empty() && area.selectionMask.size() != kTerrainAreaSampleCount))
					return Fail(TerrainResultCode::InvalidArgument, "Procedural terrain areas require complete height data and an optional complete selection mask.");
				if(!AreaOrigin(area.coordinate, ignored)) return Fail(TerrainResultCode::Overflow, "Terrain area coordinate cannot be expanded to canonical samples.");
				if(index != 0u && area.coordinate == result.areas[index - 1u].coordinate)
					return Fail(TerrainResultCode::InvalidArgument, "Procedural terrain requests cannot contain duplicate areas.");
			}
			return result;
		}

		void Append(std::vector<std::uint8_t>& bytes, const void* value, std::size_t count)
		{
			const auto* first = static_cast<const std::uint8_t*>(value); bytes.insert(bytes.end(), first, first + count);
		}

		ContentHash HashResult(const std::vector<TerrainProceduralArea>& areas)
		{
			std::vector<std::uint8_t> bytes; bytes.reserve(areas.size() * (16u + kTerrainAreaSampleCount * sizeof(std::int32_t)));
			for(const TerrainProceduralArea& area : areas)
			{
				Append(bytes, &area.coordinate.x, sizeof(area.coordinate.x)); Append(bytes, &area.coordinate.z, sizeof(area.coordinate.z));
				Append(bytes, area.heightMm.data(), area.heightMm.size() * sizeof(std::int32_t));
			}
			return HashTerrainBytes(bytes);
		}

		std::int32_t Clamp64(std::int64_t value) noexcept
		{
			return static_cast<std::int32_t>(std::clamp(value, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()), static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
		}

		std::uint64_t Mix(std::uint64_t value) noexcept
		{
			value ^= value >> 30u; value *= 0xbf58476d1ce4e5b9ULL; value ^= value >> 27u; value *= 0x94d049bb133111ebULL; return value ^ (value >> 31u);
		}

		template<typename Transform>
		TerrainProceduralResult Apply(const TerrainProceduralRequest& request, Transform&& transform)
		{
			TerrainProceduralResult result = Prepare(request); if(!result.Succeeded()) return result;
			for(TerrainProceduralArea& area : result.areas)
			{
				TerrainSampleCoordinate origin{}; AreaOrigin(area.coordinate, origin);
				for(std::size_t index = 0; index < area.heightMm.size(); ++index)
				{
					if((index & 255u) == 0u && Cancelled(request)) return Fail(TerrainResultCode::Cancelled, "Procedural terrain operation was cancelled before publication.");
					if(!IsSelected(area, index)) continue;
					const TerrainSampleCoordinate sample{origin.x + static_cast<std::int64_t>(index % kTerrainAreaExtentSamples), origin.z + static_cast<std::int64_t>(index / kTerrainAreaExtentSamples)};
					area.heightMm[index] = transform(area.heightMm[index], sample);
				}
			}
			result.outputHash = HashResult(result.areas); return result;
		}
	}

	TerrainProceduralResult ApplyTerrainHeightRemap(const TerrainProceduralRequest& request, const TerrainHeightRemapSettings& settings)
	{
		if(settings.multiplyDenominator == 0 || settings.minimumMm > settings.maximumMm) return Fail(TerrainResultCode::InvalidArgument, "Height remap requires a non-zero divisor and ordered clamp range.");
		return Apply(request, [&](std::int32_t height, TerrainSampleCoordinate) { return static_cast<std::int32_t>(std::clamp((static_cast<std::int64_t>(height) * settings.multiplyNumerator) / settings.multiplyDenominator + settings.addMm, static_cast<std::int64_t>(settings.minimumMm), static_cast<std::int64_t>(settings.maximumMm))); });
	}

	TerrainProceduralResult ApplyTerrainTerrace(const TerrainProceduralRequest& request, const TerrainTerraceSettings& settings)
	{
		if(settings.stepMm <= 0 || settings.blendPercent > 100u) return Fail(TerrainResultCode::InvalidArgument, "Terrace requires a positive step and a blend percentage up to 100.");
		return Apply(request, [&](std::int32_t height, TerrainSampleCoordinate) { bool overflow = false; const std::int64_t terrace = FloorDivideTerrainCoordinate(height, settings.stepMm, overflow) * settings.stepMm; return overflow ? height : Clamp64(static_cast<std::int64_t>(height) + ((terrace - height) * settings.blendPercent) / 100); });
	}

	TerrainProceduralResult ApplyTerrainSeededNoise(const TerrainProceduralRequest& request, const TerrainNoiseSettings& settings)
	{
		if(settings.periodSamples == 0u || settings.amplitudeMm < 0) return Fail(TerrainResultCode::InvalidArgument, "Noise requires a positive period and non-negative amplitude.");
		return Apply(request, [&](std::int32_t height, TerrainSampleCoordinate sample) { bool xOverflow = false; bool zOverflow = false; const std::int64_t cellX = FloorDivideTerrainCoordinate(sample.x, settings.periodSamples, xOverflow); const std::int64_t cellZ = FloorDivideTerrainCoordinate(sample.z, settings.periodSamples, zOverflow); if(xOverflow || zOverflow) return height; const std::uint64_t key = Mix(settings.seed ^ Mix(static_cast<std::uint64_t>(cellX)) ^ (Mix(static_cast<std::uint64_t>(cellZ)) << 1u)); const std::int64_t range = static_cast<std::int64_t>(settings.amplitudeMm) * 2 + 1; return Clamp64(static_cast<std::int64_t>(height) + static_cast<std::int64_t>(key % static_cast<std::uint64_t>(range)) - settings.amplitudeMm); });
	}

	TerrainProceduralResult ApplyTerrainStamp(const TerrainProceduralRequest& request, const TerrainStampSettings& settings)
	{
		if(settings.radiusSamples == 0u || settings.radiusSamples > 3'037'000'499u || settings.falloffPercent > 100u) return Fail(TerrainResultCode::InvalidArgument, "Stamp requires a non-zero representable radius and a falloff percentage up to 100.");
		const std::int64_t radius = settings.radiusSamples; const std::int64_t radiusSquared = radius * radius;
		return Apply(request, [&](std::int32_t height, TerrainSampleCoordinate sample) {
			// Compare before subtracting: distant signed coordinates can otherwise
			// overflow even though they are obviously outside this bounded stamp.
			if((sample.x >= settings.centre.x && sample.x - settings.centre.x > radius) || (sample.x < settings.centre.x && settings.centre.x - sample.x > radius)
				|| (sample.z >= settings.centre.z && sample.z - settings.centre.z > radius) || (sample.z < settings.centre.z && settings.centre.z - sample.z > radius)) return height;
			const std::int64_t dx = sample.x - settings.centre.x; const std::int64_t dz = sample.z - settings.centre.z; const std::int64_t distanceSquare = dx * dx + dz * dz;
			if(distanceSquare > radiusSquared) return height;
			const std::int64_t weight = 100 - ((distanceSquare * (100 - settings.falloffPercent)) / radiusSquared); return Clamp64(static_cast<std::int64_t>(height) + (static_cast<std::int64_t>(settings.heightDeltaMm) * weight) / 100);
		});
	}
}
