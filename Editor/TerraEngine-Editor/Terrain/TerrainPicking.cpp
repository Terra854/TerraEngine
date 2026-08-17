// TerrainPicking.cpp -- CPU fallback terrain ray query.

#include "TerrainPicking.h"

#include <glm/geometric.hpp>

#include <cmath>

namespace TerraEngine::Editor::Terrain
{
	std::optional<TerrainPickHit> TerrainPicking::PickHeightfield(const TerrainCanonicalRay& ray,
		const TerrainHeightSampler& sampleHeight, double maximumDistance, double stepMetres)
	{
		if(!sampleHeight || maximumDistance <= 0.0 || stepMetres <= 0.0 || glm::length(ray.direction) <= 0.0) return std::nullopt;
		const glm::dvec3 direction = glm::normalize(ray.direction);
		double priorDifference = 0.0;
		bool hasPrior = false;
		for(double distance = 0.0; distance <= maximumDistance; distance += stepMetres)
		{
			const glm::dvec3 point = ray.origin + direction * distance;
			const auto x = static_cast<std::int64_t>(std::floor(point.x));
			const auto z = static_cast<std::int64_t>(std::floor(point.z));
			const std::optional<double> height = sampleHeight(x, z);
			if(!height) return std::nullopt;
			const double difference = point.y - *height;
			if((hasPrior && priorDifference >= 0.0 && difference <= 0.0) || difference == 0.0)
				return TerrainPickHit{{point.x, *height, point.z}, {x, z}, distance};
			priorDifference = difference;
			hasPrior = true;
		}
		return std::nullopt;
	}

	bool TerrainPicking::QualifyGpuIdentity(const TerrainGpuPickIdentity& gpu, TerraEngine::UUID expectedAsset,
		TerraEngine::Terrain::Generation expectedGeneration) noexcept
	{
		return expectedAsset != TerraEngine::EMPTY_UUID && gpu.assetId == expectedAsset
			&& expectedGeneration != 0U && gpu.generation == expectedGeneration;
	}

	std::optional<TerrainDistantPickHit> TerrainPicking::PickDistantHeightfield(
		const TerrainDistantPickRequest& request, const TerrainHeightSampler& sampleHeight)
	{
		if(request.expectedReferenceFrameRevision == 0U
			|| request.expectedReferenceFrameRevision != request.observedReferenceFrameRevision)
			return std::nullopt;
		// This call rejects an origin that cannot be expressed safely in the active
		// bounded frame. It occurs before any double/float terrain-local operation.
		const auto localized = TerraEngine::Scene::LocalizeWorldPosition(
			request.rayOrigin, request.referenceFrameOrigin, request.maximumDistance);
		if(!localized.Succeeded()) return std::nullopt;
		const auto absolute = [](const TerraEngine::Scene::WorldPosition& position) {
			return glm::dvec3(
				static_cast<double>(position.cell.x) * TerraEngine::Scene::kWorldCellEdgeMetres + position.offsetMetres.x,
				static_cast<double>(position.cell.y) * TerraEngine::Scene::kWorldCellEdgeMetres + position.offsetMetres.y,
				static_cast<double>(position.cell.z) * TerraEngine::Scene::kWorldCellEdgeMetres + position.offsetMetres.z);
		};
		const glm::dvec3 referenceAbsolute = absolute(request.referenceFrameOrigin);
		const TerrainCanonicalRay localRay{referenceAbsolute + localized.offsetMetres, request.localDirection};
		const auto hit = PickHeightfield(localRay, sampleHeight, request.maximumDistance, request.stepMetres);
		if(!hit) return std::nullopt;
		// Re-normalize into the canonical exact-world representation before callers
		// receive it. Tile-local render floats never escape this function.
		const auto normalized = TerraEngine::Scene::NormalizeWorldPosition({{}, hit->canonicalWorldPosition});
		if(!normalized.Succeeded()) return std::nullopt;
		return TerrainDistantPickHit{normalized.position, hit->sample, hit->distance};
	}
}
