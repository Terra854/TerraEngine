// TerrainExportRuntimeTests.cpp -- Archive-only terrain runtime discovery.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainContentResolver.h"
#include "Runtime/ContentMount/TerrainRuntimeProxyLoader.h"
#include "TEPAK/PackWriter.h"
#include "Terrain/TerrainAreaCodec.h"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		class TerrainExportWorkspace final
		{
		public:
			TerrainExportWorkspace()
			{
				const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
				root = std::filesystem::temp_directory_path() / "TerraEngineEngineTests"
					/ ("TerrainExportRuntime-" + std::to_string(suffix));
				content = root / "Content";
				std::filesystem::create_directories(content);
			}

			~TerrainExportWorkspace()
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			void WriteText(std::string_view logicalPath, std::string_view value) const
			{
				const std::filesystem::path path = content / std::filesystem::path(logicalPath);
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::binary);
				output.write(value.data(), static_cast<std::streamsize>(value.size()));
			}

			void WriteBytes(std::string_view logicalPath, const std::vector<std::uint8_t>& value) const
			{
				const std::filesystem::path path = content / std::filesystem::path(logicalPath);
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::binary);
				output.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value.size()));
			}

			std::filesystem::path CreatePack() const
			{
				tepak::PackagingRequest request;
				request.inputRoot = content;
				request.outputPackPath = root / "Terrain.tepak";
				request.workerCount = 1U;
				request.enableCompression = false;
				request.enableDictionaryTraining = false;
				request.overwriteExisting = true;
				(void)tepak::PackWriter::Create(request);
				return request.outputPackPath;
			}

		private:
			std::filesystem::path root;
			std::filesystem::path content;
		};
	}

	void RegisterTerrainExportRuntimeTests(TestRegistry& registry)
	{
		registry.Add("Terrain.ExportRuntime.RejectsLooseFallbackAndFindsDedicatedShaders", [](TestContext& context) {
			ContentMount unmounted; const auto fallback = TerrainContentResolver::ReadRoot(unmounted, "Terrain/Runtime.teterrain");
			const std::filesystem::path root = TERRAENGINE_SOURCE_DIR;
			// Runtime terrain graphics must ship as a complete task/mesh/fragment
			// set; finding only a legacy vertex stage is not sufficient evidence.
			const bool shaders =
				std::filesystem::exists(root / "Engine/Shaders/Terrain/TerrainVisibility.task.hlsl")
				&& std::filesystem::exists(root / "Engine/Shaders/Terrain/TerrainVisibility.mesh.hlsl")
				&& std::filesystem::exists(root / "Engine/Shaders/Terrain/TerrainVisibility.frag.hlsl");
			context.Expect(fallback.status == TerrainContentStatus::NotMounted && shaders && TerrainContentResolver::ReadRoot(unmounted, "Editor/Runtime.teterrain").status == TerrainContentStatus::ForbiddenRuntimePath,
				"Export terrain loading must fail closed without a TEPAK mount, discover dedicated runtime shaders, and reject editor-only logical paths.");
		});

		registry.Add("Terrain.ExportRuntime.BuildsDirectMeshFromSavedMountedGeneration", [](TestContext& context) {
			TerrainExportWorkspace workspace;
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 1250);
			std::vector<std::uint8_t> heightBytes(heights.size() * sizeof(std::int32_t));
			std::memcpy(heightBytes.data(), heights.data(), heightBytes.size());
			const TerraEngine::Terrain::ContentHash heightHash = TerraEngine::Terrain::HashTerrainBytes(heightBytes);
			const TerraEngine::Terrain::ContentHash rootHash{0x1234U, 0x5678U};
			const TerraEngine::Terrain::ContentHash shardHash{0x9ABCU, 0xDEF0U};

			const std::string assetText = TerraEngine::ToHexString(assetId);
			const std::string rootText = TerraEngine::Terrain::ToHexString(rootHash);
			const std::string heightText = TerraEngine::Terrain::ToHexString(heightHash);
			std::ostringstream manifest;
			manifest << "TERRAINROOT2\n"
				<< assetText << " 7 1 " << rootText << " 0 0 256 256 1250 1 0\n"
				<< "LAYERS 0\n1\n"
				<< "0 0 256 256 " << TerraEngine::Terrain::ToHexString(shardHash) << " 1\n"
				<< "0 0 7 1250 1250 " << heightText << ' ' << heightText << " - 0\n";
			workspace.WriteText("Terrain/Runtime.teterrain", nlohmann::json{
				{"format", "TerraEngineTerrain"}, {"version", 1U}, {"assetId", assetText},
				{"width", 256U}, {"height", 256U}, {"sampleSpacingMetres", 2.0F}}.dump());
			workspace.WriteText("Terrain/Runtime.teterrain.data/CURRENT", rootText + "\n-\n");
			workspace.WriteText("Terrain/Runtime.teterrain.data/roots/" + rootText + ".root", manifest.str());
			workspace.WriteBytes("Terrain/Runtime.teterrain.data/blobs/" + heightText + ".tearea", heightBytes);

			ContentMount mount;
			context.Expect(mount.Mount(workspace.CreatePack()),
				"The saved TERRAINROOT2 runtime fixture must mount through TEPAK.");
			auto loaded = TerraEngine::Runtime::BuildMountedTerrainRuntime(
				mount, assetId, "Terrain/Runtime.teterrain");
			context.Expect(loaded.Succeeded() && loaded.renderProxy.mesh->generatedDirectMeshletRoute.IsComplete()
				&& !loaded.renderProxy.mesh->meshletRecords.empty() && loaded.collisionRecipe.IsComplete()
				&& loaded.collisionRecipe.sampleSpacingMetres == 2.0F
				&& loaded.collisionRecipe.heightMillimetres.front() == 1250,
				"A saved raw-channel terrain generation must become complete direct-resident render and collision products without editor services. "
				+ loaded.renderProxy.diagnostic);
		});

		registry.Add("Terrain.ExportRuntime.MultiViewPacketsRetainOneCurrentSourceIdentity", [](TestContext& context) {
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 0); Renderer::Terrain::TerrainCookInput input{}; input.assetId = TerraEngine::GenerateUUID(); input.authoredHash = {1u, 2u}; input.authoredGeneration = 3u; input.heightMillimetres = heights;
			const auto cook = Renderer::Terrain::CookTerrainPage(input); Renderer::Terrain::TerrainViewPacket a{}; a.assetId = input.assetId; a.authoredHash = input.authoredHash; a.authoredGeneration = 3u; a.pageSourceIdentity = cook.sourceIdentity; a.frameId = 8u; auto b = a; b.frameId = 9u;
			const std::array views{a, b};
			const std::array cooks{cook};
			const auto packets = RenderSystem::PrepareCommittedTerrainPackets(views, cooks, {});
			context.Expect(packets.size() == 2u && packets[0].pick.pageSourceIdentity == packets[1].pick.pageSourceIdentity && !packets[0].usesLastKnownGood && !packets[1].usesLastKnownGood,
				"Two runtime views must consume packets carrying the same current terrain source and picking identity rather than editor-owned state.");
		});
	}
}
