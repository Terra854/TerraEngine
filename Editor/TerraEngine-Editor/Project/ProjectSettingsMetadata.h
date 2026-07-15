// ProjectSettingsMetadata.h - Shared Project Settings menu and category metadata.
//
// Keeps the Project Settings command/category names in one lightweight header so
// both the editor UI and unit tests can validate the same user-facing contract
// without constructing ImGui windows.

#pragma once

#include "Engine/Physics/PhysicsSettings.h"

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
		Physics,
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

	inline constexpr std::array<ProjectSettingsCategoryDescriptor, 4> kProjectSettingsCategories = {{
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
		},
		{
			ProjectSettingsCategoryId::Physics,
			"physics",
			"Physics",
			"World gravity, fixed stepping, collision layers, solver, query, and debug limits.",
			{ "physics", "gravity", "timestep", "layer", "collision", "solver", "query", "debug" }
		}
	}};

	// A UI edit produces this plain request rather than changing a live world.
	// The editor lifecycle owner must publish it at PhysicsSnapshot (or another
	// declared protected boundary) after persisting and validating the settings.
	struct PhysicsSettingsPublicationRequest
	{
		TerraEngine::Physics::PhysicsSettings settings = TerraEngine::Physics::PhysicsSettings::Defaults();
		std::uint64_t requestSequence = 0;
		bool requiresWorldRebuild = true;
		std::string reason;
	};

	// Layer IDs, rather than names, are stored by physics components.  Rename is
	// therefore safe, but this preflight gives creators the visible reference
	// impact before the durable display-name change is accepted.
	struct PhysicsLayerRenameImpact
	{
		bool accepted = false;
		std::size_t layerIndex = TerraEngine::Physics::kPhysicsCollisionLayerCount;
		std::size_t directReferenceCount = 0;
		std::string message;
	};

	inline PhysicsLayerRenameImpact PreflightPhysicsLayerRename(
		const TerraEngine::Physics::PhysicsSettings& settings,
		std::size_t layerIndex,
		std::string_view requestedName,
		std::size_t directReferenceCount)
	{
		if (layerIndex >= settings.layers.size())
		{
			return { false, layerIndex, directReferenceCount, "The selected collision layer does not exist." };
		}
		TerraEngine::Physics::PhysicsSettings candidate = settings;
		candidate.layers[layerIndex].name = std::string(requestedName);
		const auto validation = candidate.Validate();
		if (!validation.accepted)
		{
			return { false, layerIndex, directReferenceCount, validation.message };
		}
		return { true, layerIndex, directReferenceCount,
			directReferenceCount == 0U
				? "The layer display name has no currently known component references."
				: "The layer ID remains stable; existing component references continue to target this layer." };
	}

	// Collision acceptance is symmetric in the runtime: both layers must include
	// the other layer's stable ID. Keep the authoring masks synchronized so one
	// checkbox always describes the behavior users will actually see in Play Mode.
	inline bool SetPhysicsLayerPairCollision(
		TerraEngine::Physics::PhysicsSettings& settings,
		std::size_t firstLayerIndex,
		std::size_t secondLayerIndex,
		bool shouldCollide)
	{
		if (firstLayerIndex >= settings.layers.size() || secondLayerIndex >= settings.layers.size())
		{
			return false;
		}

		auto& firstLayer = settings.layers[firstLayerIndex];
		auto& secondLayer = settings.layers[secondLayerIndex];
		const std::uint32_t secondLayerBit = 1U << secondLayer.id;
		const std::uint32_t firstLayerBit = 1U << firstLayer.id;
		if (shouldCollide)
		{
			firstLayer.collisionMask |= secondLayerBit;
			secondLayer.collisionMask |= firstLayerBit;
		}
		else
		{
			firstLayer.collisionMask &= ~secondLayerBit;
			secondLayer.collisionMask &= ~firstLayerBit;
		}
		return true;
	}

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
