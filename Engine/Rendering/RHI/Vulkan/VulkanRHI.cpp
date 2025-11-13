#include "VulkanRHI.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    define VK_USE_PLATFORM_WIN32_KHR
#else
#    define VK_USE_PLATFORM_XLIB_KHR
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dlfcn.h>
#    include <X11/Xlib.h>
#endif

namespace RHI::Vulkan {

namespace {

class VulkanSymbols
{
public:
    static VulkanSymbols& Get()
    {
        static VulkanSymbols instance;
        return instance;
    }

    void EnsureInstanceFunctions(VkInstance inst)
    {
        if (instanceHandle == inst) {
            return;
        }

        LoadInstanceFunctions(inst);
        instanceHandle = inst;
    }

    void EnsureDeviceFunctions(VkDevice dev)
    {
        if (deviceHandle == dev) {
            return;
        }

        LoadDeviceFunctions(dev);
        deviceHandle = dev;
    }

    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
#ifdef _WIN32
    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR = nullptr;
#else
    PFN_vkCreateXlibSurfaceKHR vkCreateXlibSurfaceKHR = nullptr;
#endif

    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;

    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;

    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;
    PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;

    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
    PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
    PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
    PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT = nullptr;

    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;

    PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
    PFN_vkCreateFence vkCreateFence = nullptr;
    PFN_vkDestroyFence vkDestroyFence = nullptr;
    PFN_vkWaitForFences vkWaitForFences = nullptr;
    PFN_vkResetFences vkResetFences = nullptr;

    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;

private:
    VulkanSymbols()
    {
#ifdef _WIN32
        library = LoadLibraryA("vulkan-1.dll");
        if (!library) {
            throw std::runtime_error("Failed to load vulkan-1.dll");
        }

        auto getInstanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
#else
        library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (!library) {
            throw std::runtime_error("Failed to load libvulkan");
        }

        auto getInstanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
        if (!getInstanceProc) {
            throw std::runtime_error("Failed to resolve vkGetInstanceProcAddr");
        }

        vkGetInstanceProcAddr = getInstanceProc;
        LoadGlobalFunction(vkCreateInstance, "vkCreateInstance");
        LoadGlobalFunction(vkEnumerateInstanceExtensionProperties, "vkEnumerateInstanceExtensionProperties");
    }

    ~VulkanSymbols()
    {
#ifdef _WIN32
        if (library) {
            FreeLibrary(library);
            library = nullptr;
        }
#else
        if (library) {
            dlclose(library);
            library = nullptr;
        }
#endif
    }

    template <typename T>
    void LoadGlobalFunction(T& fn, const char* name)
    {
#ifdef _WIN32
        fn = reinterpret_cast<T>(GetProcAddress(library, name));
#else
        fn = reinterpret_cast<T>(dlsym(library, name));
#endif
        if (!fn) {
            throw std::runtime_error(std::string("Failed to load Vulkan function: ") + name);
        }
    }

    template <typename T>
    void LoadInstanceFunction(T& fn, VkInstance inst, const char* name)
    {
        fn = reinterpret_cast<T>(vkGetInstanceProcAddr(inst, name));
        if (!fn) {
            throw std::runtime_error(std::string("Failed to load Vulkan instance function: ") + name);
        }
    }

    template <typename T>
    void LoadDeviceFunction(T& fn, VkDevice dev, const char* name)
    {
        fn = reinterpret_cast<T>(vkGetDeviceProcAddr(dev, name));
        if (!fn) {
            throw std::runtime_error(std::string("Failed to load Vulkan device function: ") + name);
        }
    }

