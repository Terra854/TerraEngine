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
			const Json owner = {{"type", "object"}, {"required", {"worldId", "worldGeneration", "ownerId", "ownerGeneration"}},
				{"properties", {{"worldId", {{"type", "integer"}, {"minimum", 1U}}},
					{"worldGeneration", {{"type", "integer"}, {"minimum", 1U}}},
					{"ownerId", {{"type", "integer"}, {"minimum", 1U}}},
					{"ownerGeneration", {{"type", "integer"}, {"minimum", 1U}}}}}, {"additionalProperties", false}};
			if(name == "navigation.route.request") {
				schema["properties"] = {{"start", {{"type", "object"}}}, {"destination", {{"type", "object"}}},
					{"owner", owner}, {"profileId", {{"type", "string"}}},
					{"allowPartial", {{"type", "boolean"}}}, {"maxItems", {{"type", "integer"}, {"minimum", 1U}, {"maximum", 4096U}}}};
				schema["required"] = {"owner", "start", "destination", "profileId"};
			} else if(name == "navigation.route.read" || name == "navigation.route.cancel") {
				const Json request = {{"type", "object"}, {"required", {"owner", "slotIndex", "slotGeneration"}},
					{"properties", {{"owner", owner}, {"slotIndex", {{"type", "integer"}, {"minimum", 0U}}},
						{"slotGeneration", {{"type", "integer"}, {"minimum", 1U}}}}}, {"additionalProperties", false}};
				schema["properties"] = {{"request", request},
					{"maxItems", {{"type", "integer"}, {"minimum", 1U}, {"maximum", 4096U}}}};
				schema["required"] = {"request"};
			} else if(name == "navigation.cache.status")
				schema["properties"] = {{"jobId", {{"type", "string"}, {"minLength", 1}}}};
			else if(name == "navigation.authoring.apply")
				schema["properties"] = {{"operations", {{"type", "array"}, {"maxItems", 1024U}}}};
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
			"navigation.sources.inspect", "navigation.route.request", "navigation.route.read",
			"navigation.route.cancel", "navigation.authoring.apply", "navigation.cache.prepare",
			"navigation.cache.status"})
		{
			const bool read = name == "navigation.settings.get" || name == "navigation.diagnostics.get"
				|| name == "navigation.sources.inspect" || name == "navigation.route.read"
				|| name == "navigation.cache.status";
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::string(name);
			descriptor.category = "navigation";
			descriptor.description = "Provider-neutral ground navigation capability.";
			descriptor.inputSchema = InputSchema(name);
			descriptor.resultSchema = ResultSchema({"snapshot", "route", "status"});
			ApplyAccess(descriptor, read);
			descriptor.availability.requiresProject = true;
			descriptor.execution.workerPreparation = !read;
			descriptor.execution.protectedCommit = !read;
			descriptor.execution.visibilityBoundary = read ? "immutable_navigation_snapshot" : "navigation_commit_receipt";
			descriptor.execution.preparationReads = {"NavigationSourceSnapshot", "NavigationCoverageRoot"};
			descriptor.execution.commitWrites = read ? std::vector<std::string>{}
				: std::vector<std::string>{"NavigationCommandQueue", "NavigationResults"};
			result.push_back({std::move(descriptor), resolveHandler(name), {}});
		}
		return result;
	}
}
