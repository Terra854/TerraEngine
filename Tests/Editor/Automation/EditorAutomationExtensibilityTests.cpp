#include "../TestHarness.h"

#include "EditorAutomationRegistry.h"
#include "EditorAutomationService.h"
#include "Core/Object/Component.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		struct SelfDeclaredAutomationComponent final : Component<SelfDeclaredAutomationComponent>
		{
			static constexpr std::string_view kComponentName = "SelfDeclaredAutomationComponent";

			static std::vector<ComponentAutomationDeclaration> automation()
			{
				ComponentAutomationDeclaration declaration;
				declaration.name = "self_declared_component.inspect";
				declaration.category = "self_declared_component";
				declaration.description = "Inspect a component-owned automation declaration.";
				declaration.inputSchema = {{"type", "object"}};
				declaration.resultSchema = {{"type", "object"}};
				declaration.requiresScene = true;
				declaration.handler = [](const nlohmann::json&) {
					return nlohmann::json{{"componentType", "SelfDeclaredAutomationComponent"}};
				};
				return {std::move(declaration)};
			}
		};

		bool SchemaHasProperty(const nlohmann::json& schema, std::string_view property)
		{
			return schema.contains("properties")
				&& schema["properties"].is_object()
				&& schema["properties"].contains(std::string(property));
		}

		// This provider stands in for a future physics, audio, or animation
		// subsystem. Registering it directly proves that adding a domain does not
		// require another branch in EditorAutomationService's name dispatch.
		class FutureComponentAutomationProvider final : public AutomationProvider
		{
		public:
			std::string_view GetProviderId() const override
			{
				return "test_future_component";
			}

			std::vector<AutomationCapabilityRegistration> GetCapabilities() override
			{
				AutomationCapabilityDescriptor descriptor;
				descriptor.name = "future_component.inspect";
				descriptor.category = "future_component";
				descriptor.description = "Inspect a future component through its self-declared automation surface.";
				descriptor.inputSchema = {
					{"type", "object"},
					{"properties", {{"target", {{"type", "string"}}}}},
					{"required", nlohmann::json::array({"target"})}
				};
				descriptor.resultSchema = {
					{"type", "object"},
					{"properties", {{"componentType", {{"type", "string"}}}}}
				};
				descriptor.permission = AutomationPermission::Read;
				descriptor.mutationMode = MutationMode::ReadOnly;
				descriptor.availability.requiresScene = true;
				descriptor.execution.workerPreparation = true;
				descriptor.execution.visibilityBoundary = "immutable_snapshot";

				return {{
					std::move(descriptor),
					[](const AutomationRequestContext&) {
						return AutomationDispatchResult{true, ErrorCategory::Internal, {}, {{"componentType", "FutureComponent"}}};
					},
					{}
				}};
			}
		};
	}

	void RegisterEditorAutomationExtensibilityTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.Extensibility.CommandsUseSemanticAllowlist", [](TestContext& context) {
			EditorAutomationService service;
			const auto list = service.GetRegistry().Find("editor.commands.list", 1, service.BuildAutomationStateSnapshot());
			const auto invoke = service.GetRegistry().Find("editor.commands.invoke", 1, service.BuildAutomationStateSnapshot());
			context.Expect(list.has_value() && invoke.has_value(), "Semantic command discovery and invocation should be registered.");
			if (list.has_value())
			{
				context.Expect(list->descriptor.permission == AutomationPermission::Read, "Command discovery should remain read-only.");
				context.Expect(SchemaHasProperty(list->descriptor.resultSchema, "commands"), "Command discovery should return the explicit allowlist.");
			}
			if (invoke.has_value())
			{
				context.Expect(SchemaHasProperty(invoke->descriptor.inputSchema, "commandId"), "Command invocation should require a stable semantic command ID.");
				context.Expect(!SchemaHasProperty(invoke->descriptor.inputSchema, "menuLabel"), "Command invocation must not execute arbitrary menu labels.");
				context.Expect(!SchemaHasProperty(invoke->descriptor.inputSchema, "screenX"), "Command invocation must not emulate desktop input.");
				context.Expect(invoke->descriptor.execution.protectedCommit, "Editor commands should run at the protected editor boundary.");
			}
		});

		registry.Add("Editor.Automation.Extensibility.ProviderSelfRegistersWithoutCentralDispatch", [](TestContext& context) {
			EditorAutomationRegistry registryUnderTest;
			const auto provider = std::make_shared<FutureComponentAutomationProvider>();
			const ProviderRegistrationResult registered = registryUnderTest.RegisterProvider(provider);
			context.Expect(registered.registered, "A future component provider should register its descriptor and handler atomically.");
			context.ExpectEqual(std::size_t(1), registered.capabilityCount, "The provider should publish its complete declared surface.");

			AutomationStateSnapshot unavailableState;
			const auto unavailable = registryUnderTest.Find("future_component.inspect", 1, unavailableState);
			context.Expect(unavailable.has_value(), "The future capability should appear in registry discovery without central tool-list edits.");
			if (unavailable.has_value())
				context.Expect(!unavailable->availability.available, "The provider's scene availability declaration should be enforced.");

			AutomationRequestContext request;
			request.requestId = "future-provider-dispatch";
			request.arguments = {{"target", "object-1"}};
			request.state.sceneAvailable = true;
			const AutomationDispatchResult dispatched = registryUnderTest.Dispatch("future_component.inspect", 1, request);
			context.Expect(dispatched.ok, "The registry should route directly to the self-registered provider handler.");
			context.ExpectEqual(std::string("FutureComponent"), dispatched.data.value("componentType", std::string()), "Provider dispatch should return its typed result.");
		});

		registry.Add("Editor.Automation.Extensibility.UnregisterRemovesFutureSurface", [](TestContext& context) {
			EditorAutomationRegistry registryUnderTest;
			const auto provider = std::make_shared<FutureComponentAutomationProvider>();
			context.Expect(registryUnderTest.RegisterProvider(provider).registered, "The future component provider should register before removal.");
			context.Expect(registryUnderTest.UnregisterProvider(provider->GetProviderId()), "Subsystem shutdown should unregister its automation surface.");
			context.Expect(!registryUnderTest.Find("future_component.inspect", 1).has_value(), "A stopped subsystem must not leave stale capabilities discoverable.");

			AutomationRequestContext request;
			request.state.sceneAvailable = true;
			const AutomationDispatchResult dispatched = registryUnderTest.Dispatch("future_component.inspect", 1, request);
			context.Expect(!dispatched.ok, "Removed provider handlers must not remain executable.");
			context.Expect(dispatched.error == ErrorCategory::UnsupportedCapability, "Removed capabilities should report unsupported capability.");
		});

		registry.Add("Editor.Automation.Extensibility.ComponentDeclarationPublishesWithoutCentralDispatch", [](TestContext& context) {
			SelfDeclaredAutomationComponent::registerType();
			EditorAutomationRegistry registryUnderTest;
			const auto registrationResults = registryUnderTest.RegisterComponentAutomationDeclarations();
			context.Expect(!registrationResults.empty(), "The registry should discover component-owned automation declarations.");

			AutomationStateSnapshot state;
			state.sceneAvailable = true;
			const auto capability = registryUnderTest.Find("self_declared_component.inspect", 1, state);
			context.Expect(capability.has_value(), "A component declaration should publish without editing the central provider list.");

			AutomationRequestContext request;
			request.state = state;
			const AutomationDispatchResult dispatched = registryUnderTest.Dispatch("self_declared_component.inspect", 1, request);
			context.Expect(dispatched.ok, "The declaration adapter should dispatch the component-owned handler.");
			context.ExpectEqual(std::string("SelfDeclaredAutomationComponent"), dispatched.data.value("componentType", std::string()), "The typed declaration handler should return its component identity.");
		});
	}
}
