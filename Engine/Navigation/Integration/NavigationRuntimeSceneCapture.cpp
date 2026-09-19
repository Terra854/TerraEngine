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
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <variant>

namespace TerraEngine::Navigation {
	namespace {
		[[nodiscard]] std::uint64_t NonZero(std::uint64_t value) noexcept { return value == 0U ? 1U : value; }

		[[nodiscard]] TerraEngine::UUID MakeSourceId(
			const TerraEngine::UUID& objectId, const TerraEngine::UUID& assetId,
			NavigationSourceKind kind, std::uint32_t subshapeIndex) noexcept {
			// Source IDs are opaque index keys, while NavigationEligibleSourceIdentity
			// retains the authoritative object/asset/kind/subshape tuple. Do not XOR
			// those UUIDs: sequential fixture IDs can be equal and would cancel every
			// distinguishing byte. Two independently seeded FNV-style 64-bit lanes
			// provide a deterministic 128-bit key without adding a hot-path dependency.
			constexpr std::uint64_t kFNVPrime = 1'099'511'628'211ULL;
			std::uint64_t firstLane = 14'695'981'039'346'656'037ULL;
			std::uint64_t secondLane = 0x9e3779b97f4a7c15ULL;
			const auto appendByte = [&firstLane, &secondLane, kFNVPrime](const std::uint8_t value) {
				firstLane ^= value;
				firstLane *= kFNVPrime;
				// Domain-separate the second lane before the same stable multiply. This
				// makes repeated/sequential fields unable to cancel as they did with XOR.
				secondLane ^= static_cast<std::uint8_t>(value + 0x9dU);
				secondLane *= kFNVPrime;
			};
			const auto appendUuid = [&appendByte](const TerraEngine::UUID& value) {
				for(const std::uint8_t byte : value) appendByte(byte);
			};
			const auto appendUInt32 = [&appendByte](const std::uint32_t value) {
				for(std::uint32_t shift = 24U;; shift -= 8U) {
					appendByte(static_cast<std::uint8_t>(value >> shift));
					if(shift == 0U) break;
				}
			};

			// Prefix the serialized tuple with a format tag. Future source-key formats
			// can then remain distinct while the captured full identity still validates.
			appendByte(0x4eU);
			appendByte(0x53U);
			appendByte(0x01U);
			appendUuid(objectId);
			appendUuid(assetId);
			appendByte(static_cast<std::uint8_t>(kind));
			appendUInt32(subshapeIndex);

			TerraEngine::UUID sourceId{};
			for(std::size_t byte = 0U; byte < sizeof(firstLane); ++byte) {
				const std::uint32_t shift = static_cast<std::uint32_t>((sizeof(firstLane) - 1U - byte) * 8U);
				sourceId[byte] = static_cast<std::uint8_t>(firstLane >> shift);
				sourceId[byte + sizeof(firstLane)] = static_cast<std::uint8_t>(secondLane >> shift);
			}
			// EMPTY_UUID is reserved as an invalid source identity. Keep the valid
			// tuple key non-empty even in the astronomically unlikely all-zero hash.
			if(sourceId == TerraEngine::EMPTY_UUID) sourceId.back() = 1U;
			return sourceId;
		}

