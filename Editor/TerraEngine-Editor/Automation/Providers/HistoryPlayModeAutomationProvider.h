// HistoryPlayModeAutomationProvider.h - Transaction, history, and play tools.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class HistoryPlayModeAutomationProvider final : public AutomationProvider
	{
	public:
		explicit HistoryPlayModeAutomationProvider(AutomationHandlerResolver resolver);
		std::string_view GetProviderId() const override;
		std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		AutomationHandlerResolver resolveHandler;
	};
}
