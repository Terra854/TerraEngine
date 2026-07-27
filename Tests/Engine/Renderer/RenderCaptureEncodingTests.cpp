#include "TestHarness.h"
#include "Renderer/RenderCaptureEncoding.h"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace TerraEngine::Tests {
namespace {

	std::uint32_t ReadBigEndianU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
	{
		return (static_cast<std::uint32_t>(bytes[offset]) << 24u)
			| (static_cast<std::uint32_t>(bytes[offset + 1u]) << 16u)
			| (static_cast<std::uint32_t>(bytes[offset + 2u]) << 8u)
			| static_cast<std::uint32_t>(bytes[offset + 3u]);
	}

}

	void RegisterRenderCaptureEncodingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.CaptureEncoding.ResizeRgba8PreservesRequestedDimensions", [](TestContext& context) {
			const std::array<std::uint8_t, 8> source = {
				255u, 0u, 0u, 255u,
				0u, 0u, 255u, 128u
			};

			const std::vector<std::uint8_t> resized = TerraEngine::Renderer::ResizeRgba8(source, 2u, 1u, 4u, 2u);

			context.ExpectEqual(static_cast<std::size_t>(32u), resized.size(), "RGBA resize should publish targetWidth*targetHeight*4 bytes.");
			for (std::uint32_t y = 0u; y < 2u; ++y)
			{
				const std::size_t row = static_cast<std::size_t>(y) * 16u;
				context.ExpectEqual(static_cast<std::uint8_t>(255u), resized[row], "Nearest resize should preserve the first source pixel.");
				context.ExpectEqual(static_cast<std::uint8_t>(255u), resized[row + 7u], "Nearest resize should duplicate the first pixel alpha.");
				context.ExpectEqual(static_cast<std::uint8_t>(255u), resized[row + 10u], "Nearest resize should preserve the second source pixel blue channel.");
				context.ExpectEqual(static_cast<std::uint8_t>(128u), resized[row + 15u], "Nearest resize should preserve the second source pixel alpha.");
			}
		});

		registry.Add("Renderer.CaptureEncoding.Rgba8PngPublishesDimensions", [](TestContext& context) {
			const std::array<std::uint8_t, 8> pixels = {
				255u, 0u, 0u, 255u,
				0u, 64u, 255u, 128u
			};

			const std::vector<std::uint8_t> png = TerraEngine::Renderer::EncodeRgba8ToPng(pixels, 2u, 1u);

			context.Expect(png.size() > 33u, "PNG output should include signature, IHDR, IDAT, and IEND chunks.");
			context.ExpectEqual(static_cast<std::uint8_t>(137u), png[0], "PNG signature should start with the binary marker.");
			context.ExpectEqual(static_cast<std::uint8_t>('P'), png[1], "PNG signature should include P.");
			context.ExpectEqual(static_cast<std::uint8_t>('N'), png[2], "PNG signature should include N.");
			context.ExpectEqual(static_cast<std::uint8_t>('G'), png[3], "PNG signature should include G.");
			context.ExpectEqual(2u, ReadBigEndianU32(png, 16u), "IHDR should publish the requested width.");
			context.ExpectEqual(1u, ReadBigEndianU32(png, 20u), "IHDR should publish the requested height.");
			context.ExpectEqual(static_cast<std::uint8_t>(8u), png[24], "PNG captures should use 8-bit channels.");
			context.ExpectEqual(static_cast<std::uint8_t>(6u), png[25], "PNG captures should use RGBA color type.");
		});

		registry.Add("Renderer.CaptureEncoding.Base64DoesNotInsertWhitespace", [](TestContext& context) {
			const std::array<std::uint8_t, 5> bytes = { 0u, 1u, 2u, 253u, 254u };
			const std::string encoded = TerraEngine::Renderer::EncodeBase64(bytes);

			context.ExpectEqual(std::string("AAEC/f4="), encoded, "Base64 encoding should match the MCP byte-content alphabet.");
			context.Expect(encoded.find('\n') == std::string::npos, "MCP image data should not contain line breaks.");
			context.Expect(encoded.find('\r') == std::string::npos, "MCP image data should not contain carriage returns.");
			context.Expect(encoded.find(' ') == std::string::npos, "MCP image data should not contain spaces.");
		});

		registry.Add("Renderer.CaptureEncoding.RejectsMismatchedPixelData", [](TestContext& context) {
			bool rejected = false;
			try
			{
				const std::array<std::uint8_t, 4> tooFewPixels = { 255u, 255u, 255u, 255u };
				(void)TerraEngine::Renderer::EncodeRgba8ToPng(tooFewPixels, 2u, 1u);
			}
			catch (const std::invalid_argument&)
			{
				rejected = true;
			}

			context.Expect(rejected, "PNG capture encoding should reject buffers that do not match width*height*4.");
		});
	}

}