		[[nodiscard]] NavigationEligibleSourceCandidate MakeCandidate(
			const ObjectManager& objects, const Object& object, const TerraEngine::UUID& assetId,
			NavigationSourceKind kind, std::uint32_t subshapeIndex,
			NavigationGeneration geometryRevision, NavigationGeneration physicalGeneration,
			NavigationGeneration resourceGeneration) {
			NavigationEligibleSourceCandidate candidate{};
			candidate.identity.objectId = objects.getUUID(object);
			candidate.identity.objectLifetimeGeneration = NonZero(objects.getObjectLifetimeGeneration(object));
			candidate.identity.assetId = assetId;
			candidate.identity.kind = kind;
			candidate.identity.subshapeIndex = subshapeIndex;
			candidate.identity.sourceId = MakeSourceId(candidate.identity.objectId, assetId, kind, subshapeIndex);
			// Serialized WorldPose values intentionally omit runtime revision counters.
			// ObjectManager owns the canonical revision assigned while reconstructing
			// the Transform, so derive evidence from that authority rather than the
			// copied pose value. Geometry still retains the pose itself for placement.
			const NavigationGeneration canonicalTransformRevision =
				NonZero(objects.GetObjectTransformRevision(object));
			candidate.revisions = {
				candidate.identity.objectLifetimeGeneration,
				canonicalTransformRevision,
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
			// The descriptor and retained source evidence must name the same protected
			// canonical placement revision. A serialized pose can carry no runtime
			// revision even though its shape coordinates remain valid.
			descriptor.canonicalPlacementRevision = candidate.revisions.transformRevision;
			descriptor.physicalReadyRevision = candidate.revisions.physicalPublicationGeneration;
			descriptor.sourceKind = candidate.identity.kind;
			descriptor.subshapeIndex = candidate.identity.subshapeIndex;
			descriptor.canonicalFrameOrigin = pose.position;
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
			// Terrain heights are local to this collision area, so make that exact
			// normalized area origin the immutable source frame rather than the owning
			// terrain object's possibly different canonical placement.
			samples.descriptor.canonicalFrameOrigin = samples.canonicalOrigin;
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
			// The descriptor carries the canonical owner frame. Retain a bounded local
			// primitive here so collider capture preserves pose.cell instead of silently
			// dropping it while constructing a float "world" center.
			const glm::vec3 worldCenter = objectRotation * (localCenter * pose.scale);
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

		[[nodiscard]] std::optional<Scene::NavigationSourceMutationBounds> ColliderBounds(
			const TerraEngine::Physics::ColliderRecord& record, const Scene::WorldPose& pose) {
			NavigationGeometryDocument document{};
			if(!CopyPrimitiveColliderGeometry(record, pose, document) || document.primitives.size() != 1U) return std::nullopt;
			const NavigationGeometryPrimitive& primitive = document.primitives.front();
			glm::vec3 extents{};
			switch(primitive.kind) {
			case NavigationGeometryPrimitiveKind::Box:
				extents = {primitive.halfExtents.x, primitive.halfExtents.y, primitive.halfExtents.z};
				break;
			case NavigationGeometryPrimitiveKind::Sphere:
				extents = {primitive.radius, primitive.radius, primitive.radius};
				break;
			case NavigationGeometryPrimitiveKind::Capsule:
				extents = {primitive.radius, std::max(primitive.radius, primitive.height * 0.5F), primitive.radius};
				break;
			default:
				return std::nullopt;
			}
			// NavigationGeometryVector3 is a serializable value type, not a GLM
			// vector. Convert its copied coordinates explicitly before calculating
			// the old/new canonical bounds for the dirty coverage certificate.
			const glm::vec3 center{primitive.center.x, primitive.center.y, primitive.center.z};
			const Scene::WorldCoordinateResult minimum = Scene::ApplyWorldDelta(pose.position,
				{{}, glm::dvec3(center - extents)});
			const Scene::WorldCoordinateResult maximum = Scene::ApplyWorldDelta(pose.position,
				{{}, glm::dvec3(center + extents)});
			if(!minimum.Succeeded() || !maximum.Succeeded()) return std::nullopt;
			return Scene::NavigationSourceMutationBounds{minimum.position, maximum.position, true};
		}

		struct CapturedStaticColliderSource {
			Object object{};
			const TerraEngine::Physics::ColliderRecord* record = nullptr;
			std::uint32_t subshapeIndex = 0U;
			Scene::WorldPose pose{};
		};

		[[nodiscard]] std::optional<NavigationDynamicSourceReplacement> CaptureStaticColliderReplacement(
			ObjectManager& objects, const CapturedStaticColliderSource& captured) {
			if(captured.record == nullptr) return std::nullopt;
			NavigationBootstrapSourceInput source{};
			source.candidate = MakeCandidate(objects, captured.object, captured.record->recordId,
				NavigationSourceKind::Collider, captured.subshapeIndex, captured.record->recordVersion,
				captured.record->recordVersion, captured.record->recordVersion);
			NavigationGeometryDocument document{};
			if(!CopyPrimitiveColliderGeometry(*captured.record, captured.pose, document)) return std::nullopt;
			const NavigationGeometrySnapshotCreateResult created = NavigationGeometrySnapshot::Create(
				MakeDescriptor(source.candidate, captured.pose), document);
			if(!created.accepted || !created.snapshot) return std::nullopt;
			NavigationDynamicSourceReplacement replacement{};
			replacement.snapshot = created.snapshot;
			replacement.source.sourceId = source.candidate.identity.sourceId;
			replacement.source.sourceRevision = created.snapshot->Descriptor().sourceRevision;
			replacement.source.geometryRevision = source.candidate.revisions.geometryRevision;
			replacement.source.enabled = true;
			replacement.source.identity = source.candidate.identity;
			replacement.source.revisionEvidence = source.candidate.revisions;
			return replacement;
		}

		[[nodiscard]] std::unordered_map<TerraEngine::UUID, CapturedStaticColliderSource>
		CaptureCurrentStaticColliderSources(ObjectManager& objects) {
			std::unordered_map<TerraEngine::UUID, CapturedStaticColliderSource> result;
			objects.forEachObjectWithComponents<::ColliderSetComponent>(
				[&](const Object& object, const ::ColliderSetComponent& colliderSet) {
					const std::optional<Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
					if(!pose || !objects.isObjectActiveInHierarchy(object) || !objects.isObjectStatic(object)
						|| !colliderSet.Validate().accepted) return;
					for(std::size_t index = 0U; index < colliderSet.colliders.size(); ++index) {
						const TerraEngine::Physics::ColliderRecord& record = colliderSet.colliders[index];
						if(!record.enabled || record.isTrigger) continue;
						const TerraEngine::UUID sourceId = MakeSourceId(objects.getUUID(object), record.recordId,
							NavigationSourceKind::Collider, static_cast<std::uint32_t>(index));
						result.emplace(sourceId, CapturedStaticColliderSource{object, &record,
							static_cast<std::uint32_t>(index), *pose});
					}
				});
			return result;
		}
	}

	TerraEngine::UUID MakeNavigationRuntimeSourceId(
		const TerraEngine::UUID& objectId, const TerraEngine::UUID& assetId,
		NavigationSourceKind kind, std::uint32_t subshapeIndex) noexcept {
		return MakeSourceId(objectId, assetId, kind, subshapeIndex);
	}

	bool NavigationAutomaticSourceStructureChanged(
		ObjectManager& objects, std::span<const NavigationSourceIndexRecord> sources) {
		std::unordered_set<TerraEngine::UUID> expected;
		expected.reserve(sources.size());
		for(const NavigationSourceIndexRecord& source : sources) {
			if(!source.identity.IsValid() || (source.identity.kind != NavigationSourceKind::Collider
				&& source.identity.kind != NavigationSourceKind::Mesh
				&& source.identity.kind != NavigationSourceKind::Terrain)) continue;
			expected.insert(source.sourceId);
			const auto object = objects.getObject(source.identity.objectId);
			if(!object || objects.getObjectLifetimeGeneration(*object) != source.identity.objectLifetimeGeneration) return true;
			switch(source.identity.kind) {
			case NavigationSourceKind::Collider: {
				const ::ColliderSetComponent* colliders = objects.GetComponent<::ColliderSetComponent>(*object);
				if(!colliders || !objects.isObjectActiveInHierarchy(*object) || !objects.isObjectStatic(*object)
					|| !colliders->Validate().accepted
					|| source.identity.subshapeIndex >= colliders->colliders.size()) return true;
				const TerraEngine::Physics::ColliderRecord& record = colliders->colliders[source.identity.subshapeIndex];
				if(!record.enabled || record.isTrigger || record.recordId != source.identity.assetId
					|| record.recordVersion != source.revisionEvidence.geometryRevision) return true;
				break;
			}
			case NavigationSourceKind::Mesh: {
				const ::MeshRenderComponent* mesh = objects.GetComponent<::MeshRenderComponent>(*object);
				if(!mesh || mesh->meshId != source.identity.assetId
					|| objects.getObjectComponentGeneration(*object) != source.revisionEvidence.geometryRevision) return true;
				break;
			}
			case NavigationSourceKind::Terrain: {
				const ::TerrainComponent* terrain = objects.GetComponent<::TerrainComponent>(*object);
				if(!terrain || terrain->terrainAssetId != source.identity.assetId
					|| terrain->schemaVersion != source.revisionEvidence.geometryRevision) return true;
				break;
			}
			default:
				continue;
			}
		}

		// Detect eligible additions/removals explicitly. This protected scan is small
		// (only terrain, mesh companions, and static colliders), unlike the former
		// broad component revision which woke navigation for every agent edit.
		bool added = false;
		auto contains = [&expected, &added](const TerraEngine::UUID& sourceId) {
			if(!expected.contains(sourceId)) added = true;
		};
		// Terrain and mesh companions have separate physical/resource publication
		// generations in MainEngine. Do not treat an unready authored component as
		// an automatic-source addition every frame; its authoritative publication
		// wakes the full capture once it is actually eligible.
		objects.forEachObjectWithComponents<::ColliderSetComponent>([&](const Object& object, const ::ColliderSetComponent& colliderSet) {
			// Hierarchy activity is part of the authored static-bake contract. Skipping
			// inactive children here makes activation/deactivation a precise structural
			// transition instead of repeatedly recapturing an invisible collider.
			if(!objects.isObjectActiveInHierarchy(object) || !objects.isObjectStatic(object)
				|| !colliderSet.Validate().accepted) return;
			for(std::size_t index = 0U; index < colliderSet.colliders.size(); ++index) {
				const TerraEngine::Physics::ColliderRecord& record = colliderSet.colliders[index];
				if(record.enabled && !record.isTrigger) contains(MakeSourceId(objects.getUUID(object), record.recordId,
					NavigationSourceKind::Collider, static_cast<std::uint32_t>(index)));
			}
		});
		return added;
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
				++result.terrainComponentsObserved;
				const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
				if(!pose) {
					// A terrain without a protected canonical pose cannot be safely
					// rasterized from an authored/local transform.
					result.terrainCandidateRejectionReason =
						"Active TerrainComponent has no canonical world pose.";
					return;
				}
				++result.terrainComponentsWithCanonicalPose;
				if(terrain.terrainAssetId == TerraEngine::EMPTY_UUID) {
					result.terrainCandidateRejectionReason =
						"Active TerrainComponent has no terrain asset identity.";
					return;
				}
				++result.terrainComponentsWithAsset;
				NavigationBootstrapSourceInput captured{};
				captured.candidate = MakeCandidate(objects, object, terrain.terrainAssetId,
					NavigationSourceKind::Terrain, 0U, terrain.schemaVersion,
					terrainCollision.sourcePublicationGeneration, terrainCollision.currentRecipe.authoredGeneration);
				std::string reason;
				captured.terrain = CopyPublishedTerrain(terrain, captured.candidate, *pose, terrainCollision, reason);
				captured.candidate.physicalBackingCurrent = captured.terrain.has_value();
				if(captured.candidate.physicalBackingCurrent) {
					++result.terrainCandidatesWithCurrentPhysics;
				} else if(!reason.empty()) {
					// Keep the deterministic reason from the most recently examined
					// active terrain. It is emitted once by the frame-end bootstrap log.
					result.terrainCandidateRejectionReason = std::move(reason);
				}
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
					geometryPublicationGeneration, geometryPublicationGeneration);
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
				if(!pose || !objects.isObjectActiveInHierarchy(object) || !colliderSet.Validate().accepted) return;
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
						// Collider record bytes are the physical backing. A global component
						// revision would spuriously revise every unchanged static collider when
						// an unrelated agent/editor value changes, defeating local rebuilds.
						record.recordVersion, record.recordVersion);
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

