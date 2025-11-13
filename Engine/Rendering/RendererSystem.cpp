#include "RendererSystem.h"

#include <iostream>

RendererSystem::RendererSystem(WindowSystem& windowSystem)
    : windowSystem(windowSystem)
    , pool(ThreadPool::instance())
{
}

RendererSystem::~RendererSystem()
{
    shutdown();
}

void RendererSystem::Initialize()
{
    if (initialized) {
        return;
    }

    RHI::DeviceCreateInfo info{};
    info.backend = RHI::Backend::Vulkan;
    info.windowHandle = windowSystem.GetNativeHandle();
    info.enableValidation = false;

    device = RHI::CreateDevice(info);

    initialized = true;

    std::cout << "Renderer system initialized using Vulkan backend" << std::endl;
}

void RendererSystem::Update()
{
    if (!initialized || !device) {
        return;
    }

    SubmitFrameWork();
}

void RendererSystem::shutdown()
{
    if (!initialized) {
        return;
    }

    if (device) {
        device->WaitIdle();
        device.reset();
    }

    initialized = false;
}

void RendererSystem::SubmitFrameWork()
{
    auto meshTask = pool.createTask([this]() {
        RHI::MeshTaskDesc desc{};
        desc.debugName = "MeshPass";
        desc.recordFunc = []() {
            // Placeholder for future mesh shader work submission
        };
        device->SubmitMeshTask(desc);
    });

    auto computeTask = pool.createTask([this]() {
        RHI::ComputeTaskDesc desc{};
        desc.debugName = "ComputePass";
        desc.recordFunc = []() {
            // Placeholder for compute shader work submission
        };
        device->SubmitComputeTask(desc);
    });

    auto meshFuture = meshTask->getFuture();
    auto computeFuture = computeTask->getFuture();

    pool.enqueue(meshTask);
    pool.enqueue(computeTask);

    computeFuture.get();
    meshFuture.get();

    device->Present();
}

