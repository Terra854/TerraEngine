#pragma once

#include "Resource/ShaderCollectionAsset.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace TerraEngine::Editor::Assets
{
	inline constexpr char kShaderSourceSetExtension[] = ".teshadersource";
	inline constexpr char kShaderSourceSetImporter[] = "ShaderSourceSetImporter";
	inline constexpr char kCompatibleShaderImporter[] = "CompatibleShaderImporter";
	inline constexpr char kShaderCollectionImporter[] = "ShaderCollectionImporter";

	// Distinguish new engine-ABI replacements from existing authored HLSL files.
	inline const TerraEngine::Resource::ShaderCollectionSlot* FindCompatibleShaderSlot(const std::filesystem::path& path) {
		const auto name = path.filename().u8string();
		const std::string_view utf8(reinterpret_cast<const char*>(name.data()), name.size());
		for(const auto& slot : TerraEngine::Resource::kShaderCollectionSlots) {
			if(utf8.ends_with(slot.sourceSuffix)) return &slot;
		}
		return nullptr;
	}

	// Resolve the nearest set so shared includes can invalidate its stages.
	inline std::filesystem::path FindShaderSourceSet(const std::filesystem::path& path)
	{
		for(auto directory = path.parent_path(); !directory.empty(); directory = directory.parent_path())
		{
			if(directory.extension() == kShaderSourceSetExtension) return directory;
			if(directory == directory.root_path()) break;
		}
		return {};
	}

	// Use the stage suffix rather than the shader's editable HLSL contents.
	inline const char* GetShaderSourceSetStage(const std::filesystem::path& path)
	{
		if(path.extension() != ".hlsl") return nullptr;
		const auto stageExtension = path.stem().extension();
		if(stageExtension == ".task") return "ts";
		if(stageExtension == ".mesh") return "ms";
		if(stageExtension == ".frag") return "ps";
		if(stageExtension == ".vert") return "vs";
		return nullptr;
	}

	// Pick up shared source edits made while the project was closed. Live
	// changes also invalidate the set through the regular file watcher.
	inline bool ShaderSourceSetOutputIsCurrent(const std::filesystem::path& set,
		const std::filesystem::path& output)
	{
		std::error_code error;
		const auto compiledTime = std::filesystem::last_write_time(output, error);
		if(error) return false;
		for(std::filesystem::recursive_directory_iterator it(set, error), end;
			!error && it != end; it.increment(error))
		{
			const auto extension = it->path().extension();
			if((extension == ".hlsl" || extension == ".hlsli") && it->is_regular_file(error)
				&& it->last_write_time(error) > compiledTime) return false;
		}
		return !error;
	}

}
