#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Platform/Window/WindowSystem.h"

namespace RHI {

enum class Backend
{
    Vulkan
};

enum class QueueType
{
    Graphics,
    Compute
};

struct DeviceCreateInfo
{
    Backend backend = Backend::Vulkan;
    NativeWindowHandle windowHandle{};
    bool enableValidation = false;
};

struct MeshTaskDesc
{
    std::string debugName;
    std::function<void()> recordFunc;
};

struct ComputeTaskDesc
{
    std::string debugName;
    std::function<void()> recordFunc;
};

} // namespace RHI

