#pragma once

// These supplementary source checks inspect Vulkan wiring without a device.
// Call-site presence cannot prove GPU behavior, fence timing, or lifetime safety.
// Read every listed module so relocation cannot hide a missing wiring input.

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
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
			// Every module is required, including files used only by exclusions.
			if(!input) throw std::runtime_error("Vulkan source contract cannot read " + sourcePath.string());
			std::ostringstream contents;
			contents << input.rdbuf();
			if(input.bad() || contents.str().empty())
				throw std::runtime_error("Vulkan source contract found empty/unreadable file " + sourcePath.string());
			combined << "\n// Vulkan source-contract boundary: "
				<< relativePath << '\n';
			combined << contents.str();
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
			"Engine/Renderer/RHI/Vulkan/VulkanReadback.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanReadbackLifecycle.cpp",
			"Engine/Renderer/RHI/Vulkan/VulkanReadbackLifecycle.h"
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
