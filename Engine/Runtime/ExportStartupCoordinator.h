// ExportStartupCoordinator.h - deterministic exported startup validation
//
// This coordinator is intentionally data-oriented: runtime systems report the
// facts they already know, and the coordinator turns those facts into one boot
// summary plus a single runtime-entry decision. Tests can exercise the startup
// policy without creating windows, render devices, or managed runtimes.

#pragma once

#include "Core/Export.h"
#include "Runtime/BootSummary.h"

#include <string>
#include <vector>

namespace TerraEngine::Runtime {

struct ExportStartupSystemCheck
{
	std::string name;
	bool required = true;
	bool initialized = false;
	std::string detail;
};

struct ExportStartupCoordinatorInput
{
	bool exportRuntime = false;
	bool contentMounted = false;
	bool metadataLoaded = false;
	bool startupSceneConfigured = false;
	bool startupSceneLoaded = false;
	bool validationEnabled = false;
	bool debugEnabled = false;
	std::string projectName = "TerraEngine";
	std::string version = "1.0.0";
	std::string contentSource = "<none>";
	std::string startupScene = "<none>";
	std::string renderProfile = "profile=Default";
	std::vector<ExportStartupSystemCheck> requiredSystems;
};

struct ExportStartupCoordinatorResult
{
	BootSummary summary;

	bool CanEnterRuntime() const
	{
		return summary.runtimeEntryAllowed;
	}
};

TERRAENGINE_API ExportStartupCoordinatorResult CoordinateExportStartup(const ExportStartupCoordinatorInput& input);

}
