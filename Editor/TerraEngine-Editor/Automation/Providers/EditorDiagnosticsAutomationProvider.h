// EditorDiagnosticsAutomationProvider.h - Diagnostics and capture MCP tools.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class EditorDiagnosticsAutomationProvider final : public AutomationProvider
	{
	public:
		explicit EditorDiagnosticsAutomationProvider(AutomationHandlerResolver resolver);

		[[nodiscard]] std::string_view GetProviderId() const override;
		[[nodiscard]] std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		AutomationHandlerResolver resolveHandler;
	};
}