    void LoadInstanceFunctions(VkInstance inst)
    {
        LoadInstanceFunction(vkDestroyInstance, inst, "vkDestroyInstance");
        LoadInstanceFunction(vkEnumeratePhysicalDevices, inst, "vkEnumeratePhysicalDevices");
        LoadInstanceFunction(vkGetPhysicalDeviceQueueFamilyProperties, inst, "vkGetPhysicalDeviceQueueFamilyProperties");
        LoadInstanceFunction(vkEnumerateDeviceExtensionProperties, inst, "vkEnumerateDeviceExtensionProperties");
        LoadInstanceFunction(vkGetPhysicalDeviceFeatures2, inst, "vkGetPhysicalDeviceFeatures2");
        LoadInstanceFunction(vkGetPhysicalDeviceMemoryProperties, inst, "vkGetPhysicalDeviceMemoryProperties");
        LoadInstanceFunction(vkGetPhysicalDeviceFormatProperties, inst, "vkGetPhysicalDeviceFormatProperties");
        LoadInstanceFunction(vkGetPhysicalDeviceSurfaceSupportKHR, inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
        LoadInstanceFunction(vkGetPhysicalDeviceSurfaceFormatsKHR, inst, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        LoadInstanceFunction(vkGetPhysicalDeviceSurfacePresentModesKHR, inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        LoadInstanceFunction(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
#ifdef _WIN32
        LoadInstanceFunction(vkCreateWin32SurfaceKHR, inst, "vkCreateWin32SurfaceKHR");
#else
        LoadInstanceFunction(vkCreateXlibSurfaceKHR, inst, "vkCreateXlibSurfaceKHR");
#endif
        LoadInstanceFunction(vkCreateDevice, inst, "vkCreateDevice");
        LoadInstanceFunction(vkDestroySurfaceKHR, inst, "vkDestroySurfaceKHR");

        auto getDeviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetInstanceProcAddr(inst, "vkGetDeviceProcAddr"));
        if (!getDeviceProc) {
            throw std::runtime_error("Failed to load vkGetDeviceProcAddr");
        }
        vkGetDeviceProcAddr = getDeviceProc;
    }

    void LoadDeviceFunctions(VkDevice dev)
    {
        LoadDeviceFunction(vkDestroyDevice, dev, "vkDestroyDevice");
        LoadDeviceFunction(vkDeviceWaitIdle, dev, "vkDeviceWaitIdle");
        LoadDeviceFunction(vkGetDeviceQueue, dev, "vkGetDeviceQueue");
        LoadDeviceFunction(vkCreateCommandPool, dev, "vkCreateCommandPool");
        LoadDeviceFunction(vkDestroyCommandPool, dev, "vkDestroyCommandPool");
        LoadDeviceFunction(vkAllocateCommandBuffers, dev, "vkAllocateCommandBuffers");
        LoadDeviceFunction(vkFreeCommandBuffers, dev, "vkFreeCommandBuffers");
        LoadDeviceFunction(vkBeginCommandBuffer, dev, "vkBeginCommandBuffer");
        LoadDeviceFunction(vkEndCommandBuffer, dev, "vkEndCommandBuffer");
        LoadDeviceFunction(vkResetCommandBuffer, dev, "vkResetCommandBuffer");
        LoadDeviceFunction(vkQueueSubmit, dev, "vkQueueSubmit");
        LoadDeviceFunction(vkQueueWaitIdle, dev, "vkQueueWaitIdle");

        LoadDeviceFunction(vkCreateSwapchainKHR, dev, "vkCreateSwapchainKHR");
        LoadDeviceFunction(vkDestroySwapchainKHR, dev, "vkDestroySwapchainKHR");
        LoadDeviceFunction(vkGetSwapchainImagesKHR, dev, "vkGetSwapchainImagesKHR");
        LoadDeviceFunction(vkAcquireNextImageKHR, dev, "vkAcquireNextImageKHR");
        LoadDeviceFunction(vkQueuePresentKHR, dev, "vkQueuePresentKHR");

        LoadDeviceFunction(vkCreateImageView, dev, "vkCreateImageView");
        LoadDeviceFunction(vkDestroyImageView, dev, "vkDestroyImageView");
        LoadDeviceFunction(vkCreateRenderPass, dev, "vkCreateRenderPass");
        LoadDeviceFunction(vkDestroyRenderPass, dev, "vkDestroyRenderPass");
        LoadDeviceFunction(vkCreateFramebuffer, dev, "vkCreateFramebuffer");
        LoadDeviceFunction(vkDestroyFramebuffer, dev, "vkDestroyFramebuffer");
        LoadDeviceFunction(vkCmdBeginRenderPass, dev, "vkCmdBeginRenderPass");
        LoadDeviceFunction(vkCmdEndRenderPass, dev, "vkCmdEndRenderPass");

        LoadDeviceFunction(vkCreatePipelineLayout, dev, "vkCreatePipelineLayout");
        LoadDeviceFunction(vkDestroyPipelineLayout, dev, "vkDestroyPipelineLayout");
        LoadDeviceFunction(vkCreateGraphicsPipelines, dev, "vkCreateGraphicsPipelines");
        LoadDeviceFunction(vkDestroyPipeline, dev, "vkDestroyPipeline");
        LoadDeviceFunction(vkCmdBindPipeline, dev, "vkCmdBindPipeline");
        LoadDeviceFunction(vkCmdSetViewport, dev, "vkCmdSetViewport");
        LoadDeviceFunction(vkCmdSetScissor, dev, "vkCmdSetScissor");
        LoadDeviceFunction(vkCmdPushConstants, dev, "vkCmdPushConstants");
        LoadDeviceFunction(vkCmdDrawMeshTasksEXT, dev, "vkCmdDrawMeshTasksEXT");

        LoadDeviceFunction(vkCreateShaderModule, dev, "vkCreateShaderModule");
        LoadDeviceFunction(vkDestroyShaderModule, dev, "vkDestroyShaderModule");

        LoadDeviceFunction(vkCreateSemaphore, dev, "vkCreateSemaphore");
        LoadDeviceFunction(vkDestroySemaphore, dev, "vkDestroySemaphore");
        LoadDeviceFunction(vkCreateFence, dev, "vkCreateFence");
        LoadDeviceFunction(vkDestroyFence, dev, "vkDestroyFence");
        LoadDeviceFunction(vkWaitForFences, dev, "vkWaitForFences");
        LoadDeviceFunction(vkResetFences, dev, "vkResetFences");

        LoadDeviceFunction(vkCreateImage, dev, "vkCreateImage");
        LoadDeviceFunction(vkDestroyImage, dev, "vkDestroyImage");
        LoadDeviceFunction(vkAllocateMemory, dev, "vkAllocateMemory");
        LoadDeviceFunction(vkFreeMemory, dev, "vkFreeMemory");
        LoadDeviceFunction(vkBindImageMemory, dev, "vkBindImageMemory");
        LoadDeviceFunction(vkGetImageMemoryRequirements, dev, "vkGetImageMemoryRequirements");
    }

#ifdef _WIN32
    HMODULE library = nullptr;
#else
    void* library = nullptr;
#endif
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    VkInstance instanceHandle = VK_NULL_HANDLE;
    VkDevice deviceHandle = VK_NULL_HANDLE;
};

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    std::optional<uint32_t> computeFamily;

    bool IsComplete() const
    {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct MeshPipeline
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<std::uint32_t> meshCode;
    std::vector<std::uint32_t> fragmentCode;
};

struct FrameResources
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;
};

class VulkanDevice;

class VulkanCommandContext : public CommandContext
{
public:
    VulkanCommandContext(VulkanDevice& device,
        VkCommandBuffer cmd,
        VkFramebuffer framebuffer,
        VkExtent2D extent)
        : device(device)
        , commandBuffer(cmd)
        , framebuffer(framebuffer)
        , extent(extent)
    {
    }

    void BeginRendering() override;
    void EndRendering() override;
    void BindMeshPipeline(MeshPipelineHandle pipeline) override;
    void SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth) override;
    void SetScissor(std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height) override;
    void PushConstants(const void* data, std::uint32_t size) override;
    void DispatchMesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;

private:
    VulkanDevice& device;
    VkCommandBuffer commandBuffer;
    VkFramebuffer framebuffer;
    VkExtent2D extent{};
    bool renderPassActive = false;
    MeshPipelineHandle currentPipeline = 0;
};

class VulkanDevice : public Device
{
public:
    explicit VulkanDevice(const DeviceCreateInfo& info)
        : windowHandle(info.windowHandle)
    {
        CreateInstance(info.enableValidation);
        CreateSurface();
        PickPhysicalDevice();
        CreateDeviceAndQueues();
        CreateCommandPools();
        CreateSwapchainResources();
        CreateSyncObjects();
    }

