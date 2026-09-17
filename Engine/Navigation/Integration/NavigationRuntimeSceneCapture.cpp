// NavigationRuntimeSceneCapture.cpp - copies automatic navigation input from live owners.

#include "Navigation/Integration/NavigationRuntimeSceneCapture.h"

#include "Components/MeshRenderComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/TerrainComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Resource/ResourceManager.h"
#include "Terrain/TerrainTypes.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <optional>
#include <type_traits>
#include <variant>

namespace TerraEngine::Navigation {
	namespace {
		[[nodiscard]] std::uint64_t NonZero(std::uint64_t value) noexcept { return value == 0U ? 1U : value; }

		[[nodiscard]] TerraEngine::UUID MakeSourceId(
			const TerraEngine::UUID& objectId, const TerraEngine::UUID& assetId,
			NavigationSourceKind kind, std::uint32_t subshapeIndex) noexcept {
			TerraEngine::UUID sourceId = objectId;
			for(std::size_t index = 0U; index < sourceId.size(); ++index) {
				const std::uint8_t kindByte = static_cast<std::uint8_t>(kind) << (index % 3U);
				const std::uint8_t subshapeByte = static_cast<std::uint8_t>(subshapeIndex >> ((index % 4U) * 8U));
				sourceId[index] ^= assetId[index] ^ kindByte ^ subshapeByte;
			}
			if(sourceId == TerraEngine::EMPTY_UUID) sourceId.back() = 1U;
			return sourceId;
		}

		[[nodiscard]] NavigationEligibleSourceCandidate MakeCandidate(
			const ObjectManager& objects, const Object& object, const TerraEngine::UUID& assetId,
			NavigationSourceKind kind, std::uint32_t subshapeIndex,
			NavigationGeneration geometryRevision, NavigationGeneration physicalGeneration,
			NavigationGeneration resourceGeneration, const TerraEngine::Scene::WorldPose& pose) {
			NavigationEligibleSourceCandidate candidate{};
			candidate.identity.objectId = objects.getUUID(object);
			candidate.identity.objectLifetimeGeneration = NonZero(objects.getObjectLifetimeGeneration(object));
			candidate.identity.assetId = assetId;
			candidate.identity.kind = kind;
			candidate.identity.subshapeIndex = subshapeIndex;
			candidate.identity.sourceId = MakeSourceId(candidate.identity.objectId, assetId, kind, subshapeIndex);
			candidate.revisions = {
				candidate.identity.objectLifetimeGeneration,
				NonZero(pose.transformRevision),
				NonZero(geometryRevision),
				NonZero(physicalGeneration),
				NonZero(resourceGeneration)};
			candidate.activeInHierarchy = true;
			return candidate;
		}

		[[nodiscard]] NavigationGeometrySnapshotDescriptor MakeDescriptor(
			const NavigationEligibleSourceCandidate& candidate, const TerraEngine::Scene::WorldPose& pose) {
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceUuid.value = candidate.identity.sourceId;
			descriptor.sourceRevision = ComputeNavigationEligibleSourceRevision(
				candidate.identity, candidate.revisions);
			descriptor.canonicalPlacementRevision = NonZero(pose.transformRevision);
			descriptor.physicalReadyRevision = candidate.revisions.physicalPublicationGeneration;
			descriptor.sourceKind = candidate.identity.kind;
			descriptor.subshapeIndex = candidate.identity.subshapeIndex;
			return descriptor;
		}

		[[nodiscard]] bool IsTerrainCurrent(
			const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& collision) noexcept {
			return collision.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Current
				&& collision.currentRecipe.IsComplete();
		}

