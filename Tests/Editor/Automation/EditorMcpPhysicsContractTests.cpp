// EditorMcpPhysicsContractTests.cpp - Discovery/schema contracts for physics MCP.

#include "../TestHarness.h"

#include "EditorAutomationRegistry.h"
#include "Providers/PhysicsAutomationProvider.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		std::shared_ptr<PhysicsAutomationProvider> MakeProvider()
		{
			// The provider must receive one handler per declared operation.  The
			// service replaces this harmless test handler with boundary-aware work.
			return std::make_shared<PhysicsAutomationProvider>([](std::string_view name) {
				return [operation = std::string(name)](const AutomationRequestContext& request) {
					return AutomationDispatchResult{
						true,
						ErrorCategory::Internal,
						{},
						{{"operation", operation}, {"correlationId", request.correlationId}}};
				};
			});
		}

		const AutomationCapabilitySnapshot* Find(
			const std::vector<AutomationCapabilitySnapshot>& capabilities,
			std::string_view name)
		{
			for (const AutomationCapabilitySnapshot& capability : capabilities)
			{
				if (capability.descriptor.name == name)
					return &capability;
			}
			return nullptr;
		}
	}

	void RegisterEditorMcpPhysicsContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Physics.DiscoveryPublishesOnlyBoundPhysicsCapabilities", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			const auto provider = MakeProvider();
			const ProviderRegistrationResult registration = capabilityRegistry.RegisterProvider(provider);
			context.Expect(registration.registered, "The dedicated physics provider should register atomically.");
			context.ExpectEqual(std::string("physics"), std::string(provider->GetProviderId()), "Physics capability ownership should have a stable provider ID.");
			context.ExpectEqual(std::size_t(25), registration.capabilityCount, "The provider should expose the complete bounded physics surface.");

			const AutomationStateSnapshot activeState{
				.projectAvailable = true,
				.sceneAvailable = true,
				.rendererAvailable = false,
				.playState = "playing"
			};
			const auto capabilities = capabilityRegistry.Snapshot(activeState);
			const std::array<std::string_view, 10> required{
				"physics.state", "physics.settings.get", "physics.layers.list", "physics.material.inspect",
				"physics.body.inspect", "physics.character.inspect", "physics.raycast", "physics.settings.set",
				"physics.body.apply_impulse", "physics.collider.cook"
			};
			for (const std::string_view name : required)
			{
				const AutomationCapabilitySnapshot* capability = Find(capabilities, name);
				context.Expect(capability != nullptr, std::string(name) + " should be discoverable.");
				if (capability != nullptr)
				{
					context.Expect(capability->descriptor.inputSchema.value("additionalProperties", true) == false,
						"Physics requests must use closed, bounded schemas rather than arbitrary payloads.");
					context.Expect(!capability->descriptor.execution.visibilityBoundary.empty(),
						"Every physics capability must declare how its data becomes visible.");
				}
			}

			const std::array<std::string_view, 5> forbiddenProjectLifecycle{
				"project.create", "project.open", "project.close", "project.select", "physics.project.open"
			};
			for (const std::string_view name : forbiddenProjectLifecycle)
				context.Expect(!capabilityRegistry.Find(name, 1, activeState).has_value(), "Physics automation must not expose project lifecycle control.");
		});

		registry.Add("Editor.MCP.Physics.ProjectAndSceneAvailabilityAreExplicit", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			context.Expect(capabilityRegistry.RegisterProvider(MakeProvider()).registered, "Physics provider registration should succeed.");

			const auto settingsWithoutProject = capabilityRegistry.Find("physics.settings.get", 1, {});
			context.Expect(settingsWithoutProject.has_value(), "Discovery should retain the settings descriptor even while unavailable.");
			if (settingsWithoutProject.has_value())
				context.Expect(!settingsWithoutProject->availability.available, "Physics settings require an already-open native project.");

			AutomationStateSnapshot projectOnly;
			projectOnly.projectAvailable = true;
			projectOnly.playState = "playing";
			const auto queryWithoutScene = capabilityRegistry.Find("physics.raycast", 1, projectOnly);
			context.Expect(queryWithoutScene.has_value(), "The raycast descriptor should remain inspectable while unavailable.");
			if (queryWithoutScene.has_value())
				context.Expect(!queryWithoutScene->availability.available, "Physics scene queries require an active scene and never manufacture one.");

			const auto state = capabilityRegistry.Find("physics.state", 1, projectOnly);
			context.Expect(state.has_value() && state->availability.available, "World state should remain available to an open project without a scene.");
		});

		registry.Add("Editor.MCP.Physics.QuerySchemasBoundInputAndResults", [](TestContext& context) {
			EditorAutomationRegistry capabilityRegistry;
			context.Expect(capabilityRegistry.RegisterProvider(MakeProvider()).registered, "Physics provider registration should succeed.");
			AutomationStateSnapshot state;
			state.projectAvailable = true;
			state.sceneAvailable = true;
			state.playState = "playing";
			const auto raycast = capabilityRegistry.Find("physics.raycast", 1, state);
			context.Expect(raycast.has_value(), "Raycast should have a typed descriptor.");
			if (!raycast.has_value())
				return;

			const nlohmann::json& properties = raycast->descriptor.inputSchema["properties"];
			context.ExpectEqual(std::uint32_t(64), properties["maximumHits"].value("maximum", 0u), "Raycast hit results must match the engine's hard per-query bound.");
			context.ExpectEqual(std::uint32_t(1024), properties["batchSize"].value("maximum", 0u), "Batch query requests must have a fixed upper bound.");
			context.ExpectEqual(100000.0, properties["distance"].value("maximum", 0.0), "Raycast distance must be bounded before a provider query is scheduled.");
			context.Expect(raycast->descriptor.resultSchema["properties"].contains("jobId"), "A query result must reserve a durable-job receipt for over-budget work.");
			context.ExpectEqual(std::string("immutable_physics_snapshot"), raycast->descriptor.execution.visibilityBoundary, "Read queries must not mutate the live provider world.");
		});
	}
}
