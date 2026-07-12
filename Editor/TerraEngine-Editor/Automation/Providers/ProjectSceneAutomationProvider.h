// ProjectSceneAutomationProvider.h - Scene graph automation capability ownership.
//
// This provider gives the typed registry complete metadata for the established
// scene, object, and component MCP tools. Each descriptor receives its bound
// handler during registration, preserving established names and results without
// routing requests through a shared name dispatcher.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class ProjectSceneAutomationProvider final : public AutomationProvider
	{
	public:
		explicit ProjectSceneAutomationProvider(AutomationHandlerResolver resolver);

		[[nodiscard]] std::string_view GetProviderId() const override;
		[[nodiscard]] std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		AutomationHandlerResolver resolveHandler;
	};
}
