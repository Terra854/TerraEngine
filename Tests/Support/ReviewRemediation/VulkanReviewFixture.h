#pragma once

// Validation-only offscreen Vulkan ownership. Each watched case owns a complete
// fixture; domain clients may record commands but never retain its handles after
// destruction or share mutable resources with another test.
#include <volk.h>
#include <vk_mem_alloc.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests::Review
{
	struct VulkanReviewBuffer
	{
		VkBuffer handle = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkDeviceSize bytes = 0;
	};
	struct VulkanReviewImage
	{
		VkImage handle = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		std::uint32_t width = 0, height = 0;
	};
	struct VulkanReviewSubmission
	{
		std::uint64_t commandSerial = 0, submitSerial = 0, completionSerial = 0;
		std::uint64_t commandHandle = 0, fenceHandle = 0;
		VkResult result = VK_SUCCESS;
		bool submitted = false, completed = false;
	};
	struct VulkanReviewChild
	{
		bool started = false, timedOut = false;
		int exitCode = -1;
		std::string output;
	};

	class VulkanReviewFixture
	{
	public:
		struct Options
		{
			std::string requiredDeviceName;
			std::uint32_t requiredVendor = 0, requiredDevice = 0;
			bool requireHardware = true, requireValidation = true;
			bool requireRuntimeDescriptorArrays = false, requireShaderInt64 = false;
			bool requireSampledImageArrayDynamicIndexing = false;
			bool requireSampledImageArrayNonUniformIndexing = false;
			bool requireComputeDerivativeGroupQuads = false;
			// Fragment discard may compile to SPIR-V demote; request its actual
			// Vulkan 1.3 feature explicitly rather than rewriting shader code.
			bool requireShaderDemoteToHelperInvocation = false;
			bool requireStorageImageExtendedFormats = false;
			bool requireShaderBufferInt64Atomics = false, requireShaderSharedInt64Atomics = false;
		};
		// An optional exact device name selects the accepting host's adapter.
		// Unset/empty leaves vendor/name unrestricted; hardware, validation and
		// each suite's required features still apply, with no software fallback.
		static Options NativeDeviceOptions();
		struct Native
		{
			VkInstance instance = VK_NULL_HANDLE;
			VkPhysicalDevice physical = VK_NULL_HANDLE;
			VkDevice device = VK_NULL_HANDLE;
			VmaAllocator allocator = VK_NULL_HANDLE;
			const VolkDeviceTable* functions = nullptr;
			std::uint32_t queueFamily = 0;
		};
		// Identity and feature requirements are validation inputs, not product
		// requirements. A missing/ambiguous match fails; no software fallback.
		explicit VulkanReviewFixture(std::string caseName);
		VulkanReviewFixture(std::string caseName, Options options);
		~VulkanReviewFixture();
		VulkanReviewFixture(const VulkanReviewFixture&) = delete;
		VulkanReviewFixture& operator=(const VulkanReviewFixture&) = delete;
		const Native& GetNative() const;
		const std::string& DeviceName() const;

		VulkanReviewBuffer CreateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
			std::span<const std::byte> initial = {});
		VulkanReviewImage CreateImage(std::uint32_t width, std::uint32_t height,
			VkFormat format, VkImageUsageFlags usage);
		void Upload(const VulkanReviewBuffer& buffer, std::span<const std::byte> data, VkDeviceSize offset = 0);
		std::vector<std::byte> Read(const VulkanReviewBuffer& buffer, VkDeviceSize bytes = VK_WHOLE_SIZE);
		// Caller supplies actual format bytes (e.g. packed half floats). Images
		// start UNDEFINED; this helper uploads once and leaves requested layout.
		void UploadImage(const VulkanReviewImage& image, std::span<const std::byte> bytes,
			VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		VulkanReviewBuffer CopyImageToReadback(VkCommandBuffer command,
			const VulkanReviewImage& image, VkDeviceSize byteCount,
			VkImageLayout oldLayout = VK_IMAGE_LAYOUT_GENERAL);
		// Domain-owned descriptors/pipelines/render passes must be released before
		// the fixture. Cleanup callbacks must not throw; the defensive catch records
		// incomplete diagnostics and continues releasing the completed cohort.
		void RegisterCleanup(std::function<void(const Native&)> cleanup);
		VulkanReviewSubmission Submit(const std::function<void(VkCommandBuffer, const Native&)>& record,
			std::chrono::milliseconds timeout = std::chrono::seconds(5));
		// Explicitly initialize dispatch INSIDE the loaded Engine module before
		// invoking exported production recorders. Requires quiescent renderer state.
		void EnsureEngineDispatch();
		std::vector<std::string> ValidationMessages() const;
		void RequireNoValidationErrors() const;
		// Close a completed cohort explicitly before recording final evidence, so
		// cleanup-only validation failures cannot arrive after a test reports pass.
		void Finish();
		// Reflect emitted OpCapability instructions before creating a pipeline.
		// Unsupported/unrequested optional capabilities are prerequisite errors.
		void RequireShaderCapabilities(std::span<const std::uint32_t> words) const;
		void WriteEvidence(const std::filesystem::path& output) const;
		static std::vector<std::uint32_t> CompileShader(const std::filesystem::path& source,
			std::string profile, std::string entry, const std::filesystem::path& output,
			std::span<const std::string> additionalArguments = {}, bool builtinInstallEnvironment = false);

		static bool IsWatchedChild(std::string_view testName);
		// Registration-only opt-in. This performs no Vulkan initialization and
		// keeps legacy unfiltered headless suites free of native GPU subprocesses.
		static bool IsNativeReviewEnabled();
		static VulkanReviewChild RunWatchedSelf(std::string testName,
			std::chrono::seconds timeout = std::chrono::seconds(30));
	private:
		struct State;
		std::unique_ptr<State> state;
	};
}
