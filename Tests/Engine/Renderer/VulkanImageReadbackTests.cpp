#include "TestHarness.h"
#include "VulkanSourceTestUtils.h"
#include "Renderer/RHI/Vulkan/VulkanReadbackConversion.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		// Source checks supplement runtime tests. Missing definitions must fail even
		// when the caller only searches for forbidden text; ordering stays in one file.
		std::string RequiredContractSource(const std::filesystem::path& relative)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relative, std::ios::binary);
			std::string text((std::istreambuf_iterator<char>(input)), {});
			if(!input || input.bad() || text.empty())
				throw std::runtime_error("Required source unavailable: " + relative.generic_string());
			return text;
		}
		std::string RequiredContractRegion(const std::string& text, const char* first, const char* next)
		{
			const auto begin = text.find(first);
			const auto end = begin == std::string::npos ? std::string::npos : text.find(next, begin + std::string(first).size());
			if(begin == std::string::npos || end == std::string::npos || end <= begin)
				throw std::runtime_error(std::string("Required definition boundaries unavailable: ") + first);
			return text.substr(begin, end - begin);
		}

		std::string ReadSourceFile(const std::filesystem::path& relativePath)
		{
			return RequiredContractSource(relativePath);
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

		// These source checks supplement the executable Renderer.ReadbackLifetime
		// controller cases and installed ReadbackLifetime native fixture. Finding
		// a call site cannot prove fence timing, retained owners or safe teardown.
		registry.Add("Renderer.VulkanImageReadback.SupplementaryNativeWiring", [](TestContext& context) {
			const std::string contextSource = VulkanSourceContracts::ReadContextSources();
			const std::string initSource = ReadSourceFile("Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp");
			const auto renderSource = RequiredContractRegion(ReadSourceFile("Engine/Renderer/RenderSystemCapture.cpp"),
				"void RenderSystem::DrainCaptureReadbacks()", "void RenderSystem::DrainConventionalVisibilityIdentityReadbacks()");

			context.Expect(contextSource.find("vkCmdCopyImageToBuffer") != std::string::npos, "Supplementary wiring: image-to-buffer copy call site is present.");
			context.Expect(contextSource.find("const VkResult endResult = vkEndCommandBuffer") != std::string::npos, "Supplementary wiring: command finalization retains its result.");
			context.Expect(contextSource.find("submitResult = impl->SubmitGraphicsQueue") != std::string::npos, "Supplementary wiring: capture uses synchronized submission.");
			context.Expect(contextSource.find("vkGetFenceStatus") != std::string::npos, "Supplementary wiring: a native fence query call site is present.");
			context.Expect(contextSource.find("request.sourceImage != target.image") != std::string::npos, "Supplementary wiring: source-image identity comparison is present.");
			context.Expect(contextSource.find("iterator->lifecycle.Poll(adapter,") != std::string::npos, "Supplementary wiring: polling reaches the production lifecycle controller.");
			context.Expect(initSource.find("CancelReadbackRequests(") != std::string::npos, "Supplementary wiring: cleanup reaches readback cancellation.");
			context.Expect(renderSource.find("rhi->PollCaptureReadbacks()") != std::string::npos, "Supplementary wiring: RenderSystem consumes backend results.");
		});
	}
}
