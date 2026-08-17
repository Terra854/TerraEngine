// TerrainAutomationProvider.h -- Discoverable terrain capability surface.
#pragma once
#include "AutomationProvider.h"
namespace TerraEngine::Editor::Automation
{
	class TerrainAutomationProvider final : public AutomationProvider
	{
	public:
		explicit TerrainAutomationProvider(AutomationHandlerResolver resolver);
		std::string_view GetProviderId() const override;
		std::vector<AutomationCapabilityRegistration> GetCapabilities() override;
	private: AutomationHandlerResolver resolveHandler;
	};
}
