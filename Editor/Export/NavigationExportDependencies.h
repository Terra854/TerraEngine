// NavigationExportDependencies.h - source-free navigation export closure.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace TerraEngine::Export
{
	struct NavigationExportDependency
	{
		std::string logicalPath;
		bool optionalCache = false;
	};

	struct NavigationExportIssue
	{
		std::string code;
		std::string path;
		std::string message;
	};

	struct NavigationExportClosure
	{
		bool sourceFree = true;
		std::vector<NavigationExportDependency> dependencies;
		std::vector<NavigationExportIssue> issues;

		[[nodiscard]] bool Complete() const noexcept { return issues.empty(); }
	};

	class NavigationExportDependencyResolver final
	{
	public:
		// Discovery reads only the already-staged runtime tree. Editor source files
		// and absolute project paths can therefore never leak into the archive.
		[[nodiscard]] static NavigationExportClosure Resolve(
			const std::filesystem::path& stagedContentRoot);
		[[nodiscard]] static bool WriteRuntimeManifest(
			const NavigationExportClosure& closure,
			const std::filesystem::path& stagedContentRoot,
			std::string& outError);
	};
}
