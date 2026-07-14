// ColliderSetComponent.cpp - primitive validation and stable record operations.

#include "ColliderSetComponent.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace TerraEngine::Physics
{
	namespace
	{
		constexpr float kMaximumPrimitiveExtent = 1000.0F;

		ColliderSetValidationResult Reject(ColliderSetValidationCode code, std::size_t index, std::string message)
		{
			return {false, code, index, std::move(message)};
		}

		bool IsEmpty(const TerraEngine::UUID& id)
		{
			return id == TerraEngine::EMPTY_UUID;
		}

		bool IsPositiveExtent(float value)
		{
			return std::isfinite(value) && value > 0.0F && value <= kMaximumPrimitiveExtent;
		}

		bool HasValidDimensions(const PrimitiveColliderShape& shape)
		{
			return std::visit([](const auto& payload) {
				using Payload = std::decay_t<decltype(payload)>;
				if constexpr(std::is_same_v<Payload, BoxColliderShape>)
					return payload.halfExtents.IsFinite()
						&& IsPositiveExtent(payload.halfExtents.x)
						&& IsPositiveExtent(payload.halfExtents.y)
						&& IsPositiveExtent(payload.halfExtents.z);
				else if constexpr(std::is_same_v<Payload, SphereColliderShape>)
					return IsPositiveExtent(payload.radius);
				else if constexpr(std::is_same_v<Payload, CapsuleColliderShape>)
					return IsPositiveExtent(payload.radius) && IsPositiveExtent(payload.height)
						&& payload.height >= payload.radius * 2.0F;
				else if constexpr(std::is_same_v<Payload, CylinderColliderShape>)
					return IsPositiveExtent(payload.radius) && IsPositiveExtent(payload.height);
				else if constexpr(std::is_same_v<Payload, TaperedCapsuleColliderShape>)
					return IsPositiveExtent(payload.topRadius) && IsPositiveExtent(payload.bottomRadius)
						&& IsPositiveExtent(payload.height)
						&& payload.height >= 2.0F * (std::max)(payload.topRadius, payload.bottomRadius);
				else
					return payload.colliderAssetId != TerraEngine::EMPTY_UUID;
			}, shape);
		}

		bool HasValidRotation(const PhysicsQuaternion& rotation)
		{
			if(!rotation.IsFinite())
				return false;
			const float magnitudeSquared = rotation.x * rotation.x + rotation.y * rotation.y
				+ rotation.z * rotation.z + rotation.w * rotation.w;
			return std::isfinite(magnitudeSquared) && magnitudeSquared > 0.000001F;
		}
	}

	bool PhysicsColliderDebugColor::IsFiniteNormalized() const noexcept
	{
		return std::isfinite(red) && std::isfinite(green) && std::isfinite(blue) && std::isfinite(alpha)
			&& red >= 0.0F && red <= 1.0F && green >= 0.0F && green <= 1.0F
			&& blue >= 0.0F && blue <= 1.0F && alpha >= 0.0F && alpha <= 1.0F;
	}

	PhysicsShapeKind ColliderRecord::Kind() const noexcept
	{
		return std::visit([](const auto& payload) -> PhysicsShapeKind {
			using Payload = std::decay_t<decltype(payload)>;
			if constexpr(std::is_same_v<Payload, BoxColliderShape>) return PhysicsShapeKind::Box;
			else if constexpr(std::is_same_v<Payload, SphereColliderShape>) return PhysicsShapeKind::Sphere;
			else if constexpr(std::is_same_v<Payload, CapsuleColliderShape>) return PhysicsShapeKind::Capsule;
			else if constexpr(std::is_same_v<Payload, CylinderColliderShape>) return PhysicsShapeKind::Cylinder;
			else if constexpr(std::is_same_v<Payload, TaperedCapsuleColliderShape>) return PhysicsShapeKind::TaperedCapsule;
			else if constexpr(std::is_same_v<Payload, ConvexHullColliderShape>) return PhysicsShapeKind::ConvexHull;
			else if constexpr(std::is_same_v<Payload, TriangleMeshColliderShape>) return PhysicsShapeKind::Mesh;
			else return PhysicsShapeKind::Compound;
		}, shape);
	}
}

