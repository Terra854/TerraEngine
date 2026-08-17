// TerrainSplineGrader.cpp -- Canonical-world, cross-area road and river grading.

#include "Editor/TerraEngine-Editor/Terrain/TerrainSplineGrader.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Editor::Terrain
{
	namespace
	{
		using namespace ::TerraEngine::Terrain;
		struct QuantizedPoint { std::int64_t xMm = 0; std::int64_t yMm = 0; std::int64_t zMm = 0; };
		TerrainProceduralResult Fail(TerrainResultCode code, std::string message) { TerrainProceduralResult result; result.code = code; result.message = std::move(message); return result; }
		bool Selected(const TerrainProceduralArea& area, std::size_t index) noexcept { return area.selectionMask.empty() || area.selectionMask[index] != 0u; }
		bool AreaOrigin(const TerrainAreaCoordinate& area, TerrainSampleCoordinate& out) noexcept
		{
			if(area.x > std::numeric_limits<std::int64_t>::max() / kTerrainAreaExtentSamples || area.x < std::numeric_limits<std::int64_t>::min() / kTerrainAreaExtentSamples || area.z > std::numeric_limits<std::int64_t>::max() / kTerrainAreaExtentSamples || area.z < std::numeric_limits<std::int64_t>::min() / kTerrainAreaExtentSamples) return false;
			out = {area.x * kTerrainAreaExtentSamples, area.z * kTerrainAreaExtentSamples}; return true;
		}
		std::optional<std::int64_t> AbsoluteMillimetres(std::int64_t cell, double offset) noexcept
		{
			const long double value = (static_cast<long double>(cell) * ::TerraEngine::Scene::kWorldCellEdgeMetres + static_cast<long double>(offset)) * 1000.0L;
			if(!std::isfinite(static_cast<double>(value)) || value < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) || value > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
			return static_cast<std::int64_t>(std::llround(value));
		}
		std::int32_t ClampHeight(std::int64_t height) noexcept { return static_cast<std::int32_t>(std::clamp(height, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()), static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()))); }
	}

	std::optional<TerrainSampleCoordinate> TerrainWorldToSample(const ::TerraEngine::Scene::WorldPosition& position, double sampleSpacingMetres)
	{
		if(!std::isfinite(sampleSpacingMetres) || sampleSpacingMetres <= 0.0) return std::nullopt;
		const auto x = AbsoluteMillimetres(position.cell.x, position.offsetMetres.x); const auto z = AbsoluteMillimetres(position.cell.z, position.offsetMetres.z);
		const long double spacingMm = static_cast<long double>(sampleSpacingMetres) * 1000.0L;
		if(!x || !z || spacingMm < 1.0L) return std::nullopt;
		const long double sampleX = std::floor(static_cast<long double>(*x) / spacingMm); const long double sampleZ = std::floor(static_cast<long double>(*z) / spacingMm);
		if(sampleX < std::numeric_limits<std::int64_t>::min() || sampleX > std::numeric_limits<std::int64_t>::max() || sampleZ < std::numeric_limits<std::int64_t>::min() || sampleZ > std::numeric_limits<std::int64_t>::max()) return std::nullopt;
		return TerrainSampleCoordinate{static_cast<std::int64_t>(sampleX), static_cast<std::int64_t>(sampleZ)};
	}

	TerrainProceduralResult ApplyTerrainSplineGrade(const TerrainSplineGradeRequest& request)
	{
		const TerrainSplineGradeSettings& settings = request.settings;
		if(request.controlPoints.size() < 2u || !std::isfinite(settings.sampleSpacingMetres) || settings.sampleSpacingMetres <= 0.0 || !std::isfinite(settings.widthMetres) || settings.widthMetres <= 0.0 || !std::isfinite(settings.shoulderWidthMetres) || settings.shoulderWidthMetres < 0.0 || !std::isfinite(settings.falloffMetres) || settings.falloffMetres < 0.0 || settings.bankHeightMm < 0 || settings.riverDepthMm < 0)
			return Fail(TerrainResultCode::InvalidArgument, "Spline grading requires at least two finite points and non-negative dimensions.");
		std::vector<QuantizedPoint> points; points.reserve(request.controlPoints.size());
		for(const TerrainSplineControlPoint& point : request.controlPoints)
		{
			const auto x = AbsoluteMillimetres(point.position.cell.x, point.position.offsetMetres.x); const auto y = AbsoluteMillimetres(point.position.cell.y, point.position.offsetMetres.y); const auto z = AbsoluteMillimetres(point.position.cell.z, point.position.offsetMetres.z);
			if(!x || !y || !z) return Fail(TerrainResultCode::Overflow, "Spline point cannot be represented in canonical millimetres.");
			points.push_back({*x, *y, *z});
		}
		const std::int64_t spacingMm = static_cast<std::int64_t>(std::llround(settings.sampleSpacingMetres * 1000.0));
		const std::int64_t coreMm = static_cast<std::int64_t>(std::llround(settings.widthMetres * 500.0));
		const std::int64_t shoulderMm = static_cast<std::int64_t>(std::llround(settings.shoulderWidthMetres * 1000.0));
		const std::int64_t falloffMm = static_cast<std::int64_t>(std::llround(settings.falloffMetres * 1000.0));
		if(spacingMm <= 0 || coreMm <= 0) return Fail(TerrainResultCode::InvalidArgument, "Spline dimensions are too small after millimetre quantization.");

		TerrainProceduralResult result = ApplyTerrainHeightRemap(request.terrain, {}); if(!result.Succeeded()) return result;
		for(TerrainProceduralArea& area : result.areas)
		{
			TerrainSampleCoordinate origin{}; if(!AreaOrigin(area.coordinate, origin)) return Fail(TerrainResultCode::Overflow, "Spline grading area origin overflowed.");
			for(std::size_t index = 0; index < area.heightMm.size(); ++index)
			{
				if((index & 255u) == 0u && request.terrain.cancellation && request.terrain.cancellation->load(std::memory_order_relaxed)) return Fail(TerrainResultCode::Cancelled, "Spline grading was cancelled before publication.");
				if(!Selected(area, index)) continue;
				const long double sampleX = static_cast<long double>(origin.x + static_cast<std::int64_t>(index % kTerrainAreaExtentSamples)) * spacingMm;
				const long double sampleZ = static_cast<long double>(origin.z + static_cast<std::int64_t>(index / kTerrainAreaExtentSamples)) * spacingMm;
				long double nearestSquared = std::numeric_limits<long double>::infinity(); long double baseY = 0.0L; long double signedSide = 0.0L;
				for(std::size_t segment = 1; segment < points.size(); ++segment)
				{
					const QuantizedPoint& a = points[segment - 1u]; const QuantizedPoint& b = points[segment];
					const long double dx = static_cast<long double>(b.xMm) - a.xMm; const long double dz = static_cast<long double>(b.zMm) - a.zMm; const long double lengthSquared = dx * dx + dz * dz; if(lengthSquared == 0.0L) continue;
					const long double t = std::clamp(((sampleX - a.xMm) * dx + (sampleZ - a.zMm) * dz) / lengthSquared, 0.0L, 1.0L);
					const long double nearestX = a.xMm + dx * t; const long double nearestZ = a.zMm + dz * t; const long double offsetX = sampleX - nearestX; const long double offsetZ = sampleZ - nearestZ; const long double squared = offsetX * offsetX + offsetZ * offsetZ;
					if(squared < nearestSquared) { nearestSquared = squared; baseY = a.yMm + (static_cast<long double>(b.yMm) - a.yMm) * t; signedSide = dx * offsetZ - dz * offsetX; }
				}
				if(!std::isfinite(static_cast<double>(nearestSquared))) continue;
				const std::int64_t distanceMm = static_cast<std::int64_t>(std::llround(std::sqrt(nearestSquared)));
				const std::int64_t shoulderEnd = coreMm + shoulderMm; const std::int64_t outerEnd = shoulderEnd + falloffMm; if(distanceMm > outerEnd) continue;
				std::int64_t target = static_cast<std::int64_t>(std::llround(baseY));
				if(settings.kind == TerrainSplineGradeKind::River) target -= settings.riverDepthMm;
				else {
					// Banking uses the signed segment side, making the crown/shoulder
					// profile continuous while following a curved canonical-world spline.
					const std::int64_t side = signedSide < 0.0L ? -1 : 1;
					const std::int64_t bankDistance = distanceMm <= coreMm ? distanceMm : std::min(distanceMm - coreMm, shoulderMm);
					const std::int64_t bankDenominator = distanceMm <= coreMm ? coreMm : std::max<std::int64_t>(shoulderMm, 1);
					target += side * (static_cast<std::int64_t>(settings.bankHeightMm) * bankDistance) / bankDenominator;
				}
				const std::int64_t influence = distanceMm <= shoulderEnd || falloffMm == 0 ? 10000 : ((outerEnd - distanceMm) * 10000) / falloffMm;
				const std::int64_t original = area.heightMm[index]; area.heightMm[index] = ClampHeight(original + ((target - original) * influence) / 10000);
			}
		}
		TerrainProceduralRequest finalRequest = request.terrain; finalRequest.areas = std::move(result.areas); return ApplyTerrainHeightRemap(finalRequest, {});
	}
}
