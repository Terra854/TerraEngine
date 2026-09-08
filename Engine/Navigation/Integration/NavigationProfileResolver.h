// NavigationProfileResolver.h - immutable authored-profile resolution.
//
// The scene/component bridge owns the relationship between authored UUIDs and
// runtime profile values. Navigation workers receive the resolved value only;
// they never read component storage or editor configuration directly.

#pragma once

#include "Components/Navigation/NavigationAgentComponent.h"
#include "Core/Export.h"
#include "Navigation/NavigationSettings.h"

#include <span>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Navigation {
	struct NavigationProfileRegistration {
		TerraEngine::UUID authoredProfileId = TerraEngine::EMPTY_UUID;
		NavigationProfile profile{};
	};

	enum class NavigationProfileResolutionCode : std::uint8_t {
		ResolvedDefault,
		ResolvedRegistered,
		InvalidAgent,
		UnknownProfile,
		InvalidProfile,
		DuplicateProfile
	};

	struct NavigationProfileResolution {
		bool accepted = false;
		NavigationProfileResolutionCode code = NavigationProfileResolutionCode::InvalidProfile;
		NavigationProfile profile{};
		std::string message;
	};

	// This registry is assembled from copied project configuration at a protected
	// boundary. Empty component UUIDs deliberately resolve to the documented
	// default profile, while a non-empty unknown UUID is an explicit rejection.
	class TERRAENGINE_API NavigationProfileResolver final {
	public:
		[[nodiscard]] static NavigationProfileResolver Create(
			NavigationProfile defaultProfile,
			std::span<const NavigationProfileRegistration> registrations);

		[[nodiscard]] NavigationProfileResolution Resolve(
			const ::NavigationAgentComponent& component) const;
		[[nodiscard]] const NavigationProfile& DefaultProfile() const noexcept;
		[[nodiscard]] const std::vector<NavigationProfileRegistration>& Registrations() const noexcept;
		[[nodiscard]] bool IsConfigurationValid() const noexcept;

	private:
		NavigationProfile defaultProfile_ = NavigationProfile::Defaults();
		std::vector<NavigationProfileRegistration> registrations_;
		bool configurationValid_ = true;
	};

	[[nodiscard]] TERRAENGINE_API std::string_view ToString(
		NavigationProfileResolutionCode code) noexcept;
}
