#include "../TestHarness.h"
#include "EditorAutomationService.h"
#include "EditorMcpBridge.h"
#include "Providers/EditorDiagnosticsAutomationProvider.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <set>
#include <thread>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		// This independent structural check covers the JSON Schema keywords used
		// by these descriptors; it never delegates to the production validator.
		bool ValidSchema(const nlohmann::json& schema)
		{
			if (!schema.is_object()) return false;
			const std::set<std::string> types{"null", "boolean", "object", "array", "number", "integer", "string"};
			if (schema.contains("type"))
			{
				const auto& type = schema.at("type");
				if (type.is_string())
				{
					if (!types.contains(type.get<std::string>())) return false;
				}
				else if (type.is_array() && !type.empty())
				{
					for (const auto& item : type)
						if (!item.is_string() || !types.contains(item.get<std::string>())) return false;
				}
				else return false;
			}
			if (schema.contains("properties"))
			{
				if (!schema.at("properties").is_object()) return false;
				for (const auto& property : schema.at("properties"))
					if (!ValidSchema(property)) return false;
			}
			if (schema.contains("enum") && (!schema.at("enum").is_array() || schema.at("enum").empty())) return false;
			if (schema.contains("items") && !ValidSchema(schema.at("items"))) return false;
			if (schema.contains("additionalProperties") && !schema.at("additionalProperties").is_boolean()
				&& !ValidSchema(schema.at("additionalProperties"))) return false;
			if (schema.contains("required"))
			{
				if (!schema.at("required").is_array()) return false;
				for (const auto& key : schema.at("required")) if (!key.is_string()) return false;
			}
			return true;
		}

		// Invoke the actual transport admission and protected pump. A malformed
		// envelope must settle its waiter before the subsequent normal call.
		void ExerciseBridgeRecovery(TestContext& context)
		{
			EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			bridge.SetEnabled(true);
			auto call = [&](nlohmann::json arguments) {
				auto result = std::async(std::launch::async, [&bridge, arguments = std::move(arguments)] {
					return bridge.SubmitToolCall("diagnostics.get_summary", arguments);
				});
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
					&& std::chrono::steady_clock::now() < deadline)
				{
					bridge.PumpPendingTransportToolCallsAtProtectedFrame();
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				context.Expect(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready,
					"The protected bridge must settle each admitted request promptly.");
				return result.get();
			};
			const auto failed = call({{"correlationId", nlohmann::json::array()}});
			const auto correlated = call({{"correlationId", "exception-correlation"}, {"idempotencyKey", nlohmann::json::array()}});
			context.Expect(!correlated.value("ok", true) && correlated.value("correlationId", "") == "exception-correlation", "Unexpected envelope conversion must preserve valid caller correlation.");
			const auto activity = bridge.GetRecentActivity(20U);
			context.Expect(std::any_of(activity.begin(), activity.end(), [&](const auto& row) { return row.requestId == correlated.value("requestId", "") && row.type == ActivityEventType::Rejection; }), "The failed admitted request must have a matching rejection activity record.");
			const auto next = call(nlohmann::json::object());
			bridge.Shutdown();
			context.Expect(!failed.value("ok", true), "Malformed correlation identity must produce one failed call.");
			context.Expect(next.value("ok", false), "A failed call must leave the next diagnostics call executable.");
		}
	}

	void RegisterCapabilityFailureRecoveryTests(TestRegistry& registry)
	{
		registry.Add("Editor.CapabilityFailureRecovery.UnavailableEnvelopeRetainsNullMetrics", [](TestContext& context) {
			// Invoke the public envelope, not just the provider dispatch result: the
			// service must preserve unavailable metrics when wrapping a typed error.
			EditorAutomationService service;
			for(const auto* name : {"diagnostics.snapshot", "capture.provenance"}) {
				const auto response = service.HandleTool(name, nlohmann::json::object(), WriteMode::ReadOnly, "unavailable-envelope");
				context.Expect(!response.value("ok", true) && response.contains("data") && response["data"].contains("unavailableMetrics"),
					"Unavailable capability errors must retain the provider's explicit data payload.");
				for(const auto& field : response["data"]["unavailableMetrics"])
					context.Expect(response["data"].at(field.get<std::string>()).is_null(), "An unavailable metric must remain null rather than disappear or become zero.");
			}
		});
		registry.Add("Editor.CapabilityFailureRecovery.ThrowingBridgeCallSettlesAndNextCallSucceeds", [](TestContext& context) {
			if (std::getenv("TERRAENGINE_CAPABILITY_RECOVERY_CHILD") != nullptr)
			{
				ExerciseBridgeRecovery(context);
				return;
			}
			// Pre-fix the pump throws after marking Processing, stranding its waiter.
			// Contain that path in an owned child so regression admission stays bounded.
			const auto executable = TerraEngine::PlatformUtils::GetExecutablePath();
			TerraEngine::Utilities::OwnedProcessRequest request;
			request.workingDirectory = executable.parent_path();
			request.commandLine = "\"" + executable.string() + "\"";
			request.timeout = std::chrono::seconds(6);
			request.environment = {{"TERRAENGINE_CAPABILITY_RECOVERY_CHILD", "1"},
				{"TERRAENGINE_TEST_FILTER", "Editor.CapabilityFailureRecovery.ThrowingBridgeCallSettlesAndNextCallSucceeds"},
				{"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}};
			const std::atomic_bool canceled{false};
			const auto child = TerraEngine::Utilities::OwnedProcess::Run(request, canceled);
			context.Expect(child.started && !child.timedOut && child.exitCode == 0,
				"Throwing bridge recovery child must finish without a stranded waiter (exit " + std::to_string(child.exitCode)
					+ ", timeout " + std::to_string(child.timedOut) + "): " + child.output);
		});

		registry.Add("Editor.CapabilityFailureRecovery.RouteLockSchemasAreWellFormed", [](TestContext& context) {
			EditorDiagnosticsAutomationProvider provider([](std::string_view) { return AutomationHandler{}; });
			bool found = false;
			for (const auto& registration : provider.GetCapabilities())
			{
				if (registration.descriptor.name != "route.lock") continue;
				found = true;
				const auto& input = registration.descriptor.inputSchema;
				context.Expect(ValidSchema(input) && ValidSchema(registration.descriptor.resultSchema),
					"The complete route.lock input/result descriptors must use valid schema keyword shapes.");
				context.Expect(input.at("properties").at("route").at("type") == "string",
					"The route property must advertise a string type.");
				context.Expect(input.at("properties").at("route").at("enum")
					== nlohmann::json::array({"automatic", "direct", "virtual-hardware", "virtual-compute"}),
					"Every supported route must have a separately declared enum value.");
			}
			context.Expect(found, "The production provider must advertise route.lock.");
		});

		registry.Add("Editor.CapabilityFailureRecovery.AdvertisedEvidenceSurfacesHaveRealHandlers", [](TestContext& context) {
			EditorAutomationService service;
			AutomationRequestContext request;
			request.requestId = "evidence-handler-regression";
			request.state.projectAvailable = request.state.sceneAvailable = request.state.rendererAvailable = true;
			request.preserveLegacyAvailability = true;
			std::size_t invoked = 0;
			for (const auto& capability : service.GetRegistry().Snapshot(request.state))
			{
				if (capability.providerId != "editor_diagnostics"
					|| (capability.descriptor.name != "diagnostics.snapshot" && capability.descriptor.name != "capture.provenance")) continue;
				const auto result = service.GetRegistry().Dispatch(capability.descriptor.name, capability.descriptor.version, request);
				++invoked;
				// Compatibility adapters retain their real response in data while the
				// outer typed error remains Internal. Check both error representations.
				std::string legacyCategory;
				if (result.data.is_object() && result.data.contains("error") && result.data.at("error").is_object())
					legacyCategory = result.data.at("error").value("category", std::string{});
				context.Expect(result.error != ErrorCategory::UnsupportedCapability && legacyCategory != "unsupported_capability",
					"An advertised evidence surface must invoke a real handler: " + capability.descriptor.name);
			}
			// Removing unsupported operations is allowed; retained operations cannot
			// be compatibility placeholders. Existing summary remains a control.
			const auto control = service.GetRegistry().Dispatch("diagnostics.get_summary", 1, request);
			context.Expect(control.ok, "The supported summary control must execute through the real registry.");
			context.Expect(invoked <= 2, "Evidence discovery must contain no duplicate supported surface.");
		});
	}
}
