// AudioAutomationProvider.cpp - schemas for queue-backed audio operations.

#include "AudioAutomationProvider.h"

namespace TerraEngine::Editor::Automation {
	namespace {
		AutomationCapabilityRegistration MakeAudioCapability(
			const AutomationHandlerResolver& resolver,
			std::string name,
			MutationMode mode) {
			AutomationCapabilityDescriptor descriptor;
			descriptor.name = std::move(name); descriptor.category = "audio";
			descriptor.description = "Typed audio control/inspection through AudioSnapshot and AudioCommit boundaries.";
			descriptor.inputSchema = {{"type", "object"}, {"additionalProperties", true}};
			descriptor.resultSchema = {{"type", "object"}};
			descriptor.permission = mode == MutationMode::ReadOnly ? AutomationPermission::Read : AutomationPermission::ProjectWrite;
			descriptor.mutationMode = mode; descriptor.availability.requiresProject = true;
			descriptor.execution.workerPreparation = true; descriptor.execution.protectedCommit = mode != MutationMode::ReadOnly;
			descriptor.execution.visibilityBoundary = mode == MutationMode::ReadOnly ? "audio_commit_snapshot" : "next_audio_commit";
			descriptor.execution.preparationReads = {"AudioDiagnostics", "AudioAuthoring"};
			descriptor.execution.commitWrites = mode == MutationMode::ReadOnly ? std::vector<std::string>{} : std::vector<std::string>{"AudioAuthoring"};
		const std::string operation = descriptor.name;
		return {std::move(descriptor), resolver(operation), {}};
		}
	}

	AudioAutomationProvider::AudioAutomationProvider(AutomationHandlerResolver resolver) : resolveHandler(std::move(resolver)) {}
	std::string_view AudioAutomationProvider::GetProviderId() const { return "audio"; }
	std::vector<AutomationCapabilityRegistration> AudioAutomationProvider::GetCapabilities() {
		return {
			MakeAudioCapability(resolveHandler, "audio.control", MutationMode::ProjectMutation),
			MakeAudioCapability(resolveHandler, "audio.mix", MutationMode::ProjectMutation),
			MakeAudioCapability(resolveHandler, "audio.inspect", MutationMode::ReadOnly)
		};
	}
}
