#include "RenderSystem.h"
#include "RHI/Vulkan/VulkanContext.h"
#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"
#include "Scene/MeshRenderComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Transform.h"

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

	ObjectManager::instance().forEachObjectWithComponents<Transform, MeshRenderComponent>(
		[&](Transform& transform, MeshRenderComponent& meshComp)
		{
			if (meshComp.meshId != TerraEngine::EMPTY_UUID && meshComp.materialId != TerraEngine::EMPTY_UUID)
			{
				DrawCommand cmd;
				cmd.meshId = meshComp.meshId;
				cmd.materialId = meshComp.materialId;
				
				glm::mat4 model = glm::mat4(1.0f);
				model = glm::translate(model, transform.position);
				model = model * glm::mat4_cast(transform.rotation);
				model = glm::scale(model, transform.scale);
				
				cmd.modelMatrix = model;
				
				frameData.commands.push_back(cmd);
			}
		}
	);

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
