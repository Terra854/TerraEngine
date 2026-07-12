#include "../TestHarness.h"

#include "EditorAutomationRegistry.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		AutomationCapabilityRegistration MakeRegistration(std::string name, int resultValue = 1)
		{
			AutomationCapabilityRegistration registration;
			registration.descriptor.name = std::move(name);
			registration.descriptor.category = "test";
			registration.descriptor.description = "Exercise a typed test capability.";
			registration.handler = [resultValue](const AutomationRequestContext&) {
				return AutomationDispatchResult{true, ErrorCategory::Internal, {}, {{"value", resultValue}}};
			};
			return registration;
		}

		class TestAutomationProvider final : public AutomationProvider
		{
		public:
			TestAutomationProvider(std::string providerId, std::vector<AutomationCapabilityRegistration> capabilities)
				: id(std::move(providerId)), registrations(std::move(capabilities))
			{
			}

			std::string_view GetProviderId() const override
			{
				return id;
			}

			std::vector<AutomationCapabilityRegistration> GetCapabilities() override
			{
				return registrations;
			}

		private:
			std::string id;
			std::vector<AutomationCapabilityRegistration> registrations;
		};
	}

	void RegisterEditorAutomationRegistryTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.Registry.RejectsInvalidDescriptorsAtomically", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			auto invalid = MakeRegistration("invalid");
			invalid.handler = {};
			const auto provider = std::make_shared<TestAutomationProvider>(
				"invalid-provider",
				std::vector<AutomationCapabilityRegistration>{MakeRegistration("test.valid"), std::move(invalid)});

			const ProviderRegistrationResult result = capabilityRegistry.RegisterProvider(provider);
			context.Expect(!result.registered, "A provider with any invalid descriptor must be rejected.");
			context.ExpectEqual(std::size_t(0), capabilityRegistry.CapabilityCount(), "Failed registration must publish no capabilities.");
			context.ExpectEqual(std::size_t(0), capabilityRegistry.ProviderCount(), "Failed registration must not retain the provider.");
		});

		registry.Add("Editor.Automation.Registry.DuplicateCapabilityRollsBackWholeProvider", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			const auto first = std::make_shared<TestAutomationProvider>(
				"first-provider",
				std::vector<AutomationCapabilityRegistration>{MakeRegistration("test.shared", 1)});
			context.Expect(capabilityRegistry.RegisterProvider(first).registered, "The first provider should register.");

			const auto conflicting = std::make_shared<TestAutomationProvider>(
				"conflicting-provider",
				std::vector<AutomationCapabilityRegistration>{MakeRegistration("test.unique", 2), MakeRegistration("test.shared", 3)});
			const ProviderRegistrationResult result = capabilityRegistry.RegisterProvider(conflicting);

			context.Expect(!result.registered, "A duplicate name and version must reject the provider.");
			context.ExpectEqual(std::size_t(1), capabilityRegistry.CapabilityCount(), "The provider's unique descriptor must also roll back.");
			context.Expect(!capabilityRegistry.Find("test.unique", 1).has_value(), "A failed provider must not leak partial discovery state.");
		});

		registry.Add("Editor.Automation.Registry.DiscoveryHasExecutableHandlerParity", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			const auto provider = std::make_shared<TestAutomationProvider>(
				"parity-provider",
				std::vector<AutomationCapabilityRegistration>{MakeRegistration("test.one", 11), MakeRegistration("test.two", 22)});
			context.Expect(capabilityRegistry.RegisterProvider(provider).registered, "The parity provider should register.");

			const auto capabilities = capabilityRegistry.Snapshot();
			context.ExpectEqual(std::size_t(2), capabilities.size(), "Discovery should return every registered capability.");
			for (const auto& capability : capabilities)
			{
				const AutomationDispatchResult dispatched = capabilityRegistry.Dispatch(capability.descriptor.name, capability.descriptor.version, {});
				context.Expect(dispatched.ok, "Every discovered capability must resolve to an executable handler.");
			}
		});

		registry.Add("Editor.Automation.Registry.BuiltInDispatchUsesBoundOperationIds", [](TestContext& context) {
			// This source-level contract prevents request-time string comparisons from quietly returning to the
			// built-in provider. Each registry handler binds its stable operation ID once during registration.
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp";
			std::ifstream sourceFile(sourcePath, std::ios::binary);
			context.Expect(sourceFile.good(), "The built-in automation service source must be available to the contract test.");
			if (!sourceFile.good())
			{
				return;
			}

			std::ostringstream sourceStream;
			sourceStream << sourceFile.rdbuf();
			const std::string source = sourceStream.str();
			context.Expect(source.find("const AutomationOperationId operation = AutomationOperation(name);") != std::string::npos,
				"Provider registration must bind a stable operation ID before request dispatch.");
			context.Expect(source.find("boundProviderOperationNames.emplace(operation, operationName)") != std::string::npos,
				"Operation-ID collisions must be rejected while providers are registered.");
			context.Expect(source.find("toolName ==") == std::string::npos,
				"Built-in dispatch must not compare request-time tool-name strings.");
			context.Expect(source.find("operationName.rfind(") == std::string::npos,
				"Built-in dispatch must not infer capability groups from request-time name prefixes.");
		});

		registry.Add("Editor.Automation.Registry.UnregisterReleasesProviderLifetime", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			auto provider = std::make_shared<TestAutomationProvider>(
				"temporary-provider",
				std::vector<AutomationCapabilityRegistration>{MakeRegistration("test.temporary")});
			const std::weak_ptr<TestAutomationProvider> lifetime = provider;
			context.Expect(capabilityRegistry.RegisterProvider(provider).registered, "The temporary provider should register.");
			provider.reset();
			context.Expect(!lifetime.expired(), "The registry must own a provider while its handlers are advertised.");

			context.Expect(capabilityRegistry.UnregisterProvider("temporary-provider"), "Unregister should remove the provider.");
			context.Expect(lifetime.expired(), "Unregister should release provider ownership after synchronous dispatches finish.");
			context.Expect(!capabilityRegistry.Find("test.temporary", 1).has_value(), "Removed capabilities must no longer be discoverable.");
		});
	}
}
