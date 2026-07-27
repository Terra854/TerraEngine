// AnimationAutomationProvider.cpp - animation descriptors and typed handlers.

#include "AnimationAutomationProvider.h"

#include "../AnimationAutomationSchemas.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	AnimationAutomationProvider::AnimationAutomationProvider(AutomationHandlerResolver resolver)
		: resolver_(std::move(resolver))
	{
	}

	std::string_view AnimationAutomationProvider::GetProviderId() const
	{
		return "animation";
	}

	std::vector<AutomationCapabilityRegistration> AnimationAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> result;
		const std::vector<std::string> names = AnimationAutomationOperationNames();
		result.reserve(names.size());
		for (const std::string& name : names)
		{
			std::optional<AutomationCapabilityDescriptor> descriptor = DescribeAnimationAutomationOperation(name);
			if (!descriptor)
			{
				continue;
			}
			const bool capture = name == "animation.camera.capture";
			AutomationCapabilityRegistration registration{
				std::move(*descriptor),
				resolver_(name),
				{}
			};
			if (capture)
			{
				registration.descriptor.availability.requiresRenderer = true;
				registration.availabilityPredicate = [](const AutomationStateSnapshot& state) {
					AutomationAvailability availability;
					availability.requiresProject = true;
					availability.requiresRenderer = true;
					availability.available = state.projectAvailable && state.rendererAvailable;
					if (!availability.available)
					{
						availability.reason = "Animation capture requires an available renderer and project.";
					}
					return availability;
				};
			}
			result.push_back(std::move(registration));
		}
		return result;
	}
}
