#pragma once

#include "Core/SystemBase.h"
#include "RHI/RHIContext.h"
#include "Core/Object/ObjectManager.h"
#include <memory>

class RenderSystem : public SystemsBase
{
public:
	static RenderSystem& Instance();

	RenderSystem(const RenderSystem&) = delete;
	RenderSystem& operator=(const RenderSystem&) = delete;
	RenderSystem(RenderSystem&&) = delete;
	RenderSystem& operator=(RenderSystem&&) = delete;
	~RenderSystem() override = default;

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

	void ReloadMaterial(const TerraEngine::UUID& materialId);
	void ReloadMesh(const TerraEngine::UUID& meshId);
	void ReloadTexture(const TerraEngine::UUID& textureId);

	RHIContext* GetContext() const { return rhi.get(); }

private:
	RenderSystem();
	std::unique_ptr<RHIContext> rhi;
	ObjectManager& objManager = ObjectManager::instance();
	bool initialized = false;
};
