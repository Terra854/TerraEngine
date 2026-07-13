// ColliderSetComponent.h - ordered primitive collider authoring records.
//
// EnTT stores one component of each concrete type per object. This collection
// therefore owns stable child records so an object can author two boxes, three
// spheres, or any other repeated primitive without hidden child objects.

#pragma once

#include "Core/Object/Component.h"
#include "Physics/PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace TerraEngine::Physics
{
	inline constexpr std::uint32_t kColliderSetComponentSchemaVersion = 1U;
	inline constexpr std::uint32_t kColliderRecordSchemaVersion = 1U;

	// All primitive dimensions use metres. Capsules and cylinders use total
	// height, including both rounded capsule ends, for one consistent UI rule.
	struct BoxColliderShape { PhysicsVector3 halfExtents{0.5F, 0.5F, 0.5F}; };
	struct SphereColliderShape { float radius = 0.5F; };
	struct CapsuleColliderShape { float radius = 0.5F; float height = 2.0F; };
	struct CylinderColliderShape { float radius = 0.5F; float height = 1.0F; };
	struct TaperedCapsuleColliderShape { float topRadius = 0.5F; float bottomRadius = 0.5F; float height = 2.0F; };
	// Rich geometry is referenced by the cooked asset GUID. Source-model paths
	// never enter scene data, so moving or renaming the asset preserves identity.
	struct ConvexHullColliderShape { TerraEngine::UUID colliderAssetId = TerraEngine::EMPTY_UUID; };
	struct TriangleMeshColliderShape { TerraEngine::UUID colliderAssetId = TerraEngine::EMPTY_UUID; };
	struct CompoundColliderShape { TerraEngine::UUID colliderAssetId = TerraEngine::EMPTY_UUID; };

	using PrimitiveColliderShape = std::variant<
		BoxColliderShape,
		SphereColliderShape,
		CapsuleColliderShape,
		CylinderColliderShape,
		TaperedCapsuleColliderShape,
		ConvexHullColliderShape,
		TriangleMeshColliderShape,
		CompoundColliderShape>;

	enum class PhysicsColliderEventPolicy : std::uint8_t
	{
		None,
		Collision,
		Trigger,
		All
	};

	// The flags state which query families may see this record. Keeping them
	// engine-owned prevents Jolt filter bits from entering authored scene data.
	enum PhysicsColliderQueryFlag : std::uint32_t
	{
		PhysicsColliderQueryRay = 1U << 0U,
		PhysicsColliderQueryShapeCast = 1U << 1U,
		PhysicsColliderQueryOverlap = 1U << 2U,
		PhysicsColliderQueryPoint = 1U << 3U,
		kPhysicsColliderAllQueryFlags = PhysicsColliderQueryRay | PhysicsColliderQueryShapeCast
			| PhysicsColliderQueryOverlap | PhysicsColliderQueryPoint
	};

	struct PhysicsColliderDebugColor
	{
		float red = 1.0F;
		float green = 1.0F;
		float blue = 1.0F;
		float alpha = 1.0F;

		[[nodiscard]] bool IsFiniteNormalized() const noexcept;
	};

	// recordId remains unchanged through routine edits and reordering. Duplication
	// intentionally generates a fresh ID so prefab overrides remain unambiguous.
	struct ColliderRecord
	{
		TerraEngine::UUID recordId = TerraEngine::GenerateUUID();
		std::uint32_t recordVersion = kColliderRecordSchemaVersion;
		PhysicsVector3 localCenter{};
		PhysicsQuaternion localRotation{};
		PrimitiveColliderShape shape = BoxColliderShape{};
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		bool useDefaultMaterial = true;
		bool isTrigger = false;
		bool useLayerOverride = false;
		std::uint8_t collisionLayerId = 0U;
		PhysicsColliderEventPolicy eventPolicy = PhysicsColliderEventPolicy::All;
		std::uint32_t queryFlags = kPhysicsColliderAllQueryFlags;
		bool enabled = true;
		bool hasDebugColorOverride = false;
		PhysicsColliderDebugColor debugColor{};

		[[nodiscard]] PhysicsShapeKind Kind() const noexcept;
	};

	enum class ColliderSetValidationCode : std::uint8_t
	{
		None,
		UnsupportedSchema,
		EmptyRecordId,
		DuplicateRecordId,
		UnsupportedRecordSchema,
		NonfiniteLocalPose,
		InvalidRotation,
		InvalidShapeDimensions,
		InvalidLayer,
		InvalidEventPolicy,
		InvalidQueryFlags,
		InvalidDebugColor
	};

	struct ColliderSetValidationResult
	{
		bool accepted = false;
		ColliderSetValidationCode code = ColliderSetValidationCode::None;
		std::size_t recordIndex = static_cast<std::size_t>(-1);
		std::string message;
	};
}

struct ColliderSetComponent final : public Component<ColliderSetComponent>
{
	static constexpr std::string_view kComponentName = "ColliderSetComponent";

	std::uint32_t schemaVersion = TerraEngine::Physics::kColliderSetComponentSchemaVersion;
	std::vector<TerraEngine::Physics::ColliderRecord> colliders;

	// Appending preserves authored order, which is also the deterministic tie
	// breaker for identical primitive types during snapshot construction.
	TerraEngine::Physics::ColliderRecord& Add(TerraEngine::Physics::ColliderRecord record = {});
	[[nodiscard]] bool Remove(const TerraEngine::UUID& recordId);
	[[nodiscard]] TerraEngine::Physics::ColliderSetValidationResult Validate() const;

	// Collection serialization is deliberately handled by the physics serializer,
	// while reflection exposes the component schema and collection identity to tools.
	static void reflect(entt::meta_factory<ColliderSetComponent>& factory);
};
