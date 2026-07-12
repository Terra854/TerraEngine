// EditorAutomationRegistry.cpp - Atomic provider publication and dispatch.

#include "EditorAutomationRegistry.h"
#include "Core/Object/Component.h"

#include <algorithm>
#include <exception>
#include <unordered_set>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		AutomationPermission ConvertPermission(ComponentAutomationPermission permission)
		{
			switch (permission)
			{
				case ComponentAutomationPermission::ProjectWrite: return AutomationPermission::ProjectWrite;
				case ComponentAutomationPermission::GlobalWrite: return AutomationPermission::GlobalWrite;
				case ComponentAutomationPermission::Read: return AutomationPermission::Read;
			}
			return AutomationPermission::Read;
		}

		AutomationApprovalPolicy ConvertApproval(ComponentAutomationApproval approval)
		{
			switch (approval)
			{
				case ComponentAutomationApproval::Destructive: return AutomationApprovalPolicy::Destructive;
				case ComponentAutomationApproval::HighImpact: return AutomationApprovalPolicy::HighImpact;
				case ComponentAutomationApproval::None: return AutomationApprovalPolicy::None;
			}
			return AutomationApprovalPolicy::None;
		}

		MutationMode ConvertMutation(ComponentAutomationMutation mutation)
		{
			switch (mutation)
			{
				case ComponentAutomationMutation::Project: return MutationMode::ProjectMutation;
				case ComponentAutomationMutation::Scene: return MutationMode::SceneMutation;
				case ComponentAutomationMutation::Asset: return MutationMode::AssetMutation;
				case ComponentAutomationMutation::Destructive: return MutationMode::Destructive;
				case ComponentAutomationMutation::ReadOnly: return MutationMode::ReadOnly;
			}
			return MutationMode::ReadOnly;
		}

		class ComponentDeclarationProvider final : public AutomationProvider
		{
		public:
			ComponentDeclarationProvider(std::string componentName, std::vector<ComponentAutomationDeclaration> declarations)
				: providerId("component." + std::move(componentName)), ownedDeclarations(std::move(declarations))
			{
			}

			std::string_view GetProviderId() const override { return providerId; }

			std::vector<AutomationCapabilityRegistration> GetCapabilities() override
			{
				std::vector<AutomationCapabilityRegistration> registrations;
				registrations.reserve(ownedDeclarations.size());
				for (const ComponentAutomationDeclaration& declaration : ownedDeclarations)
				{
					AutomationCapabilityDescriptor descriptor;
					descriptor.name = declaration.name;
					descriptor.category = declaration.category;
					descriptor.description = declaration.description;
					descriptor.inputSchema = declaration.inputSchema;
					descriptor.resultSchema = declaration.resultSchema;
					descriptor.permission = ConvertPermission(declaration.permission);
					descriptor.approvalPolicy = ConvertApproval(declaration.approval);
					descriptor.mutationMode = ConvertMutation(declaration.mutation);
					descriptor.availability.requiresProject = declaration.requiresProject;
					descriptor.availability.requiresScene = declaration.requiresScene;
					descriptor.availability.requiresRenderer = declaration.requiresRenderer;
					descriptor.execution.workerPreparation = declaration.workerPreparation;
					descriptor.execution.protectedCommit = declaration.protectedCommit;
					descriptor.execution.visibilityBoundary = declaration.visibilityBoundary;

					const auto handler = declaration.handler;
					registrations.push_back({
						std::move(descriptor),
						[handler](const AutomationRequestContext& request) {
							if (!handler)
								return AutomationDispatchResult{false, ErrorCategory::Internal, "Component automation handler is unavailable.", {}};
							return AutomationDispatchResult{true, ErrorCategory::Internal, {}, handler(request.arguments)};
						},
						{}
					});
				}
				return registrations;
			}

		private:
			std::string providerId;
			std::vector<ComponentAutomationDeclaration> ownedDeclarations;
		};
	}

	std::size_t EditorAutomationRegistry::CapabilityKeyHash::operator()(const CapabilityKey& value) const noexcept
	{
		const std::size_t nameHash = std::hash<std::string>{}(value.name);
		return nameHash ^ (std::hash<std::uint32_t>{}(value.version) + 0x9e3779b9u + (nameHash << 6u) + (nameHash >> 2u));
	}

	ProviderRegistrationResult EditorAutomationRegistry::RegisterProvider(std::shared_ptr<AutomationProvider> provider)
	{
		if (!provider)
			return {false, "Provider ownership is required.", 0};

		const std::string providerId(provider->GetProviderId());
		if (providerId.empty())
			return {false, "Provider ID must not be empty.", 0};

		std::vector<AutomationCapabilityRegistration> pending;
		try
		{
			pending = provider->GetCapabilities();
		}
		catch (const std::exception& exception)
		{
			return {false, std::string("Provider descriptor creation failed: ") + exception.what(), 0};
		}

		if (pending.empty())
			return {false, "Provider must register at least one capability.", 0};

		std::unordered_set<CapabilityKey, CapabilityKeyHash> pendingKeys;
		for (const AutomationCapabilityRegistration& registration : pending)
		{
			if (const std::string error = ValidateDescriptor(registration); !error.empty())
				return {false, error, 0};

			CapabilityKey key{registration.descriptor.name, registration.descriptor.version};
			if (!pendingKeys.insert(key).second)
				return {false, "Provider contains a duplicate capability version: " + key.name, 0};
		}

		std::scoped_lock lock(registryMutex);
		if (providers.contains(providerId))
			return {false, "Provider is already registered: " + providerId, 0};

		// Check every key before publishing any entry. This is the rollback
		// guarantee required when one descriptor in a provider conflicts.
		for (const CapabilityKey& key : pendingKeys)
		{
			if (capabilities.contains(key))
				return {false, "Capability version is already registered: " + key.name, 0};
		}

		providers.emplace(providerId, provider);
		for (AutomationCapabilityRegistration& registration : pending)
		{
			CapabilityKey key{registration.descriptor.name, registration.descriptor.version};
			capabilities.emplace(std::move(key), RegisteredCapability{std::move(registration), providerId, provider});
		}

		return {true, {}, pending.size()};
	}

	bool EditorAutomationRegistry::UnregisterProvider(std::string_view providerId)
	{
		std::scoped_lock lock(registryMutex);
		const auto providerIt = providers.find(std::string(providerId));
		if (providerIt == providers.end())
			return false;

		// Erasing discovery entries first makes the provider immediately
		// unavailable. Any dispatch which already copied an entry retains shared
		// provider ownership until its handler returns.
		for (auto it = capabilities.begin(); it != capabilities.end();)
		{
			if (it->second.providerId == providerId)
				it = capabilities.erase(it);
			else
				++it;
		}
		providers.erase(providerIt);
		return true;
	}

	std::optional<AutomationCapabilitySnapshot> EditorAutomationRegistry::Find(
		std::string_view name,
		std::uint32_t version,
		const AutomationStateSnapshot& state) const
	{
		AutomationCapabilityRegistration registration;
		std::string providerId;
		{
			std::scoped_lock lock(registryMutex);
			const auto found = capabilities.find(CapabilityKey{std::string(name), version});
			if (found == capabilities.end())
				return std::nullopt;
			registration = found->second.registration;
			providerId = found->second.providerId;
		}
		return AutomationCapabilitySnapshot{registration.descriptor, EvaluateAvailability(registration, state), std::move(providerId)};
	}

	std::vector<AutomationCapabilitySnapshot> EditorAutomationRegistry::Snapshot(const AutomationStateSnapshot& state) const
	{
		std::vector<std::pair<AutomationCapabilityRegistration, std::string>> registrations;
		{
			std::scoped_lock lock(registryMutex);
			registrations.reserve(capabilities.size());
			for (const auto& [key, capability] : capabilities)
				registrations.emplace_back(capability.registration, capability.providerId);
		}

		std::vector<AutomationCapabilitySnapshot> snapshots;
		snapshots.reserve(registrations.size());
		for (const auto& [registration, providerId] : registrations)
			snapshots.push_back({registration.descriptor, EvaluateAvailability(registration, state), providerId});
		std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right)
		{
			return std::tie(left.descriptor.name, left.descriptor.version) < std::tie(right.descriptor.name, right.descriptor.version);
		});
		return snapshots;
	}

	AutomationDispatchResult EditorAutomationRegistry::Dispatch(
		std::string_view name,
		std::uint32_t version,
		const AutomationRequestContext& request) const
	{
		RegisteredCapability capability;
		{
			std::scoped_lock lock(registryMutex);
			const auto found = capabilities.find(CapabilityKey{std::string(name), version});
			if (found == capabilities.end())
				return {false, ErrorCategory::UnsupportedCapability, "Capability version is not registered.", {}};
			capability = found->second;
		}

		const AutomationAvailability availability = EvaluateAvailability(capability.registration, request.state);
		if (!availability.available && !request.preserveLegacyAvailability)
			return {false, ErrorCategory::Unavailable, availability.reason.empty() ? "Capability is unavailable." : availability.reason, {}};
		if (capability.registration.descriptor.permission == AutomationPermission::ProjectWrite
			&& request.grantedPermission == AutomationPermission::Read)
		{
			return {false, ErrorCategory::Permission, "Capability requires project write permission.", {}};
		}
		if (capability.registration.descriptor.permission == AutomationPermission::GlobalWrite
			&& request.grantedPermission != AutomationPermission::GlobalWrite)
		{
			return {false, ErrorCategory::Permission, "Capability requires editor-global write permission.", {}};
		}

		try
		{
			return capability.registration.handler(request);
		}
		catch (const std::exception& exception)
		{
			return {false, ErrorCategory::Internal, std::string("Capability handler failed: ") + exception.what(), {}};
		}
		catch (...)
		{
			return {false, ErrorCategory::Internal, "Capability handler failed with an unknown exception.", {}};
		}
	}

	std::size_t EditorAutomationRegistry::ProviderCount() const
	{
		std::scoped_lock lock(registryMutex);
		return providers.size();
	}

	std::size_t EditorAutomationRegistry::CapabilityCount() const
	{
		std::scoped_lock lock(registryMutex);
		return capabilities.size();
	}

	std::vector<ProviderRegistrationResult> EditorAutomationRegistry::RegisterComponentAutomationDeclarations()
	{
		std::vector<std::pair<std::string, std::vector<ComponentAutomationDeclaration>>> declarations;
		{
			std::scoped_lock componentLock(ComponentAccessRegistryMutex());
			for (const auto& [componentName, access] : ComponentAccessRegistry())
			{
				if (!access.automationDeclarations.empty())
					declarations.emplace_back(componentName, access.automationDeclarations);
			}
		}

		std::vector<ProviderRegistrationResult> results;
		results.reserve(declarations.size());
		for (auto& [componentName, operations] : declarations)
		{
			results.push_back(RegisterProvider(std::make_shared<ComponentDeclarationProvider>(
				std::move(componentName), std::move(operations))));
		}
		return results;
	}

	std::string EditorAutomationRegistry::ValidateDescriptor(const AutomationCapabilityRegistration& registration)
	{
		const AutomationCapabilityDescriptor& descriptor = registration.descriptor;
		if (descriptor.name.empty() || descriptor.name.find('.') == std::string::npos)
			return "Capability names must be non-empty and namespaced.";
		if (descriptor.version == 0)
			return "Capability versions start at one.";
		if (descriptor.category.empty() || descriptor.description.empty())
			return "Capability category and description are required.";
		if (!descriptor.inputSchema.is_object() || !descriptor.resultSchema.is_object())
			return "Capability input and result schemas must be JSON objects.";
		if (!registration.handler)
			return "Capability handler is required.";
		if (descriptor.permission == AutomationPermission::GlobalWrite && descriptor.approvalPolicy == AutomationApprovalPolicy::None)
			return "Global-write capabilities require an approval policy.";
		if (descriptor.mutationMode != MutationMode::ReadOnly && descriptor.execution.visibilityBoundary.empty())
			return "State-changing capabilities must declare a visibility boundary.";
		return {};
	}

	AutomationAvailability EditorAutomationRegistry::EvaluateAvailability(
		const AutomationCapabilityRegistration& registration,
		const AutomationStateSnapshot& state)
	{
		AutomationAvailability availability = registration.descriptor.availability;
		if (registration.availabilityPredicate)
		{
			try
			{
				availability = registration.availabilityPredicate(state);
			}
			catch (...)
			{
				availability.available = false;
				availability.reason = "Availability predicate failed.";
				return availability;
			}
		}

		if (availability.requiresProject && !state.projectAvailable)
		{
			availability.available = false;
			availability.reason = "A project must be open.";
		}
		else if (availability.requiresScene && !state.sceneAvailable)
		{
			availability.available = false;
			availability.reason = "A scene must be open.";
		}
		else if (availability.requiresRenderer && !state.rendererAvailable)
		{
			availability.available = false;
			availability.reason = "The renderer is unavailable.";
		}
		else if (!availability.allowedPlayStates.empty()
			&& std::find(availability.allowedPlayStates.begin(), availability.allowedPlayStates.end(), state.playState) == availability.allowedPlayStates.end())
		{
			availability.available = false;
			availability.reason = "The current Play Mode state is unsupported.";
		}
		return availability;
	}
}
