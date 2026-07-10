#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanReadbackConversion.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadSourceFile(const std::filesystem::path& relativePath)
		{
			// TERRAENGINE_SOURCE_DIR is provided by the test target, so source
			// contracts do not depend on CTest's current working directory.
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			return contents.str();
		}
	}

	void RegisterVulkanImageReadbackTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanImageReadback.ConvertsRgbaAndBgra", [](TestContext& context) {
			const std::vector<std::uint8_t> rgbaSource = { 1u, 2u, 3u, 4u, 10u, 20u, 30u, 40u };
			const std::vector<std::uint8_t> rgba = TerraEngine::Renderer::VulkanReadback::ConvertToRgba8(
				rgbaSource,
				2u,
				1u,
				VK_FORMAT_R8G8B8A8_UNORM);
			context.Expect(rgba == rgbaSource, "RGBA8 readback should preserve channel bytes exactly.");

			const std::vector<std::uint8_t> bgraSource = { 3u, 2u, 1u, 4u, 30u, 20u, 10u, 40u };
			const std::vector<std::uint8_t> swizzled = TerraEngine::Renderer::VulkanReadback::ConvertToRgba8(
				bgraSource,
				2u,
				1u,
				VK_FORMAT_B8G8R8A8_UNORM);
			context.Expect(swizzled == rgbaSource, "BGRA8 readback should swizzle red and blue into standard RGBA8 order.");
		});

		registry.Add("Renderer.VulkanImageReadback.ConvertsHdrHalfFloats", [](TestContext& context) {
			// One binary16 pixel: red=1, green=0.5, blue=0, alpha=1. Values are
			// little-endian because Vulkan staging bytes retain host byte order.
			const std::vector<std::uint8_t> hdrSource = {
				0x00u, 0x3cu,
				0x00u, 0x38u,
				0x00u, 0x00u,
				0x00u, 0x3cu
			};
			const std::vector<std::uint8_t> rgba = TerraEngine::Renderer::VulkanReadback::ConvertToRgba8(
				hdrSource,
				1u,
				1u,
				VK_FORMAT_R16G16B16A16_SFLOAT);
			context.ExpectEqual(static_cast<std::size_t>(4u), rgba.size(), "HDR conversion should return four bytes per pixel.");
			context.ExpectEqual(static_cast<std::uint8_t>(255u), rgba[0], "Linear red at one should map to full display red.");
			context.ExpectEqual(static_cast<std::uint8_t>(186u), rgba[1], "Linear green at one half should be gamma encoded.");
			context.ExpectEqual(static_cast<std::uint8_t>(0u), rgba[2], "Linear blue at zero should remain zero.");
			context.ExpectEqual(static_cast<std::uint8_t>(255u), rgba[3], "Linear alpha should not receive color gamma encoding.");
		});

		registry.Add("Renderer.VulkanImageReadback.RejectsInvalidInput", [](TestContext& context) {
			bool shortInputRejected = false;
			try
			{
				(void)TerraEngine::Renderer::VulkanReadback::ConvertToRgba8(
					std::vector<std::uint8_t>{ 1u, 2u, 3u },
					1u,
					1u,
					VK_FORMAT_R8G8B8A8_UNORM);
			}
			catch (const std::invalid_argument&)
			{
				shortInputRejected = true;
			}
			context.Expect(shortInputRejected, "Readback conversion should reject a byte count that cannot contain the declared image.");

			bool depthRejected = false;
			try
			{
				(void)TerraEngine::Renderer::VulkanReadback::ConvertToRgba8(
					std::vector<std::uint8_t>{ 0u, 0u, 0u, 0u },
					1u,
					1u,
					VK_FORMAT_D32_SFLOAT);
			}
			catch (const std::invalid_argument&)
			{
				depthRejected = true;
			}
			context.Expect(depthRejected, "Depth/stencil formats should fail cleanly instead of producing partial color data.");
		});

		registry.Add("Renderer.VulkanImageReadback.OwnsAsyncLifetimeContracts", [](TestContext& context) {
			const std::string contextSource = ReadSourceFile("Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");
			const std::string initSource = ReadSourceFile("Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp");
			const std::string renderSource = ReadSourceFile("Engine/Renderer/RenderSystem.cpp");

			context.Expect(contextSource.find("vkCmdCopyImageToBuffer") != std::string::npos, "Vulkan readback should record a real image-to-staging-buffer copy.");
			context.Expect(contextSource.find("const VkResult endResult = vkEndCommandBuffer") != std::string::npos, "Readback should check command-buffer finalization before submission.");
			context.Expect(contextSource.find("submitResult = vkQueueSubmit") != std::string::npos, "Readback should retain and check the queue submission result.");
			context.Expect(contextSource.find("vkGetFenceStatus") != std::string::npos, "Readback completion should use non-blocking fence polling.");
			context.Expect(contextSource.find("A timeout is a client-visible cancellation, not proof that Vulkan") != std::string::npos, "Timeout handling should retain GPU-owned resources until completion is proven.");
			context.Expect(contextSource.find("request.sourceImage != target.image") != std::string::npos, "Resize validation should compare source image identity as well as dimensions.");
			context.Expect(initSource.find("CancelReadbackRequests(") != std::string::npos, "Renderer teardown should explicitly drain in-flight capture resources.");
			context.Expect(renderSource.find("rhi->PollCaptureReadbacks()") != std::string::npos, "RenderSystem should drain backend completion results through the bounded capture queue.");
		});
	}
}
