// BootSummary.h - Export runtime startup report
//
// Exported games need a compact, deterministic summary of startup decisions so
// CI logs and manual QA can tell whether startup failed because of content,
// systems, render settings, or runtime entry.

#pragma once

#include "Core/Export.h"

#include <string>
#include <vector>

namespace TerraEngine::Runtime {

enum class BootStepStatus
{
	NotRun,
	Passed,
	Warning,
	Failed
};

struct BootSystemResult
{
	std::string name;
	BootStepStatus status = BootStepStatus::NotRun;
	std::string message;
};

struct BootSummary
{
	std::string projectName = "TerraEngine";
	std::string version = "1.0.0";
	std::string contentSource = "<none>";
	std::string startupScene = "<none>";
	std::string renderProfile = "profile=Default";
	std::vector<BootSystemResult> requiredSystems;
	std::vector<std::string> materialFallbackWarnings;
	bool validationEnabled = false;
	bool debugEnabled = false;
	bool runtimeEntryAllowed = false;
	std::string runtimeEntryReason = "Startup has not run.";
};

TERRAENGINE_API const char* ToString(BootStepStatus status);
TERRAENGINE_API std::string FormatBootSummary(const BootSummary& summary);

}
