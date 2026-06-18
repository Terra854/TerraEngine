// BootSummary.cpp - Export runtime startup report formatting

#include "BootSummary.h"

#include <sstream>

namespace TerraEngine::Runtime {

const char* ToString(BootStepStatus status)
{
	switch (status)
	{
	case BootStepStatus::NotRun: return "not-run";
	case BootStepStatus::Passed: return "pass";
	case BootStepStatus::Warning: return "warning";
	case BootStepStatus::Failed: return "fail";
	default: return "unknown";
	}
}

std::string FormatBootSummary(const BootSummary& summary)
{
	std::ostringstream stream;
	stream
		<< "Boot summary: project=" << summary.projectName
		<< " version=" << summary.version
		<< " contentSource=" << summary.contentSource
		<< " startupScene=" << summary.startupScene
		<< " renderProfile={" << summary.renderProfile << "}"
		<< " validation=" << (summary.validationEnabled ? "on" : "off")
		<< " debug=" << (summary.debugEnabled ? "on" : "off")
		<< " runtimeEntry=" << (summary.runtimeEntryAllowed ? "allowed" : "blocked")
		<< " reason=\"" << summary.runtimeEntryReason << "\"";

	for (const BootSystemResult& system : summary.requiredSystems)
	{
		stream
			<< " system[" << system.name << "]=" << ToString(system.status)
			<< "(\"" << system.message << "\")";
	}

	for (const std::string& warning : summary.materialFallbackWarnings)
	{
		stream << " materialFallback=\"" << warning << "\"";
	}

	return stream.str();
}

}
