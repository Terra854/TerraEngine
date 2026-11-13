// WorkspaceLayoutDecode.h - checked decoding for user-authored visibility entries.
#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace TerraEngine::Editor {
	// Return a borrowed inventory only while the caller owns the parsed document.
	// Malformed documents have no window inventory and must be diagnosed by the host.
	inline const nlohmann::json* FindWorkspaceLayoutWindows(const nlohmann::json& layout)
	{
		if(!layout.is_object() || !layout.contains("windows") || !layout.at("windows").is_array()) return nullptr;
		return &layout.at("windows");
	}

	struct WorkspaceWindowVisibility final {
		std::string title;
		bool visible = false;
		bool managed = false;
	};

	// Validate every user-controlled type before const JSON access. The returned
	// title owns its bytes, so visibility dispatch never borrows a JSON string.
	inline std::optional<WorkspaceWindowVisibility> DecodeWorkspaceWindowVisibility(const nlohmann::json& entry)
	{
		if(!entry.is_object() || !entry.contains("title") || !entry.at("title").is_string()
			|| !entry.contains("visible") || !entry.at("visible").is_boolean()
			|| (entry.contains("managed") && !entry.at("managed").is_boolean())) return std::nullopt;
		return WorkspaceWindowVisibility{entry.at("title").get<std::string>(),
			entry.at("visible").get<bool>(), entry.contains("managed") && entry.at("managed").get<bool>()};
	}

	enum class WorkspaceLayoutIssue { InvalidWindowInventory, InvalidWindowEntry };

	// Visibility and camera are independent workspace sections. Invalid inventory
	// only skips visibility dispatch; valid camera input must still reach its own
	// existing validator. Callbacks run synchronously while the layout is owned.
	template<typename ApplyVisibility, typename RestoreCamera, typename ReportIssue>
	inline void ApplyWorkspaceLayoutSections(const nlohmann::json& layout,
		ApplyVisibility applyVisibility, RestoreCamera restoreCamera, ReportIssue reportIssue)
	{
		if(const auto* windows = FindWorkspaceLayoutWindows(layout))
		{
			for(const auto& entry : *windows)
			{
				if(const auto decoded = DecodeWorkspaceWindowVisibility(entry)) applyVisibility(*decoded);
				else reportIssue(WorkspaceLayoutIssue::InvalidWindowEntry);
			}
		}
		else reportIssue(WorkspaceLayoutIssue::InvalidWindowInventory);
		if(layout.is_object())
		{
			const auto camera = layout.find("sceneViewCamera");
			if(camera != layout.end()) restoreCamera(*camera);
		}
	}

}
