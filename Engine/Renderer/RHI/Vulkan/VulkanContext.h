#pragma once

#include "../RHIContext.h"
#include <memory>

class VulkanContext : public RHIContext
{
public:
	VulkanContext();
	~VulkanContext() override;

	void Initialize(WindowSystem& windowSystem) override;
	void BeginFrame() override;
	void RenderFrame(const FrameRenderData& frameData) override;
	void EndFrame() override;
	void Shutdown() override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
