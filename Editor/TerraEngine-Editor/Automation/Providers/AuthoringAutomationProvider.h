// AuthoringAutomationProvider.h - Advanced editor authoring declarations.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class AuthoringAutomationProvider final : public AutomationProvider
	{
	public:
		explicit AuthoringAutomationProvider(AutomationHandlerResolver resolver);
		std::string_view GetProviderId() const override;
		std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		AutomationHandlerResolver resolveHandler;
	};
}
