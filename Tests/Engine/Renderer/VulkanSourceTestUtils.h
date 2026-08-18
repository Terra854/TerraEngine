#pragma once

// Source-contract tests validate Vulkan behavior without requiring a physical
// device. The implementation is split by responsibility, so these helpers read
// the complete context or resource family instead of coupling tests to one file.

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>

namespace TerraEngine::Tests::VulkanSourceContracts
{
	inline std::string ReadSources(
		std::initializer_list<std::string_view> relativePaths)
	{
		std::ostringstream combined;
		for (const std::string_view relativePath : relativePaths)
		{
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream input(sourcePath, std::ios::binary);
			combined << "\n// Vulkan source-contract boundary: "
				<< relativePath << '\n';
			combined << input.rdbuf();
		}
		return combined.str();
	}

	inline std::string ReadContextSources()
	{
		return ReadSources({
			"Engine/Renderer/RHI/Vulkan/VulkanContext.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanContextSource.h",
			"Engine/Renderer/RHI/Vulkan/VulkanBuiltinPasses.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanEditorRendering.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanFrameRendering.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanFrameSubmission.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanPostProcessing.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanReadback.cpp"
		});
	}

	inline std::string ReadResourceSources()
	{
		return ReadSources({
			"Engine/Renderer/RHI/Vulkan/VulkanContextResources.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanResourceSource.h",
			"Engine/Renderer/RHI/Vulkan/VulkanAnimationResources.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanVirtualGeometryResources.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanPipelineResources.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanGlobalResources.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanLightingResources.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanGpuDiagnostics.cpp"
		});
	}
}
