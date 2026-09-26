// LocomotionAutomationProvider.h - copied character movement automation boundary.
#pragma once
#include "AutomationProvider.h"

namespace TerraEngine::Editor::Automation {
	// Handlers own only copied JSON/native records; the movement system remains
	// responsible for admission and protected physical publication.
	class LocomotionAutomationProvider final : public AutomationProvider {
	public:
		std::string_view GetProviderId() const override;
		std::vector<AutomationCapabilityRegistration> GetCapabilities() override;
	};
}
