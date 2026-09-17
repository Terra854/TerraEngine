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
				return left.registeredProfileId < right.registeredProfileId;
			});
		for(std::size_t index = 0U; index < result.registrations_.size(); ++index) {
			const NavigationProfileRegistration& entry = result.registrations_[index];
			if(entry.registeredProfileId == TerraEngine::EMPTY_UUID
				|| !entry.profile.Validate().Succeeded()
				|| (index != 0U && result.registrations_[index - 1U].registeredProfileId == entry.registeredProfileId)) {
				result.configurationValid_ = false;
			}
		}
		return result;
	}

	NavigationProfileResolution NavigationProfileResolver::ResolveExplicit(NavigationProfile profile) const {
		if(!configurationValid_) {
			return {false, NavigationProfileResolutionCode::InvalidProfile, {},
				"Navigation profile configuration contains an invalid or duplicate registration."};
		}
		if(!profile.Validate().Succeeded()) {
			return {false, NavigationProfileResolutionCode::InvalidRequest, {},
				"Navigation route request supplies an invalid explicit profile."};
		}
		return {true, NavigationProfileResolutionCode::ResolvedExplicit, std::move(profile), {}};
	}

	NavigationProfileResolution NavigationProfileResolver::ResolveRegistered(
		const TerraEngine::UUID& registeredProfileId) const {
		if(!configurationValid_) {
			return {false, NavigationProfileResolutionCode::InvalidProfile, {},
				"Navigation profile configuration contains an invalid or duplicate registration."};
		}
		if(registeredProfileId == TerraEngine::EMPTY_UUID) {
			return {false, NavigationProfileResolutionCode::InvalidRequest, {},
				"Navigation route request must name a non-empty registered profile ID."};
		}
		const auto found = std::lower_bound(registrations_.begin(), registrations_.end(), registeredProfileId,
			[](const NavigationProfileRegistration& registration, const TerraEngine::UUID& profileId) {
				return registration.registeredProfileId < profileId;
			});
		if(found == registrations_.end() || found->registeredProfileId != registeredProfileId) {
			return {false, NavigationProfileResolutionCode::UnknownProfile, {},
				"Navigation route request references an unknown registered profile."};
		}
		if(!found->profile.Validate().Succeeded()) {
			return {false, NavigationProfileResolutionCode::InvalidProfile, {},
				"Navigation route request references an invalid registered profile."};
		}
		return {true, NavigationProfileResolutionCode::ResolvedRegistered, found->profile, {}};
	}

	const NavigationProfile& NavigationProfileResolver::DefaultProfile() const noexcept { return defaultProfile_; }
	const std::vector<NavigationProfileRegistration>& NavigationProfileResolver::Registrations() const noexcept { return registrations_; }
	bool NavigationProfileResolver::IsConfigurationValid() const noexcept { return configurationValid_; }

	std::string_view ToString(NavigationProfileResolutionCode code) noexcept {
		switch(code) {
		case NavigationProfileResolutionCode::ResolvedExplicit: return "ResolvedExplicit";
		case NavigationProfileResolutionCode::ResolvedRegistered: return "ResolvedRegistered";
		case NavigationProfileResolutionCode::InvalidRequest: return "InvalidRequest";
		case NavigationProfileResolutionCode::UnknownProfile: return "UnknownProfile";
		case NavigationProfileResolutionCode::InvalidProfile: return "InvalidProfile";
		case NavigationProfileResolutionCode::DuplicateProfile: return "DuplicateProfile";
		}
		return "InvalidProfile";
	}
}