    ~VulkanDevice() override
    {
        Cleanup();
    }

    MeshPipelineHandle CreateMeshPipeline(const MeshPipelineDesc& desc) override;
    void DestroyMeshPipeline(MeshPipelineHandle handle) override;

    bool BeginFrame() override;
    void SubmitMeshTask(const MeshTaskDesc& desc) override;
    void SubmitComputeTask(const ComputeTaskDesc& desc) override;
    void Present() override;
    void WaitIdle() override;

    MeshPipeline* GetPipeline(MeshPipelineHandle handle)
    {
        auto it = meshPipelines.find(handle);
        if (it == meshPipelines.end()) {
            return nullptr;
        }
        return &it->second;
    }

    VkRenderPass GetRenderPass() const { return renderPass; }
    VkDevice GetDevice() const { return device; }

private:
    VkShaderModule CreateShaderModule(const std::uint32_t* data, std::size_t size);

    void CreatePipelineObjects(MeshPipeline& pipeline);
    void RebuildMeshPipelines();

    void CreateInstance(bool enableValidation);
    void CreateSurface();
    void PickPhysicalDevice();
    void CreateDeviceAndQueues();
    void CreateCommandPools();
    void CreateSwapchainResources();
    void CreateDepthResources();
    void CreateRenderPass();
    void CreateFramebuffers();
    void CreateSyncObjects();
    void CleanupSwapchain();
    bool RecreateSwapchain();

    QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
    SwapchainSupportDetails QuerySwapchainSupport(VkPhysicalDevice device);
    bool CheckDeviceExtensionSupport(VkPhysicalDevice device);

    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
    VkFormat FindDepthFormat();
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    void Cleanup();

