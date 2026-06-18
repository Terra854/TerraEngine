#include "Runtime/ExportStartupCoordinator.h"

#include <utility>

namespace TerraEngine::Runtime {
	namespace {
		void AddGate(BootSummary& summary, std::string name, bool passed, std::string passedMessage, std::string failedMessage) {
			summary.requiredSystems.push_back({
				std::move(name),
				passed ? BootStepStatus::Passed : BootStepStatus::Failed,
				passed ? std::move(passedMessage) : std::move(failedMessage)
			});
		}

		bool HasFailedGate(const BootSummary& summary) {
			for (const BootSystemResult& result : summary.requiredSystems)
			{
				if (result.status == BootStepStatus::Failed)
				{
					return true;
				}
			}

			return false;
		}
	}

	ExportStartupCoordinatorResult CoordinateExportStartup(const ExportStartupCoordinatorInput& input) {
		ExportStartupCoordinatorResult result{};
		BootSummary& summary = result.summary;
		summary.projectName = input.projectName;
		summary.version = input.version;
		summary.contentSource = input.contentSource;
		summary.startupScene = input.startupSceneConfigured ? input.startupScene : std::string("<none>");
		summary.renderProfile = input.renderProfile;
		summary.validationEnabled = input.validationEnabled;
		summary.debugEnabled = input.debugEnabled;

		// Exported builds must mount content before metadata or scenes can be
		// trusted. Editor sessions still report the gate as passed because they
		// read directly from the project filesystem instead of a TEPAK mount.
		AddGate(
			summary,
			"Content",
			!input.exportRuntime || input.contentMounted,
			input.exportRuntime ? "Export content mounted" : "Project filesystem content",
			"Export content archive was not mounted");

		AddGate(
			summary,
			"Metadata",
			input.metadataLoaded,
			"Project metadata loaded",
			"Project metadata was not loaded");

		AddGate(
			summary,
			"StartupScene",
			!input.exportRuntime || (input.startupSceneConfigured && input.startupSceneLoaded),
			input.startupSceneConfigured ? "Startup scene loaded" : "No startup scene required",
			input.startupSceneConfigured ? "Startup scene failed to load" : "Startup scene was not configured");

		for (const ExportStartupSystemCheck& check : input.requiredSystems)
		{
			// Required systems gate runtime entry; optional systems are recorded as
			// warnings so validation mode can still show partial readiness.
			summary.requiredSystems.push_back({
				check.name,
				check.initialized ? BootStepStatus::Passed : (check.required ? BootStepStatus::Failed : BootStepStatus::Warning),
				check.initialized ? "Initialized" : (check.detail.empty() ? "Not initialized" : check.detail)
			});
		}

		summary.runtimeEntryAllowed = !HasFailedGate(summary);
		summary.runtimeEntryReason = summary.runtimeEntryAllowed
			? "All startup gates passed."
			: "One or more startup gates failed.";
		return result;
	}
}
