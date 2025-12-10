#include "RenderSystem.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"
#include "Scene/MeshRenderComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Transform.h"
#include "ThreadPool/ThreadPool.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

RenderSystem::RenderSystem() = default;
RenderSystem::~RenderSystem() = default;

void RenderSystem::Initialize()
{
	TERRAENGINE_LOG(LogLevel::Info, "Initializing RenderSystem...");
	
	rhi = std::make_unique<VulkanContext>();
	rhi->Initialize(WindowSystem::Instance());
}

void RenderSystem::Update()
{
	FrameRenderData frameData;
	
	// Hardcoded camera for now
	frameData.viewMatrix = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	frameData.projectionMatrix = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);
	frameData.projectionMatrix[1][1] *= -1;

	// Snapshot renderable components to avoid locking the registry during parallel work.
	std::vector<std::pair<Transform, MeshRenderComponent>> renderables;
	ObjectManager::instance().forEachObjectWithComponents<Transform, MeshRenderComponent>(
		[&](Transform& transform, MeshRenderComponent& meshComp)
		{
			const TerraEngine::UUID meshId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "meshId");
			const TerraEngine::UUID materialId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "materialId");
			if (meshId != TerraEngine::EMPTY_UUID && materialId != TerraEngine::EMPTY_UUID)
			{
				// Capture reflected mesh bindings without touching fields directly.
				MeshRenderComponent reflectedMesh{};
				reflectedMesh.meshId = meshId;
				reflectedMesh.materialId = materialId;
				reflectedMesh.shaderCollectionId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComp, "shaderCollectionId");

				Transform reflectedTransform{};
				reflectedTransform.position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
				reflectedTransform.rotation = GetComponentProperty<Transform, glm::quat>(transform, "Rotation");
				reflectedTransform.scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");

				renderables.emplace_back(reflectedTransform, reflectedMesh);
			}
		}
	);

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

			auto task = ThreadPool::instance().createTask([start, end, &renderables]() {
				std::vector<DrawCommand> cmds;
				cmds.reserve(end - start);
				for (size_t i = start; i < end; ++i)
				{
					const Transform& transform = renderables[i].first;
					const MeshRenderComponent& meshComp = renderables[i].second;

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
			frameData.commands.insert(frameData.commands.end(), chunk.begin(), chunk.end());
		}
	}

	rhi->BeginFrame();
	rhi->RenderFrame(frameData);
	rhi->EndFrame();
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
