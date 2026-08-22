// TerrainPicking.cpp -- CPU fallback terrain ray query.

#include "TerrainPicking.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

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
			if(!height)
			{
				// A normal editor camera starts outside the finite authored patch.
				// Keep marching until the ray enters resident terrain; carrying the
				// previous difference across a gap could manufacture a false hit.
				hasPrior = false;
				continue;
			}
			const double difference = point.y - *height;
			if((hasPrior && priorDifference >= 0.0 && difference <= 0.0) || difference == 0.0)
				return TerrainPickHit{{point.x, *height, point.z}, {x, z}, distance};
			priorDifference = difference;
			hasPrior = true;
		}
		return std::nullopt;
	}

	std::optional<TerrainViewportRay> TerrainPicking::BuildViewportRay(
		const glm::mat4& view, const glm::mat4& projection,
		double normalizedX, double normalizedY, bool orthographic)
	{
		if(!std::isfinite(normalizedX) || !std::isfinite(normalizedY)
			|| normalizedX < 0.0 || normalizedX > 1.0
			|| normalizedY < 0.0 || normalizedY > 1.0)
			return std::nullopt;

		// Scene View uses a right-handed zero-to-one projection. ImGui's pointer
		// origin is top-left, so y is inverted while x maps directly to clip space.
		const float clipX = static_cast<float>(normalizedX * 2.0 - 1.0);
		const float clipY = static_cast<float>(1.0 - normalizedY * 2.0);
		const glm::mat4 inverseViewProjection = glm::inverse(projection * view);
		auto unproject = [&inverseViewProjection](const glm::vec4& clip) -> std::optional<glm::dvec3>
		{
			const glm::vec4 homogeneous = inverseViewProjection * clip;
			if(!std::isfinite(homogeneous.x) || !std::isfinite(homogeneous.y)
				|| !std::isfinite(homogeneous.z) || !std::isfinite(homogeneous.w)
				|| std::abs(homogeneous.w) <= 1.0e-7F)
				return std::nullopt;
			return glm::dvec3(homogeneous) / static_cast<double>(homogeneous.w);
		};
		const auto nearPoint = unproject({clipX, clipY, 0.0F, 1.0F});
		const auto farPoint = unproject({clipX, clipY, 1.0F, 1.0F});
		if(!nearPoint || !farPoint) return std::nullopt;
		const glm::dvec3 delta = *farPoint - *nearPoint;
		if(glm::length(delta) <= 1.0e-9) return std::nullopt;

		glm::dvec3 localOrigin = *nearPoint;
		if(!orthographic)
		{
			const glm::mat4 inverseView = glm::inverse(view);
			const glm::vec4 camera = inverseView * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
			if(!std::isfinite(camera.x) || !std::isfinite(camera.y)
				|| !std::isfinite(camera.z) || std::abs(camera.w) <= 1.0e-7F)
				return std::nullopt;
			localOrigin = glm::dvec3(camera) / static_cast<double>(camera.w);
		}
		return TerrainViewportRay{localOrigin, glm::normalize(delta)};
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
