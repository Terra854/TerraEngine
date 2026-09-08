// NavigationProfileResolver.cpp - copied runtime-profile configuration.

#include "Navigation/Integration/NavigationProfileResolver.h"

#include <algorithm>
#include <utility>

namespace TerraEngine::Navigation {
	NavigationProfileResolver NavigationProfileResolver::Create(
		NavigationProfile defaultProfile,
		std::span<const NavigationProfileRegistration> registrations) {
		NavigationProfileResolver result{};
		result.defaultProfile_ = std::move(defaultProfile);
		result.configurationValid_ = result.defaultProfile_.Validate().Succeeded();
		result.registrations_.assign(registrations.begin(), registrations.end());

		// Stable UUID ordering makes a copied registry deterministic and lets the
		// binary-search resolve path avoid retaining mutable editor containers.
		std::sort(result.registrations_.begin(), result.registrations_.end(),
			[](const NavigationProfileRegistration& left, const NavigationProfileRegistration& right) {
				return left.authoredProfileId < right.authoredProfileId;
			});
		for(std::size_t index = 0U; index < result.registrations_.size(); ++index) {
			const NavigationProfileRegistration& entry = result.registrations_[index];
			if(entry.authoredProfileId == TerraEngine::EMPTY_UUID
				|| !entry.profile.Validate().Succeeded()
				|| (index != 0U && result.registrations_[index - 1U].authoredProfileId == entry.authoredProfileId)) {
				result.configurationValid_ = false;
			}
		}
		return result;
	}

	NavigationProfileResolution NavigationProfileResolver::Resolve(
		const ::NavigationAgentComponent& component) const {
		const NavigationAgentValidationResult agentValidation = component.Validate();
		if(!agentValidation.accepted) {
			return {false, NavigationProfileResolutionCode::InvalidAgent, {}, agentValidation.message};
		}
		if(!configurationValid_) {
			return {false, NavigationProfileResolutionCode::InvalidProfile, {},
				"Navigation profile configuration contains an invalid or duplicate registration."};
		}
		if(component.profileId == TerraEngine::EMPTY_UUID) {
			return {true, NavigationProfileResolutionCode::ResolvedDefault, defaultProfile_,
				"Agent has no authored profile UUID; using the project default ground profile."};
		}

		const auto found = std::lower_bound(registrations_.begin(), registrations_.end(), component.profileId,
			[](const NavigationProfileRegistration& registration, const TerraEngine::UUID& profileId) {
				return registration.authoredProfileId < profileId;
			});
		if(found == registrations_.end() || found->authoredProfileId != component.profileId) {
			return {false, NavigationProfileResolutionCode::UnknownProfile, {},
				"NavigationAgentComponent references an unknown authored navigation profile."};
		}
		if(!found->profile.Validate().Succeeded()) {
			return {false, NavigationProfileResolutionCode::InvalidProfile, {},
				"NavigationAgentComponent references an invalid authored navigation profile."};
		}
		return {true, NavigationProfileResolutionCode::ResolvedRegistered, found->profile, {}};
	}

	const NavigationProfile& NavigationProfileResolver::DefaultProfile() const noexcept { return defaultProfile_; }
	const std::vector<NavigationProfileRegistration>& NavigationProfileResolver::Registrations() const noexcept { return registrations_; }
	bool NavigationProfileResolver::IsConfigurationValid() const noexcept { return configurationValid_; }

	std::string_view ToString(NavigationProfileResolutionCode code) noexcept {
		switch(code) {
		case NavigationProfileResolutionCode::ResolvedDefault: return "ResolvedDefault";
		case NavigationProfileResolutionCode::ResolvedRegistered: return "ResolvedRegistered";
		case NavigationProfileResolutionCode::InvalidAgent: return "InvalidAgent";
		case NavigationProfileResolutionCode::UnknownProfile: return "UnknownProfile";
		case NavigationProfileResolutionCode::InvalidProfile: return "InvalidProfile";
		case NavigationProfileResolutionCode::DuplicateProfile: return "DuplicateProfile";
		}
		return "InvalidProfile";
	}
}
