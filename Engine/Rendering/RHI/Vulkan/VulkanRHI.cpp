#include "VulkanRHI.h"

#include <array>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
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
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;

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
        LoadInstanceFunction(vkCreateDevice, inst, "vkCreateDevice");

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
        LoadDeviceFunction(vkQueueSubmit, dev, "vkQueueSubmit");
        LoadDeviceFunction(vkQueueWaitIdle, dev, "vkQueueWaitIdle");
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

class VulkanDevice : public Device
{
public:
    explicit VulkanDevice(const DeviceCreateInfo& info)
        : windowHandle(info.windowHandle)
    {
        CreateInstance(info.enableValidation);
        PickPhysicalDevice();
        CreateDeviceAndQueues();
        CreateCommandPool();
    }

    ~VulkanDevice() override
    {
        Cleanup();
    }

    void SubmitMeshTask(const MeshTaskDesc& desc) override
    {
        SubmitToQueue(desc.debugName, desc.recordFunc, graphicsQueue, graphicsQueueFamilyIndex);
    }

    void SubmitComputeTask(const ComputeTaskDesc& desc) override
    {
        SubmitToQueue(desc.debugName, desc.recordFunc, computeQueue, computeQueueFamilyIndex);
    }

    void Present() override
    {
        // Swapchain support is not yet wired. For now just wait for the device to be idle
        // which ensures that GPU work submitted this frame has completed.
        WaitIdle();
    }

    void WaitIdle() override
    {
        if (device != VK_NULL_HANDLE) {
            VulkanSymbols::Get().vkDeviceWaitIdle(device);
        }
    }

private:
    void CreateInstance(bool enableValidation)
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

        VkResult result = symbols.vkCreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create Vulkan instance");
        }

        symbols.EnsureInstanceFunctions(instance);
    }

    void PickPhysicalDevice()
    {
        auto& symbols = VulkanSymbols::Get();
        uint32_t deviceCount = 0;
        symbols.vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            throw std::runtime_error("Failed to find GPUs with Vulkan support");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        symbols.vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        physicalDevice = devices.front();

        uint32_t queueFamilyCount = 0;
        symbols.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        symbols.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && graphicsQueueFamilyIndex == UINT32_MAX) {
                graphicsQueueFamilyIndex = i;
            }

            if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && computeQueueFamilyIndex == UINT32_MAX) {
                computeQueueFamilyIndex = i;
            }
        }

        if (graphicsQueueFamilyIndex == UINT32_MAX) {
            throw std::runtime_error("Failed to find graphics queue family");
        }

        if (computeQueueFamilyIndex == UINT32_MAX) {
            computeQueueFamilyIndex = graphicsQueueFamilyIndex;
        }
    }

    void CreateDeviceAndQueues()
    {
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::vector<uint32_t> uniqueFamilies;

        uniqueFamilies.push_back(graphicsQueueFamilyIndex);
        if (computeQueueFamilyIndex != graphicsQueueFamilyIndex) {
            uniqueFamilies.push_back(computeQueueFamilyIndex);
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

        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.multiDrawIndirect = VK_TRUE;
        deviceFeatures.drawIndirectFirstInstance = VK_TRUE;

        std::vector<const char*> extensions;
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        auto& symbols = VulkanSymbols::Get();
        VkResult result = symbols.vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create logical device");
        }

        symbols.EnsureDeviceFunctions(device);

        symbols.vkGetDeviceQueue(device, graphicsQueueFamilyIndex, 0, &graphicsQueue);
        symbols.vkGetDeviceQueue(device, computeQueueFamilyIndex, 0, &computeQueue);
    }

    void CreateCommandPool()
    {
        auto& symbols = VulkanSymbols::Get();
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsQueueFamilyIndex;

        VkResult result = symbols.vkCreateCommandPool(device, &poolInfo, nullptr, &graphicsCommandPool);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create graphics command pool");
        }

        if (computeQueueFamilyIndex == graphicsQueueFamilyIndex) {
            computeCommandPool = graphicsCommandPool;
            return;
        }

        poolInfo.queueFamilyIndex = computeQueueFamilyIndex;
        result = symbols.vkCreateCommandPool(device, &poolInfo, nullptr, &computeCommandPool);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create compute command pool");
        }
    }

    VkCommandBuffer AllocateCommandBuffer(VkCommandPool pool)
    {
        auto& symbols = VulkanSymbols::Get();
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmd;
        VkResult result = symbols.vkAllocateCommandBuffers(device, &allocInfo, &cmd);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate command buffer");
        }

        return cmd;
    }

    void SubmitToQueue(const std::string& debugName,
        const std::function<void()>& recordFunc,
        VkQueue queue,
        uint32_t queueFamily)
    {
        if (!recordFunc) {
            return;
        }

        VkCommandPool commandPool = (queueFamily == graphicsQueueFamilyIndex) ? graphicsCommandPool : computeCommandPool;

        std::lock_guard<std::mutex> lock(commandPoolMutex);
        VkCommandBuffer cmd = AllocateCommandBuffer(commandPool);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        auto& symbols = VulkanSymbols::Get();

        symbols.vkBeginCommandBuffer(cmd, &beginInfo);

        recordFunc();

        symbols.vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkResult result = symbols.vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            symbols.vkFreeCommandBuffers(device, commandPool, 1, &cmd);
            throw std::runtime_error("Failed to submit command buffer");
        }

        symbols.vkQueueWaitIdle(queue);
        symbols.vkFreeCommandBuffers(device, commandPool, 1, &cmd);

        if (!debugName.empty()) {
            std::cout << "Executed Vulkan task: " << debugName << std::endl;
        }
    }

    void Cleanup()
    {
        auto& symbols = VulkanSymbols::Get();
        if (device != VK_NULL_HANDLE) {
            symbols.vkDeviceWaitIdle(device);
        }

        if (graphicsCommandPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
            symbols.vkDestroyCommandPool(device, graphicsCommandPool, nullptr);
        }

        if (computeCommandPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE && computeCommandPool != graphicsCommandPool) {
            symbols.vkDestroyCommandPool(device, computeCommandPool, nullptr);
        }

        if (device != VK_NULL_HANDLE) {
            symbols.vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }

        if (instance != VK_NULL_HANDLE) {
            symbols.vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;

    VkCommandPool graphicsCommandPool = VK_NULL_HANDLE;
    VkCommandPool computeCommandPool = VK_NULL_HANDLE;

    uint32_t graphicsQueueFamilyIndex = UINT32_MAX;
    uint32_t computeQueueFamilyIndex = UINT32_MAX;

    std::mutex commandPoolMutex;
    NativeWindowHandle windowHandle{};
};

} // namespace

std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info)
{
    return std::make_unique<VulkanDevice>(info);
}

} // namespace RHI::Vulkan