TerraEngine::Physics::ColliderRecord& ColliderSetComponent::Add(TerraEngine::Physics::ColliderRecord record)
{
	// A caller may intentionally provide an existing ID when reconstructing a
	// serialized record. Empty IDs are never persisted, so assign one on creation.
	if(record.recordId == TerraEngine::EMPTY_UUID)
		record.recordId = TerraEngine::GenerateUUID();
	colliders.push_back(std::move(record));
	return colliders.back();
}

bool ColliderSetComponent::Remove(const TerraEngine::UUID& recordId)
{
	for(auto iterator = colliders.begin(); iterator != colliders.end(); ++iterator)
	{
		if(iterator->recordId == recordId)
		{
			colliders.erase(iterator);
			return true;
		}
	}
	return false;
}

TerraEngine::Physics::ColliderSetValidationResult ColliderSetComponent::Validate() const
{
	using namespace TerraEngine::Physics;
	if(schemaVersion != kColliderSetComponentSchemaVersion)
		return {false, ColliderSetValidationCode::UnsupportedSchema, static_cast<std::size_t>(-1), "Collider set schema version requires an explicit migration."};

	std::unordered_set<TerraEngine::UUID> seenIds;
	for(std::size_t index = 0; index < colliders.size(); ++index)
	{
		const ColliderRecord& record = colliders[index];
		if(record.recordId == TerraEngine::EMPTY_UUID)
			return {false, ColliderSetValidationCode::EmptyRecordId, index, "Collider records require a stable non-empty record ID."};
		if(!seenIds.insert(record.recordId).second)
			return {false, ColliderSetValidationCode::DuplicateRecordId, index, "Collider record IDs must be unique within one collider set."};
		if(record.recordVersion != kColliderRecordSchemaVersion)
			return {false, ColliderSetValidationCode::UnsupportedRecordSchema, index, "Collider record schema version requires an explicit migration."};
		if(!record.localCenter.IsFinite())
			return {false, ColliderSetValidationCode::NonfiniteLocalPose, index, "Collider local center must contain finite metres."};
		if(!HasValidRotation(record.localRotation))
			return {false, ColliderSetValidationCode::InvalidRotation, index, "Collider local rotation must be finite and non-zero."};
		if(!HasValidDimensions(record.shape))
			return {false, ColliderSetValidationCode::InvalidShapeDimensions, index, "Collider dimensions or cooked-asset GUID are invalid."};
		if(record.collisionLayerId >= 32U)
			return {false, ColliderSetValidationCode::InvalidLayer, index, "Collider layer IDs must be between zero and 31."};
		if(record.eventPolicy != PhysicsColliderEventPolicy::None && record.eventPolicy != PhysicsColliderEventPolicy::Collision
			&& record.eventPolicy != PhysicsColliderEventPolicy::Trigger && record.eventPolicy != PhysicsColliderEventPolicy::All)
			return {false, ColliderSetValidationCode::InvalidEventPolicy, index, "Collider event policy is not an engine-owned value."};
		if((record.queryFlags & ~kPhysicsColliderAllQueryFlags) != 0U)
			return {false, ColliderSetValidationCode::InvalidQueryFlags, index, "Collider query flags contain unsupported bits."};
		if(record.hasDebugColorOverride && !record.debugColor.IsFiniteNormalized())
			return {false, ColliderSetValidationCode::InvalidDebugColor, index, "Collider debug color must be finite and normalized to [0, 1]."};
	}

	return {true, ColliderSetValidationCode::None, static_cast<std::size_t>(-1), {}};
}

void ColliderSetComponent::reflect(entt::meta_factory<ColliderSetComponent>& factory)
{
	TerraEngine::Reflection::RegisterDataProperty<&ColliderSetComponent::schemaVersion>(factory, "schemaVersion");
	TerraEngine::Reflection::RegisterDataProperty<&ColliderSetComponent::colliders>(factory, "colliders");
}
