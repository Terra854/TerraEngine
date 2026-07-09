// ProjectSettingsMetadata.h - Shared Project Settings menu and category metadata.
//
// Keeps the Project Settings command/category names in one lightweight header so
// both the editor UI and unit tests can validate the same user-facing contract
// without constructing ImGui windows.

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Editor::ProjectSettings
{
	inline constexpr std::string_view kProjectSettingsWindowTitle = "Project Settings";
	inline constexpr std::string_view kProjectSettingsMenuLabel = "Project Settings...";
	inline constexpr std::string_view kProjectSettingsMenuName = "Edit";
	inline constexpr std::string_view kBuildProfilesWindowTitle = "Build Profiles";
	inline constexpr std::string_view kBuildProfilesMenuName = "File";
	inline constexpr std::string_view kSkySettingsWindowTitle = "Sky Settings";
	inline constexpr std::string_view kRenderSettingsWindowTitle = "Render Settings";

	// ProjectSettingsCategoryId names known category identifiers explicitly so
	// tests can verify removed categories are no longer listed by metadata.
	enum class ProjectSettingsCategoryId
	{
		Project,
		Graphics,
		Resources,
		Sky
	};

	struct ProjectSettingsCategoryDescriptor
	{
		ProjectSettingsCategoryId id;
		std::string_view stableId;
		std::string_view label;
		std::string_view description;
		std::array<std::string_view, 8> keywords;
	};

	inline constexpr std::array<ProjectSettingsCategoryDescriptor, 3> kProjectSettingsCategories = {{
		{
			ProjectSettingsCategoryId::Project,
			"project",
			"Project",
			"General project identity and configuration.",
			{ "project", "name", "product", "company", "configuration", "settings", "", "" }
		},
		{
			ProjectSettingsCategoryId::Graphics,
			"graphics",
			"Graphics",
			"Renderer quality, lighting, shadow, and virtual geometry detail options.",
			{ "graphics", "render", "quality", "shadow", "lighting", "lod", "virtual geometry", "detail" }
		},
		{
			ProjectSettingsCategoryId::Resources,
			"resources",
			"Resources",
			"Loaded resource cleanup, RAM/VRAM residency, texture mip streaming, and virtual geometry page budgets.",
			{ "resources", "residency", "memory", "ram", "vram", "streaming", "mip", "virtual geometry" }
		}
	}};

	inline std::string NormalizeSearchText(std::string_view text)
	{
		std::string normalized;
		normalized.reserve(text.size());
		for (char value : text)
		{
			normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
		}
		return normalized;
	}

	inline bool CategoryMatchesSearch(const ProjectSettingsCategoryDescriptor& category, std::string_view searchText)
	{
		const std::string needle = NormalizeSearchText(searchText);
		if (needle.empty())
		{
			return true;
		}

		const auto containsNeedle = [&needle](std::string_view candidate) {
			if (candidate.empty())
			{
				return false;
			}
			return NormalizeSearchText(candidate).find(needle) != std::string::npos;
		};

		if (containsNeedle(category.label) || containsNeedle(category.description) || containsNeedle(category.stableId))
		{
			return true;
		}

		return std::any_of(category.keywords.begin(), category.keywords.end(), containsNeedle);
	}

	inline std::vector<ProjectSettingsCategoryDescriptor> FilterProjectSettingsCategories(std::string_view searchText)
	{
		std::vector<ProjectSettingsCategoryDescriptor> results;
		for (const ProjectSettingsCategoryDescriptor& category : kProjectSettingsCategories)
		{
			if (CategoryMatchesSearch(category, searchText))
			{
				results.push_back(category);
			}
		}
		return results;
	}

	inline bool IsConsolidatedSettingsWindowTitle(std::string_view title)
	{
		return title == kSkySettingsWindowTitle || title == kRenderSettingsWindowTitle;
	}

	inline bool ShouldShowWindowInWindowMenu(std::string_view title)
	{
		return title != kBuildProfilesWindowTitle && !IsConsolidatedSettingsWindowTitle(title);
	}

	inline const ProjectSettingsCategoryDescriptor* FindProjectSettingsCategory(ProjectSettingsCategoryId id)
	{
		for (const ProjectSettingsCategoryDescriptor& category : kProjectSettingsCategories)
		{
			if (category.id == id)
			{
				return &category;
			}
		}
		return nullptr;
	}
}
