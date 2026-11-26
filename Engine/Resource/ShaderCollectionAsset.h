#pragma once

#include "File Formats/TerraEngineShaderFile.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace TerraEngine::Resource {
	inline constexpr char kShaderCollectionExtension[] = ".teshadercollection";
	struct ShaderCollectionSlot {
		ShaderStage stage;
		const char* key;
		const char* label;
		const char* sourceSuffix;
		const char* defaultSource;
		const char* defaultPackage;
		const char* profile;
	};

	// Authored surface collections replace stages in the mesh shader pipeline.
	inline constexpr std::array kShaderCollectionSlots{
		ShaderCollectionSlot{ShaderStage::Task, "task", "Task Shader", ".task.hlsl", "Default.task.hlsl", "Default.task.tspf", "ts"},
		ShaderCollectionSlot{ShaderStage::Mesh, "mesh", "Mesh Shader", ".mesh.hlsl", "Default.mesh.hlsl", "Default.mesh.tspf", "ms"},
		ShaderCollectionSlot{ShaderStage::Fragment, "fragment", "Fragment Shader", ".frag.hlsl", "Default.frag.hlsl", "Default.frag.tspf", "ps"}
	};

	// A collection stores asset-root-relative source paths. The same path maps
	// to a package under the cooked asset root after changing its extension.
	inline bool IsShaderCollectionStagePath(const std::filesystem::path& path, const ShaderCollectionSlot& slot) {
		const auto name = path.filename().u8string();
		const std::string_view utf8(reinterpret_cast<const char*>(name.data()), name.size());
		if(path.empty() || path.has_root_path() || !utf8.ends_with(std::string(".surface") + slot.sourceSuffix)) return false;
		for(const auto& part : path) if(part == "..") return false;
		return true;
	}

	inline bool IsCompatibleSurfaceShaderPackage(const std::filesystem::path& path) {
		if(path.extension() != ".tspf") return false;
		auto sourceName = path.filename();
		sourceName.replace_extension(".hlsl");
		for(const auto& slot : kShaderCollectionSlots)
			if(IsShaderCollectionStagePath(sourceName, slot)) return true;
		return false;
	}
}
