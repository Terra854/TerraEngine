// NavigationProfileResolver.h - immutable caller/profile configuration resolution.
//
// The project-configuration boundary owns the relationship between registered
// UUIDs and runtime profile values. Navigation workers receive the resolved
// value only; they never read scene-object storage or editor configuration.

#pragma once

#include "Core/Export.h"
#include "Navigation/NavigationSettings.h"

#include <span>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Navigation {
	struct NavigationProfileRegistration {
		// This ID names a project-level preset. It is configuration identity, not
		// state attached to a scene object or a navigation component.
		TerraEngine::UUID registeredProfileId = TerraEngine::EMPTY_UUID;
		NavigationProfile profile{};
	};

	enum class NavigationProfileResolutionCode : std::uint8_t {
		ResolvedExplicit,
		ResolvedRegistered,
		InvalidRequest,
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
	// boundary. A request must supply either a complete profile value or one
	// registered-profile ID; it never inherits a value from a scene object.
	class TERRAENGINE_API NavigationProfileResolver final {
	public:
		[[nodiscard]] static NavigationProfileResolver Create(
			NavigationProfile defaultProfile,
			std::span<const NavigationProfileRegistration> registrations);

		// ResolveExplicit copies the supplied value into its result after checking
		// all limits. Callers can safely retain that result after their own input
		// object or script frame has changed.
		[[nodiscard]] NavigationProfileResolution ResolveExplicit(
			NavigationProfile profile) const;
		// ResolveRegistered copies a project preset selected by a non-empty ID.
		// Unknown and malformed IDs fail closed rather than choosing a default.
		[[nodiscard]] NavigationProfileResolution ResolveRegistered(
			const TerraEngine::UUID& registeredProfileId) const;
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
