// TerrainSplineGrader.h -- Canonical-world road and river terrain grading.

#pragma once

#include "Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.h"
#include "Scene/WorldCoordinate.h"

#include <optional>

namespace TerraEngine::Editor::Terrain
{
	enum class TerrainSplineGradeKind : std::uint8_t { Road, River };

	struct TerrainSplineControlPoint { ::TerraEngine::Scene::WorldPosition position{}; };
	struct TerrainSplineGradeSettings
	{
		double sampleSpacingMetres = 1.0;
		double widthMetres = 6.0;
		double shoulderWidthMetres = 2.0;
		double falloffMetres = 4.0;
		std::int32_t bankHeightMm = 250;
		std::int32_t riverDepthMm = 1000;
		TerrainSplineGradeKind kind = TerrainSplineGradeKind::Road;
	};

	struct TerrainSplineGradeRequest
	{
		TerrainProceduralRequest terrain;
		std::vector<TerrainSplineControlPoint> controlPoints;
		TerrainSplineGradeSettings settings{};
	};

	// Converts exact world cells to a terrain lattice sample. It rejects values
	// that cannot be represented instead of silently losing distant precision.
	std::optional<::TerraEngine::Terrain::TerrainSampleCoordinate> TerrainWorldToSample(
		const ::TerraEngine::Scene::WorldPosition& position, double sampleSpacingMetres);
	TerrainProceduralResult ApplyTerrainSplineGrade(const TerrainSplineGradeRequest& request);
}
