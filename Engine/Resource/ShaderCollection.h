#pragma once

#include <filesystem>
#include <vector>

#include "Utilities/UUID.h"
#include "File Formats/TerraEngineShaderFile.h"

// Grouped shaders keyed by stage so render code can bind a single UUID and pull
// every stage needed for a pipeline. Entries only store shader UUIDs; paths are
// resolved during load and discarded once cached.
struct ShaderCollection
{
	struct StageBinding
	{
		ShaderStage stage = ShaderStage::Vertex;
		TerraEngine::UUID shaderId = TerraEngine::EMPTY_UUID;
	};

	std::vector<StageBinding> stages;
};
