#pragma once

#include "../File Formats/TerraEngineShaderFile.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace TerraEngine::Resource {
	inline constexpr char kShaderCollectionExtension[] = ".teshadercollection";
	// Shared log category lets editor UI subscribe without matching human-readable text.
	inline constexpr char kShaderInterfaceMismatchLogCategory[] = "shader.interface-mismatch";
	// The cooked stage declares its import mode in TSPV provenance. Built-in
	// and post-process shaders can use the same file suffixes.
	inline constexpr std::uint32_t kCompatibleSurfaceShaderFeature = 1u;
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
		if(path.empty() || path.has_root_path() || !utf8.ends_with(slot.sourceSuffix)) return false;
		for(const auto& part : path) if(part == "..") return false;
		return true;
	}

	inline bool IsCompatibleSurfaceShaderPackage(const Shader& shader, const std::filesystem::path& path) {
		if(shader.provenance && (shader.provenance->featureMask & kCompatibleSurfaceShaderFeature)) return true;
		// Packages authored before the provenance flag used a dedicated suffix.
		if(path.extension() != ".tspf") return false;
		auto sourceName = path.filename();
		sourceName.replace_extension(".hlsl");
		const auto name = sourceName.u8string();
		const std::string_view utf8(reinterpret_cast<const char*>(name.data()), name.size());
		for(const auto& slot : kShaderCollectionSlots)
			if(utf8.ends_with(std::string(".surface") + slot.sourceSuffix)) return true;
		return false;
	}
}
