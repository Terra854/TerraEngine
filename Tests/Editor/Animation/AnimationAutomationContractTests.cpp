// Animation MCP discovery, schema, and authority contract tests.

#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Automation/AnimationAutomationSchemas.h"
#include "Editor/TerraEngine-Editor/Automation/EditorAutomationService.h"

#include <algorithm>
#include <cstddef>

namespace TerraEngine::EditorTests
{
	void RegisterAnimationAutomationContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.Animation.Automation.Contract.SchemaAndNoLifecycleAuthority", [](TestContext& context) {
			using namespace Editor::Automation;
			const std::vector<std::string> names = AnimationAutomationOperationNames();
			context.Expect(!names.empty(), "Animation MCP must publish a discoverable capability list.");
			for (const std::string& name : names)
			{
				const std::optional<AutomationCapabilityDescriptor> descriptor = DescribeAnimationAutomationOperation(name);
				context.Expect(
					descriptor.has_value() && descriptor->version == kAnimationAutomationSchemaVersion,
					"Every animation capability must have a versioned schema.");
				context.Expect(
					name.find("project.open") == std::string::npos && name.find("application.quit") == std::string::npos,
					"Animation automation must not obtain project or application lifecycle authority.");
			}

			EditorAutomationService service;
			context.Expect(
				service.FindCapability("animation.asset.inspect") != nullptr
					&& service.FindCapability("animation.controller.edit") != nullptr
					&& service.FindCapability("animation.camera.capture") != nullptr
					&& service.FindCapability("animation.recording.start") != nullptr
					&& service.FindCapability("animation.recording.stop") != nullptr
					&& service.FindCapability("animation.runtime.residency") != nullptr
					&& service.FindCapability("animation.runtime.profiler") != nullptr
					&& service.FindCapability("animation.runtime.procedural.goal") != nullptr
					&& service.FindCapability("animation.runtime.procedural.read") != nullptr
					&& service.FindCapability("animation.runtime.retarget.request") != nullptr
					&& service.FindCapability("animation.runtime.retarget.read") != nullptr,
				"The existing editor bridge must register the animation provider surface.");
			context.Expect(
				service.FindCapability("animation.project.open") == nullptr
					&& service.FindCapability("animation.application.quit") == nullptr,
				"Provider registration must not add project or application lifecycle tools.");
			const nlohmann::json discoveryEnvelope = service.BuildCapabilityList();
			const nlohmann::json& discovered = discoveryEnvelope["capabilities"];
			const auto importCapability = std::find_if(
				discovered.begin(),
				discovered.end(),
				[](const nlohmann::json& item) {
					return item.value("name", std::string{}) == "animation.asset.import";
				});
			context.Expect(
				importCapability != discovered.end()
					&& (*importCapability)["limits"].value("defaultConcurrentJobs", 0U) == 16U
					&& (*importCapability)["limits"].value("hardConcurrentJobs", 0U) == 64U
					&& (*importCapability)["jobBehavior"].value("returnsDurableJob", false)
					&& (*importCapability)["generationContract"].value("staleApplication", std::string{}) == "reject_atomically"
					&& (*importCapability)["diagnosticsContract"].value("bounded", false),
				"Discovery must expose machine-readable limits, generations, durable-job behavior, and diagnostics bounds.");
			const auto recordingCapability = std::find_if(
				discovered.begin(),
				discovered.end(),
				[](const nlohmann::json& item) {
					return item.value("name", std::string{}) == "animation.recording.start";
				});
			context.Expect(
				recordingCapability != discovered.end()
					&& (*recordingCapability)["jobBehavior"].value("returnsDurableJob", false)
					&& (*recordingCapability)["generationContract"].value("expectedGenerationRequired", false)
					&& (*recordingCapability)["execution"].value("visibilityBoundary", std::string{})
						== "animation_protected_snapshot_capture_then_atomic_publish",
				"Recording must be registered through the central provider as a generation-checked durable job with its protected capture boundary declared.");
			const AutomationDispatchResult recordingStop = ValidateAnimationAutomationRequest(
				"animation.recording.stop", {{"jobId", "recording-job"}, {"idempotencyKey", "stop"},
					{"correlationId", "stop"}, {"expectedGeneration", 1U}});
			context.Expect(recordingStop.ok,
				"Recording must expose a bounded typed stop request rather than treating a disconnected job poll as completion authority.");
			const nlohmann::json recordingRateBase = {
				{"target", {{"animator", {{"slotIndex", 1U}, {"generation", 1U}}}}},
				{"assetId", ToHexString(GenerateUUID())}, {"skeletonAssetId", ToHexString(GenerateUUID())},
				{"sourceAnimatorId", ToHexString(GenerateUUID())}, {"compatibilityFingerprint", 1U},
				{"publicationGeneration", 1U}, {"jointStableIds", nlohmann::json::array({1U})},
				{"destinationPath", "Captured/Rate.teanim"}, {"firstTick", 0U}, {"lastTick", 2U},
				{"sampleRate", 240U}, {"clock", "fixed_simulation"},
				{"channels", nlohmann::json::array({{{"stableChannelId", 1U}, {"stableTargetId", 1U},
					{"name", "Curve"}, {"kind", "curve"}, {"interpolation", "linear"}}})},
				{"sourceSceneIdentity", "Tests/Rate.scene"}, {"sourceSessionIdentity", "rate"},
				{"segmentDurationTicks", 1U}, {"idempotencyKey", "rate"}, {"expectedGeneration", 1U}};
			const AutomationDispatchResult accepted240 = ValidateAnimationAutomationRequest(
				"animation.recording.start", recordingRateBase);
			nlohmann::json rejected241 = recordingRateBase;
			rejected241["sampleRate"] = 241U;
			const AutomationDispatchResult rejected241Result = ValidateAnimationAutomationRequest(
				"animation.recording.start", rejected241);
			context.Expect(accepted240.ok && !rejected241Result.ok,
				"Recording schemas must match the frozen 240 Hz hard limit and reject 241 Hz before job admission.");

