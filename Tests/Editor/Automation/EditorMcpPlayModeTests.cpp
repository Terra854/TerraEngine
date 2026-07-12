#include "../TestHarness.h"

#include "EditorAutomationService.h"

#include <array>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		// Play Mode is a semantic surface: callers must never infer state from a
		// toolbar button. This helper keeps every test on the public MCP contract.
		nlohmann::json CallPlay(
			EditorAutomationService& service,
			std::string_view operation,
			nlohmann::json arguments,
			std::string_view requestId)
		{
			return service.HandleTool(operation, arguments, WriteMode::WritesEnabled, std::string(requestId));
		}

		void ExpectStateShape(TestContext& context, const nlohmann::json& result)
		{
			context.Expect(result.contains("requestId"), "Every Play Mode result should preserve the request identifier.");
			context.Expect(result.contains("data"), "Play Mode results should include structured state data.");
			if (result.contains("data") && result["data"].is_object())
			{
				context.Expect(result["data"].contains("currentState"), "Play Mode data should report the current state.");
				context.Expect(result["data"].contains("pendingTransition"), "Play Mode data should report any pending transition.");
			}
		}
	}

	void RegisterEditorMcpPlayModeTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.PlayMode.DiscoveryAdvertisesCompleteSemanticSurface", [](TestContext& context) {
			EditorAutomationService service;
			const std::array<std::string_view, 6> operations{
				"play.state",
				"play.enter",
				"play.pause",
				"play.resume",
				"play.step",
				"play.exit"
			};

			for (const std::string_view operation : operations)
			{
				const auto capability = service.GetRegistry().Find(operation, 1, service.BuildAutomationStateSnapshot());
				context.Expect(capability.has_value(), std::string(operation) + " should be discoverable only when it has an executable handler.");
				if (!capability.has_value())
				{
					continue;
				}
				context.ExpectEqual(std::string("play"), capability->descriptor.category, "Play Mode tools should share a stable discovery category.");
				context.Expect(capability->descriptor.execution.protectedCommit || operation == "play.state", "State-changing Play Mode tools must declare protected editor work.");
			}
		});

		registry.Add("Editor.MCP.PlayMode.StateAndUnavailableStartAreStructured", [](TestContext& context) {
			EditorAutomationService service;
			const nlohmann::json state = CallPlay(service, "play.state", nlohmann::json::object(), "play-state-stopped");
			context.Expect(state.value("ok", false), "Play state should remain readable when no project is open.");
			ExpectStateShape(context, state);
			if (state.contains("data"))
			{
				context.ExpectEqual(std::string("stopped"), state["data"].value("currentState", std::string()), "An editor without an active run should report stopped.");
			}

			const nlohmann::json enter = CallPlay(service, "play.enter", {{"expectedState", "stopped"}, {"correlationId", "play-run-1"}}, "play-enter-unavailable");
			context.Expect(!enter.value("ok", true), "Entering Play Mode without a valid saved scene should be rejected.");
			context.ExpectEqual(std::string("unavailable"), enter["error"].value("category", std::string()), "Missing project/scene state should use the unavailable category.");
			ExpectStateShape(context, enter);
		});

		registry.Add("Editor.MCP.PlayMode.InvalidTransitionsDoNotCollideOrStep", [](TestContext& context) {
			EditorAutomationService service;
			const std::array<std::string_view, 4> invalidFromStopped{
				"play.pause",
				"play.resume",
				"play.step",
				"play.exit"
			};

			for (const std::string_view operation : invalidFromStopped)
			{
				const nlohmann::json result = CallPlay(
					service,
					operation,
					{{"expectedState", "stopped"}, {"correlationId", "invalid-transition"}},
					std::string(operation) + "-stopped");
				context.Expect(!result.value("ok", true), std::string(operation) + " should reject an invalid stopped-state transition.");
				const std::string category = result["error"].value("category", std::string());
				context.Expect(category == "conflict" || category == "unavailable", "Invalid or colliding transitions should return conflict or unavailable, never an internal error.");
				ExpectStateShape(context, result);
			}
		});

		registry.Add("Editor.MCP.PlayMode.StepAndExitContractsExposeExactRestoration", [](TestContext& context) {
			EditorAutomationService service;
			const auto step = service.GetRegistry().Find("play.step", 1, service.BuildAutomationStateSnapshot());
			const auto exit = service.GetRegistry().Find("play.exit", 1, service.BuildAutomationStateSnapshot());
			context.Expect(step.has_value(), "Single-step should have a registered contract.");
			context.Expect(exit.has_value(), "Play exit should have a registered contract.");

			if (step.has_value())
			{
				const nlohmann::json& properties = step->descriptor.resultSchema["properties"];
				context.Expect(properties.contains("stepsCompleted"), "Single-step results must state how many simulation frames completed.");
				context.Expect(properties.contains("currentState"), "Single-step results must prove the editor returned to paused state.");
			}
			if (exit.has_value())
			{
				const nlohmann::json& properties = exit->descriptor.resultSchema["properties"];
				context.Expect(properties.contains("restoration"), "Play exit results must report authored-state restoration, including partial failures.");
			}
		});
	}
}
