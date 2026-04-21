#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct RecentProjectEntry {
	std::string name;
	std::string path;
	long long lastOpened = 0;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(RecentProjectEntry, name, path, lastOpened)
};

struct EditorPreferences {
	std::vector<RecentProjectEntry> recentProjects;
	std::string theme = "Dark";
	std::string externalScriptEditorCommand;
	bool autoBuildScripts = true;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(EditorPreferences, recentProjects, theme, externalScriptEditorCommand, autoBuildScripts)
};
