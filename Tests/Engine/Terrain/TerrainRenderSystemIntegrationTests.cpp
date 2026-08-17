// TerrainRenderSystemIntegrationTests.cpp -- RenderSystem committed terrain packet gate.

#include "TestHarness.h"

#include "Renderer/RenderSystem.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainRenderSystemIntegrationTests(TestRegistry& registry)
	{
		registry.Add("Terrain.RenderSystem.ConsumesOnlyCurrentCommittedCookPackets", [](TestContext& context) {
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 1000);
			TerraEngine::Renderer::Terrain::TerrainCookInput input{};
			input.assetId = TerraEngine::GenerateUUID();
			input.authoredHash = {31u, 47u};
			input.authoredGeneration = 5u;
			input.heightMillimetres = heights;
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			TerraEngine::Renderer::Terrain::TerrainViewPacket view{};
			view.assetId = input.assetId;
			view.authoredHash = input.authoredHash;
			view.pageSourceIdentity = cooked.sourceIdentity;
			view.authoredGeneration = input.authoredGeneration;
			view.frameId = 2u;
			const std::vector<TerraEngine::Renderer::Terrain::TerrainViewPacket> views = {view};
			const std::vector<TerraEngine::Renderer::Terrain::TerrainPageCookResult> cooks = {cooked};
			const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication> committed;
			const auto packets = RenderSystem::PrepareCommittedTerrainPackets(views, cooks, committed);
			context.Expect(packets.size() == 1u && !packets.front().usesLastKnownGood && packets.front().pick.IsComplete() && packets.front().candidate.key.pageContentGeneration == input.authoredGeneration, "RenderSystem should admit exactly one current terrain cook as an immutable page candidate with a stable pick identity.");

			// Model the already-published page as a value-only protected snapshot.
			// The next render preparation receives no cook, so it may use this exact
			// resident page only when its generation and payload hash remain current.
			TerraEngine::Resource::VirtualGeometryPagePublication lastKnownGood{};
			lastKnownGood.candidateId = packets.front().candidate.candidateId;
			lastKnownGood.publicationGeneration = packets.front().candidate.publicationGeneration;
			lastKnownGood.key = packets.front().candidate.key;
			lastKnownGood.payload = packets.front().candidate.payload;
			lastKnownGood.hierarchy = packets.front().candidate.hierarchy;
			lastKnownGood.descriptor = packets.front().candidate.descriptor;
			lastKnownGood.state = TerraEngine::Resource::VirtualGeometryPagePublicationState::Resident;
			lastKnownGood.publicationFrame = view.frameId;
			const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication> visible = {lastKnownGood};
			const auto fallbackPackets = RenderSystem::PrepareCommittedTerrainPackets(views, {}, visible);
			context.Expect(fallbackPackets.size() == 1u && fallbackPackets.front().usesLastKnownGood && fallbackPackets.front().candidate.key == lastKnownGood.key, "RenderSystem should reuse only a matching resident last-known-good terrain page when a new cook is unavailable.");

			view.authoredGeneration = 6u;
			const std::vector<TerraEngine::Renderer::Terrain::TerrainViewPacket> staleViews = {view};
			const auto stalePackets = RenderSystem::PrepareCommittedTerrainPackets(staleViews, cooks, visible);
			context.Expect(stalePackets.empty(), "RenderSystem must exclude both a stale cook and a stale last-known-good page for a newer frame generation.");
		});
	}
}
