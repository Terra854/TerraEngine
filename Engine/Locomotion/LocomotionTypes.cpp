// LocomotionTypes.cpp - dedicated translation unit for copied runtime contracts.

#include "LocomotionTypes.h"

namespace TerraEngine::Locomotion {
	static_assert(kMaximumCommandBatch <= kMaximumQueuedCommands);
	static_assert(kMaximumCommandsPerActor <= kMaximumQueuedCommands);
	static_assert(kMaximumLiveSlots < kInvalidLocomotionSlot);
}
