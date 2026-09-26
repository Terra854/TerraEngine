// LocomotionParameterTrace.h - copied optional parameter ingress for replay.
#pragma once
#include "Animation/Runtime/AnimationCommandQueue.h"
#include "Physics/PhysicsTypes.h"
#include <array>
#include <vector>

namespace TerraEngine::Locomotion {
	struct LocomotionParameterTraceRecord {
		std::uint64_t sequence = 0U;
		std::uint64_t updateEpoch = 0U;
		Physics::PhysicsTick precedingTick = 0U;
		UUID animatorObjectId = EMPTY_UUID;
		std::uint32_t count = 0U;
		std::array<Animation::AnimationCommand, 5U> commands;
	};
	struct LocomotionParameterTraceSnapshot {
		bool enabled = false;
		bool lostRecords = false;
		std::uint64_t latestSequence = 0U;
		std::size_t reservedBytes = 0U;
		std::vector<LocomotionParameterTraceRecord> records;
	};
}