	NavigationCommittedSourceMutationCapture CaptureCommittedStaticColliderNavigationSourceMutations(
		ObjectManager& objects, Scene::SceneMutationQueue& mutations,
		std::span<const Scene::TransactionResult> committedTransactions,
		std::span<const Scene::NavigationSourceMutationReceipt> explicitReceipts) {
		NavigationCommittedSourceMutationCapture result{};
		const auto currentSources = CaptureCurrentStaticColliderSources(objects);
		std::unordered_map<TerraEngine::UUID, Scene::WorldPose> previousPoses;
		for(const Scene::TransactionResult& transaction : committedTransactions) {
			if(transaction.status != Scene::TransactionStatus::Committed) continue;
			for(const Scene::TransactionInverseDelta& inverse : transaction.inverseDeltas) {
				if(inverse.operation.kind != Scene::TransactionWriteKind::SetWorldTransform
					|| inverse.operation.target.objectId == EMPTY_UUID) continue;
				// One canonical spatial write per object is validated by the transaction
				// layer. The inverse carries the exact pre-commit pose for precise old
				// bounds without preserving a live Transform pointer across a worker edge.
				previousPoses.emplace(inverse.operation.target.objectId, inverse.operation.worldPose);
			}
		}

		std::unordered_set<TerraEngine::UUID> explicitlyNamed;
		for(const Scene::NavigationSourceMutationReceipt& receipt : explicitReceipts) {
			if(!receipt.accepted || receipt.transactionStatus != Scene::TransactionStatus::Committed) continue;
			explicitlyNamed.insert(receipt.intent.sourceId);
			const auto source = currentSources.find(receipt.intent.sourceId);
			if(source == currentSources.end()) {
				// Another typed producer (for example a detached procedural source)
				// owns this ID. Leave its receipt untouched rather than claiming a
				// static-collider bridge can normalize geometry it did not capture.
				continue;
			}
			auto replacement = CaptureStaticColliderReplacement(objects, source->second);
			if(!replacement) {
				result.unhandledSourceIds.push_back(receipt.intent.sourceId);
				continue;
			}
			Scene::NavigationSourceMutationReceipt normalized = receipt;
			normalized.intent.expectedSourceRevision = replacement->source.sourceRevision;
			normalized.intent.committedSourceRevision = replacement->source.sourceRevision;
			normalized.intent.physicalReadyRevision = replacement->snapshot->Descriptor().physicalReadyRevision;
			if(normalized.intent.committedChangeSequence == 0U)
				normalized.intent.committedChangeSequence = mutations.AllocateCommittedNavigationSourceChangeSequence();
			if(normalized.intent.committedChangeSequence == 0U) {
				result.unhandledSourceIds.push_back(receipt.intent.sourceId);
				continue;
			}
			replacement->committedChangeSequence = normalized.intent.committedChangeSequence;
			result.receipts.push_back(std::move(normalized));
			result.replacements.push_back(*replacement);
		}

		struct AutomaticCandidate {
			TerraEngine::UUID sourceId = EMPTY_UUID;
			TerraEngine::UUID objectId = EMPTY_UUID;
			CapturedStaticColliderSource source{};
			Scene::WorldPose previousPose{};
		};
		std::vector<AutomaticCandidate> automaticCandidates;
		for(const auto& [sourceId, source] : currentSources) {
			const TerraEngine::UUID objectId = objects.getUUID(source.object);
			const auto previous = previousPoses.find(objectId);
			if(previous == previousPoses.end() || explicitlyNamed.contains(sourceId)) continue;
			automaticCandidates.push_back({sourceId, objectId, source, previous->second});
		}
		std::sort(automaticCandidates.begin(), automaticCandidates.end(), [](const AutomaticCandidate& left,
			const AutomaticCandidate& right) { return left.sourceId < right.sourceId; });
		for(const AutomaticCandidate& candidate : automaticCandidates) {
			const auto oldBounds = ColliderBounds(*candidate.source.record, candidate.previousPose);
			const auto newBounds = ColliderBounds(*candidate.source.record, candidate.source.pose);
			auto replacement = CaptureStaticColliderReplacement(objects, candidate.source);
			if(!oldBounds || !newBounds || !replacement) {
				result.unhandledSourceIds.push_back(candidate.sourceId);
				continue;
			}
			const std::uint64_t sequence = mutations.AllocateCommittedNavigationSourceChangeSequence();
			if(sequence == 0U) {
				result.unhandledSourceIds.push_back(candidate.sourceId);
				continue;
			}
			Scene::NavigationSourceMutationReceipt receipt{};
			receipt.intent.intentId = "automatic-static-collider-source-"
				+ TerraEngine::ToHexString(candidate.sourceId) + "-" + std::to_string(sequence);
			receipt.intent.transactionId = "automatic-static-collider-transform-"
				+ TerraEngine::ToHexString(candidate.objectId) + "-" + std::to_string(sequence);
			receipt.intent.sourceId = candidate.sourceId;
			receipt.intent.operation = Scene::NavigationSourceMutationOperation::Move;
			receipt.intent.oldBounds = *oldBounds;
			receipt.intent.newBounds = *newBounds;
			receipt.intent.expectedSourceRevision = replacement->source.sourceRevision;
			receipt.intent.committedSourceRevision = replacement->source.sourceRevision;
			receipt.intent.committedChangeSequence = sequence;
			receipt.intent.physicalReadyRevision = replacement->snapshot->Descriptor().physicalReadyRevision;
			receipt.intent.oldEnabled = true;
			receipt.intent.newEnabled = true;
			receipt.transactionStatus = Scene::TransactionStatus::Committed;
			receipt.accepted = true;
			receipt.message = "Static collider transform committed with its scene transaction.";
			replacement->committedChangeSequence = sequence;
			result.receipts.push_back(std::move(receipt));
			result.replacements.push_back(*replacement);
		}
		std::sort(result.unhandledSourceIds.begin(), result.unhandledSourceIds.end());
		result.unhandledSourceIds.erase(std::unique(result.unhandledSourceIds.begin(), result.unhandledSourceIds.end()),
			result.unhandledSourceIds.end());
		return result;
	}

