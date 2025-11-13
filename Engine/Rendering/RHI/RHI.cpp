#include "RHI.h"

#include <stdexcept>

#include "Vulkan/VulkanRHI.h"

namespace RHI {

std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info)
{
    switch (info.backend) {
    case Backend::Vulkan:
        return Vulkan::CreateDevice(info);
    default:
        throw std::runtime_error("Unsupported RHI backend");
    }
}

} // namespace RHI

