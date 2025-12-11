#pragma once

#include "Core/SystemBase.h"
#include "RHI/RHIContext.h"
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
	void Update() override;
	void Shutdown() override;
	std::string SystemName() override;

	RHIContext* GetContext() const { return rhi.get(); }

private:
	RenderSystem() = default;
	std::unique_ptr<RHIContext> rhi;
};
