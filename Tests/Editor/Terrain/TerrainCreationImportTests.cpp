// TerrainCreationImportTests.cpp -- Terrain source validation and atomic creation checks.

#include "TestHarness.h"

#include "Editor/AssetCompilers/TerrainCompiler.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainCreationService.h"

#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests
{
	void RegisterTerrainCreationImportTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainCreation.FlatCreatesPrivateGeneration", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerrainFlat-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			const auto destination = root / "Flat.teterrain";
			TerraEngine::AssetPipeline::TerrainImportRequest request; request.assetId = TerraEngine::GenerateUUID();
			TerraEngine::Editor::Terrain::TerrainCreationService service;
			const auto result = service.CreateOrImport(request, destination);
			context.Expect(result.succeeded && result.heightfield.heightsMm.size() == 256U * 256U
				&& std::filesystem::exists(destination) && std::filesystem::exists(destination.string() + ".data/CURRENT")
				&& std::filesystem::exists(destination.string() + ".data/roots/1.json"),
				"Flat creation must publish one parent asset plus a complete private CURRENT/root/blob generation.");
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.TerrainCreation.Raw16ValidatesEndianByteCountAndCancellation", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerrainRaw-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(root); const auto raw = root / "source.raw";
			std::ofstream output(raw, std::ios::binary); output.put('\x34'); output.put('\x12'); output.put('\x78'); output.put('\x56'); output.close();
			TerraEngine::AssetPipeline::TerrainImportRequest request; request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Raw16LittleEndian; request.sourcePath = raw; request.rawWidth = 2U; request.rawHeight = 1U; request.assetId = TerraEngine::GenerateUUID();
			const auto decoded = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			context.Expect(decoded.succeeded && decoded.heightfield.heightsMm[0] < decoded.heightfield.heightsMm[1], "RAW16 must decode declared little-endian samples without resampling.");
			TerraEngine::Editor::Terrain::TerrainCreationService service;
			const auto created = service.CreateOrImport(request, root / "TwoByOne.teterrain");
			context.Expect(created.succeeded && std::filesystem::exists(root / "TwoByOne.teterrain.data/CURRENT"),
				"A non-256 RAW import must tile and publish a complete private terrain generation.");
			request.rawWidth = 3U; const auto rejected = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			context.Expect(!rejected.succeeded, "A RAW16 byte-count mismatch must reject before asset publication.");
			request.rawWidth = 2U; const auto cancelled = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request, [](float, std::string_view, std::string_view) { return false; });
			context.Expect(cancelled.cancelled, "Cancellation must stop import before a candidate can be registered.");
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.TerrainCreation.Png16PreservesPrecisionAndRejectsInvalidDepth", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerrainPng-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(root);
			const auto png16 = root / "height16.png"; const auto png8 = root / "height8.png"; const auto corrupt = root / "corrupt.png";
			const std::array<std::uint8_t, 70> precise = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x10,0x00,0x00,0x00,0x00,0x6A,0xEE,0x47,0x16,0x00,0x00,0x00,0x0B,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x10,0x32,0x01,0x00,0x00,0x5B,0x00,0x47,0x96,0xFB,0x1B,0x65,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};
			const std::array<std::uint8_t, 67> lowPrecision = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x00,0x00,0x00,0x00,0x3A,0x7E,0x9B,0x55,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x68,0x00,0x00,0x00,0x82,0x00,0x81,0x77,0xCD,0x72,0xB6,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};
			std::ofstream(png16, std::ios::binary).write(reinterpret_cast<const char*>(precise.data()), precise.size());
			std::ofstream(png8, std::ios::binary).write(reinterpret_cast<const char*>(lowPrecision.data()), lowPrecision.size());
			std::ofstream(corrupt, std::ios::binary) << "not a png";
			TerraEngine::AssetPipeline::TerrainImportRequest request; request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Png16; request.sourcePath = png16; request.assetId = TerraEngine::GenerateUUID(); request.heightScaleMetres = 10.0F; request.heightOffsetMetres = -1.0F;
			const auto decoded = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			context.Expect(decoded.succeeded && decoded.heightfield.heightsMm.size() == 1U && decoded.heightfield.heightsMm[0] == -289,
				"A true PNG16 sample must retain its 0x1234 precision while declared scale and origin are applied.");
			request.sourcePath = png8; const auto rejected8 = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			request.sourcePath = corrupt; const auto rejectedCorrupt = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			context.Expect(!rejected8.succeeded && !rejectedCorrupt.succeeded, "8-bit and corrupt PNG sources must be rejected before terrain publication.");
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.TerrainCreation.RegistrationFailureRollsBackParent", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerrainRollback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			const auto destination = root / "Rejected.teterrain";
			TerraEngine::AssetPipeline::TerrainImportRequest request; request.assetId = TerraEngine::GenerateUUID();
			TerraEngine::Editor::Terrain::TerrainCreationService service([](const std::filesystem::path&, std::string& error) { error = "Test registration rejection."; return false; });
			const auto result = service.CreateOrImport(request, destination);
			context.Expect(!result.succeeded && !std::filesystem::exists(destination) && !std::filesystem::exists(destination.string() + ".data"),
				"Failed registration must leave neither a public parent nor private data behind.");
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		});
	}
}
