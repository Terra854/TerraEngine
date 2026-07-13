#include "PhysicsAuthoringSnapshot.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/HierarchySnapshot.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>
#include <variant>

namespace TerraEngine::Physics {
	namespace {
		bool ValidPose(const PhysicsPose& pose) noexcept {
			return pose.IsFinite();
		}

		bool ValidObjectId(const TerraEngine::UUID& id) noexcept {
			return id != TerraEngine::EMPTY_UUID;
		}

		std::uint64_t HashBytes(std::uint64_t hash, const void* data, std::size_t size) {
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for(std::size_t index = 0; index < size; ++index) {
				hash ^= bytes[index];
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		template<typename Value>
		std::uint64_t HashValue(std::uint64_t hash, const Value& value) {
			return HashBytes(hash, &value, sizeof(Value));
		}

		std::uint64_t HashRecipe(const PhysicsColliderDescriptor& collider) {
			std::uint64_t hash = 14695981039346656037ULL;
			hash = HashValue(hash, collider.kind);
			hash = HashValue(hash, collider.dimensions);
			hash = HashValue(hash, collider.localPose.position);
			hash = HashValue(hash, collider.localPose.rotation);
			hash = HashBytes(hash, collider.cookedColliderAssetId.data(), collider.cookedColliderAssetId.size());
			hash = HashBytes(hash, collider.materialId.data(), collider.materialId.size());
			hash = HashValue(hash, collider.trigger);
			hash = HashValue(hash, collider.collisionLayer);
			hash = HashValue(hash, collider.queryFlags);
			return hash == 0U ? 1U : hash;
		}

		PhysicsVector3 ToPhysicsVector(const glm::vec3& value) {
			return {value.x, value.y, value.z};
		}

		PhysicsQuaternion ToPhysicsRotation(const glm::quat& value) {
			return {value.x, value.y, value.z, value.w};
		}

		bool IsUniformScale(const glm::vec3& value) {
			return std::fabs(value.x - value.y) <= 1.0e-5F
				&& std::fabs(value.y - value.z) <= 1.0e-5F;
		}

		bool IsIdentityRotation(const PhysicsQuaternion& value) {
			return std::fabs(value.x) <= 1.0e-6F && std::fabs(value.y) <= 1.0e-6F
				&& std::fabs(value.z) <= 1.0e-6F && std::fabs(value.w - 1.0F) <= 1.0e-6F;
		}

		PhysicsVector3 ScaledPrimitiveDimensions(
			const PrimitiveColliderShape& shape,
			const glm::vec3& absoluteScale) {
			return std::visit([&](const auto& payload) -> PhysicsVector3 {
				using Payload = std::decay_t<decltype(payload)>;
				if constexpr(std::is_same_v<Payload, BoxColliderShape>) {
					return {2.0F * payload.halfExtents.x * absoluteScale.x,
						2.0F * payload.halfExtents.y * absoluteScale.y,
						2.0F * payload.halfExtents.z * absoluteScale.z};
				} else if constexpr(std::is_same_v<Payload, SphereColliderShape>) {
					return {payload.radius * std::max({absoluteScale.x, absoluteScale.y, absoluteScale.z}), 0.0F, 0.0F};
				} else if constexpr(std::is_same_v<Payload, CapsuleColliderShape>) {
					return {payload.radius * std::max(absoluteScale.x, absoluteScale.z),
						payload.height * absoluteScale.y, 0.0F};
				} else if constexpr(std::is_same_v<Payload, CylinderColliderShape>) {
					return {payload.radius * std::max(absoluteScale.x, absoluteScale.z),
						payload.height * absoluteScale.y, 0.0F};
				} else if constexpr(std::is_same_v<Payload, TaperedCapsuleColliderShape>) {
					return {payload.topRadius * std::max(absoluteScale.x, absoluteScale.z),
						payload.height * absoluteScale.y,
						payload.bottomRadius * std::max(absoluteScale.x, absoluteScale.z)};
				} else {
					return {1.0F, 1.0F, 1.0F};
				}
			}, shape);
		}

		TerraEngine::UUID CookedColliderAssetId(const PrimitiveColliderShape& shape) {
			return std::visit([](const auto& payload) {
				using Payload = std::decay_t<decltype(payload)>;
				if constexpr(std::is_same_v<Payload, ConvexHullColliderShape>
					|| std::is_same_v<Payload, TriangleMeshColliderShape>
					|| std::is_same_v<Payload, CompoundColliderShape>) {
					return payload.colliderAssetId;
				}
				return TerraEngine::EMPTY_UUID;
			}, shape);
		}
	}

	std::optional<PhysicsAuthoringSnapshot> PhysicsAuthoringSnapshot::Create(PhysicsAuthoringData data) {
		if(!data.world.IsValid() || data.snapshotGeneration == 0 || !data.settings.Validate().accepted) {
			return std::nullopt;
		}
		for(const PhysicsBodyDescriptor& body : data.bodies) {
			if(!ValidObjectId(body.objectId) || body.objectGeneration == 0
				|| body.componentGeneration == 0 || !ValidPose(body.worldPose)
				|| (body.hasParent && (!ValidPose(body.parentWorldPose) || !body.parentWorldScale.IsFinite()))
				|| body.collisionLayer >= kPhysicsCollisionLayerCount) {
				return std::nullopt;
			}
		}
		for(const PhysicsColliderDescriptor& collider : data.colliders) {
			if(!ValidObjectId(collider.objectId) || collider.componentGeneration == 0
				|| !ValidObjectId(collider.recordId) || collider.recordGeneration == 0
				|| !ValidPose(collider.localPose) || !collider.dimensions.IsFinite()
				|| collider.dimensions.x <= 0.0F
				|| collider.collisionLayer >= kPhysicsCollisionLayerCount) {
				return std::nullopt;
			}
		}
		for(const PhysicsConstraintDescriptor& constraint : data.constraints) {
			const bool firstEndpointValid = constraint.firstEndpointKind == PhysicsConstraintEndpointKind::WorldAnchor
				? constraint.firstObjectId == TerraEngine::EMPTY_UUID
				: ValidObjectId(constraint.firstObjectId);
			const bool secondEndpointValid = constraint.secondEndpointKind == PhysicsConstraintEndpointKind::WorldAnchor
				? constraint.secondObjectId == TerraEngine::EMPTY_UUID
				: ValidObjectId(constraint.secondObjectId);
			if(!ValidObjectId(constraint.ownerObjectId) || constraint.ownerObjectGeneration == 0
				|| constraint.componentGeneration == 0
				|| !ValidObjectId(constraint.recordId) || constraint.recordGeneration == 0
				|| !firstEndpointValid || !secondEndpointValid
				|| (constraint.firstEndpointKind == PhysicsConstraintEndpointKind::WorldAnchor
					&& constraint.secondEndpointKind == PhysicsConstraintEndpointKind::WorldAnchor)
				|| !ValidPose(constraint.firstFrame) || !ValidPose(constraint.secondFrame)
				|| !constraint.firstAxis.IsFinite() || !constraint.secondAxis.IsFinite()
				|| constraint.resetGeneration == 0U) {
				return std::nullopt;
			}
		}
		for(const PhysicsCharacterDescriptor& character : data.characters) {
			if(!ValidObjectId(character.objectId) || character.objectGeneration == 0
				|| character.componentGeneration == 0
				|| !ValidPose(character.worldPose) || !std::isfinite(character.radius)
				|| !std::isfinite(character.height) || character.radius <= 0.0F || character.height <= 0.0F
				|| !character.localCenter.IsFinite() || !character.upDirection.IsFinite()
				|| !std::isfinite(character.stepHeight) || !std::isfinite(character.maximumSlopeAngleDegrees)
				|| character.collisionLayer >= kPhysicsCollisionLayerCount) {
				return std::nullopt;
			}
		}
		for(const PhysicsDirtyObjectGeneration& dirty : data.dirtyObjects) {
			if(!ValidObjectId(dirty.objectId) || dirty.objectGeneration == 0) {
				return std::nullopt;
			}
		}

		return PhysicsAuthoringSnapshot(std::make_shared<const PhysicsAuthoringData>(std::move(data)));
	}

	std::optional<PhysicsAuthoringSnapshot> PhysicsAuthoringSnapshot::Capture(
		ObjectManager& objects,
		PhysicsWorldHandle world,
		const PhysicsSettings& settings,
		PhysicsTick tick,
		const PhysicsAuthoringSnapshot* previous) {
		if(!world.IsValid() || !settings.Validate().accepted) return std::nullopt;

		const Scene::HierarchySnapshot hierarchy = Scene::BuildHierarchySnapshot(objects);
		if(!hierarchy.valid) return std::nullopt;

		struct ResolvedTransform {
			glm::vec3 position{0.0F};
			glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
			glm::vec3 scale{1.0F};
		};
		std::vector<ResolvedTransform> transforms(hierarchy.orderedObjects.size());

		PhysicsAuthoringData data;
		data.world = world;
		data.settings = settings;
		data.tick = tick;
		data.snapshotGeneration = previous != nullptr
			? previous->Data().snapshotGeneration + 1U
			: 1U;
		if(data.snapshotGeneration == 0U) data.snapshotGeneration = 1U;
		data.hierarchyGeneration = hierarchy.revision;

		std::unordered_map<TerraEngine::UUID, PhysicsGeneration> previousGenerations;
		std::unordered_map<TerraEngine::UUID, std::uint64_t> previousHashes;
		if(previous != nullptr) {
			for(const PhysicsBodyDescriptor& body : previous->Data().bodies) {
				previousGenerations[body.objectId] = body.componentGeneration;
			}
			for(const PhysicsColliderDescriptor& collider : previous->Data().colliders) {
				previousHashes[collider.objectId] ^= collider.stableRecipeHash;
			}
		}

		for(std::size_t index = 0; index < hierarchy.orderedObjects.size(); ++index) {
			const Scene::HierarchySnapshotEntry& entry = hierarchy.orderedObjects[index];
			const Transform* local = objects.GetComponent<Transform>(entry.object);
			if(local == nullptr) continue;
			const glm::vec3 localPosition = local->position;
			const glm::quat localRotation = glm::normalize(local->rotation);
			const glm::vec3 localScale = local->scale;
			ResolvedTransform resolved;
			if(entry.parentIndex >= 0) {
				const ResolvedTransform& parent = transforms[static_cast<std::size_t>(entry.parentIndex)];
				resolved.position = parent.position + parent.rotation * (parent.scale * localPosition);
				resolved.rotation = glm::normalize(parent.rotation * localRotation);
				resolved.scale = parent.scale * localScale;
			} else {
				resolved = {localPosition, localRotation, localScale};
			}
			transforms[index] = resolved;
			if(!objects.isObjectActive(entry.object)) continue;

			const PhysicsGeneration objectGeneration = static_cast<PhysicsGeneration>(
				objects.getObjectLifetimeGeneration(entry.object));
			const PhysicsGeneration componentGeneration = static_cast<PhysicsGeneration>(
				objects.getObjectComponentGeneration(entry.object));

			// Constraints and virtual characters do not require a ColliderSet on their
			// owner. Capture them before the rigid-body/collider early-out so authored
			// mechanism owners and character-only objects remain visible to physics.
			if(ConstraintSetComponent* constraintSet = objects.GetComponent<ConstraintSetComponent>(entry.object);
				constraintSet != nullptr && constraintSet->active) {
				const ConstraintSetValidationResult validation = constraintSet->Validate();
				if(!validation.accepted) return std::nullopt;
				for(const ConstraintRecord& record : constraintSet->constraints) {
					data.constraints.push_back({
						entry.objectId,
						objectGeneration,
						componentGeneration,
						record.recordId,
						componentGeneration,
						record.kind,
						record.firstEndpointKind,
						record.firstObjectId,
						record.secondEndpointKind,
						record.secondObjectId,
						record.firstFrame,
						record.secondFrame,
						record.firstAxis,
						record.secondAxis,
						record.limits,
						record.spring,
						record.motor,
						record.axisLimits,
						record.axisSprings,
						record.axisMotors,
						record.firstGearConstraintId,
						record.secondGearConstraintId,
						record.gearRatio,
						record.breakForce,
						record.breakTorque,
						record.collideConnected,
						record.enabled,
						record.resetGeneration});
				}
			}

			if(CharacterControllerComponent* character = objects.GetComponent<CharacterControllerComponent>(entry.object);
				character != nullptr && character->active) {
				const CharacterControllerValidationResult validation = character->Validate();
				if(!validation.accepted) return std::nullopt;
				data.characters.push_back({
					entry.objectId,
					objectGeneration,
					componentGeneration,
					{ToPhysicsVector(resolved.position), ToPhysicsRotation(resolved.rotation)},
					character->shapeKind,
					character->radius,
					character->height,
					character->localCenter,
					character->upDirection,
					character->stepHeight,
					character->supportVolumeRadius,
					character->supportVolumeHeight,
					character->maximumSlopeAngleDegrees,
					character->contactPadding,
					character->mass,
					character->maximumStrength,
					character->collisionLayerId,
					character->collisionMask,
					character->groundAdhesion,
					character->gravityScale,
					character->jumpSpeed,
					character->rotationPolicy,
					character->allowStairWalking,
					character->useGravity,
					character->enabled});
			}

			ColliderSetComponent* colliderSet = objects.GetComponent<ColliderSetComponent>(entry.object);
			if(colliderSet == nullptr || !colliderSet->active || colliderSet->colliders.empty()
				|| !colliderSet->Validate().accepted) {
				continue;
			}

			RigidBodyComponent* authoredBody = objects.GetComponent<RigidBodyComponent>(entry.object);
			const bool hasAuthoredBody = authoredBody != nullptr && authoredBody->active
				&& authoredBody->Validate().accepted;
			if(authoredBody != nullptr && authoredBody->active && !hasAuthoredBody) return std::nullopt;

			PhysicsBodyDescriptor body;
			body.objectId = entry.objectId;
			body.objectGeneration = objectGeneration;
			body.componentGeneration = componentGeneration;
			body.worldPose = {ToPhysicsVector(resolved.position), ToPhysicsRotation(resolved.rotation)};
			if(entry.parentIndex >= 0) {
				const ResolvedTransform& parent = transforms[static_cast<std::size_t>(entry.parentIndex)];
				body.parentWorldPose = {ToPhysicsVector(parent.position), ToPhysicsRotation(parent.rotation)};
				body.parentWorldScale = ToPhysicsVector(parent.scale);
				body.hasParent = true;
			}
			if(hasAuthoredBody) {
				body.motionType = authoredBody->motionType;
				body.motionQuality = authoredBody->continuousCollisionMode;
				body.initialLinearVelocity = authoredBody->initialLinearVelocity;
				body.initialAngularVelocity = authoredBody->initialAngularVelocity;
				body.mass = authoredBody->mass;
				body.linearDamping = authoredBody->linearDamping;
				body.angularDamping = authoredBody->angularDamping;
				body.maximumLinearVelocity = authoredBody->maxLinearVelocity;
				body.maximumAngularVelocity = authoredBody->maxAngularVelocity;
				body.allowedDegreesOfFreedom = authoredBody->allowedDegreesOfFreedom;
				body.collisionLayer = authoredBody->collisionLayerId;
				body.collisionMask = authoredBody->useCollisionMaskOverride
					? authoredBody->collisionMaskOverride
					: settings.layers[authoredBody->collisionLayerId].collisionMask;
				body.useGravity = authoredBody->useGravity;
				body.allowSleeping = authoredBody->allowSleeping;
				body.startAwake = authoredBody->startAwake;
				body.enabled = authoredBody->enabled;
			}
			data.bodies.push_back(body);

			const glm::vec3 absoluteScale = glm::abs(resolved.scale);
			std::uint64_t objectRecipeHash = 0U;
			for(std::size_t colliderIndex = 0; colliderIndex < colliderSet->colliders.size(); ++colliderIndex) {
				const ColliderRecord& record = colliderSet->colliders[colliderIndex];
				if(!record.enabled) continue;
				const TerraEngine::UUID cookedAssetId = CookedColliderAssetId(record.shape);
				if(cookedAssetId != TerraEngine::EMPTY_UUID
					&& (resolved.scale.x <= 0.0F || resolved.scale.y <= 0.0F || resolved.scale.z <= 0.0F
						|| !IsUniformScale(absoluteScale)
						|| std::fabs(absoluteScale.x - 1.0F) > 1.0e-5F)) {
					// Rich geometry bakes its supported uniform source scale during
					// cooking. Reflections or another hierarchy scale require a recook.
					return std::nullopt;
				}
				if(!IsUniformScale(absoluteScale) && !IsIdentityRotation(record.localRotation)) {
					// A rotated child under nonuniform hierarchy scale introduces shear,
					// which primitive rigid shapes cannot represent without cooking.
					return std::nullopt;
				}
				PhysicsColliderDescriptor collider;
				collider.objectId = entry.objectId;
				collider.componentGeneration = componentGeneration;
				collider.recordId = record.recordId;
				collider.recordGeneration = componentGeneration;
				collider.localPose.position = ToPhysicsVector(resolved.scale * glm::vec3(
					record.localCenter.x, record.localCenter.y, record.localCenter.z));
				collider.localPose.rotation = record.localRotation;
				collider.kind = record.Kind();
				collider.dimensions = ScaledPrimitiveDimensions(record.shape, absoluteScale);
				collider.cookedColliderAssetId = cookedAssetId;
				collider.materialId = record.materialId;
				collider.subShapeKey = static_cast<std::uint32_t>(
					HashBytes(2166136261U, record.recordId.data(), record.recordId.size()));
				collider.collisionLayer = record.useLayerOverride
					? record.collisionLayerId
					: body.collisionLayer;
				collider.queryFlags = record.queryFlags;
				collider.trigger = record.isTrigger;
				collider.enabled = record.enabled;
				collider.stableRecipeHash = HashRecipe(collider);
				objectRecipeHash ^= collider.stableRecipeHash;
				data.colliders.push_back(collider);
			}

			const bool hierarchyChanged = previous == nullptr
				|| previous->Data().hierarchyGeneration != data.hierarchyGeneration;
			if(hierarchyChanged || previousGenerations[entry.objectId] != componentGeneration
				|| previousHashes[entry.objectId] != objectRecipeHash) {
				data.dirtyObjects.push_back({
					entry.objectId, objectGeneration, componentGeneration, componentGeneration});
			}
		}

		return Create(std::move(data));
	}
}
