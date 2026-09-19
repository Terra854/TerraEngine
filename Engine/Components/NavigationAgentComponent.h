// NavigationAgentComponent.h - durable authoring settings for optional agents.
//
// The component stores only values a creator can save and edit. Destinations,
// route handles, progress, receipts, and worker state belong to NavigationAgentSystem.

#pragma once

#include "Core/Object/Component.h"
#include "Navigation/Agents/NavigationAgentTypes.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>

struct NavigationAgentComponent final : public Component<NavigationAgentComponent> {
	static constexpr std::string_view kComponentName = "NavigationAgentComponent";

	// These copied profile values are intentionally provider-neutral. A system
	// builds a NavigationProfile from them at capture, rather than retaining a
	// profile identity or exposing provider-owned configuration in scenes.
	double radiusMetres = 0.4;
	double heightMetres = 1.8;
	double stepHeightMetres = 0.35;
	double maxSlopeDegrees = 45.0;
	double clearanceMarginMetres = 0.01;
	double horizontalResolutionMetres = 0.1;
	double verticalResolutionMetres = 0.05;
	std::uint32_t excludedAreasMask = 0U;
	std::array<double, TerraEngine::Navigation::kNavigationAreaCount> areaPenalties =
		TerraEngine::Navigation::NavigationProfile::Defaults().areaPenalties;
	double arrivalToleranceMetres = 0.10;
	double targetDisplacementThresholdMetres = 0.25;
	double minimumReplanIntervalSeconds = 0.50;

	[[nodiscard]] TerraEngine::Navigation::NavigationProfile ToNavigationProfile() const noexcept {
		TerraEngine::Navigation::NavigationProfile profile{};
		profile.radiusMetres = radiusMetres;
		profile.heightMetres = heightMetres;
		profile.stepHeightMetres = stepHeightMetres;
		profile.maxSlopeDegrees = maxSlopeDegrees;
		profile.clearanceMarginMetres = clearanceMarginMetres;
		profile.horizontalResolutionMetres = horizontalResolutionMetres;
		profile.verticalResolutionMetres = verticalResolutionMetres;
		profile.excludedAreasMask = excludedAreasMask;
		profile.areaPenalties = areaPenalties;
		return profile;
	}

	[[nodiscard]] TerraEngine::Navigation::NavigationAgentAuthoredSettings ToAuthoredSettings() const noexcept {
		return {ToNavigationProfile(), arrivalToleranceMetres, targetDisplacementThresholdMetres,
			minimumReplanIntervalSeconds};
	}

	// Keep validation pure so loading and transactional staging can reject a
	// complete candidate before it changes any live component value.
	[[nodiscard]] bool ValidateAuthoredValues() const noexcept {
		return ToAuthoredSettings().Validate();
	}

	// The generic component bridge intentionally handles only the common scalar
	// types. Agents need a complete authored snapshot, including doubles and the
	// fixed area-penalty array, while runtime destinations and routes remain absent.
	static nlohmann::json SerializeAuthored(const NavigationAgentComponent& value) {
		return {
			{"active", value.active}, {"radiusMetres", value.radiusMetres}, {"heightMetres", value.heightMetres},
			{"stepHeightMetres", value.stepHeightMetres}, {"maxSlopeDegrees", value.maxSlopeDegrees},
			{"clearanceMarginMetres", value.clearanceMarginMetres}, {"horizontalResolutionMetres", value.horizontalResolutionMetres},
			{"verticalResolutionMetres", value.verticalResolutionMetres}, {"excludedAreasMask", value.excludedAreasMask},
			{"areaPenalties", value.areaPenalties}, {"arrivalToleranceMetres", value.arrivalToleranceMetres},
			{"targetDisplacementThresholdMetres", value.targetDisplacementThresholdMetres},
			{"minimumReplanIntervalSeconds", value.minimumReplanIntervalSeconds}
		};
	}

	static bool DeserializeAuthored(NavigationAgentComponent& destination, const nlohmann::json& json) {
		if(!json.is_object()) return false;
		NavigationAgentComponent staged{};
		if(const auto it = json.find("active"); it != json.end()) {
			if(!it->is_boolean()) return false;
			staged.active = it->get<bool>();
		}
		auto readDouble = [&json](std::string_view key, double& output) {
			const auto it = json.find(std::string(key));
			if(it == json.end()) return true;
			if(!it->is_number()) return false;
			const double value = it->get<double>();
			if(!std::isfinite(value)) return false;
			output = value;
			return true;
		};
		if(!readDouble("radiusMetres", staged.radiusMetres) || !readDouble("heightMetres", staged.heightMetres)
			|| !readDouble("stepHeightMetres", staged.stepHeightMetres) || !readDouble("maxSlopeDegrees", staged.maxSlopeDegrees)
			|| !readDouble("clearanceMarginMetres", staged.clearanceMarginMetres)
			|| !readDouble("horizontalResolutionMetres", staged.horizontalResolutionMetres)
			|| !readDouble("verticalResolutionMetres", staged.verticalResolutionMetres)
			|| !readDouble("arrivalToleranceMetres", staged.arrivalToleranceMetres)
			|| !readDouble("targetDisplacementThresholdMetres", staged.targetDisplacementThresholdMetres)
			|| !readDouble("minimumReplanIntervalSeconds", staged.minimumReplanIntervalSeconds)) return false;
		if(const auto it = json.find("excludedAreasMask"); it != json.end()) {
			// JSON producers may encode a nonnegative mask as signed or unsigned.
			if(!it->is_number_integer()
				|| (!it->is_number_unsigned() && it->get<std::int64_t>() < 0)
				|| it->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) return false;
			staged.excludedAreasMask = it->get<std::uint32_t>();
		}
		if(const auto it = json.find("areaPenalties"); it != json.end()) {
			if(!it->is_array() || it->size() != staged.areaPenalties.size()) return false;
			for(std::size_t index = 0U; index < staged.areaPenalties.size(); ++index) {
				if(!it->at(index).is_number() || !std::isfinite(it->at(index).get<double>())) return false;
				staged.areaPenalties[index] = it->at(index).get<double>();
			}
		}
		if(!staged.ValidateAuthoredValues()) return false;
		destination = staged;
		return true;
	}

	static void reflect(entt::meta_factory<NavigationAgentComponent>& factory) {
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::radiusMetres>(factory, "radiusMetres");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::heightMetres>(factory, "heightMetres");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::stepHeightMetres>(factory, "stepHeightMetres");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::maxSlopeDegrees>(factory, "maxSlopeDegrees");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::clearanceMarginMetres>(factory, "clearanceMarginMetres");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::horizontalResolutionMetres>(factory, "horizontalResolutionMetres");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::verticalResolutionMetres>(factory, "verticalResolutionMetres");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::excludedAreasMask>(factory, "excludedAreasMask");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::areaPenalties>(factory, "areaPenalties");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::arrivalToleranceMetres>(factory, "arrivalToleranceMetres");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::targetDisplacementThresholdMetres>(factory, "targetDisplacementThresholdMetres");
		TerraEngine::Reflection::RegisterDataProperty<&NavigationAgentComponent::minimumReplanIntervalSeconds>(factory, "minimumReplanIntervalSeconds");
	}
};
