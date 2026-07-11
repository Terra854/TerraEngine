// CheckedImGuiVulkanBridge.h - Failure boundary for Dear ImGui Vulkan calls.

#pragma once

#include "Engine/Renderer/RHI/Vulkan/VulkanPresentationLifecycle.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <vulkan/vulkan.h>

namespace TerraEngine::Editor
{
	struct ImGuiVulkanBridgeResult
	{
		bool success = true;
		std::int32_t nativeResult = 0;
		TerraEngine::Renderer::Vulkan::EditorUiFrameStage stage =
			TerraEngine::Renderer::Vulkan::EditorUiFrameStage::None;
	};

	// The vendored backend calls CheckVkResultFn after each fallible Vulkan call.
	// Throwing this private exception aborts the remaining backend work; Execute
	// catches it after normal C++ stack unwinding has released engine locks.
	class CheckedImGuiVulkanBridge
	{
	public:
		static void CheckVkResult(VkResult result);
		static ImGuiVulkanBridgeResult Execute(
			TerraEngine::Renderer::Vulkan::EditorUiFrameStage stage,
			const std::function<void()>& operation) noexcept;

	private:
		class BackendFailure final : public std::exception
		{
		public:
			explicit BackendFailure(std::int32_t result) : result_(result) {}
			[[nodiscard]] std::int32_t Result() const noexcept { return result_; }
			const char* what() const noexcept override { return "Dear ImGui Vulkan backend operation failed"; }

		private:
			std::int32_t result_ = 0;
		};
	};
}
