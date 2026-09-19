// NavigationAutomationProvider.cpp - bounded schemas for ground navigation.
#include "NavigationAutomationProvider.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		using Json = nlohmann::json;

		Json EmptySchema()
		{
			return {{"type", "object"}, {"schemaVersion", 1U}, {"properties", Json::object()}, {"additionalProperties", false}};
		}

		Json ResultSchema(std::initializer_list<std::string_view> fields)
		{
			Json properties = {{"schemaVersion", {{"type", "integer"}, {"const", 1U}}},
				{"maxItems", {{"type", "integer"}, {"maximum", 1024U}}},
				{"commitReceipt", {{"type", "object"}}}};
			for(const std::string_view field : fields)
				properties[std::string(field)] = {{"type", "object"}};
			return {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", true}};
		}

		Json InputSchema(std::string_view name)
		{
			Json schema = EmptySchema();
			const Json agent = {{"type", "object"}, {"required", {"worldId", "worldGeneration", "objectId", "lifetimeGeneration", "attachmentGeneration", "runtimeSlot", "runtimeSlotGeneration"}},
				{"properties", {{"worldId", {{"type", "integer"}, {"minimum", 1U}}}, {"worldGeneration", {{"type", "integer"}, {"minimum", 1U}}},
					{"objectId", {{"type", "string"}, {"minLength", 32U}, {"maxLength", 32U}}}, {"lifetimeGeneration", {{"type", "integer"}, {"minimum", 1U}}},
					{"attachmentGeneration", {{"type", "integer"}, {"minimum", 1U}}}, {"runtimeSlot", {{"type", "integer"}, {"minimum", 0U}}},
					{"runtimeSlotGeneration", {{"type", "integer"}, {"minimum", 1U}}}}}, {"additionalProperties", false}};
			const Json owner = {{"type", "object"}, {"required", {"worldId", "worldGeneration", "ownerId", "ownerGeneration"}},
				{"properties", {{"worldId", {{"type", "integer"}, {"minimum", 1U}}},
					{"worldGeneration", {{"type", "integer"}, {"minimum", 1U}}},
					{"ownerId", {{"type", "integer"}, {"minimum", 1U}}},
					{"ownerGeneration", {{"type", "integer"}, {"minimum", 1U}}}}}, {"additionalProperties", false}};
			if(name == "navigation.route.request") {
				schema["properties"] = {{"start", {{"type", "object"}}}, {"destination", {{"type", "object"}}},
					{"owner", owner}, {"profile", {{"type", "object"}}},
					{"allowPartial", {{"type", "boolean"}}}, {"maxItems", {{"type", "integer"}, {"minimum", 1U}, {"maximum", 4096U}}}};
				schema["required"] = {"owner", "start", "destination", "profile"};
			} else if(name == "navigation.route.read" || name == "navigation.route.cancel") {
				const Json request = {{"type", "object"}, {"required", {"owner", "slotIndex", "slotGeneration"}},
					{"properties", {{"owner", owner}, {"slotIndex", {{"type", "integer"}, {"minimum", 0U}}},
						{"slotGeneration", {{"type", "integer"}, {"minimum", 1U}}}}}, {"additionalProperties", false}};
				schema["properties"] = {{"request", request},
					{"maxItems", {{"type", "integer"}, {"minimum", 1U}, {"maximum", 4096U}}}};
				schema["required"] = {"request"};
			} else if(name == "navigation.agent.bind") {
				schema["properties"] = {{"objectId", {{"type", "string"}, {"minLength", 32U}, {"maxLength", 32U}}}};
				schema["required"] = {"objectId"};
			} else if(name == "navigation.agent.destination.location") {
				schema["properties"] = {{"agent", agent}, {"location", {{"type", "object"}}}, {"expectedConfigurationRevision", {{"type", "integer"}}},
					{"expectedIntentSequence", {{"type", "integer"}}}, {"producerOrderingToken", {{"type", "integer"}, {"minimum", 1U}}}, {"correlationId", {{"type", "integer"}, {"minimum", 1U}}}};
				schema["required"] = {"agent", "location", "producerOrderingToken", "correlationId"};
			} else if(name == "navigation.agent.destination.object") {
				schema["properties"] = {{"agent", agent}, {"target", {{"type", "object"}}}, {"expectedConfigurationRevision", {{"type", "integer"}}},
					{"expectedIntentSequence", {{"type", "integer"}}}, {"producerOrderingToken", {{"type", "integer"}, {"minimum", 1U}}}, {"correlationId", {{"type", "integer"}, {"minimum", 1U}}}};
				schema["required"] = {"agent", "target", "producerOrderingToken", "correlationId"};
			} else if(name == "navigation.agent.stop" || name == "navigation.agent.receipt" || name == "navigation.agent.status" || name == "navigation.agent.guidance") {
				schema["properties"] = {{"agent", agent}, {"expectedConfigurationRevision", {{"type", "integer"}}}, {"expectedIntentSequence", {{"type", "integer"}}},
					{"producerOrderingToken", {{"type", "integer"}, {"minimum", 1U}}}, {"correlationId", {{"type", "integer"}, {"minimum", 1U}}}};
				schema["required"] = name == "navigation.agent.stop" ? Json{"agent", "producerOrderingToken", "correlationId"}
					: name == "navigation.agent.receipt" ? Json{"agent", "correlationId"} : Json{"agent"};
			} else if(name == "navigation.cache.status")
				schema["properties"] = {{"jobId", {{"type", "string"}, {"minLength", 1}}}};
			return schema;
		}

		void ApplyAccess(AutomationCapabilityDescriptor& descriptor, bool read)
		{
			descriptor.permission = read ? AutomationPermission::Read : AutomationPermission::ProjectWrite;
			descriptor.mutationMode = read ? MutationMode::ReadOnly : MutationMode::SceneMutation;
		}
	}

	NavigationAutomationProvider::NavigationAutomationProvider(AutomationHandlerResolver resolver)
		: resolveHandler(std::move(resolver))
	{
	}

	std::string_view NavigationAutomationProvider::GetProviderId() const
	{
		return "navigation";
	}

	std::vector<AutomationCapabilityRegistration> NavigationAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> result;
		for(const std::string_view name : {"navigation.settings.get", "navigation.diagnostics.get",
			"navigation.route.request", "navigation.route.read", "navigation.route.cancel", "navigation.cache.prepare",
			"navigation.cache.status", "navigation.agent.bind", "navigation.agent.destination.location",
			"navigation.agent.destination.object", "navigation.agent.stop", "navigation.agent.receipt",
			"navigation.agent.status", "navigation.agent.guidance"})
		{
			// Binding only reads the immutable agent publication.  It intentionally
			// has no authority to discover or create an attachment in live ECS.
			const bool read = name == "navigation.settings.get" || name == "navigation.diagnostics.get"
				|| name == "navigation.route.read"
				|| name == "navigation.cache.status" || name == "navigation.agent.bind"
				|| name == "navigation.agent.receipt"
				|| name == "navigation.agent.status" || name == "navigation.agent.guidance";
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::string(name);
			descriptor.category = "navigation";
			descriptor.description = "Provider-neutral ground navigation capability.";
			descriptor.inputSchema = InputSchema(name);
			descriptor.resultSchema = ResultSchema({"snapshot", "route", "status", "agent", "guidance"});
			ApplyAccess(descriptor, read);
			descriptor.availability.requiresProject = true;
			descriptor.execution.workerPreparation = !read;
			descriptor.execution.protectedCommit = !read;
			descriptor.execution.visibilityBoundary = read ? "immutable_navigation_snapshot" : "navigation_commit_receipt";
			descriptor.execution.preparationReads = {"NavigationEligibleSourceIndex", "NavigationCoverageRoot", "NavigationAgentState"};
			descriptor.execution.commitWrites = read ? std::vector<std::string>{}
				: std::vector<std::string>{"NavigationCommandQueue", "NavigationResults"};
			result.push_back({std::move(descriptor), resolveHandler(name), {}});
		}
		return result;
	}
}
