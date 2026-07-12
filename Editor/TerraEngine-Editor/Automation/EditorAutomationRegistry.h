// EditorAutomationRegistry.h - Thread-safe typed automation capability index.

#pragma once

#include "Providers/AutomationProvider.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Editor::Automation
{
	struct ProviderRegistrationResult
	{
		bool registered = false;
		std::string error;
		std::size_t capabilityCount = 0;
	};

	struct AutomationCapabilitySnapshot
	{
		AutomationCapabilityDescriptor descriptor;
		AutomationAvailability availability;
		std::string providerId;
	};

	class EditorAutomationRegistry
	{
	public:
		[[nodiscard]] ProviderRegistrationResult RegisterProvider(std::shared_ptr<AutomationProvider> provider);
		[[nodiscard]] bool UnregisterProvider(std::string_view providerId);
		[[nodiscard]] std::optional<AutomationCapabilitySnapshot> Find(
			std::string_view name,
			std::uint32_t version,
			const AutomationStateSnapshot& state = {}) const;
		[[nodiscard]] std::vector<AutomationCapabilitySnapshot> Snapshot(const AutomationStateSnapshot& state = {}) const;
		[[nodiscard]] AutomationDispatchResult Dispatch(
			std::string_view name,
			std::uint32_t version,
			const AutomationRequestContext& request) const;
		[[nodiscard]] std::size_t ProviderCount() const;
		[[nodiscard]] std::size_t CapabilityCount() const;
		// Publish component-owned declarations after their CRTP types have been
		// registered. Each component is one atomic provider boundary.
		[[nodiscard]] std::vector<ProviderRegistrationResult> RegisterComponentAutomationDeclarations();

	private:
		struct CapabilityKey
		{
			std::string name;
			std::uint32_t version = 1;

			bool operator==(const CapabilityKey&) const = default;
		};

		struct CapabilityKeyHash
		{
			std::size_t operator()(const CapabilityKey& value) const noexcept;
		};

		struct RegisteredCapability
		{
			AutomationCapabilityRegistration registration;
			std::string providerId;
			std::shared_ptr<AutomationProvider> owner;
		};

		[[nodiscard]] static std::string ValidateDescriptor(const AutomationCapabilityRegistration& registration);
		[[nodiscard]] static AutomationAvailability EvaluateAvailability(
			const AutomationCapabilityRegistration& registration,
			const AutomationStateSnapshot& state);

		mutable std::mutex registryMutex;
		std::unordered_map<CapabilityKey, RegisteredCapability, CapabilityKeyHash> capabilities;
		std::unordered_map<std::string, std::shared_ptr<AutomationProvider>> providers;
	};
}
