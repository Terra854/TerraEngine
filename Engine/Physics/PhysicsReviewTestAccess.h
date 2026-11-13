// Internal fixture ingress; production callers use protected frame migration.
#pragma once
#include "Core/Export.h"
#include <cstddef>
#include <optional>
#include "PhysicsTypes.h"
#include "Utilities/UUID.h"

namespace TerraEngine::Physics
{
	class PhysicsSystem;
	// Read-only primary-world ownership at an already protected lifecycle point.
	// Counts are copied; no provider pointer or mutation permission escapes.
	struct TerrainShapeOwnershipSnapshot
	{
		PhysicsWorldHandle world;
		std::size_t typedLiveShapes = 0u, typedShapeCapacity = 0u, typedLiveBodies = 0u;
		std::size_t providerShapeRecords = 0u, providerBodyRecords = 0u;
		std::size_t stagedShapeRecords = 0u, stagedBodyRecords = 0u;
		std::size_t providerStrongCacheEntries = 0u, activeTerrainPublications = 0u;
	};
	struct PhysicsReviewTestAccess
	{
		// Fixture-only copied clock observation; no provider/world pointer escapes.
		struct FrameClock {
			PhysicsTick issued = 0U, completed = 0U;
			float accumulator = 0.0F, clamped = 0.0F, dropped = 0.0F;
			std::uint32_t pendingExplicit = 0U;
		};
		struct BodyOwnership {
			std::size_t runtimeBodies = 0, mappings = 0, typedBodies = 0, typedShapes = 0;
			std::size_t providerBodies = 0, providerShapes = 0;
		};
		// Copy all owner layers so retirement tests cannot pass merely because a
		// query filters a stale object while its native collision body still exists.
		static TERRAENGINE_API std::optional<BodyOwnership> ReadFrameBodyOwnership(PhysicsSystem& system, PhysicsFrameHandle frame);
		static TERRAENGINE_API std::optional<FrameClock> ReadFrameClock(PhysicsSystem& system, PhysicsFrameHandle frame);
		// One-shot controlled faults at the real secondary admission/step boundary.
		static TERRAENGINE_API bool ArmSecondaryFailure(PhysicsSystem& system, PhysicsFrameHandle frame, bool admission, std::uint32_t stepOrdinal = 0U);
		static TERRAENGINE_API std::optional<std::uint32_t> ReadSecondaryStepAttempts(PhysicsSystem& system, PhysicsFrameHandle frame);
		static TERRAENGINE_API bool SetFramePaused(PhysicsSystem& system, PhysicsFrameHandle frame, bool paused);
		static TERRAENGINE_API bool RequestFrameExplicitStep(PhysicsSystem& system, PhysicsFrameHandle frame);
		static TERRAENGINE_API std::optional<TerrainShapeOwnershipSnapshot> ReadTerrainShapeOwnership(PhysicsSystem& system);
		// Only fixture-owned, not-yet-captured bodies may be assigned. The real
		// snapshot/prepare/commit path must still construct every provider record.
		static TERRAENGINE_API bool AssignFixtureFrame(PhysicsSystem& system,
			const TerraEngine::UUID& object, PhysicsFrameHandle frame);
	};
}
