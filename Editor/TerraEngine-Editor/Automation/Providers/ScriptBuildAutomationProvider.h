// ScriptBuildAutomationProvider.h - Export and build MCP capability ownership.
//
// This provider gives the script/build domain ownership of the existing export
// tool names while resolved typed handlers preserve established responses.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class ScriptBuildAutomationProvider final : public AutomationProvider
	{
	public:
		explicit ScriptBuildAutomationProvider(AutomationHandlerResolver resolver);

		[[nodiscard]] std::string_view GetProviderId() const override;
		[[nodiscard]] std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		// The resolver is consumed only while descriptors are built. The registry
		// then owns each copied handler for in-flight provider removal safety.
		AutomationHandlerResolver resolveHandler;
	};
}
