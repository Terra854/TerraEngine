// ResourceManagerShaderCollection.cpp — Shader Collection Management Implementation
//
// Implements the management of shader collection resources, which group related shader stages.
// It handles the loading and parsing of shader collection configuration files and manages the associated shader stage assets.
// This module provides the high-level shader definitions used by the material system and the graphics pipeline.

#include "Resource/ResourceManager.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "Resource/ShaderCollection.h"

namespace
{
	// struct StageRequest groups related state and operations so callers have one documented contract.
	struct StageRequest
	{
		ShaderStage stage = ShaderStage::Vertex;
		std::filesystem::path path;
		std::string key;
	};

	// Build a stable cache key from the resolved shader stage paths so callers
	// cannot accidentally swap or reorder the stages and hit a different entry.
	std::string BuildShaderCollectionKey(const std::vector<StageRequest>& stages)
	{
		std::ostringstream combined;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const StageRequest& request : stages)
		{
			combined << static_cast<std::uint32_t>(request.stage) << ":" << request.key << ";";
		}
		return combined.str();
	}

	// Verify that every fragment input has a matching vertex output by location,
	// component type, and component count so pipelines cannot be built with
	// incompatible stage interfaces.
	bool ValidateInterfaceCompatibility(const Shader& vertexShader, const Shader& fragmentShader)
	{
		if (fragmentShader.inputs.empty())
		{
			// Nothing to validate if the fragment stage declares no inputs.
			return true;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vertexShader.outputs.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Fragment shader requests inputs but vertex shader exposes no outputs.");
			return false;
		}

		std::unordered_map<std::uint32_t, const StageIOAttribute*> outputsByLocation;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const StageIOAttribute& output : vertexShader.outputs)
		{
			outputsByLocation.emplace(output.location, &output);
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const StageIOAttribute& input : fragmentShader.inputs)
		{
			const auto found = outputsByLocation.find(input.location);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (found == outputsByLocation.end())
			{
				TERRAENGINE_LOG(LogLevel::Error, "Fragment input at location " << input.location << " is not provided by the vertex shader.");
				return false;
			}

			const StageIOAttribute* source = found->second;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (input.componentType != source->componentType || input.componentCount != source->componentCount)
			{
				TERRAENGINE_LOG(LogLevel::Error, "Fragment input at location " << input.location << " does not match vertex output type/width.");
				return false;
			}
		}

		return true;
	}

	// Load and validate the requested stage set before wrapping them in
	// a ShaderCollection. This keeps pipeline creation simple because stages
	// are guaranteed to match their expected roles.
	std::unique_ptr<ShaderCollection> LoadShaderCollectionStages(ResourceManager& manager, const std::vector<StageRequest>& stages)
	{
		auto collection = std::make_unique<ShaderCollection>();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const StageRequest& request : stages)
		{
			const TerraEngine::UUID shaderId = manager.LoadShader(request.path);
			Shader* shader = manager.GetShader(shaderId);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (shader == nullptr)
			{
				TERRAENGINE_LOG(LogLevel::Error, "Failed to load shader stage from " << request.path);
				return nullptr;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (shader->reflection.source.stage != static_cast<std::uint32_t>(request.stage))
			{
				TERRAENGINE_LOG(LogLevel::Error, "Shader " << request.path << " reports stage " << shader->reflection.source.stage << " but stage " << static_cast<std::uint32_t>(request.stage) << " was requested.");
				return nullptr;
			}

			ShaderCollection::StageBinding binding{};
			binding.stage = request.stage;
			binding.shaderId = shaderId;
			collection->stages.push_back(binding);
		}

		const bool hasFragment = std::any_of(
			collection->stages.begin(), collection->stages.end(),
			[](const ShaderCollection::StageBinding& binding)
			{
				return binding.stage == ShaderStage::Fragment;
			});
		bool hasGeometryProvider = std::any_of(
			collection->stages.begin(), collection->stages.end(),
			[](const ShaderCollection::StageBinding& binding)
			{
				return binding.stage == ShaderStage::Vertex
					|| binding.stage == ShaderStage::Mesh;
			});
		if(hasFragment && !hasGeometryProvider)
		{
			// Surface-only collections inherit canonical engine geometry. Shader
			// authors therefore never bind palettes, skin streams, morph inputs,
			// or virtual-page resources.
			const TerraEngine::UUID taskId =
				manager.LoadShader("Shaders/Default.task.tspf");
			const TerraEngine::UUID meshId =
				manager.LoadShader("Shaders/Default.mesh.tspf");
			if(taskId == TerraEngine::EMPTY_UUID
				|| meshId == TerraEngine::EMPTY_UUID)
			{
				TERRAENGINE_LOG(LogLevel::Error,
					"Engine-managed surface collection cannot load canonical task/mesh stages.");
				return nullptr;
			}
			collection->stages.insert(collection->stages.begin(),
				{ ShaderStage::Mesh, meshId });
			collection->stages.insert(collection->stages.begin(),
				{ ShaderStage::Task, taskId });
			hasGeometryProvider = true;
		}

		// If both vertex and fragment stages are present, verify their interfaces match.
		Shader* vertexShader = nullptr;
		Shader* meshShader = nullptr;
		Shader* fragmentShader = nullptr;
		for (const ShaderCollection::StageBinding& binding : collection->stages)
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (binding.stage == ShaderStage::Vertex)
			{
				vertexShader = manager.GetShader(binding.shaderId);
			}
			else if(binding.stage == ShaderStage::Mesh)
			{
				meshShader = manager.GetShader(binding.shaderId);
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (binding.stage == ShaderStage::Fragment)
			{
				fragmentShader = manager.GetShader(binding.shaderId);
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		Shader* geometryOutputShader =
			meshShader != nullptr ? meshShader : vertexShader;
		if (geometryOutputShader && fragmentShader
			&& !ValidateInterfaceCompatibility(
				*geometryOutputShader, *fragmentShader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "Vertex and fragment stages in shader collection are incompatible; collection will not be created.");
			return nullptr;
		}

		collection->geometryAbiVersion = 1u;
		collection->geometryPolicy = ShaderGeometryPolicy::EngineManaged;
		collection->reflectionComplete = hasGeometryProvider
			&& (!hasFragment || fragmentShader != nullptr);
		std::uint64_t generation = 1469598103934665603ull;
		for(const ShaderCollection::StageBinding& binding : collection->stages)
		{
			generation ^= static_cast<std::uint32_t>(binding.stage);
			generation *= 1099511628211ull;
			for(const std::uint8_t byte : binding.shaderId)
			{
				generation ^= byte;
				generation *= 1099511628211ull;
			}
		}
		collection->collectionGeneration = generation == 0u ? 1u : generation;
		if(!collection->reflectionComplete)
		{
			collection->geometryContractDiagnostic =
				"Shader collection reflection is incomplete.";
			return nullptr;
		}

		return collection;
	}
}

// ResourceManager::LoadShaderCollection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::LoadShaderCollection(const std::vector<ShaderStagePath>& stagePaths)
{
	std::vector<StageRequest> stages;
	stages.reserve(stagePaths.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ShaderStagePath& stagePath : stagePaths)
	{
		StageRequest request{};
		request.stage = stagePath.stage;
		request.path = ResolveAssetPath(stagePath.path);
		request.key = NormalizeKey(request.path);
		stages.push_back(request);
	}

	const std::string key = BuildShaderCollectionKey(stages);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto cached = ResolveCached(key, shaderCollectionCache))
	{
		return *cached;
	}

	const TerraEngine::UUID id = EnsureResourceId(key, shaderCollectionCache);
	// Trace the pairing so it is clear which stage files feed this collection UUID.
	TERRAENGINE_LOG(LogLevel::Debug, "Loading shader collection with " << stages.size() << " stages (id " << ToHexString(id) << ")");
	auto collection = LoadShaderCollectionStages(*this, stages);
	return FinalizeLoad(key, shaderCollectionCache, id, std::move(collection));
}

