#include "NavigationStores.h"

namespace TerraEngine::Navigation {
	std::string_view ToString(NavigationCommandState state) noexcept {
		switch(state) {
		case NavigationCommandState::Queued: return "Queued";
		case NavigationCommandState::Applied: return "Applied";
		case NavigationCommandState::Rejected: return "Rejected";
		case NavigationCommandState::Expired: return "Expired";
		}
		return "UnknownNavigationCommandState";
	}
}
