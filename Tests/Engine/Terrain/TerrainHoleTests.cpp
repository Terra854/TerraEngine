// TerrainHoleTests.cpp -- canonical terrain hole ownership and cooker coverage.

#include "TestHarness.h"

#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainPageFormat.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainEditTransaction.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		constexpr std::size_t kHoleByteCount = (Terrain::kTerrainAreaExtentSamples * Terrain::kTerrainAreaExtentSamples) / 8u;

		Terrain::TerrainExactSpan MakeHoleSpan(std::vector<std::uint8_t> bytes)
		{
			Terrain::TerrainExactSpan span{};
			span.channel = Terrain::TerrainChannelKind::Holes;
			span.uncompressedByteCount = static_cast<std::uint32_t>(kHoleByteCount);
			span.compressedContentHash = Terrain::HashTerrainBytes(bytes);
			span.compressedBytes = std::move(bytes);
			return span;
		}
	}

	void RegisterTerrainHoleTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Holes.CanonicalMaskRefinesBoundaryMeshlets", [](TestContext& context) {
			// Page cooking consumes the complete 257-by-257 owned vertex grid plus
			// one west/south normal-only halo sample, not the authored 256 grid.
			std::vector<std::int32_t> heights(
				TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent
					* TerraEngine::Renderer::Terrain::Format::kCookHaloSampleExtent,
				10);
			std::vector<std::uint8_t> holes(8192u, 0u);
			const std::uint32_t bit = 16u * 256u + 16u;
			holes[bit / 8u] |= static_cast<std::uint8_t>(1u << (bit % 8u));
			TerraEngine::Renderer::Terrain::TerrainCookInput input{};
			input.assetId = TerraEngine::GenerateUUID(); input.authoredHash = {4u, 5u}; input.authoredGeneration = 2u; input.heightMillimetres = heights; input.holeCellBits = holes;
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			const bool boundary = std::any_of(cooked.meshlets.begin(), cooked.meshlets.end(), [](const auto& meshlet) { return meshlet.hasHoleBoundary; });
			context.Expect(cooked.complete && boundary && std::any_of(cooked.lodNodes.begin(), cooked.lodNodes.end(), [](const auto& node) { return node.refinesHoleBoundary; }),
				"A canonical hole bit must invalidate render identity and request refinement around its meshlet boundary.");
		});

		registry.Add("Terrain.Holes.BoundaryOwnershipCodecAndExactInverseAreCanonical", [](TestContext& context) {
			// Cell ownership follows the lower-left sample. These two coordinates
			// exercise both sides of zero and an exact positive area corner.
			const auto negativeOwner = Terrain::OwnerArea({-1, -1});
			const auto negativeLocal = Terrain::LocalSample({-1, -1});
			const auto cornerOwner = Terrain::OwnerArea({256, 256});
			const auto cornerLocal = Terrain::LocalSample({256, 256});
			context.Expect(negativeOwner == Terrain::TerrainAreaCoordinate{-1, -1}
				&& negativeLocal == Terrain::TerrainSampleCoordinate{255, 255}
				&& cornerOwner == Terrain::TerrainAreaCoordinate{1, 1}
				&& cornerLocal == Terrain::TerrainSampleCoordinate{0, 0},
				"Boundary and corner hole cells must have one signed-lattice owner with no duplicated edge cell.");

			std::vector<std::uint8_t> mask(kHoleByteCount, 0u);
			mask.front() = 0x01u;
			mask.back() = 0x80u;
			Terrain::Format::TerrainAreaEnvelopeHeader header{};
			header.assetUuid = TerraEngine::GenerateUUID();
			header.area = {1, 1};
			header.channelKind = Terrain::TerrainChannelKind::Holes;
			Terrain::EncodedTerrainArea encoded{};
			Terrain::DecodedTerrainArea decoded{};
			const auto encodedResult = Terrain::EncodeTerrainArea(header, mask, encoded);
			const auto decodedResult = encodedResult.Succeeded() ? Terrain::DecodeTerrainArea(encoded.bytes, decoded) : Terrain::TerrainCodecResult{Terrain::TerrainResultCode::Failed, "encode failed"};
			context.Expect(decodedResult.Succeeded() && decoded.header.channelKind == Terrain::TerrainChannelKind::Holes && decoded.canonicalPayload == mask,
				"The .tearea codec must preserve the first and last canonical hole cells exactly.");

			Terrain::TerrainAssetRoot root{};
			root.assetUuid = header.assetUuid;
			root.publicationGeneration = 1u;
			root.rootContentHash = {10u, 11u};
			root.authoredBounds = {256, 256, 512, 512};
			Terrain::TerrainAreaDescriptor before{};
			before.coordinate = header.area;
			before.editGeneration = 1u;
			before.authoredContentHash = {12u, 13u};
			Terrain::TerrainDirectoryShard shard{};
			shard.areaBounds = {1, 1, 2, 2};
			shard.descriptors.emplace(before.coordinate, before);
			root.directoryShards.push_back(std::move(shard));
			Terrain::TerrainEditTransaction edit{};
			edit.transactionId = TerraEngine::GenerateUUID();
			edit.targetAssetUuid = root.assetUuid;
			edit.inputRootHash = root.rootContentHash;
			edit.affectedBounds = root.authoredBounds;
			Terrain::TerrainPatch patch{};
			patch.kind = Terrain::TerrainPatchKind::ReplaceHoles;
			patch.replacement = before;
			patch.replacement.editGeneration = 2u;
			patch.replacement.authoredContentHash = {14u, 15u};
			patch.beforeSpans.push_back(MakeHoleSpan({0x00u}));
			patch.afterSpans.push_back(MakeHoleSpan({0x01u}));
			edit.patches.push_back(patch);
			edit.readSet.push_back({before.coordinate, before.editGeneration, before.authoredContentHash});
			context.Expect(Terrain::ValidateTerrainEditTransaction(edit, root).Succeeded(),
				"A hole edit with exact canonical before/after bytes must be accepted for protected publication.");
			edit.patches.front().beforeSpans.clear();
			context.Expect(!Terrain::ValidateTerrainEditTransaction(edit, root).Succeeded(),
				"A hole edit without its exact inverse mask must fail atomically before publication or history changes.");
		});
	}
}