	NavigationCommittedSourceMutationObservation ObserveCommittedNavigationSourceMutations(
		NavigationSystem& navigation, ObjectManager& objects, Scene::SceneMutationQueue& mutations,
		const Scene::SceneMutationBatchResult& batch) {
		// This runs only after SceneMutationQueue committed its ObjectManager writes.
		// Capture the replacement before dirty admission, then preserve the exact
		// receipt group so navigation can publish geometry and source evidence together.
		NavigationCommittedSourceMutationObservation observation{};
		const NavigationCommittedSourceMutationCapture captured =
			CaptureCommittedStaticColliderNavigationSourceMutations(objects, mutations,
				batch.transactionResults, batch.navigationSourceMutationReceipts);

		std::vector<NavigationSystem::DynamicGroundSourceReplacement> replacements;
		replacements.reserve(captured.replacements.size());
		for(const NavigationDynamicSourceReplacement& replacement : captured.replacements) {
			replacements.push_back({replacement.source, replacement.snapshot,
				replacement.committedChangeSequence});
		}
		const bool stageAccepted = replacements.empty()
			|| navigation.StageDynamicGroundSourceReplacements(std::move(replacements));
		// A no-op stage is mechanically accepted, but an unhandled committed source
		// still means the bridge did not represent the whole source transaction.
		// Report that distinction to hosts and tests instead of hiding it as success.
		observation.staged = stageAccepted && captured.unhandledSourceIds.empty();

		std::unordered_set<TerraEngine::UUID> committedSources;
		auto rememberCommitted = [&committedSources](const Scene::NavigationSourceMutationReceipt& receipt) {
			if(receipt.accepted && receipt.transactionStatus == Scene::TransactionStatus::Committed
				&& receipt.intent.sourceId != EMPTY_UUID) {
				committedSources.insert(receipt.intent.sourceId);
			}
		};
		for(const Scene::NavigationSourceMutationReceipt& receipt : captured.receipts) {
			rememberCommitted(receipt);
		}

		std::vector<Scene::NavigationSourceMutationReceipt> receipts;
		if(stageAccepted) receipts = captured.receipts;
		for(const Scene::NavigationSourceMutationReceipt& explicitReceipt : batch.navigationSourceMutationReceipts) {
			rememberCommitted(explicitReceipt);
			const bool bridgeOwnsSource = std::any_of(captured.replacements.begin(), captured.replacements.end(),
				[&explicitReceipt](const NavigationDynamicSourceReplacement& replacement) {
					return replacement.source.sourceId == explicitReceipt.intent.sourceId;
				}) || std::find(captured.unhandledSourceIds.begin(), captured.unhandledSourceIds.end(),
				explicitReceipt.intent.sourceId) != captured.unhandledSourceIds.end();
			if(!bridgeOwnsSource) receipts.push_back(explicitReceipt);
		}
		for(const TerraEngine::UUID sourceId : captured.unhandledSourceIds) {
			if(sourceId != EMPTY_UUID) committedSources.insert(sourceId);
		}

		observation.committedSourceCount = committedSources.size();
		observation.unhandledSourceCount = captured.unhandledSourceIds.size();
		for(const Scene::NavigationSourceMutationReceipt& receipt : receipts) {
			if(receipt.accepted && receipt.transactionStatus == Scene::TransactionStatus::Committed
				&& receipt.intent.sourceId != EMPTY_UUID) {
				++observation.forwardedReceiptCount;
			}
		}
		observation.observedReceiptCount = navigation.ObserveSceneMutationReceipts(receipts);

		const bool committedSourceWasUnrepresented = observation.committedSourceCount != 0U
			&& (!stageAccepted || observation.unhandledSourceCount != 0U
				|| observation.observedReceiptCount != observation.forwardedReceiptCount);
		if(committedSourceWasUnrepresented) {
			// The scene mutation is already visible. Fence the old ready root instead of
			// pretending a partial source observation can preserve navigation safety.
			// This flag requests exactly one later Frame End recapture; rejected scene
			// batches never reach this branch because they add no committed source.
			observation.fenced = navigation.PublishUnavailableTerrain(nullptr, true);
		}
		return observation;
	}
}
