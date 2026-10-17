// Private fixture-only host lifetime; never shares Engine Vulkan children.
#pragma once
// Establish platform calling conventions before GLFW and Vulkan types before its API declarations.
#if defined(_WIN32)
#include <Windows.h>
#endif
#include <volk.h>
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace RuntimeMemorySafetySupport {
	using Json = nlohmann::json;
	// Optional compatibility host: owns only a separate instance, never an Engine
	// device/surface/swapchain. GLFW owns the loader until window shutdown. Keep
	// private dispatch because each Engine context reloads Volk's global pointers.
	class HostInstanceOwner final {
	public:
		using BoundaryCallback = void(*)(void*, std::string_view, bool);
		HostInstanceOwner(bool requested, BoundaryCallback boundary, void* state) : requested_(requested), boundary_(boundary), state_(state) {}
		HostInstanceOwner(const HostInstanceOwner&) = delete;
		HostInstanceOwner& operator=(const HostInstanceOwner&) = delete;
		~HostInstanceOwner() { Close(); }
		void Initialize() {
			Require(requested_ && created_ == 0U && !closeAttempted_ && instance_ == VK_NULL_HANDLE,
				"Host instance initialization must be explicitly requested once.");
			getProc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(glfwGetInstanceProcAddress(VK_NULL_HANDLE, "vkGetInstanceProcAddr"));
			auto create = reinterpret_cast<PFN_vkCreateInstance>(glfwGetInstanceProcAddress(VK_NULL_HANDLE, "vkCreateInstance"));
			Require(getProc_ && create, "GLFW could not provide private host-instance loader dispatch.");
			VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
			application.pApplicationName = "RuntimeMemorySafetyHost"; application.apiVersion = VK_API_VERSION_1_0;
			VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &application;
			// Zero layers/extensions deliberately provides no host validation callback.
			// Every independent Engine context still enables its existing validation.
			createResult_ = create(&info, nullptr, &instance_);
			Require(*createResult_ == VK_SUCCESS && instance_ != VK_NULL_HANDLE, "Host instance creation failed.");
			++created_; identity_ = instance_;
			destroy_ = reinterpret_cast<PFN_vkDestroyInstance>(getProc_(instance_, "vkDestroyInstance"));
			auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getProc_(instance_, "vkEnumeratePhysicalDevices"));
			Require(destroy_ && enumerate, "Host instance private core dispatch is unavailable.");
			enumerateCountResult_ = enumerate(instance_, &physicalDeviceCount_, nullptr);
			Require(*enumerateCountResult_ == VK_SUCCESS && physicalDeviceCount_ > 0U, "Host instance has no enumerated physical device.");
			std::vector<VkPhysicalDevice> devices(physicalDeviceCount_);
			enumerateResult_ = enumerate(instance_, &physicalDeviceCount_, devices.data());
			Require(*enumerateResult_ == VK_SUCCESS && physicalDeviceCount_ > 0U && physicalDeviceCount_ <= devices.size(),
				"Host physical-device enumeration did not complete.");
			++enumerated_;
		}
		bool Ready() const noexcept { return instance_ != VK_NULL_HANDLE && enumerated_ == 1U; }
		bool Distinct(VkInstance other) const noexcept { return Ready() && other != VK_NULL_HANDLE && other != instance_; }
		std::string Identity() const { return std::to_string(reinterpret_cast<std::uintptr_t>(identity_)); }
		bool Close() noexcept {
			if(!requested_) return true;
			if(closeAttempted_) return cleanupCompleted_;
			closeAttempted_ = true; bool loggedBegin = false, loggedEnd = false;
			try { boundary_(state_, "HostInstanceShutdownBegin", true); loggedBegin = true; } catch(...) {}
			// Even a logging allocation failure must not skip native destruction.
			if(instance_ != VK_NULL_HANDLE && destroy_) {
				destroy_(instance_, nullptr); instance_ = VK_NULL_HANDLE; ++destroyed_;
			}
			const bool destroyed = created_ == 1U && destroyed_ == 1U && instance_ == VK_NULL_HANDLE;
			try { boundary_(state_, "HostInstanceShutdownEnd", destroyed); loggedEnd = true; } catch(...) {}
			cleanupCompleted_ = destroyed && loggedBegin && loggedEnd; return cleanupCompleted_;
		}
		Json Snapshot() const {
			const auto result = [](const std::optional<VkResult>& value) { return value ? Json(*value) : Json(nullptr); };
			return {{"requested", requested_}, {"createdCount", created_}, {"enumeratedCount", enumerated_}, {"destroyedCount", destroyed_},
				{"createResult", result(createResult_)}, {"enumerateCountResult", result(enumerateCountResult_)}, {"enumerateResult", result(enumerateResult_)},
				{"physicalDeviceCount", physicalDeviceCount_}, {"instanceHandle", Identity()},
				{"cleanupResult", destroyed_ == 1U ? Json("ReturnedNormally") : Json(nullptr)}, {"handleCleared", instance_ == VK_NULL_HANDLE},
				{"hostValidationCallbacks", false}, {"enabledExtensionCount", 0U}, {"enabledLayerCount", 0U},
				{"engineOwnersShared", false}, {"driverModulePinned", false}, {"globalProcessInstanceCountClaim", false},
				{"perIterationLastOwnedInstanceTeardownExercised", false}, {"coverageModel", "RetainedFixtureHostInstance"}};
		}
	private:
		static void Require(bool condition, const char* reason) { if(!condition) throw std::runtime_error(reason); }
		bool requested_ = false, closeAttempted_ = false, cleanupCompleted_ = false;
		BoundaryCallback boundary_;
		void* state_;
		VkInstance instance_ = VK_NULL_HANDLE, identity_ = VK_NULL_HANDLE;
		PFN_vkGetInstanceProcAddr getProc_ = nullptr;
		PFN_vkDestroyInstance destroy_ = nullptr;
		std::optional<VkResult> createResult_, enumerateCountResult_, enumerateResult_;
		std::uint32_t created_ = 0U, enumerated_ = 0U, destroyed_ = 0U, physicalDeviceCount_ = 0U;
	};
}
