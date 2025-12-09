#include "Resource/ResourceManager.h"

#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Core/Logger/Logger.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "Resource/ShaderCollection.h"

namespace
{
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

		if (vertexShader.outputs.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Fragment shader requests inputs but vertex shader exposes no outputs.");
			return false;
		}

		std::unordered_map<std::uint32_t, const StageIOAttribute*> outputsByLocation;
		for (const StageIOAttribute& output : vertexShader.outputs)
		{
			outputsByLocation.emplace(output.location, &output);
		}

		for (const StageIOAttribute& input : fragmentShader.inputs)
		{
			const auto found = outputsByLocation.find(input.location);
			if (found == outputsByLocation.end())
			{
				TERRAENGINE_LOG(LogLevel::Error, "Fragment input at location " << input.location << " is not provided by the vertex shader.");
				return false;
			}

			const StageIOAttribute* source = found->second;
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
		for (const StageRequest& request : stages)
		{
			const TerraEngine::UUID shaderId = manager.LoadShader(request.path);
			Shader* shader = manager.GetShader(shaderId);
			if (shader == nullptr)
			{
				TERRAENGINE_LOG(LogLevel::Error, "Failed to load shader stage from " << request.path);
				return nullptr;
			}

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

		// If both vertex and fragment stages are present, verify their interfaces match.
		Shader* vertexShader = nullptr;
		Shader* fragmentShader = nullptr;
		for (const ShaderCollection::StageBinding& binding : collection->stages)
		{
			if (binding.stage == ShaderStage::Vertex)
			{
				vertexShader = manager.GetShader(binding.shaderId);
			}
			else if (binding.stage == ShaderStage::Fragment)
			{
				fragmentShader = manager.GetShader(binding.shaderId);
			}
		}

		if (vertexShader && fragmentShader && !ValidateInterfaceCompatibility(*vertexShader, *fragmentShader))
		{
			TERRAENGINE_LOG(LogLevel::Error, "Vertex and fragment stages in shader collection are incompatible; collection will not be created.");
			return nullptr;
		}

		return collection;
	}
}

TerraEngine::UUID ResourceManager::LoadShaderCollection(const std::vector<ShaderStagePath>& stagePaths)
{
	std::vector<StageRequest> stages;
	stages.reserve(stagePaths.size());
	for (const ShaderStagePath& stagePath : stagePaths)
	{
		StageRequest request{};
		request.stage = stagePath.stage;
		request.path = ResolveAssetPath(stagePath.path);
		request.key = NormalizeKey(request.path);
		stages.push_back(request);
	}

	const std::string key = BuildShaderCollectionKey(stages);

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

std::shared_future<TerraEngine::UUID> ResourceManager::LoadShaderCollectionAsync(const std::vector<ShaderStagePath>& stagePaths)
{
	std::vector<StageRequest> stages;
	stages.reserve(stagePaths.size());
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
		if (auto existing = shaderCollectionCache.pathToId.find(key); existing != shaderCollectionCache.pathToId.end())
		{
			if (shaderCollectionCache.resources.find(existing->second) != shaderCollectionCache.resources.end())
			{
				return MakeReadyFuture(existing->second);
			}
		}

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