			const AutomationDispatchResult nonObject = ValidateAnimationAutomationRequest(
				"animation.asset.inspect",
				nlohmann::json::array());
			context.Expect(
				!nonObject.ok && nonObject.error == ErrorCategory::Validation,
				"Non-object requests must be rejected before execution.");
			const AutomationDispatchResult unknownField = ValidateAnimationAutomationRequest(
				"animation.asset.inspect",
				{{"assetId", "asset"}, {"projectPath", "forbidden"}});
			context.Expect(
				!unknownField.ok && unknownField.error == ErrorCategory::Validation,
				"Unknown fields must be rejected so lifecycle/path authority cannot hide in a payload.");
			const AutomationDispatchResult invalidGeneration = ValidateAnimationAutomationRequest(
				"animation.controller.edit",
				{{"assetId", "controller"}, {"idempotencyKey", "key"}, {"expectedGeneration", 0U}});
			context.Expect(
				!invalidGeneration.ok && invalidGeneration.error == ErrorCategory::Validation,
				"Mutation generation zero must be rejected before a protected commit.");

			const AutomationDispatchResult invalidJobId = ValidateAnimationAutomationRequest(
				"animation.job.get",
				{{"jobId", 17U}});
			const AutomationDispatchResult unknownJobField = ValidateAnimationAutomationRequest(
				"animation.job.cancel",
				{{"jobId", "job-1"}, {"force", true}});
			context.Expect(
				!invalidJobId.ok && invalidJobId.error == ErrorCategory::Validation
					&& !unknownJobField.ok && unknownJobField.error == ErrorCategory::Validation,
				"Job reads and cancellation must use the same strict advertised schema as every other operation.");

