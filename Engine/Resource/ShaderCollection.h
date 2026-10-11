// ShaderCollection.h — Shader Collection Data Structures
//
// Defines the data structures used to represent and group multiple shader stages into a cohesive collection.
// It provides types for stage-specific shader IDs and metadata required for pipeline creation.
// These collections serve as the high-level representation of materials' visual programs in the engine.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Utilities/UUID.h"
#include "File Formats/TerraEngineShaderFile.h"

// EngineManaged collections author only surface behavior. A compatible
// replacement may provide geometry stages after reflection proves it consumes
// the same engine-owned static/animated geometry contract.
enum class ShaderGeometryPolicy : std::uint32_t
{
	EngineManaged = 0u,
	CompatibleReplacement = 1u
};

// Grouped shaders keyed by stage so render code can bind a single UUID and pull
// every stage needed for a pipeline. Entries retain the accepted immutable
// shader owners; paths are resolved during load and discarded once cached.
struct ShaderCollection
{
	struct StageBinding
	{
		ShaderStage stage = ShaderStage::Vertex;
		TerraEngine::UUID shaderId = TerraEngine::EMPTY_UUID;
		// The reflected contract and Vulkan module must consume the same accepted
		// immutable payload, even if the shader cache publishes a replacement.
		std::shared_ptr<const Shader> shader;
		std::uint64_t publicationGeneration = 0U;
	};

	std::vector<StageBinding> stages;
	std::uint32_t geometryAbiVersion = 1u;
	ShaderGeometryPolicy geometryPolicy = ShaderGeometryPolicy::EngineManaged;
	std::uint64_t collectionGeneration = 0u;
	bool reflectionComplete = false;
	bool usesReservedBindings = false;
	bool usesAnimationBindings = false;
	std::string geometryContractDiagnostic;
};
