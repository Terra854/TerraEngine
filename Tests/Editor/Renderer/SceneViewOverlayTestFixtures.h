// SceneViewOverlayTestFixtures.h - shared Scene View overlay test helpers.

#pragma once

#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayIcons.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayMarkers.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewOrientationWidget.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	inline std::string ReadSceneViewOverlaySourceForTest(const std::string& path)
	{
		const auto read = [](const std::string& candidate) {
			std::ifstream file(candidate, std::ios::binary);
			if (!file)
			{
				return std::string();
			}
			std::ostringstream buffer;
			buffer << file.rdbuf();
			return buffer.str();
		};

		std::string text = read(path);
		if (!text.empty())
		{
			return text;
		}
		text = read(std::string("../../../") + path);
		if (!text.empty())
		{
			return text;
		}
		const std::string thisFile = __FILE__;
		const auto marker = thisFile.find("Tests\\Editor\\Renderer\\SceneViewOverlayTestFixtures.h");
		if (marker != std::string::npos)
		{
			return read(thisFile.substr(0u, marker) + path);
		}
		const auto slashMarker = thisFile.find("Tests/Editor/Renderer/SceneViewOverlayTestFixtures.h");
		if (slashMarker != std::string::npos)
		{
			return read(thisFile.substr(0u, slashMarker) + path);
		}
		return {};
	}
}
