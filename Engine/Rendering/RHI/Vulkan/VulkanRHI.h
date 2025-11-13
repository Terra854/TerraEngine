#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "Rendering/RHI/RHI.h"

namespace RHI::Vulkan {

std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info);

} // namespace RHI::Vulkan

