// TerrainExportRuntimeTests.cpp -- Archive-only terrain runtime discovery.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainPageFormat.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainContentResolver.h"
#include "Runtime/ContentMount/TerrainRuntimeProxyLoader.h"
#include "TEPAK/PackWriter.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainMaterial.h"
#include "Resource/ResourceManager.h"

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

		registry.Add("Terrain.ExportRuntime.MountedFallbackMaterialPreservesAuthoredPbrInputs", [](TestContext& context) {
			// TerrainDerivedSystem creates mounted direct proxies without a terrain
			// layer source. That exact fallback must retain component-authored PBR
			// values in its ordinary TMAT header for the export renderer to upload.
			TerraEngine::Terrain::TerrainMaterialSettings settings{};
			settings.baseColor = {0.18F, 0.55F, 0.16F};
			settings.metallic = 0.0F;
			settings.roughness = 0.92F;
			const auto material = TerraEngine::Terrain::CreateTerrainMaterial(
				ResourceManager::Instance(), settings);
			context.Expect(material != nullptr
				&& material->header.baseColorFactor[0] == settings.baseColor.r
				&& material->header.baseColorFactor[1] == settings.baseColor.g
				&& material->header.baseColorFactor[2] == settings.baseColor.b
				&& material->header.metallicFactor == settings.metallic
				&& material->header.roughnessFactor == settings.roughness,
				"Mounted direct terrain materials must preserve authored PBR values when no layer source is available.");
		});

		registry.Add("Terrain.ExportRuntime.BuildsDirectMeshFromSavedMountedGeneration", [](TestContext& context) {
			TerrainExportWorkspace workspace;
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 1250);
			std::vector<std::uint8_t> heightBytes(heights.size() * sizeof(std::int32_t));
			std::memcpy(heightBytes.data(), heights.data(), heightBytes.size());
			const TerraEngine::Terrain::ContentHash heightHash = TerraEngine::Terrain::HashTerrainBytes(heightBytes);
			const TerraEngine::Terrain::ContentHash shardHash{0x9ABCU, 0xDEF0U};

			const std::string assetText = TerraEngine::ToHexString(assetId);
			const std::string heightText = TerraEngine::Terrain::ToHexString(heightHash);
			TerraEngine::Terrain::TerrainAreaDescriptor area{};
			area.coordinate = {0, 0};
			area.editGeneration = 7u;
			area.channels.heightHash = heightHash;
			area.minHeightMm = 1250;
			area.maxHeightMm = 1250;
			area.authoredContentHash = heightHash;
			TerraEngine::Terrain::TerrainDirectoryShard shard{};
			shard.areaBounds = {0, 0, 256, 256};
			shard.identity = shardHash;
			shard.descriptors.emplace(area.coordinate, area);
			TerraEngine::Terrain::TerrainAssetRoot root{};
			root.assetUuid = assetId;
			root.publicationGeneration = 7u;
			root.persistedRootGeneration = 7u;
			root.sampleSpacingMetres = 2.0;
			root.authoredBounds = {0, 0, 256, 256};
			root.defaultHeightMm = 1250;
			root.defaultLayerId = 1u;
			root.layerStackGeneration = 1u;
			root.directoryShards.push_back(std::move(shard));
			const auto encodedRoot = TerraEngine::Terrain::EncodeTerrainRootManifest(root);
			const std::string rootText = TerraEngine::Terrain::ToHexString(encodedRoot.contentHash);
			workspace.WriteText("Terrain/Runtime.teterrain", nlohmann::json{
				{"format", "TerraEngineTerrain"}, {"version", 1U}, {"assetId", assetText},
				{"rootFormat", "TERRAROOT1"}, {"width", 256U}, {"height", 256U},
				{"sampleSpacingMetres", 2.0F}}.dump());
			workspace.WriteText("Terrain/Runtime.teterrain.data/CURRENT", rootText + "\n-\n");
			workspace.WriteText("Terrain/Runtime.teterrain.data/roots/" + rootText + ".root", encodedRoot.canonicalBytes);
			workspace.WriteBytes("Terrain/Runtime.teterrain.data/blobs/" + heightText + ".tearea", heightBytes);

			ContentMount mount;
			context.Expect(encodedRoot.Succeeded() && mount.Mount(workspace.CreatePack()),
				"The saved canonical TERRAROOT1 runtime fixture must mount through TEPAK.");
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
			std::vector<std::int32_t> heights(
				Renderer::Terrain::Format::kCookHaloSampleExtent
					* Renderer::Terrain::Format::kCookHaloSampleExtent, 0);
			Renderer::Terrain::TerrainCookInput input{}; input.assetId = TerraEngine::GenerateUUID(); input.authoredHash = {1u, 2u}; input.authoredGeneration = 3u; input.heightMillimetres = heights;
			const auto cook = Renderer::Terrain::CookTerrainPage(input); Renderer::Terrain::TerrainViewPacket a{}; a.assetId = input.assetId; a.authoredHash = input.authoredHash; a.authoredGeneration = 3u; a.pageSourceIdentity = cook.sourceIdentity; a.frameId = 8u; auto b = a; b.frameId = 9u;
			const std::array views{a, b};
			const std::array cooks{cook};
			const auto packets = RenderSystem::PrepareCommittedTerrainPackets(views, cooks, {});
			context.Expect(packets.size() == 2u && packets[0].pick.pageSourceIdentity == packets[1].pick.pageSourceIdentity && !packets[0].usesLastKnownGood && !packets[1].usesLastKnownGood,
				"Two runtime views must consume packets carrying the same current terrain source and picking identity rather than editor-owned state.");
		});

		registry.Add("Terrain.ExportRuntime.SignedSparsePartialCoverageUsesOneCurrentRoot", [](TestContext& context) {
			TerrainExportWorkspace workspace;
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			const TerraEngine::Terrain::TerrainSampleBounds authoredBounds{-256, -256, 258, 258};
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 3210);
			std::vector<std::uint8_t> heightBytes(heights.size() * sizeof(std::int32_t));
			std::memcpy(heightBytes.data(), heights.data(), heightBytes.size());
			const auto heightHash = TerraEngine::Terrain::HashTerrainBytes(heightBytes);
			TerraEngine::Terrain::TerrainAreaDescriptor explicitArea{};
			explicitArea.coordinate = {-1, -1};
			explicitArea.editGeneration = 11u;
			explicitArea.channels.heightHash = heightHash;
			explicitArea.minHeightMm = 3210;
			explicitArea.maxHeightMm = 3210;
			explicitArea.authoredContentHash = heightHash;
			TerraEngine::Terrain::TerrainDirectoryShard shard{};
			shard.areaBounds = {-1, -1, 0, 0};
			shard.identity = {0x101u, 0x202u};
			shard.descriptors.emplace(explicitArea.coordinate, explicitArea);
			TerraEngine::Terrain::TerrainAssetRoot root{};
			root.assetUuid = assetId;
			root.publicationGeneration = 11u;
			root.persistedRootGeneration = 11u;
			root.sampleSpacingMetres = 1.5;
			root.authoredBounds = authoredBounds;
			root.defaultHeightMm = -750;
			root.defaultLayerId = 3u;
			root.layerStackGeneration = 1u;
			root.directoryShards.push_back(std::move(shard));
			const auto encodedRoot = TerraEngine::Terrain::EncodeTerrainRootManifest(root);
			const std::string rootText = TerraEngine::Terrain::ToHexString(encodedRoot.contentHash);
			const std::string assetText = TerraEngine::ToHexString(assetId);
			const std::string heightText = TerraEngine::Terrain::ToHexString(heightHash);
			workspace.WriteText("Terrain/Sparse.teterrain", nlohmann::json{
				{"format", "TerraEngineTerrain"}, {"version", 1u}, {"assetId", assetText},
				{"rootFormat", "TERRAROOT1"},
				// Compatibility dimensions intentionally agree but do not enumerate
				// coverage; the immutable root remains the export authority.
				{"width", 514u}, {"height", 514u}, {"sampleSpacingMetres", 1.5}}.dump());
			workspace.WriteText("Terrain/Sparse.teterrain.data/CURRENT", rootText + "\n-\n");
			workspace.WriteText("Terrain/Sparse.teterrain.data/roots/" + rootText + ".root", encodedRoot.canonicalBytes);
			workspace.WriteBytes("Terrain/Sparse.teterrain.data/blobs/" + heightText + ".tearea", heightBytes);

			ContentMount mount;
			context.Expect(encodedRoot.Succeeded() && mount.Mount(workspace.CreatePack()),
				"The signed sparse runtime fixture must mount as one immutable root generation.");
			const auto loaded = TerraEngine::Runtime::BuildMountedTerrainRuntime(
				mount, assetId, "Terrain/Sparse.teterrain");
			const auto negative = std::find_if(loaded.areas.begin(), loaded.areas.end(), [](const auto& area) {
				return area.area == TerraEngine::Terrain::TerrainAreaCoordinate{-1, -1};
			});
			const auto partial = std::find_if(loaded.areas.begin(), loaded.areas.end(), [](const auto& area) {
				return area.area == TerraEngine::Terrain::TerrainAreaCoordinate{1, 1};
			});
			const bool oneGeneration = std::all_of(loaded.areas.begin(), loaded.areas.end(), [](const auto& area) {
				return area.authoredGeneration == 11u;
			});
			const bool removedAreaAbsent = std::none_of(loaded.areas.begin(), loaded.areas.end(), [](const auto& area) {
				return area.area.x < -1 || area.area.z < -1 || area.area.x > 1 || area.area.z > 1;
			});
			const auto recipeHole = [](const TerraEngine::Physics::TerrainHeightFieldRecipe& recipe,
				std::uint32_t x, std::uint32_t z) {
				const std::uint32_t bit = z * 256U + x;
				return (recipe.holeBits[bit / 8U] & (1U << (bit % 8U))) != 0U;
			};
			context.Expect(loaded.Succeeded() && loaded.areas.size() == 9u
				&& loaded.sourceRootHash == encodedRoot.contentHash
				&& loaded.sourcePublicationGeneration == 11u
				&& loaded.authoredBounds == authoredBounds && oneGeneration && removedAreaAbsent
				&& negative != loaded.areas.end() && !negative->implicitDefault
				&& negative->collisionRecipe.areaX == -1 && negative->collisionRecipe.areaZ == -1
				&& negative->collisionRecipe.heightMillimetres.front() == 3210
				&& partial != loaded.areas.end() && partial->implicitDefault
				&& partial->activeRect == TerraEngine::Terrain::TerrainAreaActiveRect{0u, 0u, 2u, 2u}
				&& partial->collisionRecipe.heightMillimetres.front() == -750
				&& !recipeHole(partial->collisionRecipe, 0U, 0U)
				&& recipeHole(partial->collisionRecipe, 1U, 0U)
				&& recipeHole(partial->collisionRecipe, 0U, 1U),
				"Export/runtime must derive signed explicit, sparse implicit, and partial active coverage exclusively from one complete current root. "
					+ loaded.renderProxy.diagnostic);
		});
	}
}
