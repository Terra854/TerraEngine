#pragma once

#include <memory>

#include "RHICommon.h"

namespace RHI {

class Device
{
public:
    virtual ~Device() = default;

    virtual MeshPipelineHandle CreateMeshPipeline(const MeshPipelineDesc& desc) = 0;
    virtual void DestroyMeshPipeline(MeshPipelineHandle handle) = 0;

    virtual bool BeginFrame() = 0;
    virtual void SubmitMeshTask(const MeshTaskDesc& desc) = 0;
    virtual void SubmitComputeTask(const ComputeTaskDesc& desc) = 0;
    virtual void Present() = 0;
    virtual void WaitIdle() = 0;
};

std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info);

} // namespace RHI