// ResourceManager::LoadShaderCollectionAsync is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::shared_future<TerraEngine::UUID> ResourceManager::LoadShaderCollectionAsync(const std::vector<ShaderStagePath>& stagePaths)
{
	std::vector<StageRequest> stages;
	stages.reserve(stagePaths.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ShaderStagePath& stagePath : stagePaths)
	{
		StageRequest request{};
		request.stage = stagePath.stage;
		request.path = ResolveAssetPath(stagePath.path);
		request.key = NormalizeKey(request.path);
		stages.push_back(request);
	}

	const std::string key = BuildShaderCollectionKey(stages);

	{
		std::shared_lock lock(shaderCollectionCache.mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto existing = shaderCollectionCache.pathToId.find(key); existing != shaderCollectionCache.pathToId.end())
		{
			if (shaderCollectionCache.resources.find(existing->second) != shaderCollectionCache.resources.end())
			{
				return MakeReadyFuture(existing->second);
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto pending = shaderCollectionCache.inFlight.find(key); pending != shaderCollectionCache.inFlight.end())
		{
			return pending->second;
		}
	}

	const TerraEngine::UUID id = EnsureResourceId(key, shaderCollectionCache);
	// Schedule the pair load so the calling thread stays responsive while disk I/O completes.
	TERRAENGINE_LOG(LogLevel::Debug, "Scheduling async shader collection load with " << stages.size() << " stages (id " << ToHexString(id) << ")");
	auto task = threadPool.createTask([this, key, stages, id]() -> TerraEngine::UUID {
		auto collection = LoadShaderCollectionStages(*this, stages);
		return FinalizeLoad(key, shaderCollectionCache, id, std::move(collection));
	});

	auto future = task->getFuture().share();
	{
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.inFlight[key] = future;
	}
	threadPool.enqueue(task);

	return future;
}
