#include "RenderSystem.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"
#include "Scene/CameraComponent.h"
#include "Scene/MeshRenderComponent.h"
#include "Scene/LightComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Transform.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Core/Memory/FrameArena.h"
#include "Core/Memory/FlatMap.h"
#include <entt/entt.hpp>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <algorithm>
#include <cmath>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

RenderSystem& RenderSystem::Instance()
{
	// Provide a singleton instance so the main engine loop can register the
	// system without managing heap allocations or ownership semantics.
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem()
	: objManager(ObjectManager::instance())
{
	latestGameStats.viewType = RenderViewType::Game;
	latestSceneStats.viewType = RenderViewType::Scene;
}

void RenderSystem::Initialize()
{
	TERRAENGINE_LOG(LogLevel::Info, "Initializing RenderSystem...");
	
	rhi = std::make_unique<VulkanContext>();
	rhi->Initialize(WindowSystem::Instance());

	WindowSystem::Instance().SetResizeCallback([](int width, int height) {
		try {
			auto& renderSystem = RenderSystem::Instance();
			if (renderSystem.GetContext()) {
				static_cast<VulkanContext*>(renderSystem.GetContext())->Resize(width, height);
			}
		} catch (const std::exception& e) {
			TERRAENGINE_LOG(LogLevel::Error, "Exception in resize callback: " << e.what());
		} catch (...) {
			TERRAENGINE_LOG(LogLevel::Error, "Unknown exception in resize callback");
		}
	});

	initialized = true;
}

template<typename T>
using FrameVector = std::vector<T, FrameAllocator<T>>;

template<typename K, typename V>
using FrameMap = FlatMap<K, V, FrameAllocator<std::pair<K, V>>>;

void RenderSystem::Update(float deltaTime)
{
	if (!initialized) {
		return;
	}

    // Reset the frame allocator at the start of the frame logic
    FrameArena::Instance().Reset();

	// Capture all transforms first so hierarchical world transforms can be derived for cameras and renderables.
    FrameMap<entt::entity, Transform> localTransforms;
    localTransforms.reserve(1000); // Guess initial size

	objManager.forEachObjectWithComponents<Transform>(
		[&](Object object, Transform& transform)
		{
			Transform reflectedTransform{};
			reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
			reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
			reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");
			localTransforms.emplace(object.EntityId(), reflectedTransform);
		}
	);

	// Snapshot camera components so view/projection data is sourced from
	// authored entities instead of the legacy hardcoded values.
	FrameVector<CameraEntry> cameras;
    cameras.reserve(10);

	objManager.forEachObjectWithComponents<Transform, CameraComponent>(
		[&](Object object, Transform& transform, CameraComponent& cameraComp)
		{
			CameraComponent reflectedCamera{};
			reflectedCamera.verticalFovDegrees = GetComponentProperty<CameraComponent, float>(cameraComp, "verticalFovDegrees");
			reflectedCamera.aspectRatio = GetComponentProperty<CameraComponent, float>(cameraComp, "aspectRatio");
			reflectedCamera.nearPlane = GetComponentProperty<CameraComponent, float>(cameraComp, "nearPlane");
			reflectedCamera.farPlane = GetComponentProperty<CameraComponent, float>(cameraComp, "farPlane");
			reflectedCamera.up = GetComponentProperty<CameraComponent, glm::vec3>(cameraComp, "up");

			cameras.push_back({ object.EntityId(), reflectedCamera });
			if (!localTransforms.contains(object.EntityId()))
			{
				Transform reflectedTransform{};
				reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
				reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
				reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");
				localTransforms.emplace(object.EntityId(), reflectedTransform);
			}
		}
	);
	// Throttle warning spam so missing cameras do not flood the console every frame.
	static bool loggedMissingCamera = false;
	if (cameras.empty() && !hasSceneViewCamera)
	{
		if (!loggedMissingCamera)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "RenderSystem skipped frame because no CameraComponent was found in the scene.");
			loggedMissingCamera = true;
		}
	}
	else
	{
		loggedMissingCamera = false;
	}

	// Snapshot renderable components to avoid locking the registry during parallel work.
    FrameVector<RenderableEntry> renderables;
    renderables.reserve(1000);

	if (!cameras.empty() || hasSceneViewCamera)
	{
		objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
			[&](Object object, Transform& transform, MeshRenderComponent& meshComp)
			{
				const TerraEngine::UUID meshId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "meshId");
				const TerraEngine::UUID materialId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "materialId");
				const bool objectTaskCullingEnabled = GetComponentProperty<MeshRenderComponent, bool>(meshComp, "taskShaderCullingEnabled");
				if (meshId != TerraEngine::EMPTY_UUID && materialId != TerraEngine::EMPTY_UUID)
				{
					// Capture reflected mesh bindings without touching fields directly.
					MeshRenderComponent reflectedMesh{};
					reflectedMesh.meshId = meshId;
					reflectedMesh.materialId = materialId;
					reflectedMesh.taskShaderCullingEnabled = objectTaskCullingEnabled;

					renderables.push_back({ object.EntityId(), reflectedMesh });
					if (!localTransforms.contains(object.EntityId()))
					{
						Transform reflectedTransform{};
						reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
						reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
						reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");
						localTransforms.emplace(object.EntityId(), reflectedTransform);
					}
				}
			}
		);
	}

	// Snapshot scene light components so packing can run in worker tasks without
	// touching the entity registry after this point.
	FrameVector<LightEntry> lights;
	lights.reserve(256);
	objManager.forEachObjectWithComponents<Transform, LightComponent>(
		[&](Object object, Transform& transform, LightComponent& lightComp)
		{
			LightComponent reflectedLight{};
			const std::uint32_t encodedType = GetComponentProperty<LightComponent, std::uint32_t>(lightComp, "type");
			if (encodedType > static_cast<std::uint32_t>(LightType::Spot))
			{
				// Unsupported legacy light types are intentionally skipped so they
				// are not silently remapped to another runtime behavior.
				return;
			}
			reflectedLight.type = static_cast<LightType>(encodedType);
			
			const std::uint32_t encodedUnit = GetComponentProperty<LightComponent, std::uint32_t>(lightComp, "unit");
			reflectedLight.unit = static_cast<LightUnit>(encodedUnit);

			reflectedLight.color = GetComponentProperty<LightComponent, glm::vec3>(lightComp, "color");
			reflectedLight.intensity = GetComponentProperty<LightComponent, float>(lightComp, "intensity");
			reflectedLight.range = GetComponentProperty<LightComponent, float>(lightComp, "range");
			reflectedLight.innerConeAngle = GetComponentProperty<LightComponent, float>(lightComp, "innerConeAngle");
			reflectedLight.outerConeAngle = GetComponentProperty<LightComponent, float>(lightComp, "outerConeAngle");
			reflectedLight.width = GetComponentProperty<LightComponent, float>(lightComp, "width");
			reflectedLight.height = GetComponentProperty<LightComponent, float>(lightComp, "height");
			reflectedLight.castsShadows = GetComponentProperty<LightComponent, bool>(lightComp, "castsShadows");

			lights.push_back({ object.EntityId(), reflectedLight });
			if (!localTransforms.contains(object.EntityId()))
			{
				Transform reflectedTransform{};
				reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
				reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
				reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");
				localTransforms.emplace(object.EntityId(), reflectedTransform);
			}
		}
	);

	// Cache of computed world transforms to avoid recomputing during traversal.
    FrameMap<entt::entity, Transform> worldTransforms;
    worldTransforms.reserve(localTransforms.size());

    // Use a set for cycle detection. FlatSet would be better but std::set with FrameAllocator is okayish for small depth,
    // or just use vector and linear scan for small depth. Recursion depth is usually small in scene graphs.
    // Let's use FrameVector for "visiting" stack.
    using VisitingStack = FrameVector<entt::entity>;
    
	auto computeWorldTransform = [&](auto&& self, entt::entity entity, VisitingStack& visiting) -> Transform
	{
		if (auto* cached = worldTransforms.find(entity))
		{
			return *cached;
		}
		if (!localTransforms.contains(entity))
		{
			return Transform{};
		}
		// Prevent cycles from causing infinite recursion.
        // Linear scan is fast for small hierarchies
        if (std::find(visiting.begin(), visiting.end(), entity) != visiting.end()) {
             return localTransforms.at(entity);
        }

        visiting.push_back(entity);

		Transform world = localTransforms.at(entity);
		const entt::entity parent = objManager.getParentEntity(entity);
		if (parent != entt::null)
		{
			Transform parentWorld = self(self, parent, visiting);
			world.position = parentWorld.position + parentWorld.rotation * (parentWorld.scale * world.position);
			world.rotation = parentWorld.rotation * world.rotation;
			world.scale = parentWorld.scale * world.scale;
		}

		visiting.pop_back();
		worldTransforms.emplace(entity, world);
		return world;
	};

	// Pre-compute world transforms
	{
        VisitingStack visiting;
        visiting.reserve(16);
		for (const auto& entry : renderables)
		{
			computeWorldTransform(computeWorldTransform, entry.entity, visiting);
		}
		for (const auto& entry : lights)
		{
			computeWorldTransform(computeWorldTransform, entry.entity, visiting);
		}
		for (const auto& entry : cameras)
		{
			computeWorldTransform(computeWorldTransform, entry.entity, visiting);
		}
	}

    // Note: std::vector<DrawCommand> cannot easily use FrameAllocator if we want to return it from tasks/futures 
    // because moving vectors with different allocators (thread local?) is complex?
    // Actually FrameArena is singleton, so allocators are compatible.
    // But std::future/async usage suggests we might want standard heap for result transport OR
    // we just use FrameVector everywhere. FrameArena is thread-safe?
    // FrameArena::Allocate is NOT thread safe currently. offset variable is not atomic.
    
    // **CRITICAL FIX**: FrameArena is NOT thread safe.
    // Parallelizing command generation using FrameAllocator in threads requires locking or ThreadLocal arenas.
    // For now, let's revert to Heap Vector for the parallel result merging, BUT
    // we can use FrameVector for the `drawCommands` main list if we fill it sequentially.
    // The tasks return `std::vector<DrawCommand>`. We'll keep that as heap vector to avoid Arena race conditions.
    // `worldTransforms` is read-only in threads.

	std::vector<DrawCommand> drawCommands;
	if (!renderables.empty())
	{
		const size_t workerCount = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t taskCount = std::min(renderables.size(), workerCount);
		const size_t chunkSize = (renderables.size() + taskCount - 1) / taskCount;

		std::vector<std::future<std::vector<DrawCommand>>> futures;
		futures.reserve(taskCount);

		for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex)
		{
			const size_t start = taskIndex * chunkSize;
			if (start >= renderables.size()) break;
			const size_t end = std::min(renderables.size(), start + chunkSize);

			// Pass worldTransforms by reference (read-only access is safe here)
            // Note: capturing FrameMap by reference is fine.
			auto task = ThreadPool::instance().createTask([start, end, &renderables, &worldTransforms]() {
				std::vector<DrawCommand> cmds;
				cmds.reserve(end - start);
				for (size_t i = start; i < end; ++i)
				{
					const RenderableEntry& entry = renderables[i];
					// Transform is guaranteed to be in the map now
					Transform transform = worldTransforms.at(entry.entity);
					const MeshRenderComponent& meshComp = entry.mesh;

					DrawCommand cmd;
					cmd.meshId = meshComp.meshId;
					cmd.materialId = meshComp.materialId;
					cmd.taskShaderCullingEnabled = meshComp.taskShaderCullingEnabled;

					glm::mat4 model = glm::mat4(1.0f);
					model = glm::translate(model, transform.position);
					model = model * glm::mat4_cast(transform.rotation);
					model = glm::scale(model, transform.scale);
					cmd.modelMatrix = model;

					cmds.push_back(cmd);
				}
				return cmds;
			});

			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}

		// Stitch results together
		for (auto& future : futures)
		{
			auto chunk = future.get();
			drawCommands.insert(drawCommands.end(), chunk.begin(), chunk.end());
		}
	}

	// Pack reflected lights in parallel so large scenes keep the CPU upload path
	// scalable while preserving deterministic ordering per frame.
	std::vector<GPULight> packedLights;
	if (!lights.empty())
	{
		const size_t workerCount = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t taskCount = std::min(lights.size(), workerCount);
		const size_t chunkSize = (lights.size() + taskCount - 1) / taskCount;

		std::vector<std::future<std::vector<GPULight>>> futures;
		futures.reserve(taskCount);

		for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex)
		{
			const size_t start = taskIndex * chunkSize;
			if (start >= lights.size())
			{
				break;
			}
			const size_t end = std::min(lights.size(), start + chunkSize);

			auto task = ThreadPool::instance().createTask([start, end, &lights, &worldTransforms]() {
				std::vector<GPULight> chunkLights;
				chunkLights.reserve(end - start);

				for (size_t i = start; i < end; ++i)
				{
					const LightEntry& entry = lights[i];
					const Transform transform = worldTransforms.at(entry.entity);
					const LightComponent& light = entry.light;

					const glm::quat normalizedRotation = glm::normalize(transform.rotation);
					glm::vec3 direction = normalizedRotation * glm::vec3(0.0f, 0.0f, -1.0f);
					if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) || glm::length(direction) < 1e-5f)
					{
						direction = glm::vec3(0.0f, -1.0f, 0.0f);
					}
					else
					{
						direction = glm::normalize(direction);
					}

					float clampedIntensity = std::max(light.intensity, 0.0f);
					if (light.type != LightType::Directional && light.unit == LightUnit::Lumens)
					{
						clampedIntensity /= (4.0f * 3.14159265f); // Convert Lumens to Candela
					}

					const float clampedRange = std::max(light.range, 0.001f);
					const float clampedInnerAngle = std::clamp(light.innerConeAngle, 0.0f, 1.56f);
					const float clampedOuterAngle = std::clamp(std::max(light.outerConeAngle, clampedInnerAngle + 0.001f), 0.0f, 1.57f);

					GPULight gpuLight{};
					gpuLight.positionAndType = glm::vec4(transform.position, static_cast<float>(static_cast<std::uint32_t>(light.type)));
					gpuLight.directionAndRange = glm::vec4(direction, clampedRange);
					gpuLight.colorAndIntensity = glm::vec4(
						std::max(light.color.x, 0.0f),
						std::max(light.color.y, 0.0f),
						std::max(light.color.z, 0.0f),
						clampedIntensity);
					gpuLight.coneAnglesAndArea = glm::vec4(
						std::cos(clampedInnerAngle),
						std::cos(clampedOuterAngle),
						std::max(light.width, 0.001f),
						std::max(light.height, 0.001f));
					gpuLight.shadowAndPadding = glm::vec4(light.castsShadows ? 1.0f : 0.0f, -1.0f, 0.0f, 0.0f);
					gpuLight.extraParams = glm::vec4(
						std::clamp(light.shadowStrength, 0.0f, 1.0f),
						std::max(light.shadowBias, 0.0f),
						std::max(light.indirectMultiplier, 0.0f),
						0.0f);

					chunkLights.push_back(gpuLight);
				}

				return chunkLights;
			});

			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}

		for (auto& future : futures)
		{
			auto chunk = future.get();
			packedLights.insert(packedLights.end(), chunk.begin(), chunk.end());
		}
	}

	FrameRenderData frameData;
	frameData.deltaTimeSeconds = deltaTime;
	frameData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
	frameData.cullingEpsilon = 0.001f;
	frameData.lights = std::move(packedLights);
	frameData.commands = drawCommands;

	rhi->SetGlobalTaskShaderCullingEnabled(taskShaderCullingEnabled);
	rhi->BeginFrame();

    VulkanContext* vulkan = static_cast<VulkanContext*>(rhi.get());

    if (hasSceneViewCamera)
    {
        frameData.viewMatrix = sceneViewCamera.view;
        frameData.projectionMatrix = sceneViewCamera.proj;
		frameData.cameraWorldPosition = sceneViewCamera.pos;
        vulkan->RenderFrame(frameData, VulkanContext::ViewType::Scene);
    }

    if (!gameViewCameras.empty())
    {
        for (const auto& cam : gameViewCameras)
        {
            frameData.viewMatrix = cam.view;
            frameData.projectionMatrix = cam.proj;
			frameData.cameraWorldPosition = cam.pos;
            vulkan->RenderFrame(frameData, VulkanContext::ViewType::Game);
        }
    }
	else if (cameras.empty())
	{
		FrameRenderData blankData{};
		blankData.viewMatrix = glm::mat4(1.0f);
		blankData.projectionMatrix = glm::mat4(1.0f);
		blankData.cameraWorldPosition = glm::vec3(0.0f);
		blankData.deltaTimeSeconds = deltaTime;
		blankData.globalTaskShaderCullingEnabled = taskShaderCullingEnabled;
		blankData.cullingEpsilon = 0.001f;
		vulkan->RenderFrame(blankData, VulkanContext::ViewType::Game);
	}
	else
	{
		for (size_t cameraIndex = 0; cameraIndex < cameras.size(); ++cameraIndex)
		{
			const entt::entity cameraEntity = cameras[cameraIndex].entity;
			const auto& camera = cameras[cameraIndex].camera;
            
            VisitingStack visiting;
            visiting.reserve(16);
            
			Transform cameraTransform = computeWorldTransform(computeWorldTransform, cameraEntity, visiting);

			// Normalize the rotation so derived direction vectors remain unit length
			// even if authoring data drifts slightly off the identity quaternion.
			const glm::quat cameraRotation = glm::normalize(cameraTransform.rotation);
			const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
			const glm::vec3 forward = cameraRotation * defaultForward;
			glm::vec3 up = cameraRotation * camera.up;
			if (glm::length(up) < 0.0001f)
			{
				up = glm::vec3(0.0f, 1.0f, 0.0f);
			}

			// Clamp projection inputs to keep glm::perspective from receiving invalid values.
			const float clampedFov = std::max(0.001f, camera.verticalFovDegrees);
			const float safeAspect = camera.aspectRatio > 0.0f ? camera.aspectRatio : 1.0f;
			const float nearPlane = std::max(0.0001f, camera.nearPlane);
			const float farPlane = std::max(nearPlane + 0.001f, camera.farPlane);
			frameData.viewMatrix = glm::lookAt(cameraTransform.position, cameraTransform.position + forward, glm::normalize(up));
			frameData.projectionMatrix = glm::perspective(glm::radians(clampedFov), safeAspect, nearPlane, farPlane);
			frameData.projectionMatrix[1][1] *= -1;
			frameData.cameraWorldPosition = cameraTransform.position;

			vulkan->RenderFrame(frameData, VulkanContext::ViewType::Game);
		}
	}
	rhi->EndFrame();

	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestGameStats = rhi->GetLatestRenderStats(RenderViewType::Game);
		latestSceneStats = rhi->GetLatestRenderStats(RenderViewType::Scene);
	}

    ClearOverrideCameras();
}

