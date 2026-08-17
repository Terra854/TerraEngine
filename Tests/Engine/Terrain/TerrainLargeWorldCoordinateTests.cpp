// TerrainLargeWorldCoordinateTests.cpp -- signed terrain lattice regression coverage.

#include "TestHarness.h"

#include "Terrain/TerrainSpatialProxy.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainTypes.h"

#include <cstdint>
#include <cmath>
#include <memory>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainLargeWorldCoordinateTests(TestRegistry& registry)
	{
		registry.Add("Terrain.LargeWorld.SignedMillionAreasAndRevisitRemainExact", [](TestContext& context) {
			const Terrain::TerrainSampleCoordinate sample{ -256'000'001LL, 256'000'127LL };
			const auto area = Terrain::OwnerArea(sample);
			const auto local = Terrain::LocalSample(sample);
			context.Expect(area.has_value() && area->x == -1'000'001LL && area->z == 1'000'000LL,
				"A signed million-area route must use exact mathematical ownership on both axes.");
			context.Expect(local.has_value() && local->x == 255 && local->z == 127,
				"Returning to a distant negative area must retain the original local sample identity.");

			Terrain::TerrainAreaDescriptor descriptor{};
			descriptor.coordinate = *area;
			descriptor.editGeneration = 9u;
			descriptor.minHeightMm = -250;
			descriptor.maxHeightMm = 750;
			TerraEngine::Scene::WorldPosition origin{};
			TerraEngine::Scene::SpatialReferenceFrame frame{};
			frame.canonicalOrigin = origin;
			const auto proxy = Terrain::BuildTerrainSpatialProxy(descriptor, TerraEngine::GenerateUUID(), 1.0, 0.0, origin, frame);
			context.Expect(!proxy.has_value(), "Distant terrain must refuse an unsafe local-float spatial frame instead of losing exact coordinates.");
		});

		registry.Add("Terrain.LargeWorld.LazyDirectoryRevisitAndCentimetrePickStayExact", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			const Terrain::TerrainAreaCoordinate distantArea{-1'000'000, 3'906}; // Exactly 1,000 km at one metre samples.
			std::vector<std::uint8_t> heightBytes(Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t));
			Terrain::Format::TerrainAreaEnvelopeHeader header{};
			header.assetUuid = asset; header.area = distantArea; header.channelKind = Terrain::TerrainChannelKind::Height;
			Terrain::EncodedTerrainArea encoded{};
			const auto encodedResult = Terrain::EncodeTerrainArea(header, heightBytes, encoded);
			const Terrain::ContentHash blobHash = Terrain::HashTerrainBytes(encoded.bytes);
			Terrain::TerrainAreaDescriptor descriptor{};
			descriptor.coordinate = distantArea; descriptor.editGeneration = 7u; descriptor.authoredContentHash = {1u, 7u}; descriptor.channels.heightHash = blobHash;
			Terrain::TerrainDirectoryShard shard{};
			shard.areaBounds = {distantArea.x, distantArea.z, distantArea.x + 1, distantArea.z + 1}; shard.identity = {2u, 7u}; shard.descriptors.emplace(distantArea, descriptor);
			auto root = std::make_shared<Terrain::TerrainAssetRoot>();
			root->assetUuid = asset; root->publicationGeneration = 7u; root->rootContentHash = {3u, 7u}; root->authoredBounds = {-256'000'000, 0, 1, 1}; root->directoryShards = {shard};
			Terrain::TerrainAssetStore store(root);
			context.Expect(encodedResult.Succeeded() && store.Blobs().PutVerified(blobHash, encoded.bytes), "The million-area fixture must contain one verified immutable height blob.");
			const auto first = store.LoadHeightAreaLazy(store.CaptureRoot(), distantArea);
			const auto revisit = store.LoadHeightAreaLazy(store.CaptureRoot(), distantArea);
			context.Expect(first.Succeeded() && revisit.Succeeded() && revisit.fromCache && first.sourceHash == revisit.sourceHash,
				"Revisiting a distant signed directory entry must retain its exact blob identity and use the bounded cache.");

			TerraEngine::Scene::WorldPosition origin{};
			TerraEngine::Scene::WorldDelta route{}; route.offsetMetres.x = 1'000'000.015;
			TerraEngine::Scene::WorldDelta referenceRoute{}; referenceRoute.offsetMetres.x = 1'000'000.0;
			const auto travelled = TerraEngine::Scene::ApplyWorldDelta(origin, route);
			const auto reference = TerraEngine::Scene::ApplyWorldDelta(origin, referenceRoute);
			const auto local = travelled.Succeeded() && reference.Succeeded() ? TerraEngine::Scene::LocalizeWorldPosition(travelled.position, reference.position, 0.02) : TerraEngine::Scene::WorldLocalizationResult{};
			context.Expect(travelled.Succeeded() && reference.Succeeded() && local.Succeeded() && std::abs(local.offsetMetres.x - 0.015) <= 0.000001,
				"A 1,000-km world-cell route must retain a pick working frame within two centimetres.");
		});
	}
}
