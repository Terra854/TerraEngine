// AnimationAutomationProvider.h - discoverable animation MCP capability surface.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class AnimationAutomationProvider final : public AutomationProvider
	{
	public:
		explicit AnimationAutomationProvider(AutomationHandlerResolver resolver);

		[[nodiscard]] std::string_view GetProviderId() const override;
		[[nodiscard]] std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		AutomationHandlerResolver resolver_;
	};
}
