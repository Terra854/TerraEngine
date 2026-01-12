#include "RenderSystem.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"
#include "Scene/CameraComponent.h"
#include "Scene/MeshRenderComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Transform.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include <entt/entt.hpp>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <algorithm>
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

void RenderSystem::Update(float deltaTime)
{
	if (!initialized) {
		return;
	}
	// Capture all transforms first so hierarchical world transforms can be derived for cameras and renderables.
	std::unordered_map<entt::entity, Transform> localTransforms;
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
	struct CameraEntry
	{
		entt::entity entity;
		CameraComponent camera;
	};
	std::vector<CameraEntry> cameras;
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
	if (cameras.empty())
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
	struct RenderableEntry
	{
		entt::entity entity;
		MeshRenderComponent mesh;
	};
	std::vector<RenderableEntry> renderables;
	if (!cameras.empty())
	{
		objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
			[&](Object object, Transform& transform, MeshRenderComponent& meshComp)
			{
				const TerraEngine::UUID meshId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "meshId");
				const TerraEngine::UUID materialId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "materialId");
				if (meshId != TerraEngine::EMPTY_UUID && materialId != TerraEngine::EMPTY_UUID)
				{
					// Capture reflected mesh bindings without touching fields directly.
					MeshRenderComponent reflectedMesh{};
					reflectedMesh.meshId = meshId;
					reflectedMesh.materialId = materialId;

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

	// Cache of computed world transforms to avoid recomputing during traversal.
	std::unordered_map<entt::entity, Transform> worldTransforms;
	auto computeWorldTransform = [&](auto&& self, entt::entity entity, std::unordered_set<entt::entity>& visiting) -> Transform
	{
		if (auto cached = worldTransforms.find(entity); cached != worldTransforms.end())
		{
			return cached->second;
		}
		if (!localTransforms.contains(entity))
		{
			return Transform{};
		}
		// Prevent cycles from causing infinite recursion.
		if (!visiting.insert(entity).second)
		{
			return localTransforms[entity];
		}

		Transform world = localTransforms[entity];
		const entt::entity parent = objManager.getParentEntity(entity);
		if (parent != entt::null)
		{
			Transform parentWorld = self(self, parent, visiting);
			world.position = parentWorld.position + parentWorld.rotation * (parentWorld.scale * world.position);
			world.rotation = parentWorld.rotation * world.rotation;
			world.scale = parentWorld.scale * world.scale;
		}

		visiting.erase(entity);
		worldTransforms[entity] = world;
		return world;
	};

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

			auto task = ThreadPool::instance().createTask([start, end, &renderables, &computeWorldTransform]() {
				std::vector<DrawCommand> cmds;
				cmds.reserve(end - start);
				std::unordered_set<entt::entity> visiting;
				for (size_t i = start; i < end; ++i)
				{
					const RenderableEntry& entry = renderables[i];
					Transform transform = computeWorldTransform(computeWorldTransform, entry.entity, visiting);
					const MeshRenderComponent& meshComp = entry.mesh;

					DrawCommand cmd;
					cmd.meshId = meshComp.meshId;
					cmd.materialId = meshComp.materialId;

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

		// Stitch results together in submission order to maintain deterministic command order.
		for (auto& future : futures)
		{
			auto chunk = future.get();
			drawCommands.insert(drawCommands.end(), chunk.begin(), chunk.end());
		}
	}

	FrameRenderData frameData;
	// Commands are reused for every camera since the scene contents are shared;
	// only the view/projection matrices will differ per camera iteration.
	frameData.commands = drawCommands;

	// Render once for every camera found so multi-camera scenes can present
	// each authored viewpoint in sequence.
	rhi->BeginFrame();

    VulkanContext* vulkan = static_cast<VulkanContext*>(rhi.get());

    // 1. Scene View Pass
    if (hasSceneViewCamera)
    {
        frameData.viewMatrix = sceneViewCamera.view;
        frameData.projectionMatrix = sceneViewCamera.proj;
        vulkan->RenderFrame(frameData, VulkanContext::ViewType::Scene);
    }

    // 2. Game View Pass
    if (!gameViewCameras.empty())
    {
        for (const auto& cam : gameViewCameras)
        {
            frameData.viewMatrix = cam.view;
            frameData.projectionMatrix = cam.proj;
            vulkan->RenderFrame(frameData, VulkanContext::ViewType::Game);
        }
    }
	else if (cameras.empty())
	{
		// If no cameras are present, render a blank frame to ensure the swapchain
		// is updated and image layouts are transitioned (Undefined -> ShaderReadOnly/Present).
		// This prevents validation errors and potential crashes in Editor mode where
		// UI rendering depends on the scene pass completing.
		FrameRenderData blankData{};
		blankData.viewMatrix = glm::mat4(1.0f);
		blankData.projectionMatrix = glm::mat4(1.0f);
		vulkan->RenderFrame(blankData, VulkanContext::ViewType::Game);
	}
	else
	{
		for (size_t cameraIndex = 0; cameraIndex < cameras.size(); ++cameraIndex)
		{
			const entt::entity cameraEntity = cameras[cameraIndex].entity;
			const auto& camera = cameras[cameraIndex].camera;
			std::unordered_set<entt::entity> visiting;
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

			vulkan->RenderFrame(frameData, VulkanContext::ViewType::Game);
		}
	}
	rhi->EndFrame();

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
    ClearOverrideCameras();
}
