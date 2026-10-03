#include "PhysicsAuthoringSnapshot.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/HierarchySnapshot.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
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
			// Material, collision layer, query flags, and component generations belong
			// to the body/subshape instance, not immutable geometry. Leaving them out
			// lets equivalent primitives share a backend shape without sharing their
			// per-body behavior or lifetime.
			hash = HashValue(hash, collider.trigger);
			return hash == 0U ? 1U : hash;
		}

		std::uint64_t HashSettings(const PhysicsSettings& settings) {
			std::uint64_t hash = 14695981039346656037ULL;
			hash = HashValue(hash, settings.schemaVersion);
			hash = HashValue(hash, settings.gravity);
			hash = HashValue(hash, settings.fixedTimeStep);
			hash = HashValue(hash, settings.maxSubsteps);
			hash = HashValue(hash, settings.maxAccumulatedTime);
			hash = HashValue(hash, settings.velocityIterations);
			hash = HashValue(hash, settings.positionIterations);
			for(const CollisionLayer& layer : settings.layers) {
				hash = HashValue(hash, layer.id);
				hash = HashBytes(hash, layer.name.data(), layer.name.size());
				hash = HashValue(hash, layer.collisionMask);
				hash = HashValue(hash, layer.broadPhaseGroup);
				hash = HashValue(hash, layer.enabled);
			}
			hash = HashValue(hash, settings.maxCastDistance);
			hash = HashValue(hash, settings.maxShapeExtent);
			hash = HashValue(hash, settings.maxHitsPerQuery);
			hash = HashValue(hash, settings.maxRequestsPerBatch);
			hash = HashValue(hash, settings.retainedResultTicks);
			hash = HashValue(hash, settings.debugVisualizationEnabled);
			hash = HashValue(hash, settings.retainedDebugTicks);
			hash = HashValue(hash, settings.maxDebugPrimitives);
			return hash == 0U ? 1U : hash;
		}

		std::uint64_t HashBodyProperties(const PhysicsBodyDescriptor& body) {
			std::uint64_t hash = 14695981039346656037ULL;
			hash = HashValue(hash, body.frameMembership.frame.frameId.value);
			hash = HashValue(hash, body.frameMembership.frame.frameGeneration);
			hash = HashValue(hash, body.frameMembership.world.worldId.value);
			hash = HashValue(hash, body.frameMembership.world.worldGeneration);
			hash = HashValue(hash, body.frameMembership.residencyGeneration);
			hash = HashValue(hash, body.motionType);
			hash = HashValue(hash, body.motionQuality);
			hash = HashValue(hash, body.initialLinearVelocity);
			hash = HashValue(hash, body.initialAngularVelocity);
			hash = HashValue(hash, body.mass);
			hash = HashValue(hash, body.linearDamping);
			hash = HashValue(hash, body.angularDamping);
			hash = HashValue(hash, body.maximumLinearVelocity);
			hash = HashValue(hash, body.maximumAngularVelocity);
			hash = HashValue(hash, body.allowedDegreesOfFreedom);
			hash = HashValue(hash, body.collisionMask);
			hash = HashValue(hash, body.useGravity);
			hash = HashValue(hash, body.allowSleeping);
			hash = HashValue(hash, body.startAwake);
			hash = HashValue(hash, body.enabled);
			hash = HashValue(hash, body.collisionLayer);
			return hash == 0U ? 1U : hash;
		}

		std::uint64_t HashCharacterProperties(const PhysicsCharacterDescriptor& character) {
			// Characters have no body descriptor or ColliderSet dependency. Hash their
			// complete authored packet independently so a character-only object enters
			// the typed runtime closure when its pose or configuration changes.
			std::uint64_t hash = 14695981039346656037ULL;
			hash = HashValue(hash, character.worldPose.position);
			hash = HashValue(hash, character.worldPose.rotation);
			hash = HashValue(hash, character.shapeKind);
			hash = HashValue(hash, character.radius);
			hash = HashValue(hash, character.height);
			hash = HashValue(hash, character.localCenter);
			hash = HashValue(hash, character.upDirection);
			hash = HashValue(hash, character.stepHeight);
			hash = HashValue(hash, character.supportVolumeRadius);
			hash = HashValue(hash, character.supportVolumeHeight);
			hash = HashValue(hash, character.maximumSlopeAngleDegrees);
			hash = HashValue(hash, character.contactPadding);
			hash = HashValue(hash, character.mass);
			hash = HashValue(hash, character.maximumStrength);
			hash = HashValue(hash, character.collisionLayer);
			hash = HashValue(hash, character.collisionMask);
			hash = HashValue(hash, character.groundAdhesion);
			hash = HashValue(hash, character.gravityScale);
			hash = HashValue(hash, character.jumpSpeed);
			hash = HashValue(hash, character.rotationPolicy);
			hash = HashValue(hash, character.allowStairWalking);
			hash = HashValue(hash, character.useGravity);
			hash = HashValue(hash, character.enabled);
			return hash == 0U ? 1U : hash;
		}

		std::uint64_t HashColliderProperties(const PhysicsColliderDescriptor& collider) {
			std::uint64_t hash = collider.stableRecipeHash == 0U
				? 14695981039346656037ULL
				: collider.stableRecipeHash;
			hash = HashBytes(hash, collider.recordId.data(), collider.recordId.size());
			// ObjectManager currently exposes a coarse object component generation.
			// Hashing it here would misclassify a rigid-body-only edit as a collider
			// rebuild. The value fields below identify actual collider changes.
			hash = HashBytes(hash, collider.materialId.data(), collider.materialId.size());
			hash = HashValue(hash, collider.subShapeKey);
			hash = HashValue(hash, collider.collisionLayer);
			hash = HashValue(hash, collider.queryFlags);
			hash = HashValue(hash, collider.trigger);
			hash = HashValue(hash, collider.enabled);
			return hash == 0U ? 1U : hash;
		}

		std::unordered_map<TerraEngine::UUID, std::uint64_t> BuildColliderHashes(
			const PhysicsAuthoringData& data) {
			std::unordered_map<TerraEngine::UUID, std::uint64_t> hashes;
			for(const PhysicsColliderDescriptor& collider : data.colliders) {
				std::uint64_t& hash = hashes[collider.objectId];
				if(hash == 0U) hash = 14695981039346656037ULL;
				hash ^= HashColliderProperties(collider) + 0x9e3779b97f4a7c15ULL
					+ (hash << 6U) + (hash >> 2U);
			}
			return hashes;
		}

		void BuildSnapshotIndexes(
			PhysicsAuthoringData& data,
			const PhysicsAuthoringSnapshot* previous) {
			// Group collider records by body while preserving authored order within
			// each body. Descriptor ranges can then be consumed without hash lookups.
			std::stable_sort(data.colliders.begin(), data.colliders.end(),
				[](const PhysicsColliderDescriptor& left, const PhysicsColliderDescriptor& right) {
					return left.objectId < right.objectId;
				});
			std::unordered_map<TerraEngine::UUID, PhysicsDescriptorIndex> previousIndices;
			PhysicsDescriptorIndex nextIndex = 0U;
			if(previous != nullptr) {
				nextIndex = previous->Data().nextStableBodyDescriptorIndex;
				for(const PhysicsBodyDescriptorIndexRecord& record : previous->Data().bodyDescriptorIndices) {
					previousIndices.emplace(record.objectId, record.stableIndex);
				}
			}

			std::unordered_map<TerraEngine::UUID, std::pair<std::uint32_t, std::uint32_t>> colliderRanges;
			for(std::uint32_t index = 0U; index < data.colliders.size(); ++index) {
				auto [found, inserted] = colliderRanges.try_emplace(
					data.colliders[index].objectId, std::pair{index, 0U});
				(void)inserted;
				++found->second.second;
			}

			data.bodyPresence.clear();
			data.bodyDescriptorIndices.clear();
			data.bodyPresence.reserve(data.bodies.size());
			data.bodyDescriptorIndices.reserve(data.bodies.size());
			for(std::uint32_t vectorIndex = 0U; vectorIndex < data.bodies.size(); ++vectorIndex) {
				const PhysicsBodyDescriptor& body = data.bodies[vectorIndex];
				PhysicsDescriptorIndex stableIndex = kInvalidPhysicsDescriptorIndex;
				if(const auto retained = previousIndices.find(body.objectId); retained != previousIndices.end()) {
					stableIndex = retained->second;
				} else {
					stableIndex = nextIndex++;
				}
				const auto range = colliderRanges.find(body.objectId);
				data.bodyPresence.push_back(body.objectId);
				data.bodyDescriptorIndices.push_back({
					body.objectId,
					stableIndex,
					vectorIndex,
					range != colliderRanges.end() ? range->second.first : 0U,
					range != colliderRanges.end() ? range->second.second : 0U});
			}
			std::sort(data.bodyPresence.begin(), data.bodyPresence.end());
			std::sort(data.bodyDescriptorIndices.begin(), data.bodyDescriptorIndices.end(),
				[](const PhysicsBodyDescriptorIndexRecord& left,
					const PhysicsBodyDescriptorIndexRecord& right) {
					return left.objectId < right.objectId;
				});
			data.nextStableBodyDescriptorIndex = nextIndex;

			struct Dependency {
				PhysicsAssetKind kind;
				TerraEngine::UUID assetId;
				TerraEngine::UUID objectId;
			};
			std::vector<Dependency> dependencies;
			for(const PhysicsColliderDescriptor& collider : data.colliders) {
				if(collider.cookedColliderAssetId != TerraEngine::EMPTY_UUID) {
					dependencies.push_back({PhysicsAssetKind::CookedCollider,
						collider.cookedColliderAssetId, collider.objectId});
				}
				if(collider.materialId != TerraEngine::EMPTY_UUID) {
					dependencies.push_back({PhysicsAssetKind::Material,
						collider.materialId, collider.objectId});
				}
			}
			std::sort(dependencies.begin(), dependencies.end(),
				[](const Dependency& left, const Dependency& right) {
					return std::tie(left.kind, left.assetId, left.objectId)
						< std::tie(right.kind, right.assetId, right.objectId);
				});
			dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
				[](const Dependency& left, const Dependency& right) {
					return left.kind == right.kind && left.assetId == right.assetId
						&& left.objectId == right.objectId;
				}), dependencies.end());
			data.assetDependencies.clear();
			data.assetDependentObjects.clear();
			for(std::size_t begin = 0U; begin < dependencies.size();) {
				std::size_t end = begin + 1U;
				while(end < dependencies.size() && dependencies[end].kind == dependencies[begin].kind
					&& dependencies[end].assetId == dependencies[begin].assetId) ++end;
				const std::uint32_t objectBegin = static_cast<std::uint32_t>(data.assetDependentObjects.size());
				for(std::size_t index = begin; index < end; ++index) {
					data.assetDependentObjects.push_back(dependencies[index].objectId);
				}
				data.assetDependencies.push_back({
					dependencies[begin].assetId,
					dependencies[begin].kind,
					objectBegin,
					static_cast<std::uint32_t>(end - begin)});
				begin = end;
			}

			std::vector<std::pair<std::uint64_t, std::uint32_t>> primitiveColliders;
			for(std::uint32_t index = 0U; index < data.colliders.size(); ++index) {
				const PhysicsColliderDescriptor& collider = data.colliders[index];
				if(collider.cookedColliderAssetId == TerraEngine::EMPTY_UUID) {
					primitiveColliders.emplace_back(collider.stableRecipeHash, index);
				}
			}
			std::sort(primitiveColliders.begin(), primitiveColliders.end());
			data.primitiveShapeShares.clear();
			data.primitiveShapeColliderIndices.clear();
			for(std::size_t begin = 0U; begin < primitiveColliders.size();) {
				std::size_t end = begin + 1U;
				while(end < primitiveColliders.size()
					&& primitiveColliders[end].first == primitiveColliders[begin].first) ++end;
				const std::uint32_t colliderBegin =
					static_cast<std::uint32_t>(data.primitiveShapeColliderIndices.size());
				for(std::size_t index = begin; index < end; ++index) {
					data.primitiveShapeColliderIndices.push_back(primitiveColliders[index].second);
				}
				data.primitiveShapeShares.push_back({
					primitiveColliders[begin].first,
					colliderBegin,
					static_cast<std::uint32_t>(end - begin)});
				begin = end;
			}

			struct ConstraintDependency {
				TerraEngine::UUID objectId;
				TerraEngine::UUID recordId;
			};
			std::vector<ConstraintDependency> constraintDependencies;
			std::unordered_map<TerraEngine::UUID, const PhysicsConstraintDescriptor*> constraintsByRecord;
			constraintsByRecord.reserve(data.constraints.size());
			for(const PhysicsConstraintDescriptor& constraint : data.constraints) {
				constraintsByRecord.emplace(constraint.recordId, &constraint);
			}
			for(const PhysicsConstraintDescriptor& constraint : data.constraints) {
				// A constraint rebuilds when its owner or either concrete endpoint changes.
				constraintDependencies.push_back({constraint.ownerObjectId, constraint.recordId});
				if(constraint.firstObjectId != TerraEngine::EMPTY_UUID)
					constraintDependencies.push_back({constraint.firstObjectId, constraint.recordId});
				if(constraint.secondObjectId != TerraEngine::EMPTY_UUID)
					constraintDependencies.push_back({constraint.secondObjectId, constraint.recordId});
				if(constraint.kind == PhysicsConstraintKind::Gear) {
					// A gear retains provider references to two source constraints. Expand
					// their owner/endpoints here, so changing either source body rebuilds
					// the gear without a later scan across all constraint records.
					for(const TerraEngine::UUID sourceId : {constraint.firstGearConstraintId,
						constraint.secondGearConstraintId}) {
						const auto source = constraintsByRecord.find(sourceId);
						if(source == constraintsByRecord.end()) continue;
						for(const TerraEngine::UUID objectId : {source->second->ownerObjectId,
							source->second->firstObjectId, source->second->secondObjectId}) {
							if(objectId != TerraEngine::EMPTY_UUID)
								constraintDependencies.push_back({objectId, constraint.recordId});
						}
					}
				}
			}
			std::sort(constraintDependencies.begin(), constraintDependencies.end(),
				[](const ConstraintDependency& left, const ConstraintDependency& right) {
					return std::tie(left.objectId, left.recordId) < std::tie(right.objectId, right.recordId);
				});
			constraintDependencies.erase(std::unique(constraintDependencies.begin(), constraintDependencies.end(),
				[](const ConstraintDependency& left, const ConstraintDependency& right) {
					return left.objectId == right.objectId && left.recordId == right.recordId;
				}), constraintDependencies.end());
			data.constraintDependencies.clear();
			data.constraintDependentRecordIds.clear();
			for(std::size_t begin = 0U; begin < constraintDependencies.size();) {
				std::size_t end = begin + 1U;
				while(end < constraintDependencies.size()
					&& constraintDependencies[end].objectId == constraintDependencies[begin].objectId) ++end;
				const std::uint32_t recordBegin = static_cast<std::uint32_t>(data.constraintDependentRecordIds.size());
				for(std::size_t index = begin; index < end; ++index)
					data.constraintDependentRecordIds.push_back(constraintDependencies[index].recordId);
				data.constraintDependencies.push_back({constraintDependencies[begin].objectId, recordBegin,
					static_cast<std::uint32_t>(end - begin)});
				begin = end;
			}
			data.characterObjects.clear();
			data.characterDescriptorIndices.clear();
			data.characterDescriptorIndices.reserve(data.characters.size());
			for(std::uint32_t index = 0U; index < data.characters.size(); ++index) {
				data.characterObjects.push_back(data.characters[index].objectId);
				data.characterDescriptorIndices.push_back({data.characters[index].objectId, index});
			}
			std::sort(data.characterObjects.begin(), data.characterObjects.end());
			data.characterObjects.erase(std::unique(data.characterObjects.begin(), data.characterObjects.end()), data.characterObjects.end());
			data.constraintDescriptorIndices.clear();
			data.constraintDescriptorIndices.reserve(data.constraints.size());
			for(std::uint32_t index = 0U; index < data.constraints.size(); ++index) {
				data.constraintDescriptorIndices.push_back({data.constraints[index].recordId, index});
			}
			std::sort(data.constraintDescriptorIndices.begin(), data.constraintDescriptorIndices.end(),
				[](const PhysicsConstraintDescriptorIndexRecord& left,
					const PhysicsConstraintDescriptorIndexRecord& right) {
					return left.recordId < right.recordId;
				});
			std::sort(data.characterDescriptorIndices.begin(), data.characterDescriptorIndices.end(),
				[](const PhysicsCharacterDescriptorIndexRecord& left,
					const PhysicsCharacterDescriptorIndexRecord& right) {
					return left.objectId < right.objectId;
				});
		}

		std::uint64_t CombineTransformRevision(
			std::uint64_t parentRevision,
			std::uint64_t localRevision) noexcept {
			// FNV-style mixing keeps parent-before-child dependencies deterministic
			// without treating the revision as a provider-facing identity.
			std::uint64_t combined = 14695981039346656037ULL;
			combined ^= parentRevision;
			combined *= 1099511628211ULL;
			combined ^= localRevision;
			combined *= 1099511628211ULL;
			return combined;
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

	std::optional<PhysicsAuthoringSnapshot> PhysicsAuthoringSnapshot::Create(
		PhysicsAuthoringData data,
		const PhysicsAuthoringSnapshot* previous) {
		if(!data.world.IsValid() || data.snapshotGeneration == 0 || !data.settings.Validate().accepted) {
			return std::nullopt;
		}
		if((data.bodyDescriptorIndices.empty() && !data.bodies.empty())
			|| (!data.constraints.empty() && data.constraintDependencies.empty())
			|| data.constraintDescriptorIndices.size() != data.constraints.size()
			|| data.characterDescriptorIndices.size() != data.characters.size()
			|| (!data.characters.empty() && data.characterObjects.empty())) {
			BuildSnapshotIndexes(data, previous);
		}
		for(const PhysicsBodyDescriptor& body : data.bodies) {
			if(!ValidObjectId(body.objectId) || body.objectGeneration == 0
				|| body.componentGeneration == 0 || !ValidPose(body.worldPose)
				|| !body.frameMembership.IsValid()
				|| !std::isfinite(body.canonicalWorldPose.position.offsetMetres.x)
				|| !std::isfinite(body.canonicalWorldPose.position.offsetMetres.y)
				|| !std::isfinite(body.canonicalWorldPose.position.offsetMetres.z)
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
				|| character.objectTransformRevision == 0U || character.hierarchyGeneration == 0U
				|| (character.hasParent && (!ValidPose(character.parentWorldPose)
					|| !character.parentWorldScale.IsFinite() || character.parentObjectId == TerraEngine::EMPTY_UUID
					|| character.parentObjectGeneration == 0U || character.parentTransformRevision == 0U))
				|| (!character.hasParent && (character.parentObjectId != TerraEngine::EMPTY_UUID
					|| character.parentObjectGeneration != 0U || character.parentTransformRevision != 0U))
				|| character.collisionLayer >= kPhysicsCollisionLayerCount) {
				return std::nullopt;
			}
		}
		for(const PhysicsDirtyObjectGeneration& dirty : data.dirtyObjects) {
			if(!ValidObjectId(dirty.objectId) || dirty.objectGeneration == 0) {
				return std::nullopt;
			}
		}
		if(data.bodyPresence.size() != data.bodies.size()
			|| data.bodyDescriptorIndices.size() != data.bodies.size()) {
			return std::nullopt;
		}
		std::set<PhysicsDescriptorIndex> stableIndices;
		std::uint32_t expectedColliderBegin = 0U;
		for(std::size_t index = 0U; index < data.bodyDescriptorIndices.size(); ++index) {
			const PhysicsBodyDescriptorIndexRecord& record = data.bodyDescriptorIndices[index];
			if(record.objectId == TerraEngine::EMPTY_UUID
				|| record.stableIndex == kInvalidPhysicsDescriptorIndex
				|| record.stableIndex >= data.nextStableBodyDescriptorIndex
				|| record.bodyVectorIndex >= data.bodies.size()
				|| data.bodies[record.bodyVectorIndex].objectId != record.objectId
				|| data.bodyPresence[index] != record.objectId
				|| record.colliderBegin + record.colliderCount > data.colliders.size()
				|| (record.colliderCount > 0U && record.colliderBegin != expectedColliderBegin)
				|| (index > 0U && data.bodyDescriptorIndices[index - 1U].objectId >= record.objectId)
				|| !stableIndices.insert(record.stableIndex).second) {
				return std::nullopt;
			}
			for(std::uint32_t colliderIndex = record.colliderBegin;
				colliderIndex < record.colliderBegin + record.colliderCount;
				++colliderIndex) {
				if(data.colliders[colliderIndex].objectId != record.objectId) return std::nullopt;
			}
			expectedColliderBegin += record.colliderCount;
		}
		if(expectedColliderBegin != data.colliders.size()) return std::nullopt;
		if(!std::is_sorted(data.bodyPresence.begin(), data.bodyPresence.end())
			|| std::adjacent_find(data.bodyPresence.begin(), data.bodyPresence.end()) != data.bodyPresence.end()) {
			return std::nullopt;
		}
		if(data.constraintDescriptorIndices.size() != data.constraints.size()
			|| data.characterDescriptorIndices.size() != data.characters.size()) {
			return std::nullopt;
		}
		for(std::size_t index = 0U; index < data.constraintDescriptorIndices.size(); ++index) {
			const PhysicsConstraintDescriptorIndexRecord& record = data.constraintDescriptorIndices[index];
			if(record.recordId == TerraEngine::EMPTY_UUID
				|| record.constraintVectorIndex >= data.constraints.size()
				|| data.constraints[record.constraintVectorIndex].recordId != record.recordId
				|| (index > 0U && data.constraintDescriptorIndices[index - 1U].recordId >= record.recordId)) {
				return std::nullopt;
			}
		}
		for(std::size_t index = 0U; index < data.characterDescriptorIndices.size(); ++index) {
			const PhysicsCharacterDescriptorIndexRecord& record = data.characterDescriptorIndices[index];
			if(record.objectId == TerraEngine::EMPTY_UUID
				|| record.characterVectorIndex >= data.characters.size()
				|| data.characters[record.characterVectorIndex].objectId != record.objectId
				|| (index > 0U && data.characterDescriptorIndices[index - 1U].objectId >= record.objectId)) {
				return std::nullopt;
			}
		}
		std::uint32_t expectedDependentObjectBegin = 0U;
		for(std::size_t index = 0U; index < data.assetDependencies.size(); ++index) {
			const PhysicsAssetDependencyRange& dependency = data.assetDependencies[index];
			if(dependency.assetId == TerraEngine::EMPTY_UUID || dependency.objectCount == 0U
				|| dependency.objectBegin != expectedDependentObjectBegin
				|| dependency.objectBegin + dependency.objectCount > data.assetDependentObjects.size()
				|| (index > 0U
					&& std::tie(data.assetDependencies[index - 1U].assetKind,
						data.assetDependencies[index - 1U].assetId)
						>= std::tie(dependency.assetKind, dependency.assetId))) {
				return std::nullopt;
			}
			for(std::uint32_t objectIndex = dependency.objectBegin;
				objectIndex < dependency.objectBegin + dependency.objectCount;
				++objectIndex) {
				const TerraEngine::UUID& objectId = data.assetDependentObjects[objectIndex];
				if(!std::binary_search(data.bodyPresence.begin(), data.bodyPresence.end(), objectId)
					|| (objectIndex > dependency.objectBegin
						&& data.assetDependentObjects[objectIndex - 1U] >= objectId)) {
					return std::nullopt;
				}
			}
			expectedDependentObjectBegin += dependency.objectCount;
		}
		if(expectedDependentObjectBegin != data.assetDependentObjects.size()) return std::nullopt;
		std::uint32_t expectedPrimitiveColliderBegin = 0U;
		for(std::size_t index = 0U; index < data.primitiveShapeShares.size(); ++index) {
			const PhysicsPrimitiveShapeShareRange& share = data.primitiveShapeShares[index];
			if(share.stableRecipeHash == 0U || share.colliderCount == 0U
				|| share.colliderBegin != expectedPrimitiveColliderBegin
				|| share.colliderBegin + share.colliderCount > data.primitiveShapeColliderIndices.size()
				|| (index > 0U
					&& data.primitiveShapeShares[index - 1U].stableRecipeHash >= share.stableRecipeHash)) {
				return std::nullopt;
			}
			for(std::uint32_t colliderOffset = share.colliderBegin;
				colliderOffset < share.colliderBegin + share.colliderCount;
				++colliderOffset) {
				const std::uint32_t colliderIndex = data.primitiveShapeColliderIndices[colliderOffset];
				if(colliderIndex >= data.colliders.size()
					|| data.colliders[colliderIndex].cookedColliderAssetId != TerraEngine::EMPTY_UUID
					|| data.colliders[colliderIndex].stableRecipeHash != share.stableRecipeHash) {
					return std::nullopt;
				}
			}
			expectedPrimitiveColliderBegin += share.colliderCount;
		}
		if(expectedPrimitiveColliderBegin != data.primitiveShapeColliderIndices.size()) {
			return std::nullopt;
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
			std::uint64_t dependencyRevision = 0U;
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
		data.sceneGeneration = objects.GetSceneGeneration();
		data.hierarchyGeneration = hierarchy.revision;

		std::unordered_map<TerraEngine::UUID, PhysicsGeneration> previousGenerations;
		std::unordered_map<TerraEngine::UUID, std::uint64_t> previousTransformRevisions;
		std::unordered_map<TerraEngine::UUID, std::uint64_t> previousHashes;
		if(previous != nullptr) {
			for(const PhysicsBodyDescriptor& body : previous->Data().bodies) {
				previousGenerations[body.objectId] = body.componentGeneration;
				previousTransformRevisions[body.objectId] = body.sourceTransformRevision;
			}
			for(const PhysicsColliderDescriptor& collider : previous->Data().colliders) {
				previousHashes[collider.objectId] ^= collider.stableRecipeHash;
			}
		}

		for(std::size_t index = 0; index < hierarchy.orderedObjects.size(); ++index) {
			const Scene::HierarchySnapshotEntry& entry = hierarchy.orderedObjects[index];
			const std::optional<Scene::WorldPose> canonicalPose = objects.GetCanonicalWorldPose(entry.object);
			if(!canonicalPose) continue;
			const glm::vec3 localPosition{
				static_cast<float>(canonicalPose->position.offsetMetres.x),
				static_cast<float>(canonicalPose->position.offsetMetres.y),
				static_cast<float>(canonicalPose->position.offsetMetres.z)};
			const glm::quat localRotation = glm::normalize(canonicalPose->rotation);
			const glm::vec3 localScale = canonicalPose->scale;
			const std::uint64_t localTransformRevision = objects.GetObjectTransformRevision(entry.object);
			ResolvedTransform resolved;
			// Hierarchy propagation already produced the canonical pose. Physics only
			// consumes its bounded frame-local offset, never rebuilds a global float
			// location by composing parent translations.
			resolved = {localPosition, localRotation, localScale,
				CombineTransformRevision(0U, localTransformRevision)};
			transforms[index] = resolved;
			// Physics follows effective hierarchy activation: disabling a parent
			// removes every descendant body without rewriting child authoring data.
			if(!objects.isObjectActiveInHierarchy(entry.object)) continue;

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
				PhysicsCharacterDescriptor descriptor{
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
					character->enabled};
				descriptor.objectTransformRevision = localTransformRevision;
				descriptor.hierarchyGeneration = data.hierarchyGeneration;
				if(entry.parentIndex >= 0) {
					const Scene::HierarchySnapshotEntry& parentEntry =
						hierarchy.orderedObjects[static_cast<std::size_t>(entry.parentIndex)];
					const ResolvedTransform& parent = transforms[static_cast<std::size_t>(entry.parentIndex)];
					descriptor.parentObjectId = parentEntry.objectId;
					descriptor.parentObjectGeneration = objects.getObjectLifetimeGeneration(parentEntry.object);
					descriptor.parentTransformRevision = objects.GetObjectTransformRevision(parentEntry.object);
					descriptor.parentWorldPose = {ToPhysicsVector(parent.position), ToPhysicsRotation(parent.rotation)};
					descriptor.parentWorldScale = ToPhysicsVector(parent.scale);
					descriptor.hasParent = true;
				}
				data.characters.push_back(std::move(descriptor));
			}

			RigidBodyComponent* authoredBody = objects.GetComponent<RigidBodyComponent>(entry.object);
			ColliderSetComponent* colliderSet = objects.GetComponent<ColliderSetComponent>(entry.object);
			const ColliderSetValidationResult colliderValidation = colliderSet != nullptr
				? colliderSet->Validate()
				: ColliderSetValidationResult{};
			if(colliderSet == nullptr || !colliderSet->active || colliderSet->colliders.empty()
				|| !colliderValidation.accepted) {
				if(authoredBody != nullptr && authoredBody->active) {
					std::string message = "RigidBodyComponent was excluded because it requires at least one enabled, valid collider. ";
					message += colliderValidation.message.empty()
						? "Add or enable a ColliderSetComponent record."
						: "Fix the ColliderSetComponent: " + colliderValidation.message;
					data.diagnostics.push_back({entry.objectId, "RigidBodyMissingValidCollider", std::move(message)});
				}
				continue;
			}

			const bool hasAuthoredBody = authoredBody != nullptr && authoredBody->active
				&& authoredBody->Validate().accepted;
			if(authoredBody != nullptr && authoredBody->active && !hasAuthoredBody) return std::nullopt;

			PhysicsBodyDescriptor body;
			body.objectId = entry.objectId;
			body.objectGeneration = objectGeneration;
			body.componentGeneration = componentGeneration;
			body.objectTransformRevision = localTransformRevision;
			body.canonicalWorldPose = *canonicalPose;
			// The original frame is explicit authority. A later protected migration
			// changes this membership together with provider state; float poses never
			// select or imply a frame on their own.
			body.frameMembership = {{kPrimaryPhysicsFrameId, world.worldGeneration}, world, 1U};
			body.worldPose = {ToPhysicsVector(resolved.position), ToPhysicsRotation(resolved.rotation)};
			body.sourceTransformRevision = resolved.dependencyRevision;
			if(entry.parentIndex >= 0) {
				const Scene::HierarchySnapshotEntry& parentEntry =
					hierarchy.orderedObjects[static_cast<std::size_t>(entry.parentIndex)];
				const ResolvedTransform& parent = transforms[static_cast<std::size_t>(entry.parentIndex)];
				body.parentObjectId = parentEntry.objectId;
				body.parentObjectGeneration = objects.getObjectLifetimeGeneration(parentEntry.object);
				body.parentTransformRevision = objects.GetObjectTransformRevision(parentEntry.object);
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
				|| previousTransformRevisions[entry.objectId] != body.sourceTransformRevision
				|| previousHashes[entry.objectId] != objectRecipeHash) {
				data.dirtyObjects.push_back({
					entry.objectId, objectGeneration, componentGeneration, componentGeneration});
			}
		}

		BuildSnapshotIndexes(data, previous);
		for(PhysicsDirtyObjectGeneration& dirty : data.dirtyObjects) {
			const auto index = std::lower_bound(data.bodyDescriptorIndices.begin(),
				data.bodyDescriptorIndices.end(), dirty.objectId,
				[](const PhysicsBodyDescriptorIndexRecord& record, const TerraEngine::UUID& id) {
					return record.objectId < id;
				});
			const bool descriptorFound = index != data.bodyDescriptorIndices.end()
				&& index->objectId == dirty.objectId;
			if(descriptorFound) {
				dirty.descriptorIndex = index->stableIndex;
			}
			dirty.reasons = previous == nullptr
				? PhysicsAuthoringChangeReason::Added | PhysicsAuthoringChangeReason::Lifecycle
					| PhysicsAuthoringChangeReason::BodyComponent
					| PhysicsAuthoringChangeReason::ColliderComponent
				: PhysicsAuthoringChangeReason::BodyComponent
					| PhysicsAuthoringChangeReason::ColliderComponent;
			if(previous != nullptr && descriptorFound
				&& previousTransformRevisions[dirty.objectId] != 0U
				&& previousTransformRevisions[dirty.objectId]
					!= data.bodies[index->bodyVectorIndex].sourceTransformRevision) {
				dirty.reasons |= PhysicsAuthoringChangeReason::Transform;
				if(previous->Data().hierarchyGeneration != data.hierarchyGeneration) {
					dirty.reasons |= PhysicsAuthoringChangeReason::HierarchyDependency;
				}
			}
		}
		return Create(std::move(data));
	}

	const PhysicsBodyDescriptorIndexRecord* PhysicsAuthoringSnapshot::FindBodyDescriptorIndex(
		const TerraEngine::UUID& objectId) const noexcept {
		if(data_ == nullptr) return nullptr;
		const auto found = std::lower_bound(data_->bodyDescriptorIndices.begin(),
			data_->bodyDescriptorIndices.end(), objectId,
			[](const PhysicsBodyDescriptorIndexRecord& record, const TerraEngine::UUID& id) {
				return record.objectId < id;
			});
		return found != data_->bodyDescriptorIndices.end() && found->objectId == objectId
			? &*found
			: nullptr;
	}

	std::vector<TerraEngine::UUID> PhysicsAuthoringSnapshot::ObjectsDependingOn(
		const TerraEngine::UUID& assetId,
		PhysicsAssetKind assetKind) const {
		std::vector<TerraEngine::UUID> objects;
		if(data_ == nullptr || assetId == TerraEngine::EMPTY_UUID) return objects;
		const auto found = std::lower_bound(data_->assetDependencies.begin(),
			data_->assetDependencies.end(), std::pair{assetKind, assetId},
			[](const PhysicsAssetDependencyRange& range,
				const std::pair<PhysicsAssetKind, TerraEngine::UUID>& key) {
				return std::tie(range.assetKind, range.assetId) < std::tie(key.first, key.second);
			});
		if(found == data_->assetDependencies.end() || found->assetKind != assetKind
			|| found->assetId != assetId) return objects;
		objects.assign(data_->assetDependentObjects.begin() + found->objectBegin,
			data_->assetDependentObjects.begin() + found->objectBegin + found->objectCount);
		return objects;
	}

	const PhysicsConstraintDescriptor* PhysicsAuthoringSnapshot::FindConstraintDescriptor(
		const TerraEngine::UUID& recordId) const noexcept {
		if(data_ == nullptr) return nullptr;
		const auto found = std::lower_bound(data_->constraintDescriptorIndices.begin(),
			data_->constraintDescriptorIndices.end(), recordId,
			[](const PhysicsConstraintDescriptorIndexRecord& record, const TerraEngine::UUID& id) {
				return record.recordId < id;
			});
		return found != data_->constraintDescriptorIndices.end() && found->recordId == recordId
			? &data_->constraints[found->constraintVectorIndex]
			: nullptr;
	}

	const PhysicsCharacterDescriptor* PhysicsAuthoringSnapshot::FindCharacterDescriptor(
		const TerraEngine::UUID& objectId) const noexcept {
		if(data_ == nullptr) return nullptr;
		const auto found = std::lower_bound(data_->characterDescriptorIndices.begin(),
			data_->characterDescriptorIndices.end(), objectId,
			[](const PhysicsCharacterDescriptorIndexRecord& record, const TerraEngine::UUID& id) {
				return record.objectId < id;
			});
		return found != data_->characterDescriptorIndices.end() && found->objectId == objectId
			? &data_->characters[found->characterVectorIndex]
			: nullptr;
	}

	std::vector<TerraEngine::UUID> PhysicsAuthoringSnapshot::ConstraintsDependingOn(
		const TerraEngine::UUID& objectId) const {
		std::vector<TerraEngine::UUID> records;
		if(data_ == nullptr || objectId == TerraEngine::EMPTY_UUID) return records;
		const auto found = std::lower_bound(data_->constraintDependencies.begin(), data_->constraintDependencies.end(), objectId,
			[](const PhysicsConstraintDependencyRange& range, const TerraEngine::UUID& id) {
				return range.objectId < id;
			});
		if(found == data_->constraintDependencies.end() || found->objectId != objectId) return records;
		records.assign(data_->constraintDependentRecordIds.begin() + found->recordBegin,
			data_->constraintDependentRecordIds.begin() + found->recordBegin + found->recordCount);
		return records;
	}

	bool PhysicsAuthoringSnapshot::HasCharacter(const TerraEngine::UUID& objectId) const noexcept {
		return data_ != nullptr && std::binary_search(data_->characterObjects.begin(),
			data_->characterObjects.end(), objectId);
	}

	std::optional<PhysicsAuthoringGeneration> PhysicsAuthoringSnapshot::BuildGeneration(
		const PhysicsAuthoringSnapshot* previous,
		const PhysicsAuthoringSnapshot& current,
		const PhysicsAssetChangeSnapshot& assetChanges) {
		if(!current.IsValid()) return std::nullopt;
		if((assetChanges.toGenerationInclusive != 0U || !assetChanges.changes.empty()
			|| assetChanges.requiresFullRefresh) && !assetChanges.IsValid()) {
			return std::nullopt;
		}

		const PhysicsAuthoringSnapshot* retainedPrevious = previous;
		if(retainedPrevious != nullptr
			&& retainedPrevious->Data().world != current.Data().world) {
			// Provider membership belongs to one concrete world lifetime. A new world
			// must recreate current descriptors even when authored values are equal.
			retainedPrevious = nullptr;
		}

		PhysicsAuthoringGenerationData generation;
		generation.world = current.Data().world;
		generation.generation = current.Data().snapshotGeneration;
		generation.snapshotGeneration = current.Data().snapshotGeneration;
		generation.sceneGeneration = std::max<std::uint64_t>(1U, current.Data().sceneGeneration);
		// Snapshot callers that do not yet provide a published packet retain a
		// monotonic local revision. PhysicsSystem supplies the authoritative packet
		// revision before it commits the generation to a live world.
		generation.scenePublicationRevision = current.Data().snapshotGeneration;
		generation.previousScenePublicationRevision = retainedPrevious != nullptr
			? retainedPrevious->Data().snapshotGeneration : 0U;
		generation.hierarchyGeneration = current.Data().hierarchyGeneration;
		generation.settingsHash = HashSettings(current.Data().settings);
		generation.assetJournalGeneration = assetChanges.toGenerationInclusive;
		generation.fullAssetRefresh = assetChanges.requiresFullRefresh;

		std::unordered_map<TerraEngine::UUID, const PhysicsBodyDescriptor*> previousBodies;
		std::unordered_map<TerraEngine::UUID, const PhysicsBodyDescriptor*> currentBodies;
		std::unordered_map<TerraEngine::UUID, const PhysicsCharacterDescriptor*> previousCharacters;
		std::unordered_map<TerraEngine::UUID, const PhysicsCharacterDescriptor*> currentCharacters;
		if(retainedPrevious != nullptr) {
			for(const PhysicsBodyDescriptor& body : retainedPrevious->Data().bodies) {
				previousBodies.emplace(body.objectId, &body);
			}
			for(const PhysicsCharacterDescriptor& character : retainedPrevious->Data().characters) {
				previousCharacters.emplace(character.objectId, &character);
			}
		}
		for(const PhysicsBodyDescriptor& body : current.Data().bodies) {
			currentBodies.emplace(body.objectId, &body);
		}
		for(const PhysicsCharacterDescriptor& character : current.Data().characters) {
			currentCharacters.emplace(character.objectId, &character);
		}
		const std::unordered_map<TerraEngine::UUID, std::uint64_t> previousColliderHashes =
			retainedPrevious != nullptr ? BuildColliderHashes(retainedPrevious->Data())
				: std::unordered_map<TerraEngine::UUID, std::uint64_t>{};
		const std::unordered_map<TerraEngine::UUID, std::uint64_t> currentColliderHashes =
			BuildColliderHashes(current.Data());

		std::map<TerraEngine::UUID, PhysicsAuthoringChange> changes;
		auto ensureCurrentChange = [&](const PhysicsBodyDescriptor& body) -> PhysicsAuthoringChange& {
			PhysicsAuthoringChange& change = changes[body.objectId];
			change.objectId = body.objectId;
			change.objectGeneration = body.objectGeneration;
			if(const PhysicsBodyDescriptorIndexRecord* index = current.FindBodyDescriptorIndex(body.objectId)) {
				change.descriptorIndex = index->stableIndex;
			}
			return change;
		};

		for(const PhysicsBodyDescriptor& body : current.Data().bodies) {
			const auto old = previousBodies.find(body.objectId);
			if(old == previousBodies.end()) {
				PhysicsAuthoringChange& change = ensureCurrentChange(body);
				change.reasons |= PhysicsAuthoringChangeReason::Added
					| PhysicsAuthoringChangeReason::Lifecycle
					| PhysicsAuthoringChangeReason::BodyComponent
					| PhysicsAuthoringChangeReason::ColliderComponent;
				continue;
			}

			PhysicsAuthoringChangeReason reasons = PhysicsAuthoringChangeReason::None;
			const PhysicsBodyDescriptor& oldBody = *old->second;
			if(oldBody.objectGeneration != body.objectGeneration) {
				reasons |= PhysicsAuthoringChangeReason::Lifecycle
					| PhysicsAuthoringChangeReason::BodyComponent
					| PhysicsAuthoringChangeReason::ColliderComponent;
			}
			if(oldBody.sourceTransformRevision != body.sourceTransformRevision) {
				reasons |= PhysicsAuthoringChangeReason::Transform;
				if(retainedPrevious != nullptr
					&& retainedPrevious->Data().hierarchyGeneration != current.Data().hierarchyGeneration) {
					reasons |= PhysicsAuthoringChangeReason::HierarchyDependency;
				}
			}
			if(HashBodyProperties(oldBody) != HashBodyProperties(body)) {
				reasons |= PhysicsAuthoringChangeReason::BodyComponent;
			}
			const auto oldColliderHash = previousColliderHashes.find(body.objectId);
			const auto newColliderHash = currentColliderHashes.find(body.objectId);
			if(oldColliderHash == previousColliderHashes.end()
				|| newColliderHash == currentColliderHashes.end()
				|| oldColliderHash->second != newColliderHash->second) {
				reasons |= PhysicsAuthoringChangeReason::ColliderComponent;
			}
			if(reasons != PhysicsAuthoringChangeReason::None) {
				ensureCurrentChange(body).reasons |= reasons;
			}
		}

		if(retainedPrevious != nullptr) {
			for(const PhysicsBodyDescriptor& body : retainedPrevious->Data().bodies) {
				if(currentBodies.contains(body.objectId)) continue;
				changes[body.objectId] = {
					body.objectId,
					body.objectGeneration,
					kInvalidPhysicsDescriptorIndex,
					PhysicsAuthoringChangeReason::Removed | PhysicsAuthoringChangeReason::Lifecycle};
			}
			if(HashSettings(retainedPrevious->Data().settings) != generation.settingsHash) {
				for(const PhysicsBodyDescriptor& body : current.Data().bodies) {
					ensureCurrentChange(body).reasons |= PhysicsAuthoringChangeReason::Settings;
				}
			}
		}

		// Character controllers are an independent provider family. They are not
		// allowed to invent a rigid body merely to enter the change map: doing so
		// would make a valid controller-only owner disappear when its ColliderSet
		// is intentionally disabled. Track their exact lifecycle/configuration
		// changes as a separate closure consumed by PhysicsPrepare.
		std::set<TerraEngine::UUID> changedCharacterObjects;
		for(const auto& [objectId, character] : currentCharacters) {
			const auto previousCharacter = previousCharacters.find(objectId);
			if(previousCharacter == previousCharacters.end()
				|| previousCharacter->second->objectGeneration != character->objectGeneration
				|| previousCharacter->second->componentGeneration != character->componentGeneration
				|| HashCharacterProperties(*previousCharacter->second) != HashCharacterProperties(*character)) {
				changedCharacterObjects.insert(objectId);
			}
		}
		if(retainedPrevious != nullptr) {
			for(const auto& [objectId, character] : previousCharacters) {
				(void)character;
				if(!currentCharacters.contains(objectId)) changedCharacterObjects.insert(objectId);
			}
			if(HashSettings(retainedPrevious->Data().settings) != generation.settingsHash) {
				for(const auto& [objectId, character] : currentCharacters) {
					(void)character;
					changedCharacterObjects.insert(objectId);
				}
			}
		}

		auto addAssetReason = [&](const TerraEngine::UUID& objectId, PhysicsAssetKind kind) {
			const auto body = currentBodies.find(objectId);
			if(body == currentBodies.end()) return;
			ensureCurrentChange(*body->second).reasons |= kind == PhysicsAssetKind::CookedCollider
				? PhysicsAuthoringChangeReason::ColliderAsset
				: PhysicsAuthoringChangeReason::MaterialAsset;
		};
		if(assetChanges.requiresFullRefresh) {
			for(const PhysicsAssetDependencyRange& dependency : current.Data().assetDependencies) {
				for(std::uint32_t index = 0U; index < dependency.objectCount; ++index) {
					addAssetReason(current.Data().assetDependentObjects[dependency.objectBegin + index],
						dependency.assetKind);
				}
			}
		} else {
			for(const PhysicsAssetChangeRecord& assetChange : assetChanges.changes) {
				for(const TerraEngine::UUID& objectId : current.ObjectsDependingOn(
					assetChange.assetId, assetChange.assetKind)) {
					addAssetReason(objectId, assetChange.assetKind);
				}
			}
		}
		for(const PhysicsAssetChangeRecord& assetChange : assetChanges.changes) {
			generation.assetObservations.push_back({
				assetChange.assetId,
				assetChange.assetKind,
				assetChange.generation});
		}

		generation.changes.reserve(changes.size());
		for(auto& [objectId, change] : changes) {
			(void)objectId;
			generation.changes.push_back(change);
			if(HasAnyReason(change.reasons, PhysicsAuthoringChangeReason::Removed)) {
				generation.removedObjectIds.push_back(change.objectId);
			}
		}

		// Constraint and character descriptors are separate runtime families. Their
		// compact typed closure is built once with the immutable snapshot, avoiding
		// a later all-collection reconciliation scan in PhysicsPrepare.
		for(const auto& [objectId, change] : changes) {
			const PhysicsAuthoringSnapshot& closureSnapshot = HasAnyReason(change.reasons,
				PhysicsAuthoringChangeReason::Removed) && retainedPrevious != nullptr
				? *retainedPrevious : current;
			const std::vector<TerraEngine::UUID> dependentConstraints = closureSnapshot.ConstraintsDependingOn(objectId);
			generation.constraintRecordIds.insert(generation.constraintRecordIds.end(),
				dependentConstraints.begin(), dependentConstraints.end());
			if(closureSnapshot.HasCharacter(objectId)) generation.characterObjectIds.push_back(objectId);
		}
		generation.characterObjectIds.insert(generation.characterObjectIds.end(),
			changedCharacterObjects.begin(), changedCharacterObjects.end());
		// Each endpoint can name the same constraint. Preparation needs the union
		// of affected records, so normalize it before the generation validator checks
		// uniqueness. This also covers paired endpoint retirement from the old snapshot.
		std::sort(generation.constraintRecordIds.begin(), generation.constraintRecordIds.end());
		generation.constraintRecordIds.erase(std::unique(generation.constraintRecordIds.begin(),
			generation.constraintRecordIds.end()), generation.constraintRecordIds.end());
		return PhysicsAuthoringGeneration::Create(std::move(generation));
	}
}
