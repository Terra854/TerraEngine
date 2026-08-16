// HistoryPlayModeAutomationProvider.cpp - Typed safe-edit and play contracts.

#include "HistoryPlayModeAutomationProvider.h"

#include <utility>

namespace TerraEngine::Editor::Automation
{
	namespace
	{
		AutomationCapabilityRegistration Make(
			const AutomationHandlerResolver& resolver,
			std::string name,
			std::string category,
			MutationMode mode,
			bool requiresScene,
			std::vector<std::string> allowedPlayStates = {})
		{
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::move(name);
			descriptor.category = std::move(category);
			descriptor.description = "Semantic editor operation committed through the protected automation boundary.";
			descriptor.inputSchema = {{"type", "object"}, {"additionalProperties", true}};
			descriptor.resultSchema = {{"type", "object"}};
			descriptor.permission = mode == MutationMode::ReadOnly ? AutomationPermission::Read : AutomationPermission::ProjectWrite;
			descriptor.mutationMode = mode;
			descriptor.availability.requiresProject = requiresScene;
			descriptor.availability.requiresScene = requiresScene;
			descriptor.availability.allowedPlayStates = std::move(allowedPlayStates);
			descriptor.execution.workerPreparation = true;
			descriptor.execution.protectedCommit = mode != MutationMode::ReadOnly;
			descriptor.execution.visibilityBoundary = mode == MutationMode::ReadOnly ? "immutable_snapshot" : "post_editor_automation_flush";
			descriptor.execution.preparationReads = {"EditorAutomationSession", "ObjectStorage", "EditorHistory"};
			descriptor.execution.commitReads = {"ObjectStorage", "EditorHistory"};
			descriptor.execution.commitWrites = mode == MutationMode::ReadOnly
				? std::vector<std::string>{}
				: std::vector<std::string>{"SceneMutationQueue", "EditorHistory", "EditorAutomationSession"};
			const std::string toolName = descriptor.name;
			if (descriptor.category == "play")
			{
				// The original state fields remain available. Restoration is expanded
				// only with optional evidence so older MCP clients retain their exact
				// response contract while newer clients can diagnose a stale restore.
				descriptor.resultSchema = {{"type", "object"}, {"properties", {
					{"currentState", {{"type", "string"}}},
					{"pendingTransition", {{"type", nlohmann::json::array({"string", "null"})}}},
					{"stepsCompleted", {{"type", "integer"}}},
					{"restoration", {{"type", "object"}}},
					{"readiness", {{"type", "object"}}}
				}}};
			}
			return {std::move(descriptor), resolver(toolName), {}};
		}
	}

	HistoryPlayModeAutomationProvider::HistoryPlayModeAutomationProvider(AutomationHandlerResolver resolver)
		: resolveHandler(std::move(resolver))
	{
	}

	std::string_view HistoryPlayModeAutomationProvider::GetProviderId() const { return "history_play_mode"; }

	std::vector<AutomationCapabilityRegistration> HistoryPlayModeAutomationProvider::GetCapabilities()
	{
		std::vector<AutomationCapabilityRegistration> capabilities;
		AutomationCapabilityRegistration transaction = Make(resolveHandler, "transaction.execute", "transaction", MutationMode::SceneMutation, true, {"stopped"});
		// Transactions are a breaking, versioned wire contract. Advertising the
		// exact format prevents clients from assuming legacy float-vector writes.
		transaction.descriptor.inputSchema = {
			{"type", "object"},
			{"properties", {{"format", {{"const", "TerraSpatialTransactionV1"}}},
				{"operations", {{"type", "array"}, {"minItems", 1}}}}},
			{"required", nlohmann::json::array({"format", "operations"})},
			{"additionalProperties", true}};
		capabilities.push_back(std::move(transaction));
		capabilities.push_back(Make(resolveHandler, "history.state", "history", MutationMode::ReadOnly, true, {"stopped"}));
		capabilities.push_back(Make(resolveHandler, "history.undo", "history", MutationMode::SceneMutation, true, {"stopped"}));
		capabilities.push_back(Make(resolveHandler, "history.redo", "history", MutationMode::SceneMutation, true, {"stopped"}));
		capabilities.push_back(Make(resolveHandler, "play.state", "play", MutationMode::ReadOnly, false));
		capabilities.push_back(Make(resolveHandler, "play.enter", "play", MutationMode::ProjectMutation, true));
		capabilities.push_back(Make(resolveHandler, "play.pause", "play", MutationMode::ProjectMutation, true));
		capabilities.push_back(Make(resolveHandler, "play.resume", "play", MutationMode::ProjectMutation, true));
		capabilities.push_back(Make(resolveHandler, "play.step", "play", MutationMode::ProjectMutation, true));
		capabilities.push_back(Make(resolveHandler, "play.exit", "play", MutationMode::ProjectMutation, true));
		return capabilities;
	}
}
