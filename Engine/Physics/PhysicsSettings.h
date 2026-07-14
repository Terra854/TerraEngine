// PhysicsSettings.h - versioned, provider-neutral project physics policy.
//
// Values here are authoring and runtime limits, not Jolt configuration structs.
// The same settings are serialized by the editor and packaged into exports so
// editor Play Mode and shipped games use identical bounds.

#pragma once

#include "Core/Export.h"
#include "PhysicsTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace TerraEngine::Physics {
	inline constexpr std::uint32_t kPhysicsSettingsSchemaVersion = 1U;
	inline constexpr std::size_t kPhysicsCollisionLayerCount = 32U;
	inline constexpr float kPhysicsMaximumCastDistance = 100000.0F;
	inline constexpr float kPhysicsMaximumShapeExtent = 1000.0F;
	inline constexpr std::uint32_t kPhysicsMaximumHitsPerQuery = 64U;
	inline constexpr std::uint32_t kPhysicsMaximumRequestsPerBatch = 10000U;
	inline constexpr std::uint32_t kPhysicsRetainedResultTicks = 2U;
	inline constexpr std::uint32_t kPhysicsMaximumDebugHistoryTicks = 32U;
	inline constexpr std::uint32_t kPhysicsMaximumDebugPrimitives = 100000U;

	enum class PhysicsSettingsValidationCode : std::uint8_t {
		None,
		UnsupportedSchema,
		NonfiniteGravity,
		InvalidFixedTimeStep,
		InvalidSubstepLimit,
		InvalidAccumulatorLimit,
		InvalidLayerId,
		EmptyLayerName,
		DuplicateLayerName,
		DuplicateLayerId,
		InvalidBroadPhaseGroup,
		InvalidQueryLimit,
		InvalidHistoryLimit,
		InvalidDebugLimit
	};

	struct PhysicsSettingsValidationResult {
		bool accepted = false;
		PhysicsSettingsValidationCode code = PhysicsSettingsValidationCode::None;
		std::string message;
		std::size_t layerIndex = kPhysicsCollisionLayerCount;
	};

	struct CollisionLayer {
		std::uint8_t id = 0;
		std::string name;
		std::uint32_t collisionMask = 0xFFFFFFFFU;
		std::uint8_t broadPhaseGroup = 0;
		bool enabled = true;
	};

	struct PhysicsSettings {
		std::uint32_t schemaVersion = kPhysicsSettingsSchemaVersion;
		PhysicsVector3 gravity{0.0F, -9.81F, 0.0F};
		float fixedTimeStep = 1.0F / 60.0F;
		std::uint32_t maxSubsteps = 4U;
		float maxAccumulatedTime = 0.25F;
		std::uint32_t velocityIterations = 10U;
		std::uint32_t positionIterations = 2U;
		std::array<CollisionLayer, kPhysicsCollisionLayerCount> layers{};
		float maxCastDistance = kPhysicsMaximumCastDistance;
		float maxShapeExtent = kPhysicsMaximumShapeExtent;
		std::uint32_t maxHitsPerQuery = kPhysicsMaximumHitsPerQuery;
		std::uint32_t maxRequestsPerBatch = kPhysicsMaximumRequestsPerBatch;
		std::uint32_t retainedResultTicks = kPhysicsRetainedResultTicks;
		// Debug geometry is opt-in so a closed diagnostics window never creates
		// provider queries or per-frame allocations. The bounds apply equally in
		// editor Play Mode and exported diagnostic builds.
		bool debugVisualizationEnabled = false;
		std::uint32_t retainedDebugTicks = 2U;
		std::uint32_t maxDebugPrimitives = kPhysicsMaximumDebugPrimitives;

		static PhysicsSettings Defaults();
		static constexpr bool IsValidLayerMask(std::uint64_t rawMask) noexcept {
			return rawMask <= 0xFFFFFFFFULL;
		}

		PhysicsSettingsValidationResult Validate() const;
	};

	TERRAENGINE_API std::string_view ToString(PhysicsSettingsValidationCode code);
}