void RenderSystem::Shutdown()
{
	if (rhi) {
		rhi->Shutdown();
		rhi.reset();
	}
}

std::string RenderSystem::SystemName()
{
	return "RenderSystem";
}

void RenderSystem::ReloadMaterial(const TerraEngine::UUID& materialId)
{
	ResourceManager::Instance().ReloadMaterial(materialId);
	if (rhi)
	{
		rhi->InvalidateMaterial(materialId);
	}
}

void RenderSystem::ReloadMesh(const TerraEngine::UUID& meshId)
{
	ResourceManager::Instance().ReloadMesh(meshId);
	if (rhi)
	{
		rhi->InvalidateMesh(meshId);
	}
}

void RenderSystem::ReloadTexture(const TerraEngine::UUID& textureId)
{
	ResourceManager::Instance().ReloadTexture(textureId);
	if (rhi)
	{
		rhi->InvalidateTexture(textureId);
	}
}

void RenderSystem::Clear()
{
    if (rhi)
    {
        rhi->ClearResources();
    }
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestGameStats = {};
		latestSceneStats = {};
		latestGameStats.viewType = RenderViewType::Game;
		latestSceneStats.viewType = RenderViewType::Scene;
	}
    ClearOverrideCameras();
}

void RenderSystem::SetTaskShaderCullingEnabled(bool enabled)
{
	taskShaderCullingEnabled = enabled;
	if (rhi)
	{
		rhi->SetGlobalTaskShaderCullingEnabled(enabled);
	}
}

RenderStatsSnapshot RenderSystem::GetLatestRenderStats(RenderViewType viewType) const
{
	std::lock_guard<std::mutex> lock(statsMutex);
	if (viewType == RenderViewType::Scene)
	{
		return latestSceneStats;
	}
	return latestGameStats;
}
