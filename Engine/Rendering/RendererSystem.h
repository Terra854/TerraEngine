#pragma once

#include <memory>

#include "Core/SystemBase.h"
#include "Platform/Window/WindowSystem.h"
#include "Rendering/RHI/RHI.h"
#include "ThreadPool/ThreadPool.h"

class RendererSystem : public SystemsBase
{
public:
    explicit RendererSystem(WindowSystem& windowSystem);
    ~RendererSystem() override;

    void Initialize() override;
    void Update() override;
    void shutdown() override;

    std::string SystemName() override { return "RendererSystem"; }

private:
    void SubmitFrameWork();

    WindowSystem& windowSystem;
    ThreadPool& pool;

    std::unique_ptr<RHI::Device> device;
    bool initialized = false;
};

