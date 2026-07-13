// PhysicsAutomationProvider.h - Physics-specific editor automation contracts.
//
// Semantic physics operations need generation, approval, and fixed-step
// boundaries that generic reflection editing cannot describe safely.  This
// provider owns only discoverable contracts and bound handlers; the editor
// service owns the live PhysicsSystem and commits the typed work it receives.

#pragma once

#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation
{
	class PhysicsAutomationProvider final : public AutomationProvider
	{
	public:
		explicit PhysicsAutomationProvider(AutomationHandlerResolver resolver);

		[[nodiscard]] std::string_view GetProviderId() const override;
		[[nodiscard]] std::vector<AutomationCapabilityRegistration> GetCapabilities() override;

	private:
		AutomationHandlerResolver resolveHandler;
	};
}
