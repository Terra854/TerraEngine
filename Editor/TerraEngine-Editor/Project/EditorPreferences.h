// EditorPreferences.h — Editor-wide user preferences
//
// Defines the data structures for persisting editor-wide settings and the list of recently opened projects. it utilizes nlohmann/json for automated serialization of preferences such as theme selection and external script editor commands.

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

// struct EditorPreferences groups related state and operations so callers have one documented contract.
struct EditorPreferences {
	std::vector<RecentProjectEntry> recentProjects;
	std::string theme = "Dark";
	std::string externalScriptEditorCommand;
	bool autoBuildScripts = true;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(EditorPreferences, recentProjects, theme, externalScriptEditorCommand, autoBuildScripts)
};