		[[nodiscard]] std::optional<TerrainNavigationPublishedSamples> CopyPublishedTerrain(
			const ::TerrainComponent& terrain, const NavigationEligibleSourceCandidate& candidate,
			const TerraEngine::Scene::WorldPose& pose,
			const TerraEngine::Physics::Terrain::TerrainCollisionSnapshot& collision,
			std::string& reason) {
			if(!IsTerrainCurrent(collision)) {
				reason = "Terrain collision samples are not physically published and current.";
				return std::nullopt;
			}
			if(terrain.terrainAssetId != collision.currentRecipe.terrainAssetId) {
				reason = "Published terrain collision samples belong to a different active terrain asset.";
				return std::nullopt;
			}
			if(!::TerrainComponent::SupportsTranslationOnlyPlacement(pose)) {
				reason = "Terrain navigation requires the terrain's published translation-only placement.";
				return std::nullopt;
			}

			const TerraEngine::Physics::TerrainHeightFieldRecipe& recipe = collision.currentRecipe;
			TerrainNavigationPublishedSamples samples{};
			samples.terrainPresent = true;
			samples.descriptor = MakeDescriptor(candidate, pose);
			samples.canonicalOrigin = pose.position;
			// Terrain collision regions are 256 cells wide. Add their exact lattice
			// address before normalizing so workers never derive it from a renderer pose.
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

		[[nodiscard]] bool CopyPrimitiveColliderGeometry(
			const TerraEngine::Physics::ColliderRecord& record,
			const TerraEngine::Scene::WorldPose& pose, NavigationGeometryDocument& document) {
			NavigationGeometryPrimitive primitive{};
			const glm::quat objectRotation = glm::normalize(pose.rotation);
			const glm::vec3 localCenter{record.localCenter.x, record.localCenter.y, record.localCenter.z};
			const glm::vec3 translation{static_cast<float>(pose.position.offsetMetres.x),
				static_cast<float>(pose.position.offsetMetres.y), static_cast<float>(pose.position.offsetMetres.z)};
			const glm::vec3 worldCenter = objectRotation * (localCenter * pose.scale) + translation;
			primitive.center = {worldCenter.x, worldCenter.y, worldCenter.z};
			const bool supported = std::visit([&primitive](const auto& shape) {
				using Shape = std::decay_t<decltype(shape)>;
				if constexpr(std::is_same_v<Shape, TerraEngine::Physics::BoxColliderShape>) {
					primitive.kind = NavigationGeometryPrimitiveKind::Box;
					primitive.halfExtents = {shape.halfExtents.x, shape.halfExtents.y, shape.halfExtents.z};
					return true;
				} else if constexpr(std::is_same_v<Shape, TerraEngine::Physics::SphereColliderShape>) {
					primitive.kind = NavigationGeometryPrimitiveKind::Sphere;
					primitive.radius = shape.radius;
					return true;
				} else if constexpr(std::is_same_v<Shape, TerraEngine::Physics::CapsuleColliderShape>) {
					primitive.kind = NavigationGeometryPrimitiveKind::Capsule;
					primitive.radius = shape.radius;
					primitive.height = shape.height;
					return true;
				}
				return false;
			}, record.shape);
			if(primitive.kind == NavigationGeometryPrimitiveKind::Box) {
				// The geometry snapshot stores axis-aligned primitives. Rotate the scaled
				// local box into a conservative world-space AABB so its physical footprint
				// cannot disappear from a worker build when its owning object moves/rotates.
				const glm::quat colliderRotation = glm::normalize(glm::quat{
					record.localRotation.w, record.localRotation.x, record.localRotation.y, record.localRotation.z});
				const glm::quat worldRotation = objectRotation * colliderRotation;
				const glm::vec3 scaledHalfExtents{
					std::fabs(primitive.halfExtents.x * pose.scale.x),
					std::fabs(primitive.halfExtents.y * pose.scale.y),
					std::fabs(primitive.halfExtents.z * pose.scale.z)};
				const glm::vec3 axisX = worldRotation * glm::vec3{scaledHalfExtents.x, 0.0F, 0.0F};
				const glm::vec3 axisY = worldRotation * glm::vec3{0.0F, scaledHalfExtents.y, 0.0F};
				const glm::vec3 axisZ = worldRotation * glm::vec3{0.0F, 0.0F, scaledHalfExtents.z};
				primitive.halfExtents = {std::fabs(axisX.x) + std::fabs(axisY.x) + std::fabs(axisZ.x),
					std::fabs(axisX.y) + std::fabs(axisY.y) + std::fabs(axisZ.y),
					std::fabs(axisX.z) + std::fabs(axisY.z) + std::fabs(axisZ.z)};
			}
			if(supported) {
				document.kind = NavigationGeometryPayloadKind::AnalyticPrimitives;
				document.primitives.push_back(primitive);
			}
			return supported;
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
		result.sourceCaptureRevision = objects.GetComponentMutationRevision();

		// Terrain is captured from every active terrain object. Only the region whose
		// current collision publication matches that terrain is eligible this frame.
		objects.forEachObjectWithComponents<::TerrainComponent>(
			[&](const Object& object, const ::TerrainComponent& terrain) {
				const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
				if(!pose || terrain.terrainAssetId == TerraEngine::EMPTY_UUID) return;
				NavigationBootstrapSourceInput captured{};
				captured.candidate = MakeCandidate(objects, object, terrain.terrainAssetId,
					NavigationSourceKind::Terrain, 0U, terrain.schemaVersion,
					terrainCollision.sourcePublicationGeneration, terrainCollision.currentRecipe.authoredGeneration, *pose);
				std::string reason;
				captured.terrain = CopyPublishedTerrain(terrain, captured.candidate, *pose, terrainCollision, reason);
				captured.candidate.physicalBackingCurrent = captured.terrain.has_value();
				input.sources.push_back(std::move(captured));
			});

		// Mesh companions are physical navigation input only after their checked
		// immutable .tenavgeom payload is loaded. Render culling and material state
		// deliberately do not participate in this eligibility decision.
		objects.forEachObjectWithComponents<::MeshRenderComponent>(
			[&](const Object& object, const ::MeshRenderComponent& mesh) {
				const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
				if(!pose || mesh.meshId == TerraEngine::EMPTY_UUID) return;
				const NavigationGeneration geometryPublicationGeneration =
					resources.NavigationGeometryPublicationGeneration();
				NavigationBootstrapSourceInput captured{};
				captured.candidate = MakeCandidate(objects, object, mesh.meshId,
					NavigationSourceKind::Mesh, 0U, objects.getObjectComponentGeneration(object),
					geometryPublicationGeneration, geometryPublicationGeneration, *pose);
				const NavigationGeometryLoadResult loaded = resources.LoadRegisteredNavigationGeometryCompanion(
					mesh.meshId, MakeDescriptor(captured.candidate, *pose));
				captured.candidate.geometry = loaded.snapshot;
				captured.candidate.physicalBackingCurrent = loaded.accepted && loaded.snapshot != nullptr;
				input.sources.push_back(std::move(captured));
			});

		// Collider primitives are direct physical inputs. Unsupported cooked forms
		// are not guessed from render meshes; a later typed cooker may publish them
		// through the procedural packet channel instead.
		objects.forEachObjectWithComponents<::ColliderSetComponent>(
			[&](const Object& object, const ::ColliderSetComponent& colliderSet) {
				const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
				if(!pose || !colliderSet.Validate().accepted) return;
				if(!objects.isObjectStatic(object)) {
					// Static object metadata is the scene-owned bake contract. Movable
					// agents and target markers may share collider authoring, but must not
					// turn their admission-time pose into stationary route blockage.
					return;
				}
				for(std::size_t index = 0U; index < colliderSet.colliders.size(); ++index) {
					const TerraEngine::Physics::ColliderRecord& record = colliderSet.colliders[index];
					if(!record.enabled || record.isTrigger) continue;
					NavigationBootstrapSourceInput captured{};
					captured.candidate = MakeCandidate(objects, object, record.recordId,
						NavigationSourceKind::Collider, static_cast<std::uint32_t>(index), record.recordVersion,
						objects.GetComponentMutationRevision(), record.recordVersion, *pose);
					NavigationGeometryDocument document{};
					if(!CopyPrimitiveColliderGeometry(record, *pose, document)) continue;
					const NavigationGeometrySnapshotCreateResult snapshot = NavigationGeometrySnapshot::Create(
						MakeDescriptor(captured.candidate, *pose), document);
					captured.candidate.geometry = snapshot.snapshot;
					captured.candidate.physicalBackingCurrent = snapshot.accepted && snapshot.snapshot != nullptr;
					input.sources.push_back(std::move(captured));
				}
			});

		for(const NavigationEligibleSourceCandidate& procedural : settings.proceduralSources) {
			NavigationBootstrapSourceInput captured{};
			captured.candidate = procedural;
			// The producer supplied a detached immutable snapshot. The index rechecks
			// its identity and revision before a worker-visible build is created.
			input.sources.push_back(std::move(captured));
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
			if(capture.bootstrap.reason == "NavigationUnavailableMissingTerrain"
				&& navigation.PublishUnavailableTerrain(capture.bootstrap.eligibleSourceIndex)) {
				result.accepted = true;
				result.reason = "Published automatic navigation unavailable state for missing terrain.";
				return result;
			}
			result.reason = capture.reason.empty() ? "Navigation scene capture was rejected." : capture.reason;
			return result;
		}
		std::vector<NavigationSystem::InitialGroundBuildInput> builds;
		builds.reserve(capture.bootstrap.builds.size());
		for(const NavigationBootstrapBuildInput& captured : capture.bootstrap.builds) {
			builds.push_back({captured.profile, captured.sources, captured.sourceIndex,
				captured.eligibleSourceIndex, captured.terrainPublicationGeneration});
		}
		if(!navigation.QueueInitialGroundBuilds(std::move(builds))) {
			result.reason = "NavigationSystem rejected the copied bootstrap batch at admission.";
			return result;
		}
		result.accepted = true;
		result.queuedProfileBuilds = capture.bootstrap.builds.size();
		result.reason = "Queued copied automatic terrain-backed navigation bootstrap builds.";
		return result;
	}
}
