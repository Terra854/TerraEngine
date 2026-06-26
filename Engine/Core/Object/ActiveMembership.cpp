// ActiveMembership.cpp - intentionally small helpers for active query state.

#include "ActiveMembership.h"

namespace TerraEngine {
	static_assert(sizeof(ActiveObjectTag) == 1, "ActiveObjectTag should remain an empty EnTT tag.");
}