			const nlohmann::json proceduralTarget = {{"animator", {{"slotIndex", 1U}, {"generation", 1U}}}};
			const nlohmann::json proceduralSet = {
				{"target", proceduralTarget}, {"action", "set"}, {"goalId", 10U},
				{"goalKind", "position"}, {"coordinateSpace", "world"}, {"targetJointStableId", 101U},
				{"goalTransform", {{"translation", nlohmann::json::array({0.0, 0.0, 0.0})},
					{"rotation", nlohmann::json::array({0.0, 0.0, 0.0, 1.0})},
					{"scale", nlohmann::json::array({1.0, 1.0, 1.0})}}},
				{"poleVector", nlohmann::json::array({0.0, 1.0, 0.0})}, {"priority", 0},
				{"positionWeight", 1.0}, {"rotationWeight", 0.0}, {"poleWeight", 0.0},
				{"maximumStretchScale", 1.0}, {"expectedPoseGeneration", 1U},
				{"expectedPublicationGeneration", 1U}, {"idempotencyKey", "procedural-set"},
				{"correlationId", "procedural-set"}, {"expectedGeneration", 1U}
			};
			const AutomationDispatchResult validProcedural = ValidateAnimationAutomationRequest(
				"animation.runtime.procedural.goal", proceduralSet);
			nlohmann::json invalidClear = proceduralSet;
			invalidClear["action"] = "clear";
			const AutomationDispatchResult rejectedClear = ValidateAnimationAutomationRequest(
				"animation.runtime.procedural.goal", invalidClear);
			nlohmann::json missingProfile = {{"target", proceduralTarget}, {"action", "activate_profile"},
				{"idempotencyKey", "retarget"}, {"correlationId", "retarget"}, {"expectedGeneration", 1U}};
			const AutomationDispatchResult rejectedRetarget = ValidateAnimationAutomationRequest(
				"animation.runtime.retarget.request", missingProfile);
			const AutomationDispatchResult invalidReadField = ValidateAnimationAutomationRequest(
				"animation.runtime.procedural.read", {{"target", proceduralTarget}, {"liveStorage", true}});
			context.Expect(validProcedural.ok && !rejectedClear.ok && !rejectedRetarget.ok && !invalidReadField.ok,
				"Procedural and retarget MCP schemas must require typed set data, reject clear payloads, and forbid live-storage escape hatches.");
			const nlohmann::json runtimeTarget = {{"animator", {{"slotIndex", 1U}, {"generation", 1U}}}};
			const AutomationDispatchResult rejectedMotion = ValidateAnimationAutomationRequest(
				"animation.runtime.motion_match.request", {{"target", runtimeTarget}, {"action", "query"},
					{"idempotencyKey", "motion"}, {"correlationId", "motion"}, {"expectedGeneration", 1U}});
			const AutomationDispatchResult acceptedRagdoll = ValidateAnimationAutomationRequest(
				"animation.runtime.ragdoll.request", {{"target", runtimeTarget}, {"action", "activate"},
					{"assetId", ToHexString(GenerateUUID())}, {"assetGeneration", 1U}, {"physicsWorldGeneration", 1U},
					{"idempotencyKey", "rag"}, {"correlationId", "rag"}, {"expectedGeneration", 1U}});
			context.Expect(!rejectedMotion.ok && acceptedRagdoll.ok,
				"Runtime schemas must require the bounded motion database identity and accept a complete typed ragdoll envelope.");

			nlohmann::json oversizedPayload = nlohmann::json::object();
			oversizedPayload["nested"] = nlohmann::json::array();
			for (std::size_t index = 0U; index <= kAnimationAutomationMaximumItems; ++index)
			{
				oversizedPayload["nested"].push_back(index);
			}
			const AutomationDispatchResult nestedOverflow = ValidateAnimationAutomationRequest(
				"animation.controller.edit",
				{
					{"assetId", "controller"},
					{"idempotencyKey", "key"},
					{"expectedGeneration", 1U},
					{"payload", oversizedPayload}
				});
			const AutomationDispatchResult malformedCameraVector = ValidateAnimationAutomationRequest(
				"animation.camera.capture",
				{{"source", "custom"}, {"position", nlohmann::json::array({0.0, 1.0})}});
			context.Expect(
				!nestedOverflow.ok && nestedOverflow.error == ErrorCategory::Validation
					&& !malformedCameraVector.ok && malformedCameraVector.error == ErrorCategory::Validation,
				"Nested payloads and typed camera vectors must remain bounded recursively.");
		});
	}
}
