// EditorPreferencesMetadata.h - Shared Preferences menu and category metadata.
//
// Keeps editor-wide Preferences command/category names in one lightweight
// header so the menu, window, and unit tests validate the same contract.

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Editor::Preferences
{
	inline constexpr std::string_view kPreferencesWindowTitle = "Preferences";
	inline constexpr std::string_view kPreferencesMenuLabel = "Preferences...";
	inline constexpr std::string_view kPreferencesMenuName = "Edit";

	enum class PreferencesCategoryId
	{
		Diagnostics,
		SceneView,
		McpBridge
	};

	struct PreferencesCategoryDescriptor
	{
		PreferencesCategoryId id;
		std::string_view stableId;
		std::string_view label;
		std::string_view description;
		std::array<std::string_view, 8> keywords;
	};

	inline constexpr std::array<PreferencesCategoryDescriptor, 3> kPreferencesCategories = {{
		{
			PreferencesCategoryId::Diagnostics,
			"diagnostics",
			"Diagnostics",
			"Local editor renderer diagnostics and validation preferences.",
			{ "diagnostics", "renderer", "render", "vulkan", "validation", "debug", "layers", "preferences" }
		},
		{
			PreferencesCategoryId::SceneView,
			"scene-view",
			"Scene View",
			"Local large-scene marker budgets, density, and presentation safeguards.",
			{ "scene", "view", "large", "markers", "physics", "density", "budget", "performance" }
		},
		{
			PreferencesCategoryId::McpBridge,
			"mcp-bridge",
			"MCP Bridge",
			"Local assistant connection, permissions, approvals, and activity.",
			{ "mcp", "bridge", "assistant", "automation", "connection", "port", "permissions", "approvals" }
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

	inline bool CategoryMatchesSearch(const PreferencesCategoryDescriptor& category, std::string_view searchText)
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

	inline std::vector<PreferencesCategoryDescriptor> FilterPreferencesCategories(std::string_view searchText)
	{
		std::vector<PreferencesCategoryDescriptor> results;
		for (const PreferencesCategoryDescriptor& category : kPreferencesCategories)
		{
			if (CategoryMatchesSearch(category, searchText))
			{
				results.push_back(category);
			}
		}
		return results;
	}

	inline const PreferencesCategoryDescriptor* FindPreferencesCategory(PreferencesCategoryId id)
	{
		for (const PreferencesCategoryDescriptor& category : kPreferencesCategories)
		{
			if (category.id == id)
			{
				return &category;
			}
		}
		return nullptr;
	}
}
