// ActiveMembership.h - query-visible active state for scene objects.

#pragma once

#include <cstdint>

namespace TerraEngine {

	struct ActiveObjectTag {
	};

	struct ActiveMembershipState {
		bool objectActive = true;
		bool runtimeQueryMember = true;
		std::uint64_t sourceRevision = 0;
		bool serializedActive = true;
		std::uint64_t componentActiveMask = 0;
	};

	constexpr bool IsActiveMember(const ActiveMembershipState& state)
	{
		return state.objectActive && state.runtimeQueryMember;
	}

	constexpr ActiveMembershipState MakeRuntimeActiveMembershipState(bool authoredActive, std::uint64_t sourceRevision, std::uint64_t componentActiveMask = 0)
	{
		// Authored active state remains serialized/editor-visible while the
		// runtime query bit says whether hot systems should include the object
		// in their compact membership snapshot.
		return ActiveMembershipState{
			authoredActive,
			authoredActive,
			sourceRevision,
			authoredActive,
			componentActiveMask
		};
	}

}
