// AuthoringAutomationProvider.cpp - Typed schemas for advanced editor surfaces.

#include "AuthoringAutomationProvider.h"
#include "../../Project/EditorPreferences.h"
#include "../../Project/ProjectSettings.h"
#include "Engine/Profiling/ProfilerCapture.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		AutomationCapabilityRegistration Make(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string category,
			std::vector<std::string> inputs,
			std::vector<std::string> outputs,
			AutomationPermission permission,
			AutomationApprovalPolicy approval,
			bool requiresProject,
			bool requiresScene = false,
			bool sceneView = false);

		nlohmann::json SettingKeySchema(bool projectScope)
		{
			nlohmann::json values = nlohmann::json::array();
			if (projectScope)
				for (const std::string_view key : TerraEngine::Editor::Settings::kProjectAutomationSettingKeys) values.push_back(key);
			else
				for (const std::string_view key : TerraEngine::Editor::Settings::kEditorAutomationSettingKeys) values.push_back(key);
			return {{"type", "string"}, {"enum", std::move(values)}};
		}

		AutomationCapabilityRegistration MakeSetting(
			const AutomationHandlerResolver& resolver,
			std::string name,
			bool projectScope,
			bool write)
		{
			auto registration = Make(
				resolver,
				std::move(name),
				"settings",
				write ? std::vector<std::string>{"key", "value"} : std::vector<std::string>{"key"},
				write ? std::vector<std::string>{"value", "allowedKeys", "persisted"} : std::vector<std::string>{"value", "allowedKeys"},
				write ? (projectScope ? AutomationPermission::ProjectWrite : AutomationPermission::GlobalWrite) : AutomationPermission::Read,
				write && !projectScope ? AutomationApprovalPolicy::HighImpact : AutomationApprovalPolicy::None,
				projectScope);
		registration.descriptor.inputSchema["properties"]["key"] = SettingKeySchema(projectScope);
		if (write)
		{
			// Each allowlisted editor setting is currently boolean; the project
			// display name is the only project-scoped string setting.
			registration.descriptor.inputSchema["properties"]["value"] = projectScope
				? nlohmann::json{{"type", "string"}, {"minLength", 1}}
				: nlohmann::json{{"type", "boolean"}};
		}
		return registration;
	}

		AutomationCapabilityRegistration Make(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string category,
			std::vector<std::string> inputs,
			std::vector<std::string> outputs,
			AutomationPermission permission,
			AutomationApprovalPolicy approval,
			bool requiresProject,
			bool requiresScene,
			bool sceneView)
		{
			nlohmann::json inputProperties = nlohmann::json::object();
			for (const std::string& input : inputs) inputProperties[input] = {{"description", "Semantic editor-owned value."}};
			nlohmann::json outputProperties = nlohmann::json::object();
			for (const std::string& output : outputs) outputProperties[output] = {{"description", "Structured editor-owned result."}};

			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::move(name);
			descriptor.category = std::move(category);
			descriptor.description = "Advanced semantic authoring operation.";
			descriptor.inputSchema = {{"type", "object"}, {"properties", std::move(inputProperties)}, {"additionalProperties", false}};
			descriptor.resultSchema = {{"type", "object"}, {"properties", std::move(outputProperties)}};
			descriptor.permission = permission;
			descriptor.approvalPolicy = approval;
			descriptor.mutationMode = permission == AutomationPermission::Read ? MutationMode::ReadOnly : MutationMode::ProjectMutation;
			descriptor.availability.requiresProject = requiresProject;
			descriptor.availability.requiresScene = requiresScene;
			descriptor.execution.workerPreparation = true;
			descriptor.execution.protectedCommit = permission != AutomationPermission::Read;
			descriptor.execution.visibilityBoundary = permission == AutomationPermission::Read ? "immutable_snapshot" : "editor_authoring_commit";
			descriptor.execution.preparationReads = {"EditorAutomationSession"};
			descriptor.execution.commitWrites = permission == AutomationPermission::Read ? std::vector<std::string>{} : std::vector<std::string>{"ProjectState", "EditorAutomationSession"};
			const std::string toolName = descriptor.name;
			AutomationCapabilityRegistration registration{std::move(descriptor), resolver(toolName), {}};
			if (sceneView)
			{
				registration.availabilityPredicate = [availability = registration.descriptor.availability](const AutomationStateSnapshot& state) mutable {
					auto result = availability;
					result.available = state.projectAvailable && state.sceneAvailable && state.rendererAvailable && state.availableServices.contains("SceneView");
					if (!result.available) result.reason = "The Scene View authoring surface is unavailable.";
					return result;
				};
			}
			return registration;
		}

		nlohmann::json CanonicalWorldPositionSchema()
		{
			// Cell axes travel as decimal strings because a JSON number cannot
			// preserve every signed 64-bit cell identity. Offsets are bounded,
			// finite doubles in the cell-local working frame.
			return {
				{"type", "object"},
				{"required", nlohmann::json::array({"cell", "offsetMetres"})},
				{"additionalProperties", false},
				{"properties", {
					{"cell", {{"type", "object"}, {"required", nlohmann::json::array({"x", "y", "z"})},
						{"additionalProperties", false}, {"properties", {
							{"x", {{"type", "string"}}}, {"y", {{"type", "string"}}}, {"z", {{"type", "string"}}}}}}},
					{"offsetMetres", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3},
						{"items", {{"type", "number"}}}}}
				}}};
		}

		nlohmann::json SceneViewPerformanceSettingsSchema()
		{
			// Keep the automation schema aligned with the versioned performance
			// settings contract. The handler validates the complete candidate again
			// before persistence, so schema-aware and legacy clients receive the
			// same atomic behavior.
			return {
				{"type", "object"},
				{"additionalProperties", false},
				{"required", nlohmann::json::array({
					"largeSceneMode", "largeSceneObjectThreshold", "visibleMarkerBudget",
					"markerPresentationBatchSize", "densityRadiusPixels", "densityThreshold",
					"maximumMarkerDistance", "minimumProjectedSizePixels", "physicsMarkerMode",
					"densePrimitiveTier"})},
				{"properties", {
					{"largeSceneMode", {{"type", "string"}, {"enum", nlohmann::json::array({"auto", "on", "off"})}}},
					{"largeSceneObjectThreshold", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000000}}},
					{"visibleMarkerBudget", {{"type", "integer"}, {"minimum", 64}, {"maximum", 100000}}},
					{"markerPresentationBatchSize", {{"type", "integer"}, {"minimum", 32}, {"maximum", 8192}}},
					{"densityRadiusPixels", {{"type", "number"}, {"minimum", 4.0}, {"maximum", 128.0}}},
					{"densityThreshold", {{"type", "integer"}, {"minimum", 2}, {"maximum", 256}}},
					{"maximumMarkerDistance", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1000000000.0}}},
					{"minimumProjectedSizePixels", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 64.0}}},
					{"physicsMarkerMode", {{"type", "string"}, {"enum", nlohmann::json::array({"selected_only", "clustered", "all"})}}},
					{"densePrimitiveTier", {{"type", "string"}, {"enum", nlohmann::json::array({"full", "simple", "point"})}}}
				}}
			};
		}

		AutomationCapabilityRegistration MakeSceneViewSettings(
			const AutomationHandlerResolver& resolver,
			bool write)
		{
			auto registration = Make(
				resolver,
				write ? "scene_view.settings.set" : "scene_view.settings.get",
				"scene_view",
				write ? std::vector<std::string>{"settings"} : std::vector<std::string>{},
				{"configured", "effective", "persisted"},
				write ? AutomationPermission::GlobalWrite : AutomationPermission::Read,
				write ? AutomationApprovalPolicy::HighImpact : AutomationApprovalPolicy::None,
				false);
			if (write)
			{
				registration.descriptor.inputSchema["required"] = nlohmann::json::array({"settings"});
				registration.descriptor.inputSchema["properties"]["settings"] = SceneViewPerformanceSettingsSchema();
				registration.descriptor.mutationMode = MutationMode::EditorSelection;
				registration.descriptor.execution.visibilityBoundary = "editor_preferences_atomic_replace";
				registration.descriptor.execution.commitWrites = {"EditorPreferences", "SceneViewPerformanceState"};
			}
			return registration;
		}

		// Capture start has one V1 schema. Bounds are validated again by the
		// service so omitted optional controls and explicit controls share one
		// safe path without selecting a capture format.
		AutomationCapabilityRegistration MakeProfilerCaptureStart(const AutomationHandlerResolver& resolver)
		{
			auto registration = Make(
				resolver,
				"profiler.capture.start",
				"profiler",
				{"captureLevel", "maximumChunkFrames", "maximumChunkBytes", "sampleIntervalFrames", "maxFrames"},
				{"jobId", "state", "captureLevel", "formatIdentifier", "formatVersion", "bounds"},
				AutomationPermission::ProjectWrite,
				AutomationApprovalPolicy::None,
				true);
			registration.descriptor.inputSchema["properties"]["captureLevel"] = {{"type", "string"}, {"enum", nlohmann::json::array({"detailed", "sampled", "aggregate"})}};
			// The journal format is a result fact, not a caller-selectable input.
			registration.descriptor.resultSchema["properties"]["formatIdentifier"] = {{"const", "TerraEngineProfilerJournalV1"}};
			registration.descriptor.resultSchema["properties"]["formatVersion"] = {{"const", 1u}};
			registration.descriptor.inputSchema["properties"]["maximumChunkFrames"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}};
			registration.descriptor.inputSchema["properties"]["maximumChunkBytes"] = {{"type", "integer"}, {"minimum", 1024}, {"maximum", 8388608}};
			registration.descriptor.inputSchema["properties"]["sampleIntervalFrames"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 1024}};
			registration.descriptor.inputSchema["properties"]["maxFrames"] = {
				{"type", "integer"},
				{"minimum", 0},
				{"maximum", 4096},
				{"default", 0},
				{"description", "Maximum retained frames; zero keeps the complete capture."}};
			return registration;
		}
	}

	AuthoringAutomationProvider::AuthoringAutomationProvider(AutomationHandlerResolver resolver) : resolveHandler(std::move(resolver)) {}
	std::string_view AuthoringAutomationProvider::GetProviderId() const { return "advanced_authoring"; }

	std::vector<AutomationCapabilityRegistration> AuthoringAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> result;
		result.push_back(Make(resolveHandler, "build_profile.list", "build_profile", {}, {"profiles"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.get", "build_profile", {"profileId"}, {"profile"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.create", "build_profile", {"name"}, {"profileId", "validation"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.update", "build_profile", {"profileId", "changes"}, {"profileId", "validation"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.duplicate", "build_profile", {"profileId", "name"}, {"profileId", "validation"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "build_profile.prepare_delete", "build_profile", {"profileId"}, {"impact", "approvalId"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::Destructive, true));

		result.push_back(MakeSetting(resolveHandler, "settings.project.get", true, false));
		result.push_back(MakeSetting(resolveHandler, "settings.project.set", true, true));
		result.push_back(MakeSetting(resolveHandler, "settings.editor.get", false, false));
		result.push_back(MakeSetting(resolveHandler, "settings.editor.set", false, true));

		result.push_back(Make(resolveHandler, "scene_view.state", "scene_view", {}, {"state"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(MakeSceneViewSettings(resolveHandler, false));
		result.push_back(MakeSceneViewSettings(resolveHandler, true));
		auto setSceneCamera = Make(resolveHandler, "scene_view.camera.set", "scene_view", {"position", "focus"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true);
		// This is deliberately a breaking canonical-only command: accepting a
		// three-number world vector would discard the cell component of a far-away
		// scene camera before the protected authoring boundary sees it.
		setSceneCamera.descriptor.description = "Set the Scene View camera using canonical WorldPosition values.";
		setSceneCamera.descriptor.inputSchema = {
			{"type", "object"},
			{"required", nlohmann::json::array({"position", "focus"})},
			{"additionalProperties", false},
			{"properties", {{"position", CanonicalWorldPositionSchema()}, {"focus", CanonicalWorldPositionSchema()}}}};
		result.push_back(std::move(setSceneCamera));
		result.push_back(Make(resolveHandler, "scene_view.frame_object", "scene_view", {"target"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.tool.set", "scene_view", {"tool"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.orientation.set", "scene_view", {"orientation"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.snapping.set", "scene_view", {"enabled"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));
		result.push_back(Make(resolveHandler, "scene_view.visualization.set", "scene_view", {"mode"}, {"state"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true, true, true));

		result.push_back(Make(resolveHandler, "profiler.window.visibility.set", "profiler", {"visible"}, {"visible", "editorFrameIndex", "editorFrameTimeMs", "profilerWindowDrawFrameIndex", "profilerWindowDrawMs", "profilerWindowDrawValid", "imguiRenderFrameIndex", "imguiRenderMs", "imguiRenderValid", "presentUiFrameIndex", "presentUiMs", "presentUiValid"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(MakeProfilerCaptureStart(resolveHandler));
		result.push_back(Make(resolveHandler, "profiler.capture.stop", "profiler", {}, {"jobId", "state", "finishCaptureSubmissionMilliseconds"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "profiler.capture.list", "profiler", {}, {"artifacts"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "profiler.capture.load", "profiler", {"artifactId"}, {"jobId", "state"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		result.push_back(Make(resolveHandler, "profiler.capture.summary", "profiler", {"artifactId"}, {"summary"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true));
		auto health = Make(resolveHandler, "profiler.capture.health", "profiler", {}, {"formatIdentifier", "formatVersion", "projectEpoch", "sessionArtifactId", "committedFrames", "residentBytes", "residentHighWaterBytes", "pendingBytes", "persistenceLagFrames", "persistenceLagHighWaterFrames", "collectionNanoseconds", "sealingNanoseconds", "queueingNanoseconds", "persistenceNanoseconds", "finalizationNanoseconds", "persistedBytes", "persistenceOperationCount", "persistenceThroughputBytesPerSecond", "droppedDetailRecords", "rejectedCoreFrames", "terminalFailure"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true);
		health.descriptor.resultSchema["properties"]["formatIdentifier"] = {{"const", "TerraEngineProfilerJournalV1"}};
		health.descriptor.resultSchema["properties"]["formatVersion"] = {{"const", 1u}};
		health.descriptor.resultSchema["properties"]["projectEpoch"] = {{"type", "integer"}, {"minimum", 0u}};
		result.push_back(std::move(health));
		auto index = Make(resolveHandler, "profiler.capture.index", "profiler", {"offset", "limit"}, {"formatIdentifier", "formatVersion", "projectEpoch", "state", "offset", "limit", "total", "frames"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true);
		// The handler may only request one bounded V1 index page.  Publishing the
		// same limits in discovery prevents a malformed automation request from
		// reaching the reader as an untyped or unbounded value.
		index.descriptor.inputSchema["properties"]["offset"] = {{"type", "integer"}, {"minimum", 0u}};
		index.descriptor.inputSchema["properties"]["limit"] = {{"type", "integer"}, {"minimum", 1u}, {"maximum", 512u}, {"default", 128u}};
		index.descriptor.resultSchema["properties"]["formatIdentifier"] = {{"const", "TerraEngineProfilerJournalV1"}};
		index.descriptor.resultSchema["properties"]["formatVersion"] = {{"const", 1u}};
		index.descriptor.resultSchema["properties"]["projectEpoch"] = {{"type", "integer"}, {"minimum", 0u}};
		index.descriptor.resultSchema["properties"]["state"] = {{"type", "string"}, {"enum", nlohmann::json::array({"queued", "completed"})}};
		result.push_back(std::move(index));
		auto detail = Make(resolveHandler, "profiler.capture.detail", "profiler", {"frameIndex"}, {"formatIdentifier", "formatVersion", "projectEpoch", "state", "frameIndex", "durationMs", "sampleCount"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true);
		// Frame identity is an unsigned journal ordinal, never a path or a whole
		// history selector.  The detail handler therefore decodes one segment only.
		detail.descriptor.inputSchema["properties"]["frameIndex"] = {{"type", "integer"}, {"minimum", 0u}};
		detail.descriptor.inputSchema["required"] = nlohmann::json::array({"frameIndex"});
		detail.descriptor.resultSchema["properties"]["formatIdentifier"] = {{"const", "TerraEngineProfilerJournalV1"}};
		detail.descriptor.resultSchema["properties"]["formatVersion"] = {{"const", 1u}};
		detail.descriptor.resultSchema["properties"]["projectEpoch"] = {{"type", "integer"}, {"minimum", 0u}};
		detail.descriptor.resultSchema["properties"]["state"] = {{"type", "string"}, {"enum", nlohmann::json::array({"queued", "completed"})}};
		result.push_back(std::move(detail));
		auto operation = Make(resolveHandler, "profiler.capture.operation", "profiler", {"operationId"}, {"formatIdentifier", "formatVersion", "projectEpoch", "operation", "operations", "finalization", "status", "lastError"}, AutomationPermission::Read, AutomationApprovalPolicy::None, true);
		// operationId is optional: omitted returns the compact current operation
		// set, while a supplied id reads one terminal state without a history scan.
		operation.descriptor.inputSchema["properties"]["operationId"] = {{"type", "integer"}, {"minimum", 1u}};
		operation.descriptor.resultSchema["properties"]["formatIdentifier"] = {{"const", "TerraEngineProfilerJournalV1"}};
		operation.descriptor.resultSchema["properties"]["formatVersion"] = {{"const", 1u}};
		operation.descriptor.resultSchema["properties"]["projectEpoch"] = {{"type", "integer"}, {"minimum", 0u}};
		result.push_back(std::move(operation));
		result.push_back(Make(resolveHandler, "editor.commands.list", "editor_commands", {}, {"commands"}, AutomationPermission::Read, AutomationApprovalPolicy::None, false));
		result.push_back(Make(resolveHandler, "editor.commands.invoke", "editor_commands", {"commandId"}, {"commandId", "completed"}, AutomationPermission::ProjectWrite, AutomationApprovalPolicy::None, false));
		return result;
	}
}
