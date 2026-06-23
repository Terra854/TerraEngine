#pragma once

#include "Editor/TerraEngine-Editor/Project/ProjectSettingsMetadata.h"

#include <algorithm>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests::ProjectSettings
{
	inline bool ContainsTitle(const std::vector<std::string>& titles, std::string_view title)
	{
		return std::find(titles.begin(), titles.end(), std::string(title)) != titles.end();
	}

	inline std::vector<std::string> BuildWindowMenuTitlesForContractTest()
	{
		const std::vector<std::string> allWindows = {
			"Hierarchy",
			"Inspector",
			"Project Browser",
			"Game View",
			"Scene View",
			std::string(TerraEngine::Editor::ProjectSettings::kProjectSettingsWindowTitle),
			std::string(TerraEngine::Editor::ProjectSettings::kBuildProfilesWindowTitle),
			std::string(TerraEngine::Editor::ProjectSettings::kSkySettingsWindowTitle),
			std::string(TerraEngine::Editor::ProjectSettings::kRenderSettingsWindowTitle)
		};

		std::vector<std::string> visibleTitles;
		for (const std::string& title : allWindows)
		{
			if (TerraEngine::Editor::ProjectSettings::ShouldShowWindowInWindowMenu(title))
			{
				visibleTitles.push_back(title);
			}
		}
		return visibleTitles;
	}
}
