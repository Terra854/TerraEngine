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
	std::unique_ptr<ShaderCollection> LoadShaderCollectionStages(const std::vector<StageRequest>& stages,
		const std::vector<std::pair<TerraEngine::UUID, std::shared_ptr<const Shader>>>& payloads)
	{
		auto collection = std::make_unique<ShaderCollection>();

		for (std::size_t index = 0U; index < stages.size(); ++index)
		{
			const auto& request = stages[index];
			const TerraEngine::UUID shaderId = payloads[index].first;
			const Shader* shader = payloads[index].second.get();

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
			binding.shader = payloads[index].second;
			binding.publicationGeneration = shader->publicationGeneration;
			if (binding.publicationGeneration == 0U) return nullptr;
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

		// If both vertex and fragment stages are present, verify their interfaces match.
		const Shader* vertexShader = nullptr;
		const Shader* meshShader = nullptr;
		const Shader* fragmentShader = nullptr;
		for (std::size_t index = 0U; index < collection->stages.size(); ++index)
		{
			const auto& binding = collection->stages[index];

			if (binding.stage == ShaderStage::Vertex)
			{
				vertexShader = payloads[index].second.get();
			}
			else if(binding.stage == ShaderStage::Mesh)
			{
				meshShader = payloads[index].second.get();
			}

			else if (binding.stage == ShaderStage::Fragment)
			{
				fragmentShader = payloads[index].second.get();
			}
		}


		const Shader* geometryOutputShader =
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
		// FinalizeLoad assigns an actual accepted publication generation. UUID
		// hashes cannot distinguish different stage payloads at the same identity.
		if(!collection->reflectionComplete)
		{
			collection->geometryContractDiagnostic =
				"Shader collection reflection is incomplete.";
			return nullptr;
		}

		return collection;
	}
}

