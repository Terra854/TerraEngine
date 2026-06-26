// ActiveMembership.h - query-visible active state for scene objects.

#pragma once

#include <cstdint>

namespace TerraEngine {

	struct ActiveObjectTag {
	};

	struct ActiveMembershipState {
		bool objectActive = true;
		std::uint64_t sourceRevision = 0;
		bool serializedActive = true;
	};

	inline bool IsActiveMember(const ActiveMembershipState& state)
	{
		return state.objectActive;
	}

}
