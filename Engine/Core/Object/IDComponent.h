#pragma once

#include "Component.h"
#include "Utilities/UUID.h"
#include <string_view>

struct IDComponent : public Component<IDComponent> {
	static constexpr std::string_view kComponentName = "IDComponent";

	TerraEngine::UUID id = TerraEngine::GenerateUUID();

	static void reflect(rttr::registration::class_<IDComponent>& registration)
	{
		registration.property("id", &IDComponent::id)
			.property_readonly("id", &IDComponent::id);
	}
};
