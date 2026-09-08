// NavigationAutomationProvider.h - provider-neutral ground navigation tools.
#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class NavigationAutomationProvider final : public AutomationProvider
	{
	public:
		explicit NavigationAutomationProvider(AutomationHandlerResolver resolver);
		std::string_view GetProviderId() const override;
		std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		AutomationHandlerResolver resolveHandler;
	};
}
