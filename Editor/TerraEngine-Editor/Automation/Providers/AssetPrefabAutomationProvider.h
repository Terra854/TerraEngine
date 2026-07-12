// AssetPrefabAutomationProvider.h - Asset and virtual-geometry MCP tools.
//
// This provider owns discovery metadata for existing asset automation. The
// handler resolver preserves the established result envelopes while the typed
// registry remains the single dispatch and availability authority.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class AssetPrefabAutomationProvider final : public AutomationProvider
	{
	public:
		explicit AssetPrefabAutomationProvider(AutomationHandlerResolver resolver);

		[[nodiscard]] std::string_view GetProviderId() const override;
		[[nodiscard]] std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		AutomationHandlerResolver resolveHandler;
	};
}