    NativeWindowHandle windowHandle{};
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;

    QueueFamilyIndices queueFamilyIndices;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    VkCommandPool graphicsCommandPool = VK_NULL_HANDLE;
    VkCommandPool computeCommandPool = VK_NULL_HANDLE;

    std::vector<FrameResources> frames;
    uint32_t currentFrame = 0;
    uint32_t currentImageIndex = 0;
    bool frameActive = false;

    std::unordered_map<MeshPipelineHandle, MeshPipeline> meshPipelines;
    MeshPipelineHandle nextPipelineHandle = 1;

    std::mutex frameMutex;
};

void VulkanCommandContext::BeginRendering()
{
    if (renderPassActive) {
        return;
    }

    auto& symbols = VulkanSymbols::Get();

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = { { 0.02f, 0.02f, 0.05f, 1.0f } };
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = device.GetRenderPass();
    beginInfo.framebuffer = framebuffer;
    beginInfo.renderArea.offset = { 0, 0 };
    beginInfo.renderArea.extent = extent;
    beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    beginInfo.pClearValues = clearValues.data();

    symbols.vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    renderPassActive = true;
}

void VulkanCommandContext::EndRendering()
{
    if (!renderPassActive) {
        return;
    }

    VulkanSymbols::Get().vkCmdEndRenderPass(commandBuffer);
    renderPassActive = false;
}

void VulkanCommandContext::BindMeshPipeline(MeshPipelineHandle pipeline)
{
    auto* vkPipeline = device.GetPipeline(pipeline);
    if (!vkPipeline) {
        throw std::runtime_error("Invalid mesh pipeline handle");
    }

    VulkanSymbols::Get().vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline->pipeline);
    currentPipeline = pipeline;
}

void VulkanCommandContext::SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth)
{
    VkViewport viewport{};
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = minDepth;
    viewport.maxDepth = maxDepth;

    VulkanSymbols::Get().vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
}

void VulkanCommandContext::SetScissor(std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height)
{
    VkRect2D scissor{};
    scissor.offset = { x, y };
    scissor.extent = { width, height };
    VulkanSymbols::Get().vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void VulkanCommandContext::PushConstants(const void* data, std::uint32_t size)
{
    auto* vkPipeline = device.GetPipeline(currentPipeline);
    if (!vkPipeline) {
        throw std::runtime_error("No mesh pipeline bound for push constants");
    }

    VulkanSymbols::Get().vkCmdPushConstants(commandBuffer, vkPipeline->layout, VK_SHADER_STAGE_MESH_BIT_EXT, 0, size, data);
}

void VulkanCommandContext::DispatchMesh(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    VulkanSymbols::Get().vkCmdDrawMeshTasksEXT(commandBuffer, x, y, z);
}

MeshPipelineHandle VulkanDevice::CreateMeshPipeline(const MeshPipelineDesc& desc)
{
    if (!desc.meshShader || !desc.fragmentShader || desc.meshShaderSize == 0 || desc.fragmentShaderSize == 0) {
        throw std::runtime_error("Mesh pipeline shaders must be provided");
    }

    if (desc.meshShaderSize % sizeof(std::uint32_t) != 0 || desc.fragmentShaderSize % sizeof(std::uint32_t) != 0) {
        throw std::runtime_error("Shader binaries must be 32-bit aligned");
    }

    MeshPipeline pipeline{};
    pipeline.meshCode.resize(desc.meshShaderSize / sizeof(std::uint32_t));
    std::memcpy(pipeline.meshCode.data(), desc.meshShader, desc.meshShaderSize);
    pipeline.fragmentCode.resize(desc.fragmentShaderSize / sizeof(std::uint32_t));
    std::memcpy(pipeline.fragmentCode.data(), desc.fragmentShader, desc.fragmentShaderSize);

    CreatePipelineObjects(pipeline);

    MeshPipelineHandle handle = nextPipelineHandle++;
    meshPipelines.emplace(handle, std::move(pipeline));
    return handle;
}

void VulkanDevice::DestroyMeshPipeline(MeshPipelineHandle handle)
{
    auto it = meshPipelines.find(handle);
    if (it == meshPipelines.end()) {
        return;
    }

    auto& symbols = VulkanSymbols::Get();
    if (it->second.pipeline != VK_NULL_HANDLE) {
        symbols.vkDestroyPipeline(device, it->second.pipeline, nullptr);
    }
    if (it->second.layout != VK_NULL_HANDLE) {
        symbols.vkDestroyPipelineLayout(device, it->second.layout, nullptr);
    }
    meshPipelines.erase(it);
}

bool VulkanDevice::BeginFrame()
{
    std::lock_guard<std::mutex> lock(frameMutex);
    if (frameActive) {
        return true;
    }

    auto& symbols = VulkanSymbols::Get();
    FrameResources& frame = frames[currentFrame];

    if (symbols.vkWaitForFences(device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        throw std::runtime_error("Failed to wait for frame fence");
    }

    VkResult acquireResult = symbols.vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &currentImageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!RecreateSwapchain()) {
            return false;
        }
        acquireResult = symbols.vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &currentImageIndex);
    }

    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    symbols.vkResetFences(device, 1, &frame.inFlightFence);
    symbols.vkResetCommandBuffer(frame.commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (symbols.vkBeginCommandBuffer(frame.commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin command buffer");
    }

    frameActive = true;
    return true;
}

void VulkanDevice::SubmitMeshTask(const MeshTaskDesc& desc)
{
    if (!desc.recordFunc) {
        return;
    }

    std::lock_guard<std::mutex> lock(frameMutex);
    if (!frameActive) {
        throw std::runtime_error("SubmitMeshTask called without active frame");
    }

    FrameResources& frame = frames[currentFrame];
    VulkanCommandContext context(*this, frame.commandBuffer, framebuffers[currentImageIndex], swapchainExtent);
    desc.recordFunc(context);
    if (!desc.debugName.empty()) {
        std::cout << "Executed Vulkan task: " << desc.debugName << std::endl;
    }
}

void VulkanDevice::SubmitComputeTask(const ComputeTaskDesc& desc)
{
    (void)desc;
}

void VulkanDevice::Present()
{
    std::lock_guard<std::mutex> lock(frameMutex);
    if (!frameActive) {
        return;
    }

    auto& symbols = VulkanSymbols::Get();
    FrameResources& frame = frames[currentFrame];

    if (symbols.vkEndCommandBuffer(frame.commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to end command buffer");
    }

    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.imageAvailable;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame.renderFinished;

    if (symbols.vkQueueSubmit(graphicsQueue, 1, &submitInfo, frame.inFlightFence) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit command buffer");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &currentImageIndex;

    VkResult presentResult = symbols.vkQueuePresentKHR(presentQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
    } else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }

    currentFrame = (currentFrame + 1) % static_cast<uint32_t>(frames.size());
    frameActive = false;
}

void VulkanDevice::WaitIdle()
{
    auto& symbols = VulkanSymbols::Get();
    if (device != VK_NULL_HANDLE) {
        symbols.vkDeviceWaitIdle(device);
    }
}

VkShaderModule VulkanDevice::CreateShaderModule(const std::uint32_t* data, std::size_t size)
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = size;
    createInfo.pCode = data;

    VkShaderModule module;
    auto& symbols = VulkanSymbols::Get();
    if (symbols.vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    return module;
}

void VulkanDevice::CreatePipelineObjects(MeshPipeline& pipeline)
{
    auto& symbols = VulkanSymbols::Get();

    if (pipeline.pipeline != VK_NULL_HANDLE) {
        symbols.vkDestroyPipeline(device, pipeline.pipeline, nullptr);
        pipeline.pipeline = VK_NULL_HANDLE;
    }
    if (pipeline.layout != VK_NULL_HANDLE) {
        symbols.vkDestroyPipelineLayout(device, pipeline.layout, nullptr);
        pipeline.layout = VK_NULL_HANDLE;
    }

    VkShaderModule meshModule = CreateShaderModule(pipeline.meshCode.data(), pipeline.meshCode.size() * sizeof(std::uint32_t));
    VkShaderModule fragModule = CreateShaderModule(pipeline.fragmentCode.data(), pipeline.fragmentCode.size() * sizeof(std::uint32_t));

    VkPipelineShaderStageCreateInfo meshStage{};
    meshStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    meshStage.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    meshStage.module = meshModule;
    meshStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> stages = { meshStage, fragStage };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &colorBlendAttachment;

    std::array<VkDynamicState, 2> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 64;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    VkPipelineLayout layout;
    if (symbols.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
        symbols.vkDestroyShaderModule(device, meshModule, nullptr);
        symbols.vkDestroyShaderModule(device, fragModule, nullptr);
        throw std::runtime_error("Failed to create pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipelineHandle;
    if (symbols.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipelineHandle) != VK_SUCCESS) {
        symbols.vkDestroyPipelineLayout(device, layout, nullptr);
        symbols.vkDestroyShaderModule(device, meshModule, nullptr);
        symbols.vkDestroyShaderModule(device, fragModule, nullptr);
        throw std::runtime_error("Failed to create mesh pipeline");
    }

    symbols.vkDestroyShaderModule(device, meshModule, nullptr);
    symbols.vkDestroyShaderModule(device, fragModule, nullptr);

    pipeline.layout = layout;
    pipeline.pipeline = pipelineHandle;
}

void VulkanDevice::RebuildMeshPipelines()
{
    for (auto& entry : meshPipelines) {
        if (entry.second.meshCode.empty() || entry.second.fragmentCode.empty()) {
            continue;
        }
        CreatePipelineObjects(entry.second);
    }
}

void VulkanDevice::CreateInstance(bool enableValidation)
{
    auto& symbols = VulkanSymbols::Get();

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "TerraEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "TerraEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#else
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME
#endif
    };

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (enableValidation) {
        static const char* validationLayers[] = { "VK_LAYER_KHRONOS_validation" };
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = validationLayers;
    }

    if (symbols.vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Vulkan instance");
    }

    symbols.EnsureInstanceFunctions(instance);
}

void VulkanDevice::CreateSurface()
{
    auto& symbols = VulkanSymbols::Get();
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = windowHandle.instance;
    createInfo.hwnd = windowHandle.window;

    if (symbols.vkCreateWin32SurfaceKHR(instance, &createInfo, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Win32 surface");
    }
#else
    VkXlibSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    createInfo.dpy = reinterpret_cast<Display*>(windowHandle.display);
    createInfo.window = static_cast<Window>(windowHandle.window);

    if (symbols.vkCreateXlibSurfaceKHR(instance, &createInfo, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create Xlib surface");
    }
#endif
}

SwapchainSupportDetails VulkanDevice::QuerySwapchainSupport(VkPhysicalDevice device)
{
    auto& symbols = VulkanSymbols::Get();
    SwapchainSupportDetails details{};
    symbols.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t formatCount = 0;
    symbols.vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        symbols.vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount = 0;
    symbols.vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        symbols.vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

QueueFamilyIndices VulkanDevice::FindQueueFamilies(VkPhysicalDevice device)
{
    QueueFamilyIndices indices;
    auto& symbols = VulkanSymbols::Get();

    uint32_t queueFamilyCount = 0;
    symbols.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    symbols.vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    uint32_t i = 0;
    for (const auto& queueFamily : queueFamilies) {
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }

        if (queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT) {
            indices.computeFamily = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        symbols.vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
        if (presentSupport == VK_TRUE) {
            indices.presentFamily = i;
        }

        if (indices.IsComplete()) {
            break;
        }

        ++i;
    }

    if (!indices.computeFamily.has_value()) {
        indices.computeFamily = indices.graphicsFamily;
    }

    return indices;
}

bool VulkanDevice::CheckDeviceExtensionSupport(VkPhysicalDevice device)
{
    auto& symbols = VulkanSymbols::Get();
    uint32_t extensionCount = 0;
    symbols.vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    symbols.vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::vector<const char*> required = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME
    };

    for (const char* requiredName : required) {
        bool found = false;
        for (const auto& ext : availableExtensions) {
            if (std::strcmp(ext.extensionName, requiredName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

void VulkanDevice::PickPhysicalDevice()
{
    auto& symbols = VulkanSymbols::Get();
    uint32_t deviceCount = 0;
    symbols.vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("Failed to find GPUs with Vulkan support");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    symbols.vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (auto candidate : devices) {
        QueueFamilyIndices indices = FindQueueFamilies(candidate);
        if (!indices.IsComplete()) {
            continue;
        }

        if (!CheckDeviceExtensionSupport(candidate)) {
            continue;
        }

        SwapchainSupportDetails swapchainSupport = QuerySwapchainSupport(candidate);
        if (swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) {
            continue;
        }

        VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{};
        meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &meshFeatures;

        symbols.vkGetPhysicalDeviceFeatures2(candidate, &features2);
        if (meshFeatures.meshShader != VK_TRUE) {
            continue;
        }

        physicalDevice = candidate;
        queueFamilyIndices = indices;
        break;
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to find a suitable GPU");
    }
}

void VulkanDevice::CreateDeviceAndQueues()
{
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::vector<uint32_t> uniqueFamilies;

    uniqueFamilies.push_back(queueFamilyIndices.graphicsFamily.value());
    if (queueFamilyIndices.presentFamily != queueFamilyIndices.graphicsFamily) {
        uniqueFamilies.push_back(queueFamilyIndices.presentFamily.value());
    }
    if (queueFamilyIndices.computeFamily != queueFamilyIndices.graphicsFamily && queueFamilyIndices.computeFamily != queueFamilyIndices.presentFamily) {
        uniqueFamilies.push_back(queueFamilyIndices.computeFamily.value());
    }

    float queuePriority = 1.0f;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = family;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{};
    meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    meshFeatures.meshShader = VK_TRUE;
    meshFeatures.taskShader = VK_FALSE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features.multiDrawIndirect = VK_TRUE;
    features2.features.drawIndirectFirstInstance = VK_TRUE;
    features2.pNext = &meshFeatures;

    std::vector<const char*> extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME
    };

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.pNext = &features2;

    auto& symbols = VulkanSymbols::Get();
    if (symbols.vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create logical device");
    }

    symbols.EnsureDeviceFunctions(device);

    symbols.vkGetDeviceQueue(device, queueFamilyIndices.graphicsFamily.value(), 0, &graphicsQueue);
    symbols.vkGetDeviceQueue(device, queueFamilyIndices.presentFamily.value(), 0, &presentQueue);
    symbols.vkGetDeviceQueue(device, queueFamilyIndices.computeFamily.value(), 0, &computeQueue);
}

uint32_t VulkanDevice::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    auto& symbols = VulkanSymbols::Get();
    VkPhysicalDeviceMemoryProperties memProperties{};
    symbols.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

VkFormat VulkanDevice::FindDepthFormat()
{
    const std::array<VkFormat, 3> candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };

    auto& symbols = VulkanSymbols::Get();
    for (VkFormat format : candidates) {
        VkFormatProperties props{};
        symbols.vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }

    throw std::runtime_error("Failed to find supported depth format");
}

void VulkanDevice::CreateDepthResources()
{
    depthFormat = FindDepthFormat();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = swapchainExtent.width;
    imageInfo.extent.height = swapchainExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = depthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    auto& symbols = VulkanSymbols::Get();
    if (symbols.vkCreateImage(device, &imageInfo, nullptr, &depthImage) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth image");
    }

    VkMemoryRequirements memRequirements{};
    symbols.vkGetImageMemoryRequirements(device, depthImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (symbols.vkAllocateMemory(device, &allocInfo, nullptr, &depthMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate depth memory");
    }

    symbols.vkBindImageMemory(device, depthImage, depthMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (symbols.vkCreateImageView(device, &viewInfo, nullptr, &depthImageView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create depth image view");
    }
}

void VulkanDevice::CreateRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    auto& symbols = VulkanSymbols::Get();
    if (symbols.vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pass");
    }
}

void VulkanDevice::CreateFramebuffers()
{
    auto& symbols = VulkanSymbols::Get();
    framebuffers.resize(swapchainImageViews.size());

    for (size_t i = 0; i < swapchainImageViews.size(); ++i) {
        std::array<VkImageView, 2> attachments = { swapchainImageViews[i], depthImageView };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapchainExtent.width;
        framebufferInfo.height = swapchainExtent.height;
        framebufferInfo.layers = 1;

        if (symbols.vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create framebuffer");
        }
    }
}

VkSurfaceFormatKHR VulkanDevice::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats)
{
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }
    return availableFormats.front();
}

VkPresentModeKHR VulkanDevice::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes)
{
    for (const auto& availablePresentMode : availablePresentModes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanDevice::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities)
{
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    VkExtent2D actualExtent = { 1280, 720 };
    actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return actualExtent;
}

void VulkanDevice::CreateSwapchainResources()
{
    auto& symbols = VulkanSymbols::Get();
    SwapchainSupportDetails swapchainSupport = QuerySwapchainSupport(physicalDevice);

    VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapchainSupport.formats);
    VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapchainSupport.presentModes);
    VkExtent2D extent = ChooseSwapExtent(swapchainSupport.capabilities);

    uint32_t imageCount = swapchainSupport.capabilities.minImageCount + 1;
    if (swapchainSupport.capabilities.maxImageCount > 0 && imageCount > swapchainSupport.capabilities.maxImageCount) {
        imageCount = swapchainSupport.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queueFamilyIndicesArray[] = {
        queueFamilyIndices.graphicsFamily.value(),
        queueFamilyIndices.presentFamily.value()
    };

    if (queueFamilyIndices.graphicsFamily != queueFamilyIndices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndicesArray;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapchainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (symbols.vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swapchain");
    }

    symbols.vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    swapchainImages.resize(imageCount);
    symbols.vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());

    swapchainFormat = surfaceFormat.format;
    swapchainExtent = extent;

    swapchainImageViews.resize(swapchainImages.size());
    for (size_t i = 0; i < swapchainImages.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (symbols.vkCreateImageView(device, &viewInfo, nullptr, &swapchainImageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image views");
        }
    }

    CreateDepthResources();
    CreateRenderPass();
    CreateFramebuffers();
}

void VulkanDevice::CreateCommandPools()
{
    auto& symbols = VulkanSymbols::Get();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    if (symbols.vkCreateCommandPool(device, &poolInfo, nullptr, &graphicsCommandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create graphics command pool");
    }

    if (queueFamilyIndices.computeFamily != queueFamilyIndices.graphicsFamily) {
        poolInfo.queueFamilyIndex = queueFamilyIndices.computeFamily.value();
        if (symbols.vkCreateCommandPool(device, &poolInfo, nullptr, &computeCommandPool) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create compute command pool");
        }
    } else {
        computeCommandPool = graphicsCommandPool;
    }

    frames.resize(2);

    std::vector<VkCommandBuffer> commandBuffers(frames.size());
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = graphicsCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

    if (symbols.vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }

    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i].commandBuffer = commandBuffers[i];
    }
}

void VulkanDevice::CreateSyncObjects()
{
    auto& symbols = VulkanSymbols::Get();

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto& frame : frames) {
        if (symbols.vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAvailable) != VK_SUCCESS ||
            symbols.vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.renderFinished) != VK_SUCCESS ||
            symbols.vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlightFence) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create synchronization objects");
        }
    }
}

void VulkanDevice::CleanupSwapchain()
{
    auto& symbols = VulkanSymbols::Get();

    for (auto& entry : meshPipelines) {
        if (entry.second.pipeline != VK_NULL_HANDLE) {
            symbols.vkDestroyPipeline(device, entry.second.pipeline, nullptr);
            entry.second.pipeline = VK_NULL_HANDLE;
        }
        if (entry.second.layout != VK_NULL_HANDLE) {
            symbols.vkDestroyPipelineLayout(device, entry.second.layout, nullptr);
            entry.second.layout = VK_NULL_HANDLE;
        }
    }

    for (auto framebuffer : framebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            symbols.vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
    }
    framebuffers.clear();

    if (renderPass != VK_NULL_HANDLE) {
        symbols.vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }

    if (depthImageView != VK_NULL_HANDLE) {
        symbols.vkDestroyImageView(device, depthImageView, nullptr);
        depthImageView = VK_NULL_HANDLE;
    }

    if (depthImage != VK_NULL_HANDLE) {
        symbols.vkDestroyImage(device, depthImage, nullptr);
        depthImage = VK_NULL_HANDLE;
    }

    if (depthMemory != VK_NULL_HANDLE) {
        symbols.vkFreeMemory(device, depthMemory, nullptr);
        depthMemory = VK_NULL_HANDLE;
    }

    for (auto view : swapchainImageViews) {
        symbols.vkDestroyImageView(device, view, nullptr);
    }
    swapchainImageViews.clear();

    if (swapchain != VK_NULL_HANDLE) {
        symbols.vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

bool VulkanDevice::RecreateSwapchain()
{
    auto& symbols = VulkanSymbols::Get();
    symbols.vkDeviceWaitIdle(device);
    CleanupSwapchain();
    CreateSwapchainResources();
    return true;
}

void VulkanDevice::Cleanup()
{
    auto& symbols = VulkanSymbols::Get();

    CleanupSwapchain();

    for (auto& frame : frames) {
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            symbols.vkDestroySemaphore(device, frame.imageAvailable, nullptr);
        }
        if (frame.renderFinished != VK_NULL_HANDLE) {
            symbols.vkDestroySemaphore(device, frame.renderFinished, nullptr);
        }
        if (frame.inFlightFence != VK_NULL_HANDLE) {
            symbols.vkDestroyFence(device, frame.inFlightFence, nullptr);
        }
    }
    frames.clear();

    if (computeCommandPool != VK_NULL_HANDLE && computeCommandPool != graphicsCommandPool) {
        symbols.vkDestroyCommandPool(device, computeCommandPool, nullptr);
        computeCommandPool = VK_NULL_HANDLE;
    }

    if (graphicsCommandPool != VK_NULL_HANDLE) {
        symbols.vkDestroyCommandPool(device, graphicsCommandPool, nullptr);
        graphicsCommandPool = VK_NULL_HANDLE;
    }

    for (auto& pipeline : meshPipelines) {
        if (pipeline.second.pipeline != VK_NULL_HANDLE) {
            symbols.vkDestroyPipeline(device, pipeline.second.pipeline, nullptr);
            pipeline.second.pipeline = VK_NULL_HANDLE;
        }
        if (pipeline.second.layout != VK_NULL_HANDLE) {
            symbols.vkDestroyPipelineLayout(device, pipeline.second.layout, nullptr);
            pipeline.second.layout = VK_NULL_HANDLE;
        }
    }
    meshPipelines.clear();

    if (device != VK_NULL_HANDLE) {
        symbols.vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }

    if (surface != VK_NULL_HANDLE) {
        symbols.vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }

    if (instance != VK_NULL_HANDLE) {
        symbols.vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

} // namespace

std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info)
{
    return std::make_unique<VulkanDevice>(info);
}

} // namespace RHI::Vulkan