// Both public adapters share one admission/dependency continuation path.
TerraEngine::UUID ResourceManager::LoadShaderCollection(const std::vector<ShaderStagePath>& stagePaths)
{
	return RequestShaderCollection(stagePaths).get();
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadShaderCollectionAsync(const std::vector<ShaderStagePath>& stagePaths)
{
	return RequestShaderCollection(stagePaths);
}

std::shared_future<TerraEngine::UUID> ResourceManager::RequestShaderCollection(const std::vector<ShaderStagePath>& stagePaths, bool reload)
{
	std::vector<StageRequest> stages;
	RequestPtr parent;
	RequestDeliveries deliveries;
	bool admitted = false;
	try {
	{
		std::lock_guard gate(m_PublicationGate);
		if (!m_RequestAdmissionOpen) return MakeReadyFuture(TerraEngine::EMPTY_UUID);
		// Resolve explicit and implicit dependencies against one captured project
		// context. Default geometry belongs to the installed engine, not Assets.
		const bool hasFragment = std::any_of(stagePaths.begin(), stagePaths.end(), [](const auto& stage) { return stage.stage == ShaderStage::Fragment; });
		const bool hasGeometry = std::any_of(stagePaths.begin(), stagePaths.end(), [](const auto& stage) { return stage.stage == ShaderStage::Vertex || stage.stage == ShaderStage::Mesh; });
		if (hasFragment && !hasGeometry) {
			const auto task = (engineRoot / "Shaders/Default.task.tspf").lexically_normal();
			const auto mesh = (engineRoot / "Shaders/Default.mesh.tspf").lexically_normal();
			stages.push_back({ShaderStage::Task, task, NormalizeKey(task)});
			stages.push_back({ShaderStage::Mesh, mesh, NormalizeKey(mesh)});
		}
		for (const auto& stage : stagePaths) {
			const auto path = (stage.path.is_absolute() ? stage.path : assetRoot / stage.path).lexically_normal();
			stages.push_back({stage.stage, path, NormalizeKey(path)});
		}
		const auto key = BuildShaderCollectionKey(stages);
		TerraEngine::UUID id = TerraEngine::EMPTY_UUID;
		{
			std::unique_lock lock(shaderCollectionCache.mutex);
			if (const auto found = shaderCollectionCache.pathToId.find(key); found != shaderCollectionCache.pathToId.end()) {
				id = found->second;
				if (!reload && shaderCollectionCache.resources.contains(id)) return MakeReadyFuture(id);
			} else {
				id = TerraEngine::GenerateUUID();
				shaderCollectionCache.pathToId[key] = id;
				try { shaderCollectionCache.idToPath[id] = key; }
				catch (...) { shaderCollectionCache.pathToId.erase(key); throw; }
			}
		}
		const auto mapKey = std::to_string(static_cast<std::uint32_t>(CacheKind(shaderCollectionCache))) + ":" + key;
		const auto prior = m_Requests.find(mapKey);
		const auto previous = prior == m_Requests.end() ? RequestPtr{} : prior->second;
		parent = AdmitRequestLocked(CacheKind(shaderCollectionCache), key, id, reload, true, {}, deliveries);
		if (!parent) return MakeReadyFuture(TerraEngine::EMPTY_UUID);
		admitted = parent != previous;
		if (admitted) {
			parent->detach = [this, key, version = parent->version] {
				std::unique_lock lock(shaderCollectionCache.mutex);
				const auto current = shaderCollectionCache.inFlightTokens.find(key);
				if (current == shaderCollectionCache.inFlightTokens.end() || current->second == version) {
					shaderCollectionCache.inFlight.erase(key);
					if (current != shaderCollectionCache.inFlightTokens.end()) shaderCollectionCache.inFlightTokens.erase(current);
				}
			};
			std::unique_lock lock(shaderCollectionCache.mutex);
			shaderCollectionCache.inFlight[key] = parent->future;
			shaderCollectionCache.inFlightTokens[key] = parent->version;
		}
	}
	DeliverRequests(std::move(deliveries));
	if (!admitted) return parent->future;
	InvokeRequestHook(parent->key, 3U);
	if (stages.empty()) {
		FinishRequest(parent, RequestOutcome::Failed, "Shader collection has no stages.");
		return parent->future;
	}
	// The state owns dependency records, and those records retain immutable
	// stage payloads. Callbacks do only bounded bookkeeping; the last successful
	// dependency dispatches assembly once, without occupying a waiting worker.
	struct Assembly {
		std::mutex mutex;
		std::size_t remaining = 0U;
		bool scheduled = false;
		std::vector<RequestPtr> dependencies;
	};
	auto assembly = std::make_shared<Assembly>();
	assembly->remaining = stages.size();
	try {
		for (const auto& stage : stages) assembly->dependencies.push_back(AdmitShaderDependency(stage.path, parent));
		{
			std::lock_guard gate(m_PublicationGate);
			for (const auto& dependency : assembly->dependencies) parent->dependencies.push_back(dependency);
		}
		for (const auto& dependency : assembly->dependencies) {
			SubscribeRequest(dependency, [this, parent, stages, assembly](RequestOutcome outcome) {
				try {
				bool dispatch = false;
				{
					std::lock_guard lock(assembly->mutex);
					if (assembly->scheduled) return;
					if (outcome != RequestOutcome::Published) assembly->scheduled = true;
					else if (--assembly->remaining == 0U) { assembly->scheduled = true; dispatch = true; }
				}
				if (outcome != RequestOutcome::Published) {
					FinishRequest(parent, outcome == RequestOutcome::Failed ? RequestOutcome::Failed : RequestOutcome::Canceled,
						"A required shader dependency did not publish.");
				} else if (dispatch) {
					ScheduleRequestWork(parent, [this, parent, stages, assembly] {
						std::vector<std::pair<TerraEngine::UUID, std::shared_ptr<const Shader>>> payloads;
						{
							std::lock_guard gate(m_PublicationGate);
							for (const auto& dependency : assembly->dependencies) payloads.emplace_back(dependency->id, dependency->shaderPayload);
						}
						InvokeRequestHook(parent->key, 0U);
						auto collection = LoadShaderCollectionStages(stages, payloads);
						InvokeRequestHook(parent->key, 1U);
						FinalizeLoad(parent->key, shaderCollectionCache, parent->id, std::move(collection), parent->version, parent);
					});
				}
				} catch (...) {
					// Last-dependency closure allocation belongs to the collection,
					// even when completion originates in another request's delivery.
					AbortRequestSetup(parent);
				}
			});
		}
	} catch (const std::exception& error) { FinishRequest(parent, RequestOutcome::Failed, error.what()); }
	catch (...) { FinishRequest(parent, RequestOutcome::Failed, "Shader dependency admission failed."); }
	return parent->future;
	} catch (...) {
		// Assembly allocation and registration are part of admission ownership.
		// An exception must settle this producer rather than poison its key.
		if (admitted) AbortRequestSetup(parent);
		return parent ? parent->future : MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}
}

std::shared_ptr<const ShaderCollection> ResourceManager::AcquireShaderCollectionSnapshot(const TerraEngine::UUID& id) const
{
	// The collection retains the very stage owners whose reflection was checked.
	// Holding its shared owner spans cache replacement without a second lookup.
	std::shared_lock lock(shaderCollectionCache.mutex);
	const auto found = shaderCollectionCache.resources.find(id);
	return found == shaderCollectionCache.resources.end() ? nullptr : found->second;
}

std::shared_future<TerraEngine::UUID> ResourceManager::ReloadShaderCollectionAsync(const std::vector<ShaderStagePath>& stagePaths)
{
	// Reuse dependency continuations without blocking a worker. Their leases
	// retain the exact accepted payloads used for both reflection and modules.
	return RequestShaderCollection(stagePaths, true);
}
