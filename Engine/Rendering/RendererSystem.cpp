#include "RendererSystem.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "Rendering/Shaders/Generated/MeshCubeFrag.h"
#include "Rendering/Shaders/Generated/MeshCubeMesh.h"

namespace {

constexpr float Pi = 3.14159265358979323846f;

struct Mat4
{
    std::array<float, 16> m{};

    static Mat4 identity()
    {
        Mat4 result;
        result.m = { 1.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 1.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 1.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 1.0f };
        return result;
    }
};

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 result{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k) {
                value += a.m[row * 4 + k] * b.m[k * 4 + col];
            }
            result.m[row * 4 + col] = value;
        }
    }
    return result;
}

Mat4 perspective(float fovRadians, float aspect, float nearZ, float farZ)
{
    Mat4 result{};
    const float f = 1.0f / std::tan(fovRadians / 2.0f);
    result.m = { f / aspect, 0.0f, 0.0f, 0.0f,
                 0.0f, f, 0.0f, 0.0f,
                 0.0f, 0.0f, (farZ + nearZ) / (nearZ - farZ), -1.0f,
                 0.0f, 0.0f, (2.0f * farZ * nearZ) / (nearZ - farZ), 0.0f };
    return result;
}

Mat4 rotationY(float angle)
{
    Mat4 result = Mat4::identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    result.m[0] = c;
    result.m[2] = s;
    result.m[8] = -s;
    result.m[10] = c;
    return result;
}

Mat4 rotationX(float angle)
{
    Mat4 result = Mat4::identity();
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    result.m[5] = c;
    result.m[6] = -s;
    result.m[9] = s;
    result.m[10] = c;
    return result;
}

Mat4 translation(float x, float y, float z)
{
    Mat4 result = Mat4::identity();
    result.m[12] = x;
    result.m[13] = y;
    result.m[14] = z;
    return result;
}

std::array<float, 16> ComputeMVP(float angle, float aspect)
{
    Mat4 proj = perspective(45.0f * (Pi / 180.0f), aspect, 0.1f, 100.0f);
    Mat4 view = translation(0.0f, 0.0f, -5.0f);
    Mat4 model = multiply(rotationY(angle), rotationX(angle * 0.5f));

    Mat4 vp = multiply(proj, view);
    Mat4 mvp = multiply(vp, model);
    return mvp.m;
}

} // namespace

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

    CreatePipelines();

    initialized = true;

    std::cout << "Renderer system initialized using Vulkan backend" << std::endl;
}

void RendererSystem::Update()
{
    if (!initialized || !device) {
        return;
    }

    if (!device->BeginFrame()) {
        return;
    }

    rotationAngle += 0.01f;
    SubmitFrameWork();
}

void RendererSystem::shutdown()
{
    if (!initialized) {
        return;
    }

    if (device) {
        if (meshPipeline != 0) {
            device->DestroyMeshPipeline(meshPipeline);
            meshPipeline = 0;
        }
        device->WaitIdle();
        device.reset();
    }

    initialized = false;
}

void RendererSystem::CreatePipelines()
{
    RHI::MeshPipelineDesc desc{};
    desc.meshShader = TerraShaders::MeshCubeMeshSPV;
    desc.meshShaderSize = TerraShaders::MeshCubeMeshSPVSize;
    desc.fragmentShader = TerraShaders::MeshCubeFragSPV;
    desc.fragmentShaderSize = TerraShaders::MeshCubeFragSPVSize;

    meshPipeline = device->CreateMeshPipeline(desc);
}

void RendererSystem::SubmitFrameWork()
{
    const std::uint32_t width = std::max(windowSystem.GetWidth(), 1u);
    const std::uint32_t height = std::max(windowSystem.GetHeight(), 1u);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const std::array<float, 16> mvp = ComputeMVP(rotationAngle, aspect);

    auto meshTask = pool.createTask([this, mvp, width, height]() {
        RHI::MeshTaskDesc desc{};
        desc.debugName = "MeshCube";
        desc.recordFunc = [this, mvp, width, height](RHI::CommandContext& ctx) {
            ctx.BeginRendering();
            ctx.BindMeshPipeline(meshPipeline);
            ctx.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
            ctx.SetScissor(0, 0, width, height);
            ctx.PushConstants(mvp.data(), static_cast<std::uint32_t>(mvp.size() * sizeof(float)));
            ctx.DispatchMesh(1, 1, 1);
            ctx.EndRendering();
        };
        device->SubmitMeshTask(desc);
    });

    auto meshFuture = meshTask->getFuture();
    pool.enqueue(meshTask);
    meshFuture.get();

    device->Present();
}

