// NavigationRuntimeSceneCapture.cpp - copies navigation input from live owners.

#include "Navigation/Integration/NavigationRuntimeSceneCapture.h"

#include "Components/TerrainComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Resource/ResourceManager.h"
#include "Terrain/TerrainTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Navigation {
	namespace {
		[[nodiscard]] bool IsTerrainCurrent(
			const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& collision) noexcept {
			return collision.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
				&& collision.currentRecipe.IsComplete();
		}

		[[nodiscard]] std::optional<TerrainNavigationPublishedSamples> CopyPublishedTerrain(
			const ::NavigationSourceComponent& source, const ::TerrainComponent& terrain,
			const TerraEngine::Scene::WorldPose& pose,
			const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& collision,
			std::string& reason) {
			if(!IsTerrainCurrent(collision)) {
				reason = "Terrain collision samples are not physically published and current.";
				return std::nullopt;
			}
			if(terrain.terrainAssetId != collision.currentRecipe.terrainAssetId) {
				reason = "Published terrain collision samples belong to a different terrain asset.";
				return std::nullopt;
			}
			if(!::TerrainComponent::SupportsTranslationOnlyPlacement(pose)) {
				reason = "Terrain navigation requires the terrain's published translation-only placement.";
				return std::nullopt;
			}

			const TerraEngine::Physics::TerrainHeightFieldRecipe& recipe = collision.currentRecipe;
			TerrainNavigationPublishedSamples samples{};
			samples.terrainPresent = true;
			samples.descriptor.sourceUuid = {source.sourceId};
			samples.descriptor.sourceRevision = source.geometryRevision;
			samples.descriptor.canonicalPlacementRevision = std::max<std::uint64_t>(1U, pose.transformRevision);
			samples.descriptor.physicalReadyRevision = std::max<std::uint64_t>(1U,
				std::max<std::uint64_t>(collision.sourcePublicationGeneration, recipe.authoredGeneration));
			samples.descriptor.sourceKind = NavigationSourceKind::Terrain;
			samples.canonicalOrigin = pose.position;
			// Terrain collision regions are 256 cells wide. Add their exact lattice
			// address before normalizing so a worker never derives placement from a
			// renderer float transform.
			const double areaEdge = static_cast<double>(TerraEngine::Terrain::kTerrainAreaExtentSamples)
				* static_cast<double>(recipe.sampleSpacingMetres);
			samples.canonicalOrigin.offsetMetres.x += static_cast<double>(recipe.areaX) * areaEdge;
			samples.canonicalOrigin.offsetMetres.z += static_cast<double>(recipe.areaZ) * areaEdge;
			const TerraEngine::Scene::WorldCoordinateResult normalized =
				TerraEngine::Scene::NormalizeWorldPosition(samples.canonicalOrigin);
			if(!normalized.Succeeded()) {
				reason = "Terrain collision area cannot be represented as a canonical world position.";
				return std::nullopt;
			}
			samples.canonicalOrigin = normalized.position;
			samples.sampleColumns = TerraEngine::Physics::TerrainHeightFieldRecipe::kGridExtent;
			samples.sampleRows = TerraEngine::Physics::TerrainHeightFieldRecipe::kGridExtent;
			samples.sampleSpacingMetres = recipe.sampleSpacingMetres;
			samples.heights.reserve(recipe.heightMillimetres.size());
			for(const std::int32_t millimetres : recipe.heightMillimetres) {
				samples.heights.push_back(recipe.heightDatumMetres + static_cast<float>(millimetres) / 1000.0F);
			}
			const std::size_t cellCount = static_cast<std::size_t>(samples.sampleColumns - 1U)
				* static_cast<std::size_t>(samples.sampleRows - 1U);
			samples.holeMask.resize(cellCount, 0U);
			for(std::size_t index = 0U; index < cellCount; ++index) {
				const std::uint8_t bits = recipe.holeBits[index / 8U];
				samples.holeMask[index] = static_cast<std::uint8_t>((bits >> (index % 8U)) & 1U);
			}
			return samples;
		}
	}

	NavigationRuntimeSceneCapture CaptureNavigationRuntimeScene(
		ObjectManager& objects, ResourceManager& resources,
		const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& terrainCollision,
		const NavigationRuntimeSceneCaptureSettings& settings) {
		NavigationRuntimeSceneCapture result{};
		NavigationSceneBootstrapInput input{};
		input.defaultProfile = settings.defaultProfile;
		input.registeredProfiles = settings.registeredProfiles;
		result.componentRevision = objects.GetComponentMutationRevision();

		std::string terrainFailure;
		objects.forEachObjectWithComponents<::NavigationSourceComponent>(
			[&](const Object& object, const ::NavigationSourceComponent& source) {
				NavigationBootstrapSourceInput captured{};
				captured.component = source;
				if(source.sourceKind == NavigationSourceKind::Terrain) {
					const ::TerrainComponent* terrain = objects.GetComponent<::TerrainComponent>(object);
					const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
					if(terrain == nullptr || !pose) {
						terrainFailure = "Terrain navigation source requires a TerrainComponent and canonical world pose.";
						return;
					}
					captured.terrain = CopyPublishedTerrain(source, *terrain, *pose, terrainCollision, terrainFailure);
				} else {
					// Navigation companions are registered during asset discovery, but loading
					// every companion eagerly would waste memory in scenes that never reference
					// it. Materialize the source-specific immutable generation on first use.
					// A repeated capture reuses the exact matching generation without file I/O.
					captured.immutableGeometry = resources.GetNavigationGeometrySnapshot(source.sourceAssetId);
					const bool generationMatches = captured.immutableGeometry
						&& captured.immutableGeometry->Descriptor().sourceUuid.value == source.sourceId
						&& captured.immutableGeometry->Descriptor().sourceRevision == source.geometryRevision;
					if(!generationMatches) {
						const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
						NavigationGeometrySnapshotDescriptor descriptor{};
						descriptor.sourceUuid = {source.sourceId};
						descriptor.sourceRevision = source.geometryRevision;
						descriptor.canonicalPlacementRevision = pose
							? std::max<std::uint64_t>(1U, pose->transformRevision) : 1U;
						descriptor.physicalReadyRevision = std::max<std::uint64_t>(1U, source.geometryRevision);
						descriptor.sourceKind = source.sourceKind == NavigationSourceKind::Auto
							? NavigationSourceKind::Mesh : source.sourceKind;
						descriptor.subshapeIndex = source.subshapeSelection;
						const NavigationGeometryLoadResult loaded =
							resources.LoadRegisteredNavigationGeometryCompanion(source.sourceAssetId, descriptor);
						captured.immutableGeometry = loaded.snapshot;
					}
				}
				input.sources.push_back(std::move(captured));
			});

		objects.forEachObjectWithComponents<::NavigationAgentComponent>(
			[&](const Object& object, const ::NavigationAgentComponent& agent) {
				input.agents.push_back({objects.getUUID(object), agent});
			});
		if(!terrainFailure.empty()) {
			result.reason = terrainFailure;
			return result;
		}

		result.bootstrap = CaptureNavigationSceneBootstrap(input);
		result.accepted = result.bootstrap.accepted;
		result.reason = result.bootstrap.reason;
		return result;
	}

	NavigationRuntimeSceneSubmission SubmitNavigationRuntimeSceneCapture(
		NavigationSystem& navigation, const NavigationRuntimeSceneCapture& capture) {
		NavigationRuntimeSceneSubmission result{};
		if(!capture.accepted) {
			result.reason = capture.reason.empty() ? "Navigation scene capture was rejected." : capture.reason;
			return result;
		}
		std::vector<NavigationSystem::InitialGroundBuildInput> builds;
		builds.reserve(capture.bootstrap.builds.size());
		for(const NavigationBootstrapBuildInput& captured : capture.bootstrap.builds) {
			builds.push_back({captured.profile, captured.sources, captured.sourceIndex});
		}
		if(!navigation.QueueInitialGroundBuilds(std::move(builds))) {
			result.reason = "NavigationSystem rejected the copied bootstrap batch at admission.";
			return result;
		}
		result.accepted = true;
		result.queuedProfileBuilds = capture.bootstrap.builds.size();
		result.reason = "Queued copied scene navigation bootstrap builds.";
		return result;
	}
}
