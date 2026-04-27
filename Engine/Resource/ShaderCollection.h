// ShaderCollection.h — Shader Collection Data Structures
//
// Defines the data structures used to represent and group multiple shader stages into a cohesive collection.
// It provides types for stage-specific shader IDs and metadata required for pipeline creation.
// These collections serve as the high-level representation of materials' visual programs in the engine.

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
