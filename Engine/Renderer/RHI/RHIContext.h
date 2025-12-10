#pragma once

#include "RHIDefinitions.h"
#include "Renderer/RenderTypes.h"
#include "Core/Export.h"

class WindowSystem;

class TERRAENGINE_API RHIContext
{
public:
	virtual ~RHIContext() = default;

	virtual void Initialize(WindowSystem& windowSystem) = 0;
	virtual void BeginFrame() = 0;
	virtual void RenderFrame(const FrameRenderData& frameData) = 0;
	virtual void EndFrame() = 0;
	virtual void Shutdown() = 0;
};
