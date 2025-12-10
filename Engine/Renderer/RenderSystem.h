#pragma once

#include "Core/SystemBase.h"
#include "RHI/RHIContext.h"
#include <memory>

class RenderSystem : public SystemsBase
{
public:
	RenderSystem();
	~RenderSystem() override;

	void Initialize() override;
	void Update() override;
	void Shutdown() override;
	std::string SystemName() override;

	RHIContext* GetContext() const { return rhi.get(); }

private:
	std::unique_ptr<RHIContext> rhi;
};
