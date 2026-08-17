// TerrainPicking.h -- Canonical-world terrain pick qualification.

#pragma once

#include "Terrain/TerrainTypes.h"
#include "Scene/WorldCoordinate.h"

#include <functional>
#include <optional>

#include <glm/vec3.hpp>

namespace TerraEngine::Editor::Terrain
{
	struct TerrainCanonicalRay { glm::dvec3 origin{0.0}; glm::dvec3 direction{0.0, -1.0, 0.0}; };
	struct TerrainPickHit { glm::dvec3 canonicalWorldPosition{0.0}; TerraEngine::Terrain::TerrainSampleCoordinate sample{}; double distance = 0.0; };
	struct TerrainGpuPickIdentity { TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID; TerraEngine::Terrain::Generation generation = 0U; };
	// The distant route retains the engine's exact cell+offset address until the
	// caller has qualified the renderer reference frame. Only then do we form a
	// bounded local ray for the height sampler.
	struct TerrainDistantPickRequest
	{
		TerraEngine::Scene::WorldPosition rayOrigin{};
		TerraEngine::Scene::WorldPosition referenceFrameOrigin{};
		glm::dvec3 localDirection{0.0, -1.0, 0.0};
		std::uint64_t expectedReferenceFrameRevision = 0U;
		std::uint64_t observedReferenceFrameRevision = 0U;
		double maximumDistance = 0.0;
		double stepMetres = 1.0;
	};
	struct TerrainDistantPickHit { TerraEngine::Scene::WorldPosition canonicalWorldPosition{}; TerraEngine::Terrain::TerrainSampleCoordinate sample{}; double distance = 0.0; };

	using TerrainHeightSampler = std::function<std::optional<double>(std::int64_t sampleX, std::int64_t sampleZ)>;
	class TerrainPicking
	{
	public:
		// Sampling takes an immutable resident snapshot; a missing sample ends the
		// query instead of substituting data from a different terrain generation.
		[[nodiscard]] static std::optional<TerrainPickHit> PickHeightfield(
			const TerrainCanonicalRay& ray, const TerrainHeightSampler& sampleHeight,
			double maximumDistance, double stepMetres = 1.0);
		[[nodiscard]] static bool QualifyGpuIdentity(
			const TerrainGpuPickIdentity& gpu, TerraEngine::UUID expectedAsset,
			TerraEngine::Terrain::Generation expectedGeneration) noexcept;
		[[nodiscard]] static std::optional<TerrainDistantPickHit> PickDistantHeightfield(
			const TerrainDistantPickRequest& request, const TerrainHeightSampler& sampleHeight);
	};
}
