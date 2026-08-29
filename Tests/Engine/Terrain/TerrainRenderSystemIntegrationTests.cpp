// TerrainRenderSystemIntegrationTests.cpp -- RenderSystem committed terrain packet gate.

#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/Terrain/TerrainLodSelection.h"
#include "Renderer/Terrain/TerrainPageFormat.h"
#include "Renderer/Terrain/TerrainRenderPackets.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"

#include <array>
#include <cstdint>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainRenderSystemIntegrationTests(TestRegistry& registry)
	{
		registry.Add("Terrain.RenderSystem.ProxyRegistryKeepsDerivedBindingsOutOfComponents", [](TestContext& context) {
			auto& proxies = TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance();
			proxies.Clear();
			const std::uint64_t before = proxies.PublicationGeneration();
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			context.Expect(!proxies.Publish({assetId, TerraEngine::EMPTY_UUID, materialId}),
				"Terrain render proxy publication must reject an incomplete derived binding.");
			context.Expect(proxies.Publish({assetId, meshId, materialId}),
				"A complete terrain-derived mesh and material binding must publish without an ECS mesh component.");
			const auto resolved = proxies.Resolve(assetId);
			context.Expect(resolved && resolved->IsComplete()
				&& resolved->meshId == meshId && resolved->materialId == materialId
				&& resolved->publicationGeneration > before,
				"The renderer must resolve the complete derived binding from the durable terrain asset UUID.");
			const std::uint64_t publishedGeneration = proxies.PublicationGeneration();
			context.Expect(proxies.Remove(assetId) && !proxies.Resolve(assetId)
				&& proxies.PublicationGeneration() > publishedGeneration,
				"Removing the last terrain component must retire its binding and invalidate cached render membership.");
			context.Expect(!proxies.Remove(assetId),
				"Repeated terrain proxy retirement must be an idempotent no-op.");
			proxies.Clear();
			context.Expect(!proxies.Resolve(assetId).has_value(),
				"Clearing a render session must retire terrain proxy bindings rather than leaking them into the next scene.");
		});

		registry.Add("Terrain.RenderSystem.ConsumesOnlyCurrentCommittedCookPackets", [](TestContext& context) {
			std::vector<std::int32_t> heights(
				TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent
					* TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent,
				1000);
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
			// A committed page owns an RHI descriptor completed at the protected
			// Vulkan fence boundary, not the intentionally incomplete worker intent.
			lastKnownGood.descriptor = {packets.front().candidate.candidateId, 0u, 0u, true};
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

		registry.Add("Terrain.RenderSystem.AssetSnapshotPublishesNonOriginAreasWithStableIdentity", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Terrain;
			auto& proxies = TerrainRenderProxyRegistry::Instance();
			proxies.Clear();
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			TerrainAreaRenderBinding origin{};
			origin.coordinate = {0, 0};
			origin.stableAreaIdentity = {101u, 201u};
			origin.sampleBounds = {0, 0, 256, 256};
			origin.authoredHash = {301u, 401u};
			origin.authoredGeneration = 5u;
			origin.pageSourceIdentity = {501u, 601u};
			origin.controlIdentity = {701u, 801u};
			origin.holeIdentity = {901u, 1001u};
			origin.hierarchyIdentity = {1101u, 1201u};
			origin.pageDirectoryIdentity = {1301u, 1401u};
			origin.directMeshId = TerraEngine::GenerateUUID();
			origin.materialId = TerraEngine::GenerateUUID();
			origin.coarsePageId = 0u;
			TerrainAreaRenderBinding nonOrigin = origin;
			nonOrigin.coordinate = {-5, 3};
			nonOrigin.stableAreaIdentity = {102u, 202u};
			nonOrigin.sampleBounds = {-1280, 768, -1024, 1024};
			nonOrigin.authoredHash = {302u, 402u};
			nonOrigin.pageSourceIdentity = {502u, 602u};
			nonOrigin.controlIdentity = {702u, 802u};
			nonOrigin.holeIdentity = {902u, 1002u};
			nonOrigin.hierarchyIdentity = {1102u, 1202u};
			nonOrigin.pageDirectoryIdentity = {1302u, 1402u};
			nonOrigin.directMeshId = TerraEngine::GenerateUUID();
			TerrainAssetRenderSnapshot snapshot{};
			snapshot.assetId = assetId;
			snapshot.publicationGeneration = 5u;
			snapshot.areas = {origin, nonOrigin};
			context.Expect(snapshot.IsComplete() && proxies.PublishSnapshot(snapshot),
				"One atomic terrain render snapshot must accept complete origin and non-origin area bindings together.");

			// Mutating the detached producer value after publication must not alter
			// the renderer-owned generation captured by a frame.
			snapshot.areas[1].stableAreaIdentity = {9999u, 9999u};
			snapshot.areas.clear();
			const auto published = proxies.ResolveSnapshot(assetId);
			const auto publishedArea = published ? published->FindArea({-5, 3}) : nullptr;
			context.Expect(published && published->publicationGeneration == 5u
				&& published->areas.size() == 2u && publishedArea != nullptr
				&& publishedArea->stableAreaIdentity == nonOrigin.stableAreaIdentity
				&& publishedArea->coordinate == nonOrigin.coordinate,
				"Frame capture must resolve every accepted non-origin area from an immutable copied snapshot with stable area identity.");

			TerrainAssetRenderSnapshot replacement = *published;
			replacement.publicationGeneration = 6u;
			replacement.areas[1].controlIdentity = {1702u, 1802u};
			context.Expect(proxies.PublishSnapshot(replacement),
				"A complete newer asset render snapshot should replace the prior generation atomically.");
			const auto republished = proxies.ResolveSnapshot(assetId);
			const auto republishedArea = republished ? republished->FindArea({-5, 3}) : nullptr;
			context.Expect(republishedArea != nullptr
				&& republishedArea->stableAreaIdentity == nonOrigin.stableAreaIdentity
				&& republishedArea->controlIdentity == replacement.areas[1].controlIdentity,
				"Derived generation changes may replace control bindings without changing durable per-area identity.");
			proxies.Clear();
		});

		registry.Add("Terrain.RenderSystem.AreaPacketsPreserveRouteControlPickingShadowAndHoleParity", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Terrain;
			using TerraEngine::Resource::VirtualGeometryPagePublicationState;
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			TerrainAreaRenderBinding area{};
			area.coordinate = {-2, 6};
			area.stableAreaIdentity = {21u, 22u};
			area.sampleBounds = {-512, 1536, -256, 1792};
			area.activeRect = {16u, 28u, 33u, 40u};
			area.authoredHash = {31u, 32u};
			area.authoredGeneration = 8u;
			area.pageSourceIdentity = {41u, 42u};
			area.controlIdentity = {51u, 52u};
			area.holeIdentity = {61u, 62u};
			area.hierarchyIdentity = {71u, 72u};
			area.pageDirectoryIdentity = {81u, 82u};
			area.directMeshId = TerraEngine::GenerateUUID();
			area.materialId = TerraEngine::GenerateUUID();
			area.coarsePageId = 0u;
			TerrainAssetRenderSnapshot snapshot{};
			snapshot.assetId = assetId;
			snapshot.publicationGeneration = 8u;
			snapshot.areas = {area};

			TerrainPageResidencyEntry root{};
			root.assetId = assetId;
			root.area = area.coordinate;
			root.pageId = area.coarsePageId;
			root.parentPageId = kInvalidTerrainPageId;
			root.state = VirtualGeometryPagePublicationState::Resident;
			root.uploadCompleted = true;
			root.published = true;
			root.pinnedCoarseCoverage = true;
			TerrainPageResidencyEntry detailed = root;
			detailed.pageId = 4u;
			detailed.parentPageId = root.pageId;
			detailed.pinnedCoarseCoverage = false;
			TerrainPageResidencyEntry missing = detailed;
			missing.pageId = 7u;
			missing.state = VirtualGeometryPagePublicationState::Missing;
			missing.uploadCompleted = false;
			missing.published = false;
			const std::array residency{root, detailed, missing};

			TerrainAreaRenderRequest direct{};
			direct.frameId = 90u;
			direct.viewId = 1u;
			direct.area = area.coordinate;
			direct.route = TerrainGeometryRoute::Direct;
			direct.purpose = TerrainRenderPurpose::Camera;
			direct.desiredPageId = detailed.pageId;
			direct.localCellX = 17u;
			direct.localCellZ = 29u;
			direct.priority = 80;
			TerrainAreaRenderRequest page = direct;
			page.viewId = 2u;
			page.route = TerrainGeometryRoute::PageBacked;
			TerrainAreaRenderRequest shadow = page;
			shadow.viewId = 3u;
			shadow.purpose = TerrainRenderPurpose::Shadow;
			TerrainAreaRenderRequest picking = direct;
			picking.viewId = 4u;
			picking.purpose = TerrainRenderPurpose::Picking;
			TerrainAreaRenderRequest demand = page;
			demand.viewId = 5u;
			demand.desiredPageId = missing.pageId;
			demand.priority = 95;
			const std::array requests{direct, page, shadow, picking, demand};
			const auto prepared = PrepareTerrainAreaRenderPackets(
				snapshot, requests, residency);

			context.Expect(prepared.complete && prepared.packets.size() == requests.size(),
				"A complete captured terrain snapshot must produce one packet for every camera, shadow, and picking request across both geometry routes.");
			if(prepared.packets.size() != requests.size()) return;
			const auto& directPacket = prepared.packets[0];
			const auto& pagePacket = prepared.packets[1];
			const auto& shadowPacket = prepared.packets[2];
			const auto& pickingPacket = prepared.packets[3];
			const auto& demandPacket = prepared.packets[4];
			context.Expect(directPacket.route == TerrainGeometryRoute::Direct
				&& pagePacket.route == TerrainGeometryRoute::PageBacked
				&& directPacket.stableAreaIdentity == pagePacket.stableAreaIdentity
				&& directPacket.controlIdentity == pagePacket.controlIdentity
				&& directPacket.holeIdentity == pagePacket.holeIdentity
				&& directPacket.pick.assetId == pagePacket.pick.assetId
				&& directPacket.pick.area == pagePacket.pick.area
				&& directPacket.pick.authoredHash == pagePacket.pick.authoredHash,
				"Direct and page-backed geometry routes must consume the same stable area, control, hole, material, and picking identities.");
			context.Expect(shadowPacket.castsShadow
				&& shadowPacket.controlIdentity == directPacket.controlIdentity
				&& shadowPacket.holeIdentity == directPacket.holeIdentity
				&& shadowPacket.stableAreaIdentity == directPacket.stableAreaIdentity,
				"Shadow cuts must retain the exact area control and hole boundary identity used by camera rendering.");
			context.Expect(pickingPacket.pick.IsComplete()
				&& pickingPacket.pick.localCellX == picking.localCellX
				&& pickingPacket.pick.localCellZ == picking.localCellZ
				&& pickingPacket.pick.area == area.coordinate,
				"Picking must preserve non-origin area identity and exact local-cell coordinates independently of geometry route.");
			TerrainAreaRenderRequest inactiveTail = picking;
			inactiveTail.localCellX = area.activeRect.maxXExclusive;
			context.Expect(directPacket.activeRect == area.activeRect
				&& !PrepareTerrainAreaRenderPackets(snapshot,
					std::span<const TerrainAreaRenderRequest>(&inactiveTail, 1u), residency).complete,
				"Camera, shadow, and picking packets must reject cells in a partial area's inactive canonical tail.");
			context.Expect(demandPacket.actualPageId == root.pageId
				&& demandPacket.desiredPageId == missing.pageId
				&& demandPacket.usesAncestorPage
				&& prepared.demands.size() == 1u
				&& prepared.demands[0].pageId == missing.pageId
				&& prepared.demands[0].priority == demand.priority,
				"A visible missing detail page must render pinned own-ancestor coverage and emit a next-frame demand for the desired page.");
		});
	}
}
