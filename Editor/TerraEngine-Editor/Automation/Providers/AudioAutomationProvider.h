// AudioAutomationProvider.h - discoverable typed audio control adapters.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation {
	class AudioAutomationProvider final : public AutomationProvider {
	public:
		explicit AudioAutomationProvider(AutomationHandlerResolver resolver);
		std::string_view GetProviderId() const override;
		std::vector<AutomationCapabilityRegistration> GetCapabilities() override;
	private:
		AutomationHandlerResolver resolveHandler;
	};
}
