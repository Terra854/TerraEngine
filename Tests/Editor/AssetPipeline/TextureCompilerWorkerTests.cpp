#include "TestHarness.h"

#include "TextureCompiler.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>

namespace {
	// Small uncompressed BMPs exercise the real decoder without a test encoder dependency.
	std::vector<std::uint8_t> MakeScratchTestImage(std::uint32_t width, std::uint8_t red) {
		const std::uint32_t height = 4U;
		std::vector<std::uint8_t> bytes(54U + width * height * 3U, 0U);
		const auto put32 = [&](std::size_t offset, std::uint32_t value) {
			for(std::size_t index = 0; index < 4U; ++index) bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
		};
		bytes[0] = 'B'; bytes[1] = 'M'; put32(2U, static_cast<std::uint32_t>(bytes.size()));
		put32(10U, 54U); put32(14U, 40U); put32(18U, width); put32(22U, height);
		bytes[26U] = 1U; bytes[28U] = 24U;
		for(std::size_t offset = 54U; offset < bytes.size(); offset += 3U) {
			bytes[offset] = static_cast<std::uint8_t>(255U - red); bytes[offset + 2U] = red;
		}
		return bytes;
	}

	std::vector<std::uint8_t> ReadScratchTestOutput(const std::filesystem::path& path) {
		std::ifstream input(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	// Recompute the file checksum independently, with the stored CRC field zeroed.
	bool IsScratchTestOutputValid(std::vector<std::uint8_t> bytes, std::uint16_t width) {
		if(bytes.size() < sizeof(TETFHeader)) return false;
		TETFHeader header{}; std::memcpy(&header, bytes.data(), sizeof(header));
		if(header.magic != kMagic || header.totalSize != bytes.size()
			|| header.tocOffset + sizeof(TETFChunkEntry) > bytes.size()) return false;
		TETFChunkEntry entry{}; std::memcpy(&entry, bytes.data() + header.tocOffset, sizeof(entry));
		if(entry.id != kChunkDESC || entry.offset + sizeof(TETFDesc) > bytes.size()) return false;
		TETFDesc desc{}; std::memcpy(&desc, bytes.data() + entry.offset, sizeof(desc));
		if(desc.width != width || desc.height != 4U) return false;
		std::fill_n(bytes.data() + offsetof(TETFHeader, fileCrc32), sizeof(header.fileCrc32), 0U);
		std::uint32_t crc = 0xFFFFFFFFU;
		for(const auto byte : bytes) {
			crc ^= byte;
			for(unsigned bit = 0U; bit < 8U; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
		}
		return (crc ^ 0xFFFFFFFFU) == header.fileCrc32;
	}
}

namespace TerraEngine::EditorTests {
	void RegisterTextureCompilerWorkerTests(TestRegistry& registry) {
		registry.Add("Renderer.Efficiency.Editor.PackageTextureScratchIsInvocationOwned", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerraEngineTextureScratch-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			struct Cleanup {
				std::filesystem::path root;
				~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
			} cleanup{root};
			const auto imageA = MakeScratchTestImage(4U, 255U);
			const auto imageB = MakeScratchTestImage(8U, 0U);
			const auto outputA = root / "a" / "CutoutAlpha.tetf";
			const auto outputB = root / "b" / "CutoutAlpha.tetf";
			// Initialize the library before overlap so this regression isolates scratch ownership.
			const bool baseline = CompileTextureFromBytes(imageA, ".bmp", root / "reference" / "CutoutAlpha.tetf", TextureFileSemantic::Color);
			std::promise<void> staged;
			std::promise<void> release;
			auto stagedFuture = staged.get_future();
			const auto releaseFuture = release.get_future().share();
			auto first = std::async(std::launch::async, [&] {
				return CompileTextureFromBytes(imageA, ".bmp", outputA, TextureFileSemantic::Color, 1U, true, TextureFilterType::Lanczos,
					[&](float, std::string_view stage, std::string_view) {
						if(stage == "Texture Decode") { staged.set_value(); releaseFuture.wait(); }
					});
			});
			// A has closed its staged input but has not decoded it. B must not overwrite
			// or delete A's source merely because their cooked output basenames match.
			const bool reachedDecode = stagedFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
			bool second = false;
			try {
				if(reachedDecode) second = CompileTextureFromBytes(imageB, ".bmp", outputB, TextureFileSemantic::Color);
			} catch(...) { release.set_value(); first.wait(); throw; }
			release.set_value();
			const bool firstResult = first.get();
			context.Expect(baseline && reachedDecode && second && firstResult, "Overlapping same-name package texture cooks must both complete.");
			const auto bytesA = ReadScratchTestOutput(outputA);
			const auto bytesB = ReadScratchTestOutput(outputB);
			context.Expect(IsScratchTestOutputValid(bytesA, 4U), "A must retain its own dimensions and valid native CRC.");
			context.Expect(IsScratchTestOutputValid(bytesB, 8U), "B must retain its own dimensions and valid native CRC.");
			context.Expect(bytesA == ReadScratchTestOutput(root / "reference" / "CutoutAlpha.tetf"), "A must preserve the independently cooked source content.");
			context.Expect(bytesA != bytesB, "Distinct source images must remain distinct cooked payloads.");
		});
		registry.Add("Editor.AssetPipeline.TextureCompiler.ResolvesBasisuWorkers", [](TestContext& context) {
			context.ExpectEqual(1u, ResolveTextureCompilerWorkerCountForTests(0u, nullptr), "Worker policy should stay valid when hardware concurrency is unavailable.");
			context.ExpectEqual(1u, ResolveTextureCompilerWorkerCountForTests(1u, nullptr), "Single-core systems should not request extra compression workers.");
			context.ExpectEqual(4u, ResolveTextureCompilerWorkerCountForTests(16u, nullptr), "Default texture import should use four BasisU workers.");
			context.ExpectEqual(3u, ResolveTextureCompilerWorkerCountForTests(16u, "3"), "Explicit worker overrides below hardware concurrency should be honored.");
			context.ExpectEqual(16u, ResolveTextureCompilerWorkerCountForTests(16u, "64"), "Explicit worker overrides should clamp only to available hardware threads.");
			context.ExpectEqual(2u, ResolveTextureCompilerWorkerCountForTests(2u, "64"), "Worker overrides should not exceed available hardware threads.");
			context.ExpectEqual(4u, ResolveTextureCompilerWorkerCountForTests(16u, "not-a-number"), "Invalid overrides should fall back to the default worker count.");
		});
	}
}
