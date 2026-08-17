// TerrainExportRuntimeTests.cpp -- Archive-only terrain runtime discovery.

#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainContentResolver.h"

#include <array>
#include <filesystem>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainExportRuntimeTests(TestRegistry& registry)
	{
		registry.Add("Terrain.ExportRuntime.RejectsLooseFallbackAndFindsDedicatedShaders", [](TestContext& context) {
			ContentMount unmounted; const auto fallback = TerrainContentResolver::ReadRoot(unmounted, "Terrain/Runtime.teterrain");
			const std::filesystem::path root = TERRAENGINE_SOURCE_DIR;
			const bool shaders = std::filesystem::exists(root / "Engine/Shaders/Terrain/TerrainVisibility.frag.hlsl") && std::filesystem::exists(root / "Engine/Shaders/Terrain/TerrainVisibility.vert.hlsl");
			context.Expect(fallback.status == TerrainContentStatus::NotMounted && shaders && TerrainContentResolver::ReadRoot(unmounted, "Editor/Runtime.teterrain").status == TerrainContentStatus::ForbiddenRuntimePath,
				"Export terrain loading must fail closed without a TEPAK mount, discover dedicated runtime shaders, and reject editor-only logical paths.");
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
