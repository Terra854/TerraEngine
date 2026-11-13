// This implementation is validation-only. VMA entry points stay local to this
// translation unit; explicit dispatch prevents accidental Engine-module globals.
// The Engine exports volk's C loader names. Give this one bundled implementation
// private validation names so static/import-library order cannot bind the fixture
// to Engine loader functions while leaving its local PFN data uninitialized.
#define volkInitialize TerraReviewVolkInitialize
#define volkInitializeCustom TerraReviewVolkInitializeCustom
#define volkFinalize TerraReviewVolkFinalize
#define volkGetInstanceVersion TerraReviewVolkGetInstanceVersion
#define volkLoadInstance TerraReviewVolkLoadInstance
#define volkLoadInstanceOnly TerraReviewVolkLoadInstanceOnly
#define volkLoadDevice TerraReviewVolkLoadDevice
#define volkGetLoadedInstance TerraReviewVolkGetLoadedInstance
#define volkGetLoadedDevice TerraReviewVolkGetLoadedDevice
#define volkLoadInstanceTable TerraReviewVolkLoadInstanceTable
#define volkLoadDeviceTable TerraReviewVolkLoadDeviceTable
#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION
#define VMA_CALL_PRE static
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "VulkanReviewFixture.h"
#include "Tests/Support/ProcessCommandLine.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include "Tests/Editor/TestSha256.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace TerraEngine::Tests::Review
{
	namespace
	{
		std::mutex fixtureOwnership;
		std::atomic_uint64_t nextSession{1};
		void Check(VkResult result, const char* operation)
		{
			if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
		}
		std::string FileHash(const std::filesystem::path& path)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file) throw std::runtime_error("Cannot hash native input artifact: " + path.string());
			const std::string bytes{std::istreambuf_iterator<char>(file), {}};
			return TerraEngine::EditorTests::ComputeTestSha256(bytes);
		}
		std::string Quote(const std::filesystem::path& path)
		{
			return TerraEngine::Tests::QuoteProcessPath(path);
		}
	}
	struct VulkanReviewFixture::State
	{
		std::unique_lock<std::mutex> exclusive{fixtureOwnership};
		Native native;
		VolkDeviceTable functions{};
		VkQueue queue = VK_NULL_HANDLE;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties properties{};
		std::string name, deviceName, engineModule;
		std::uint64_t session = nextSession.fetch_add(1), nextCommand = 1, nextSubmit = 1;
		bool unsafe = false, engineDispatch = false, recording = false;
		std::atomic_bool diagnosticsIncomplete{false};
		std::vector<VulkanReviewBuffer> buffers;
		std::vector<std::pair<VulkanReviewImage, VmaAllocation>> images;
		std::vector<std::function<void(const Native&)>> cleanup;
		std::vector<VulkanReviewSubmission> submissions;
		nlohmann::json enabledFeatures;
		nlohmann::json requiredImageFormats = nlohmann::json::array();
		mutable std::mutex messageMutex;
		std::vector<std::string> messages, errors;
		static VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) noexcept
		{
			auto& state = *static_cast<State*>(user);
			try
			{
				std::lock_guard lock(state.messageMutex);
				const char* message = data && data->pMessage ? data->pMessage : "Missing Vulkan diagnostic";
				if (!data || !data->pMessage) state.diagnosticsIncomplete.store(true, std::memory_order_relaxed);
				const auto length = std::strlen(message);
				if (state.messages.size() >= 256 || length > 4096) { state.diagnosticsIncomplete.store(true, std::memory_order_relaxed); return VK_FALSE; }
				state.messages.emplace_back(message, length);
				if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) state.errors.push_back(state.messages.back());
			}
			catch (...) { state.diagnosticsIncomplete.store(true, std::memory_order_relaxed); }
			return VK_FALSE;
		}
		void ReleaseCompletedDevice()
		{
			// Every accepted submission has completed before normal cleanup. The
			// fixture destructor quarantines this entire owner on ambiguous work.
			if (native.device)
			{
				for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it)
				{
					try { (*it)(native); }
					catch (...) { diagnosticsIncomplete.store(true, std::memory_order_relaxed); std::fputs("Native fixture cleanup callback failed.\n", stderr); }
				}
				for (const auto& [image, allocation] : images)
				{
					functions.vkDestroyImageView(native.device, image.view, nullptr);
					vmaDestroyImage(native.allocator, image.handle, allocation);
				}
				for (const auto& buffer : buffers) vmaDestroyBuffer(native.allocator, buffer.handle, buffer.allocation);
				if (native.allocator) vmaDestroyAllocator(native.allocator);
				if (pool) functions.vkDestroyCommandPool(native.device, pool, nullptr);
				functions.vkDestroyDevice(native.device, nullptr);
				native.device = VK_NULL_HANDLE; native.allocator = VK_NULL_HANDLE; pool = VK_NULL_HANDLE;
				cleanup.clear(); images.clear(); buffers.clear();
			}
		}
		~State()
		{
			ReleaseCompletedDevice();
			if (messenger) vkDestroyDebugUtilsMessengerEXT(native.instance, messenger, nullptr);
			if (native.instance) vkDestroyInstance(native.instance, nullptr);
		}
	};
	VulkanReviewFixture::Options VulkanReviewFixture::NativeDeviceOptions()
	{
		Options options;
		// Keep device selection in one place so suites can run on other hardware
		// without dropping validation, feature checks or exclusive native ownership.
		if(const char* name = std::getenv("TERRAENGINE_VULKAN_REVIEW_DEVICE"); name && *name)
			options.requiredDeviceName = name;
		return options;
	}
	VulkanReviewFixture::VulkanReviewFixture(std::string name) : VulkanReviewFixture(std::move(name), Options{}) {}
	VulkanReviewFixture::VulkanReviewFixture(std::string name, Options options) : state(std::make_unique<State>())
	{
		state->name = std::move(name);
		if (!IsWatchedChild(state->name)) throw std::runtime_error("Native fixture requires its single-case watched child.");
		Check(volkInitialize(), "volkInitialize");
		std::uint32_t count = 0;
		Check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate layers");
		std::vector<VkLayerProperties> layers(count);
		Check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "read layers");
		const bool validation = std::any_of(layers.begin(), layers.end(), [](const auto& layer) { return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
		if (options.requireValidation && !validation) throw std::runtime_error("Khronos validation layer is unavailable.");
		const char* layer = "VK_LAYER_KHRONOS_validation";
		const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
		app.pApplicationName = "TerraEngine watched native review fixture";
		VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance.pApplicationInfo = &app;
		instance.enabledExtensionCount = 1; instance.ppEnabledExtensionNames = &extension;
		instance.enabledLayerCount = validation ? 1u : 0u; instance.ppEnabledLayerNames = &layer;
		Check(vkCreateInstance(&instance, nullptr, &state->native.instance), "create instance");
		volkLoadInstance(state->native.instance);
		VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debug.pfnUserCallback = State::Message; debug.pUserData = state.get();
		Check(vkCreateDebugUtilsMessengerEXT(state->native.instance, &debug, nullptr, &state->messenger), "create validation messenger");
		Check(vkEnumeratePhysicalDevices(state->native.instance, &count, nullptr), "enumerate devices");
		std::vector<VkPhysicalDevice> devices(count);
		Check(vkEnumeratePhysicalDevices(state->native.instance, &count, devices.data()), "read devices");
		for (auto device : devices)
		{
			VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(device, &properties);
			if ((!options.requiredDeviceName.empty() && options.requiredDeviceName != properties.deviceName)
				|| (options.requiredVendor && options.requiredVendor != properties.vendorID)
				|| (options.requiredDevice && options.requiredDevice != properties.deviceID)
				|| (options.requireHardware && properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)) continue;
			if (state->native.physical) throw std::runtime_error("Multiple matching adapters: specify exact observed vendor/device identity.");
			state->native.physical = device; state->properties = properties;
		}
		if (!state->native.physical) throw std::runtime_error("Required native adapter is unavailable; software fallback is disabled.");
		state->deviceName = state->properties.deviceName;
		VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		supported12.pNext = &supported13;
		std::uint32_t extensionCount = 0;
		Check(vkEnumerateDeviceExtensionProperties(state->native.physical, nullptr, &extensionCount, nullptr), "enumerate device extensions");
		std::vector<VkExtensionProperties> extensions(extensionCount);
		Check(vkEnumerateDeviceExtensionProperties(state->native.physical, nullptr, &extensionCount, extensions.data()), "read device extensions");
		const char* derivativeExtension = nullptr;
		for (const char* candidate : {"VK_KHR_compute_shader_derivatives", "VK_NV_compute_shader_derivatives"})
			if (std::any_of(extensions.begin(), extensions.end(), [&](const auto& row) { return std::strcmp(row.extensionName, candidate) == 0; })) { derivativeExtension = candidate; break; }
		VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
		if (derivativeExtension) supported13.pNext = &derivatives;
		VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; supported.pNext = &supported12;
		vkGetPhysicalDeviceFeatures2(state->native.physical, &supported);
		if ((options.requireRuntimeDescriptorArrays && !supported12.runtimeDescriptorArray)
			|| (options.requireShaderDemoteToHelperInvocation && !supported13.shaderDemoteToHelperInvocation)
			|| (options.requireSampledImageArrayDynamicIndexing && !supported.features.shaderSampledImageArrayDynamicIndexing)
			|| (options.requireSampledImageArrayNonUniformIndexing && !supported12.shaderSampledImageArrayNonUniformIndexing)
			|| (options.requireStorageImageExtendedFormats && !supported.features.shaderStorageImageExtendedFormats)
			|| (options.requireShaderBufferInt64Atomics && !supported12.shaderBufferInt64Atomics)
			|| (options.requireShaderSharedInt64Atomics && !supported12.shaderSharedInt64Atomics)
			|| (options.requireComputeDerivativeGroupQuads && (!derivativeExtension || !derivatives.computeDerivativeGroupQuads))
			|| (options.requireShaderInt64 && !supported.features.shaderInt64)) throw std::runtime_error("Required native shader features are unavailable.");
		VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		enabled13.shaderDemoteToHelperInvocation = options.requireShaderDemoteToHelperInvocation;
		enabled12.pNext = &enabled13;
		VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR enabledDerivatives{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
		enabledDerivatives.computeDerivativeGroupQuads = options.requireComputeDerivativeGroupQuads ? VK_TRUE : VK_FALSE;
		if (options.requireComputeDerivativeGroupQuads) enabled13.pNext = &enabledDerivatives;
		enabled12.runtimeDescriptorArray = options.requireRuntimeDescriptorArrays;
		enabled12.shaderSampledImageArrayNonUniformIndexing = options.requireSampledImageArrayNonUniformIndexing;
		enabled12.shaderBufferInt64Atomics = options.requireShaderBufferInt64Atomics;
		enabled12.shaderSharedInt64Atomics = options.requireShaderSharedInt64Atomics;
		VkPhysicalDeviceFeatures enabled{};
		enabled.shaderInt64 = options.requireShaderInt64;
		enabled.shaderStorageImageExtendedFormats = options.requireStorageImageExtendedFormats;
		enabled.shaderSampledImageArrayDynamicIndexing = options.requireSampledImageArrayDynamicIndexing;
		state->enabledFeatures = {{"shaderDemoteToHelperInvocation", bool(enabled13.shaderDemoteToHelperInvocation)}, {"shaderInt64", bool(enabled.shaderInt64)}, {"storageImageExtendedFormats", bool(enabled.shaderStorageImageExtendedFormats)},
			{"runtimeDescriptorArrays", bool(enabled12.runtimeDescriptorArray)}, {"sampledImageArrayNonUniformIndexing", bool(enabled12.shaderSampledImageArrayNonUniformIndexing)},
			{"sampledImageArrayDynamicIndexing", bool(enabled.shaderSampledImageArrayDynamicIndexing)},
			{"shaderBufferInt64Atomics", bool(enabled12.shaderBufferInt64Atomics)}, {"shaderSharedInt64Atomics", bool(enabled12.shaderSharedInt64Atomics)},
			{"computeDerivativeGroupQuads", options.requireComputeDerivativeGroupQuads}, {"derivativeExtension", options.requireComputeDerivativeGroupQuads ? derivativeExtension : ""}};
		vkGetPhysicalDeviceQueueFamilyProperties(state->native.physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> queues(count);
		vkGetPhysicalDeviceQueueFamilyProperties(state->native.physical, &count, queues.data());
		auto queue = std::find_if(queues.begin(), queues.end(), [](const auto& row) {
			return row.queueCount && (row.queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) == (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT);
		});
		if (queue == queues.end()) throw std::runtime_error("A graphics/compute queue is required.");
		state->native.queueFamily = static_cast<std::uint32_t>(queue - queues.begin());
		const float priority = 1;
		VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queueInfo.queueFamilyIndex = state->native.queueFamily; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
		VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device.pNext = &enabled12;
		if (options.requireComputeDerivativeGroupQuads) { device.enabledExtensionCount = 1; device.ppEnabledExtensionNames = &derivativeExtension; }
		device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queueInfo; device.pEnabledFeatures = &enabled;
		Check(vkCreateDevice(state->native.physical, &device, nullptr, &state->native.device), "create device");
		volkLoadDeviceTable(&state->functions, state->native.device); state->native.functions = &state->functions;
		state->functions.vkGetDeviceQueue(state->native.device, state->native.queueFamily, 0, &state->queue);
		VmaVulkanFunctions allocatorFunctions{};
		allocatorFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr; allocatorFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
		VmaAllocatorCreateInfo allocator{}; allocator.instance = state->native.instance; allocator.physicalDevice = state->native.physical;
		allocator.device = state->native.device; allocator.vulkanApiVersion = VK_API_VERSION_1_3; allocator.pVulkanFunctions = &allocatorFunctions;
		Check(vmaCreateAllocator(&allocator, &state->native.allocator), "create VMA allocator");
		VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = state->native.queueFamily;
		pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		Check(state->functions.vkCreateCommandPool(state->native.device, &pool, nullptr, &state->pool), "create command pool");
	}
	VulkanReviewFixture::~VulkanReviewFixture()
	{
		// A timed-out/ambiguous submission cannot authorize destruction. Preserve
		// this ENTIRE owner (including its dispatch lock) until watched-child exit.
		// The parent watchdog terminates the isolated case; no pending buffer is
		// freed by normal C++ cleanup and no unbounded wait blocks the parent.
		if (state && state->unsafe) static_cast<void>(state.release());
	}
	const VulkanReviewFixture::Native& VulkanReviewFixture::GetNative() const
	{
		if (state->unsafe || !state->native.device) throw std::runtime_error("Quarantined/finished native handles cannot admit further client operations.");
		return state->native;
	}
	const std::string& VulkanReviewFixture::DeviceName() const { return state->deviceName; }
	VulkanReviewBuffer VulkanReviewFixture::CreateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, std::span<const std::byte> initial)
	{
		if (state->unsafe || !state->native.device) throw std::runtime_error("Quarantined/finished owner cannot allocate a buffer.");
		if (!bytes || initial.size() > bytes) throw std::runtime_error("Invalid owned buffer extent.");
		VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = bytes;
		info.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VmaAllocationCreateInfo memory{}; memory.usage = VMA_MEMORY_USAGE_AUTO;
		memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
		VulkanReviewBuffer result; result.bytes = bytes;
		Check(vmaCreateBuffer(state->native.allocator, &info, &memory, &result.handle, &result.allocation, nullptr), "create owned buffer");
		try { state->buffers.push_back(result); }
		catch (...) { vmaDestroyBuffer(state->native.allocator, result.handle, result.allocation); throw; }
		if (!initial.empty()) Upload(result, initial);
		return result;
	}
	void VulkanReviewFixture::Upload(const VulkanReviewBuffer& buffer, std::span<const std::byte> bytes, VkDeviceSize offset)
	{
		if (state->unsafe || !state->native.device) throw std::runtime_error("Quarantined/finished owner cannot map or write upload bytes.");
		if (bytes.empty()) return;
		if (offset > buffer.bytes || bytes.size() > buffer.bytes - offset) throw std::runtime_error("Upload interval exceeds owned buffer.");
		void* mapped = nullptr; Check(vmaMapMemory(state->native.allocator, buffer.allocation, &mapped), "map upload");
		std::memcpy(static_cast<std::byte*>(mapped) + offset, bytes.data(), bytes.size());
		const auto flushed = vmaFlushAllocation(state->native.allocator, buffer.allocation, offset, bytes.size());
		vmaUnmapMemory(state->native.allocator, buffer.allocation); Check(flushed, "flush upload");
	}
	std::vector<std::byte> VulkanReviewFixture::Read(const VulkanReviewBuffer& buffer, VkDeviceSize bytes)
	{
		if (state->unsafe || !state->native.device || state->submissions.empty() || !state->submissions.back().completed) throw std::runtime_error("Readback requires real completed submission ownership.");
		if (bytes == VK_WHOLE_SIZE) bytes = buffer.bytes;
		if (bytes > buffer.bytes) throw std::runtime_error("Readback exceeds owned buffer.");
		std::vector<std::byte> result(static_cast<std::size_t>(bytes));
		void* mapped = nullptr; Check(vmaMapMemory(state->native.allocator, buffer.allocation, &mapped), "map readback");
		const auto invalidated = vmaInvalidateAllocation(state->native.allocator, buffer.allocation, 0, bytes);
		if (invalidated == VK_SUCCESS) std::memcpy(result.data(), mapped, result.size());
		vmaUnmapMemory(state->native.allocator, buffer.allocation); Check(invalidated, "invalidate readback");
		return result;
	}
	VulkanReviewImage VulkanReviewFixture::CreateImage(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage)
	{
		if (state->unsafe || !state->native.device) throw std::runtime_error("Quarantined/finished owner cannot allocate an image.");
		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format; info.extent = {width, height, 1}; info.mipLevels = info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		// Extended storage formats do not require enabling a blanket feature.
		// Qualify this exact optimal-tiling/usage combination before allocation;
		// unsupported input remains an ordinary prerequisite failure.
		VkImageFormatProperties properties{};
		const auto formatResult = vkGetPhysicalDeviceImageFormatProperties(state->native.physical,
			format, info.imageType, info.tiling, info.usage, info.flags, &properties);
		state->requiredImageFormats.push_back({{"format", format}, {"tiling", info.tiling}, {"usage", info.usage},
			{"width", width}, {"height", height}, {"result", formatResult}});
		Check(formatResult, "qualify owned image format/tiling/usage");
		if (width > properties.maxExtent.width || height > properties.maxExtent.height
			|| properties.maxExtent.depth < 1 || properties.maxMipLevels < 1 || properties.maxArrayLayers < 1
			|| !(properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) throw std::runtime_error("Required native image dimensions or samples are unsupported.");
		VmaAllocationCreateInfo memory{}; memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		VulkanReviewImage result; result.width = width; result.height = height; result.format = format;
		VmaAllocation allocation = VK_NULL_HANDLE;
		Check(vmaCreateImage(state->native.allocator, &info, &memory, &result.handle, &allocation, nullptr), "create owned image");
		VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = result.handle;
		view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
		view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		const auto status = state->functions.vkCreateImageView(state->native.device, &view, nullptr, &result.view);
		if (status != VK_SUCCESS) { vmaDestroyImage(state->native.allocator, result.handle, allocation); Check(status, "create image view"); }
		try { state->images.emplace_back(result, allocation); }
		catch (...) { state->functions.vkDestroyImageView(state->native.device, result.view, nullptr); vmaDestroyImage(state->native.allocator, result.handle, allocation); throw; }
		return result;
	}
	void VulkanReviewFixture::UploadImage(const VulkanReviewImage& image, std::span<const std::byte> bytes, VkImageLayout finalLayout)
	{
		if (state->unsafe || !state->native.device) throw std::runtime_error("Quarantined/finished owner cannot upload an image.");
		const auto staging = CreateBuffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, bytes);
		const auto proof = Submit([&](VkCommandBuffer command, const Native& native) {
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image.handle;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			native.functions->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {image.width, image.height, 1};
			native.functions->vkCmdCopyBufferToImage(command, staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			barrier.oldLayout = barrier.newLayout; barrier.newLayout = finalLayout;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			native.functions->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		});
		if (!proof.completed) throw std::runtime_error("Image upload did not complete within its bounded fence.");
	}
	VulkanReviewBuffer VulkanReviewFixture::CopyImageToReadback(VkCommandBuffer command, const VulkanReviewImage& image, VkDeviceSize bytes, VkImageLayout oldLayout)
	{
		if (state->unsafe || !state->native.device) throw std::runtime_error("Quarantined/finished owner cannot record a readback.");
		const auto readback = CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.image = image.handle;
		barrier.oldLayout = oldLayout; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		state->functions.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {image.width, image.height, 1};
		state->functions.vkCmdCopyImageToBuffer(command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &copy);
		return readback;
	}
	void VulkanReviewFixture::RegisterCleanup(std::function<void(const Native&)> cleanup)
	{
		if (state->unsafe || !state->native.device) throw std::runtime_error("Quarantined/finished cleanup ownership is sealed until child teardown.");
		state->cleanup.push_back(std::move(cleanup));
	}
	VulkanReviewSubmission VulkanReviewFixture::Submit(const std::function<void(VkCommandBuffer, const Native&)>& record, std::chrono::milliseconds timeout)
	{
		if (state->unsafe || !state->native.device || state->recording || timeout.count() <= 0) throw std::runtime_error("Invalid, nested, or quarantined submission owner.");
		RequireNoValidationErrors(); // Do not execute an already-invalid pipeline.
		// Allocate the receipt before native work. From accepted submission through
		// ownership settlement, only this preallocated POD slot is updated.
		state->submissions.push_back({});
		auto& proof = state->submissions.back(); proof.commandSerial = state->nextCommand++;
		VkCommandBuffer command = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		try
		{
			VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocate.commandPool = state->pool;
			allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
			Check(state->functions.vkAllocateCommandBuffers(state->native.device, &allocate, &command), "allocate command");
			proof.commandHandle = reinterpret_cast<std::uintptr_t>(command);
			VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			Check(state->functions.vkBeginCommandBuffer(command, &begin), "begin command");
			state->recording = true; record(command, state->native); state->recording = false;
			VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			state->functions.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
			Check(state->functions.vkEndCommandBuffer(command), "end command");
			// Some descriptor-format/feature diagnostics appear only while recording
			// the dispatch/draw. Never submit that invalid command to the device.
			RequireNoValidationErrors();
			VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			Check(state->functions.vkCreateFence(state->native.device, &fenceInfo, nullptr, &fence), "create fence");
			proof.fenceHandle = reinterpret_cast<std::uintptr_t>(fence);
		}
		catch (...)
		{
			proof.result = VK_ERROR_VALIDATION_FAILED_EXT;
			state->recording = false;
			if (fence) state->functions.vkDestroyFence(state->native.device, fence, nullptr);
			if (command) state->functions.vkFreeCommandBuffers(state->native.device, state->pool, 1, &command);
			throw;
		}
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
		proof.result = state->functions.vkQueueSubmit(state->queue, 1, &submit, fence);
		if (proof.result == VK_SUCCESS)
		{
			proof.submitted = true; proof.submitSerial = state->nextSubmit++;
			proof.result = state->functions.vkWaitForFences(state->native.device, 1, &fence, VK_TRUE, static_cast<std::uint64_t>(timeout.count()) * 1000000u);
			if (proof.result == VK_SUCCESS) { proof.completed = true; proof.completionSerial = proof.submitSerial; }
		}
		if (!proof.completed) state->unsafe = true;
		if (!state->unsafe)
		{
			state->functions.vkDestroyFence(state->native.device, fence, nullptr);
			state->functions.vkFreeCommandBuffers(state->native.device, state->pool, 1, &command);
		}
		return proof;
	}
	void VulkanReviewFixture::EnsureEngineDispatch()
	{
		if (state->unsafe || !state->native.device) throw std::runtime_error("Quarantined/finished owner cannot replace Engine dispatch.");
		const char* quiescent = std::getenv("TERRAENGINE_VULKAN_REVIEW_QUIESCENT");
		if (!IsWatchedChild(state->name) || !quiescent || std::string_view(quiescent) != "1") throw std::runtime_error("Engine dispatch requires an isolated quiescent native child.");
#ifdef _WIN32
		const auto module = GetModuleHandleW(L"TerraEngine.dll");
		auto symbol = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(module, name)); };
		wchar_t path[32768]{}; GetModuleFileNameW(module, path, 32768); state->engineModule = std::filesystem::path(path).string();
#else
		auto module = dlopen("libTerraEngine.so", RTLD_NOLOAD | RTLD_NOW);
		auto symbol = [&](const char* name) { return dlsym(module, name); };
		state->engineModule = "libTerraEngine.so";
#endif
		if (!module) throw std::runtime_error("The matching Engine module is not loaded.");
		const auto initialize = reinterpret_cast<VkResult(*)()>(symbol("volkInitialize"));
		const auto loadInstance = reinterpret_cast<void(*)(VkInstance)>(symbol("volkLoadInstance"));
		const auto loadDevice = reinterpret_cast<void(*)(VkDevice)>(symbol("volkLoadDevice"));
		if (!initialize || !loadInstance || !loadDevice) throw std::runtime_error("Engine's actual exported dispatch initialization is unavailable.");
		Check(initialize(), "Engine volkInitialize"); loadInstance(state->native.instance); loadDevice(state->native.device);
#ifndef _WIN32
		Dl_info moduleInfo{};
		if (dladdr(reinterpret_cast<void*>(initialize), &moduleInfo) && moduleInfo.dli_fname) state->engineModule = std::filesystem::canonical(moduleInfo.dli_fname).string();
#endif
		state->engineDispatch = true;
	}
	std::vector<std::string> VulkanReviewFixture::ValidationMessages() const { std::lock_guard lock(state->messageMutex); return state->messages; }
	void VulkanReviewFixture::RequireNoValidationErrors() const
	{
		std::lock_guard lock(state->messageMutex);
		if (state->diagnosticsIncomplete.load(std::memory_order_relaxed)) throw std::runtime_error("Native validation diagnostics are incomplete.");
		if (!state->errors.empty()) throw std::runtime_error("Native Vulkan validation error: " + state->errors.front());
	}
	void VulkanReviewFixture::Finish()
	{
		if (state->unsafe) throw std::runtime_error("An ambiguous native cohort must remain quarantined until watched-child teardown.");
		state->ReleaseCompletedDevice();
		RequireNoValidationErrors();
	}
	void VulkanReviewFixture::RequireShaderCapabilities(std::span<const std::uint32_t> words) const
	{
		if (words.size() < 5 || words[0] != 0x07230203) throw std::runtime_error("Invalid shader capability document.");
		for (std::size_t index = 5; index < words.size();)
		{
			const auto count = words[index] >> 16u, opcode = words[index] & 65535u;
			if (!count || index + count > words.size()) throw std::runtime_error("Truncated SPIR-V instruction.");
			if (opcode == 17 && count == 2)
			{
				const auto capability = words[index + 1]; const char* feature = nullptr;
				if (capability == 11) feature = "shaderInt64";
				// Capability 49 is core Vulkan; actual storage-format support is
				// checked by CreateImage, not an irrelevant device-enable bit.
				if (capability == 5379) feature = "shaderDemoteToHelperInvocation";
				if (capability == 5302) feature = "runtimeDescriptorArrays";
				if (capability == 5307) feature = "sampledImageArrayNonUniformIndexing";
				if (capability == 5288) feature = "computeDerivativeGroupQuads";
				if (feature && !state->enabledFeatures.at(feature).get<bool>()) throw std::runtime_error("Emitted shader capability requires enabled feature: " + std::string(feature));
				if (capability == 12 && !state->enabledFeatures.at("shaderBufferInt64Atomics").get<bool>()
					&& !state->enabledFeatures.at("shaderSharedInt64Atomics").get<bool>()) throw std::runtime_error("Int64Atomics requires its actual buffer/shared atomic feature.");
			}
			index += count;
		}
	}
	void VulkanReviewFixture::WriteEvidence(const std::filesystem::path& output) const
	{
		nlohmann::json document{{"scope", "OffscreenReviewFixtureNativeCommand"}, {"case", state->name}, {"session", state->session},
			{"device", state->deviceName}, {"vendorId", state->properties.vendorID}, {"deviceId", state->properties.deviceID},
			{"driverVersionRaw", state->properties.driverVersion}, {"apiVersion", state->properties.apiVersion},
			{"enabledFeatures", state->enabledFeatures},
			{"requiredImageFormats", state->requiredImageFormats},
			{"queueFamily", state->native.queueFamily}, {"engineModule", state->engineModule}, {"engineDispatchInitialized", state->engineDispatch},
			{"quarantined", state->unsafe}, {"completedDeviceReleased", state->native.device == VK_NULL_HANDLE}, {"diagnosticsIncomplete", state->diagnosticsIncomplete.load(std::memory_order_relaxed)}, {"validationMessages", ValidationMessages()}, {"submissions", nlohmann::json::array()}};
		for (const auto& row : state->submissions) document["submissions"].push_back({{"commandSerial", row.commandSerial},
			{"submitSerial", row.submitSerial}, {"completionSerial", row.completionSerial}, {"result", row.result},
			{"commandHandle", row.commandHandle}, {"fenceHandle", row.fenceHandle},
			{"submitted", row.submitted}, {"completed", row.completed}});
		if (!state->engineModule.empty()) document["engineModuleSha256"] = FileHash(state->engineModule);
		const auto hostExecutable = TerraEngine::PlatformUtils::GetExecutablePath();
		document["hostExecutable"] = hostExecutable.string(); document["hostExecutableSha256"] = FileHash(hostExecutable);
#ifdef _WIN32
		document["processId"] = GetCurrentProcessId();
#else
		document["processId"] = getpid();
#endif
		std::filesystem::create_directories(output.parent_path()); std::ofstream file(output); file << document.dump(2);
		if (!file) throw std::runtime_error("Could not persist native fixture evidence.");
	}
	bool VulkanReviewFixture::IsWatchedChild(std::string_view name)
	{
		const char* selected = std::getenv("TERRAENGINE_VULKAN_REVIEW_CHILD");
		return name.starts_with("Native.ReviewSimulation.") && selected && name == selected;
	}
	bool VulkanReviewFixture::IsNativeReviewEnabled()
	{
		const char* enabled = std::getenv("TERRAENGINE_VULKAN_REVIEW_ENABLE");
		const char* child = std::getenv("TERRAENGINE_VULKAN_REVIEW_CHILD");
		return (enabled && std::string_view(enabled) == "1")
			|| (child && std::string_view(child).starts_with("Native.ReviewSimulation."));
	}
	VulkanReviewChild VulkanReviewFixture::RunWatchedSelf(std::string name, std::chrono::seconds timeout)
	{
		if (!name.starts_with("Native.ReviewSimulation.")) throw std::runtime_error("Native cases require a separate Native.ReviewSimulation prefix.");
		const auto executable = TerraEngine::PlatformUtils::GetExecutablePath();
		TerraEngine::Utilities::OwnedProcessRequest request;
		request.workingDirectory = executable.parent_path(); request.commandLine = Quote(executable); request.timeout = timeout;
		request.environment = {{"TERRAENGINE_VULKAN_REVIEW_CHILD", name}, {"TERRAENGINE_VULKAN_REVIEW_QUIESCENT", "1"},
			{"TERRAENGINE_TEST_FILTER", name}, {"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}};
		const std::atomic_bool canceled{false}; const auto result = TerraEngine::Utilities::OwnedProcess::Run(request, canceled);
		return {result.started, result.timedOut, result.exitCode, result.output};
	}
	std::vector<std::uint32_t> VulkanReviewFixture::CompileShader(const std::filesystem::path& source, std::string profile, std::string entry, const std::filesystem::path& output, std::span<const std::string> additionalArguments, bool builtinInstallEnvironment)
	{
		const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
#ifdef _WIN32
		const auto compiler = root / "External/dxc/windows/bin/x64/dxc.exe";
#else
		const auto compiler = root / "External/dxc/linux/bin/dxc";
#endif
		std::filesystem::create_directories(output.parent_path());
		TerraEngine::Utilities::OwnedProcessRequest request;
		request.workingDirectory = root; request.timeout = std::chrono::seconds(20);
		request.commandLine = Quote(compiler) + (builtinInstallEnvironment ? " -spirv -T " : " -spirv -fspv-target-env=vulkan1.3 -T ") + profile + " -E " + entry
			+ " -I " + Quote(root / "Engine/Shaders") + " -Fo " + Quote(output) + " " + Quote(source);
		for (const auto& argument : additionalArguments)
		{
			// Compiler options are explicit validation inputs; never interpolate
			// shell metacharacters or modify the authored shader to relocate them.
			if (argument.empty() || !std::all_of(argument.begin(), argument.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }))
				throw std::runtime_error("Unsafe additional shader compiler token.");
			request.commandLine += " " + argument;
		}
		const std::atomic_bool canceled{false}; const auto result = TerraEngine::Utilities::OwnedProcess::Run(request, canceled);
		if (!result.started || result.timedOut || result.exitCode) throw std::runtime_error("Bundled shader compilation failed: " + result.output);
		std::ifstream input(output, std::ios::binary | std::ios::ate); const auto bytes = input.tellg();
		if (bytes < 20 || bytes % 4 != 0) throw std::runtime_error("Compiled SPIR-V has an invalid extent.");
		std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / 4); input.seekg(0); input.read(reinterpret_cast<char*>(words.data()), bytes);
		if (!input || words[0] != 0x07230203) throw std::runtime_error("Compiled SPIR-V magic/read is invalid.");
		const std::string binary(reinterpret_cast<const char*>(words.data()), words.size() * 4);
		std::vector<std::uint32_t> capabilities;
		for (std::size_t index = 5; index < words.size();)
		{
			const auto count = words[index] >> 16u; if (!count || index + count > words.size()) throw std::runtime_error("Malformed emitted SPIR-V instruction.");
			if ((words[index] & 65535u) == 17 && count == 2) capabilities.push_back(words[index + 1]);
			index += count;
		}
		std::ofstream manifest(output.string() + ".json");
		manifest << nlohmann::json{{"source", source.string()}, {"compiler", compiler.string()}, {"command", request.commandLine},
			{"sourceSha256", FileHash(source)}, {"compilerSha256", FileHash(compiler)},
			{"additionalArguments", additionalArguments},
			{"builtinInstallEnvironment", builtinInstallEnvironment},
			{"spirvCapabilities", capabilities},
			{"binarySha256", TerraEngine::EditorTests::ComputeTestSha256(binary)}, {"bytes", binary.size()}}.dump(2);
		return words;
	}
}
