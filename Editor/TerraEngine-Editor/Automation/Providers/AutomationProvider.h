// AutomationProvider.h - Domain-owned capability registration contract.
//
// Providers adapt one editor domain to semantic automation operations. They
// return descriptors and executable handlers together so discovery can never
// publish an operation which has no implementation.

#pragma once

#include "../EditorAutomationTypes.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Editor::Automation
{
	using AutomationHandler = std::function<AutomationDispatchResult(const AutomationRequestContext&)>;
	using AutomationAvailabilityPredicate = std::function<AutomationAvailability(const AutomationStateSnapshot&)>;
	// Providers resolve each named operation once while building their registry
	// entries. Dispatch then invokes the resulting typed handler directly instead
	// of sending every request back through a central name switch.
	using AutomationHandlerResolver = std::function<AutomationHandler(std::string_view)>;

	struct AutomationCapabilityRegistration
	{
		AutomationCapabilityDescriptor descriptor;
		AutomationHandler handler;
		AutomationAvailabilityPredicate availabilityPredicate;
	};

	// Providers are shared with the registry. That shared ownership guarantees
	// handlers remain alive while a dispatch already in progress completes.
	class AutomationProvider
	{
	public:
		virtual ~AutomationProvider() = default;

		[[nodiscard]] virtual std::string_view GetProviderId() const = 0;
		[[nodiscard]] virtual std::vector<AutomationCapabilityRegistration> GetCapabilities() = 0;
	};
}
