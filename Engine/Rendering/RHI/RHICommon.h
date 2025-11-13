#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "Platform/Window/WindowSystem.h"

namespace RHI {

enum class Backend
{
    Vulkan
};

struct DeviceCreateInfo
{
    Backend backend = Backend::Vulkan;
    NativeWindowHandle windowHandle{};
    bool enableValidation = false;
};

using MeshPipelineHandle = std::uint64_t;

struct MeshPipelineDesc
{
    const std::uint32_t* meshShader = nullptr;
    std::size_t meshShaderSize = 0;
    const std::uint32_t* fragmentShader = nullptr;
    std::size_t fragmentShaderSize = 0;
};

class CommandContext
{
public:
    virtual ~CommandContext() = default;

    virtual void BeginRendering() = 0;
    virtual void EndRendering() = 0;
    virtual void BindMeshPipeline(MeshPipelineHandle pipeline) = 0;
    virtual void SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth) = 0;
    virtual void SetScissor(std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height) = 0;
    virtual void PushConstants(const void* data, std::uint32_t size) = 0;
    virtual void DispatchMesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;
};

struct MeshTaskDesc
{
    std::string debugName;
    std::function<void(CommandContext&)> recordFunc;
};

struct ComputeTaskDesc
{
    std::string debugName;
    std::function<void()> recordFunc;
};

} // namespace RHI

