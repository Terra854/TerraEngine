#include "TestHarness.h"
#include "Renderer/RenderCaptureEncoding.h"
#include "../../Support/DecodedCaptureAssertions.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

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
		registry.Add("Renderer.CaptureEncoding.IndependentDecoderPreservesActualPixels", [](TestContext& context) {
			// This oracle interprets encoded bytes through bundled pv_png, rather
			// than reading only producer-authored IHDR fields or a PNG signature.
			const std::array<std::uint8_t, 24> pixels = {0,1,2,0, 3,4,5,255, 6,7,8,17, 9,10,11,255, 12,13,14,1, 15,16,17,128};
			const auto png = Renderer::EncodeRgba8ToPng(pixels, 3, 2);
			const auto decoded = TestSupport::DecodeCaptureBase64(Renderer::EncodeBase64(png));
			context.ExpectEqual(3u, decoded.width, "Independent decoding must retain non-square width.");
			context.ExpectEqual(2u, decoded.height, "Independent decoding must retain non-square height.");
			context.Expect(std::equal(pixels.begin(), pixels.end(), decoded.rgba.begin(), decoded.rgba.end()), "Independent decoding must retain every RGBA byte and alpha.");
		});

		registry.Add("Renderer.CaptureEncoding.IndependentDecoderRejectsOriginalMalformedFallback", [](TestContext& context) {
			const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tests/Fixtures/McpCameraImages/invalid-fallback.png";
			std::ifstream input(path, std::ios::binary);
			context.Expect(input.good(), "The preserved original malformed image fixture must be available.");
			const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
			context.ExpectEqual(std::size_t(68), bytes.size(), "The fixture must preserve the original literal bytes.");
			bool rejected = false;
			try { (void)TestSupport::DecodePng(bytes); } catch (const std::runtime_error&) { rejected = true; }
			context.Expect(rejected, "Independent validation must reject the original fallback CRC failure.");
		});

		registry.Add("Renderer.CaptureEncoding.IndependentDecoderChecksEveryChunkCrc", [](TestContext& context) {
			const std::array<std::uint8_t, 4> pixel = {1, 2, 3, 255};
			const auto good = Renderer::EncodeRgba8ToPng(pixel, 1, 1);
			// Corrupt the CRC word of each otherwise valid chunk, including IEND:
			// row decoding alone may finish without consuming that final checksum.
			for (std::size_t offset = 8; offset < good.size();)
			{
				const auto length = ReadBigEndianU32(good, offset);
				auto corrupted = good;
				corrupted[offset + 8 + length] ^= 1;
				bool rejected = false;
				try { (void)TestSupport::DecodePng(corrupted); } catch (const std::runtime_error&) { rejected = true; }
				context.Expect(rejected, "IHDR, IDAT and IEND CRC corruptions must each be rejected.");
				offset += std::size_t(length) + 12;
			}
		});

		registry.Add("Renderer.CaptureEncoding.IndependentDecoderRejectsNoncanonicalBase64", [](TestContext& context) {
			for (const auto* invalid : {"", "A", "====", "A===", "AB==", "AAB=", "AA=A", "AA==AAAA", "AA-_", "AA\n="})
			{
				std::string failure;
				try { (void)TestSupport::DecodeCaptureBase64(invalid); } catch (const std::runtime_error& error) { failure = error.what(); }
				context.Expect(failure.find("Base64") != std::string::npos, "Invalid alphabet, padding and unused pad bits must fail at Base64 validation, before PNG decoding.");
			}
		});

		registry.Add("Renderer.CaptureEncoding.ZeroAndExtremeDimensionsRejectBeforeReadingPixels", [](TestContext& context) {
			// Empty real spans deliberately avoid fabricated pointers or huge
			// allocations while exercising dimension preflight on public APIs.
			for (const auto dimensions : {std::array<std::uint32_t,2>{0,1}, {1,0}, {UINT32_MAX,UINT32_MAX}})
			{
				bool rejected = false;
				try { (void)Renderer::EncodeRgba8ToPng({}, dimensions[0], dimensions[1]); } catch (const std::exception&) { rejected = true; }
				context.Expect(rejected, "Zero or unrepresentable PNG dimensions must reject an empty buffer before accessing it.");
			}
		});
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

		registry.Add("Renderer.CaptureEncoding.Base64CountPreflightRejectsWithoutFabricatedSpan", [](TestContext& context) {
			for (std::size_t size = 0; size < 16; ++size) {
				const std::vector<std::uint8_t> bytes(size, 0);
				context.ExpectEqual(Renderer::ComputeBase64EncodedSize(size), Renderer::EncodeBase64(bytes).size(), "Count preflight and real producer must agree for padding boundaries.");
			}
			bool rejected = false;
			try { (void)Renderer::ComputeBase64EncodedSize(std::numeric_limits<std::size_t>::max()); }
			catch (const std::length_error& error) { rejected = std::string_view(error.what()).find("Base64") != std::string_view::npos; }
			context.Expect(rejected, "A legal byte COUNT must trip the actual producer's Base64 overflow guard without a pointer, span or allocation.");
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
