// PhysicsAutomationProvider.h - Physics-specific editor automation contracts.
//
// Semantic physics operations need generation, approval, and fixed-step
// boundaries that generic reflection editing cannot describe safely.  This
// provider owns only discoverable contracts and bound handlers; the editor
// service owns the live PhysicsSystem and commits the typed work it receives.

#pragma once

#include "AutomationProvider.h"
#include "Physics/PhysicsCommands.h"

#include <optional>

namespace TerraEngine::Editor::Automation
{
	class PhysicsAutomationProvider final : public AutomationProvider
	{
	public:
		explicit PhysicsAutomationProvider(AutomationHandlerResolver resolver);

		[[nodiscard]] std::string_view GetProviderId() const override;
		[[nodiscard]] std::vector<AutomationCapabilityRegistration> GetCapabilities() override;
		// This small typed factory keeps the public application-mode spelling and
		// the engine command kind inseparable.  The service still owns validation
		// and queue admission, while tests can prove that automation never turns an
		// acceleration request into an ordinary force by accident.
		[[nodiscard]] static std::optional<TerraEngine::Physics::PhysicsCommand> TryCreateApplyForceCommand(
			std::string_view applicationMode,
			const TerraEngine::Physics::PhysicsBodyCommandTarget& target,
			TerraEngine::Physics::PhysicsVector3 value,
			TerraEngine::Physics::PhysicsCommandMetadata metadata);

	private:
		AutomationHandlerResolver resolveHandler;
	};
}
