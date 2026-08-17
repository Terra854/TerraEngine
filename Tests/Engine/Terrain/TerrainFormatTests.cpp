// TerrainFormatTests.cpp -- Deterministic .tearea envelope validation tests.

#include "TestHarness.h"
#include "File Formats/TerraEngineTerrainFile.h"
#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	void RegisterTerrainFormatTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Format.HeightEnvelopeRoundTripsDeterministically", [](TestContext& context) {
			Terrain::Format::TerrainAreaEnvelopeHeader header{}; header.assetUuid = TerraEngine::GenerateUUID(); header.area = {-3, 5}; header.channelKind = Terrain::TerrainChannelKind::Height;
			std::vector<std::uint8_t> payload(Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t));
			for (std::size_t index = 0; index < payload.size(); ++index) payload[index] = static_cast<std::uint8_t>(index & 0xffu);
			Terrain::EncodedTerrainArea first{}; Terrain::EncodedTerrainArea second{}; const auto firstResult = Terrain::EncodeTerrainArea(header, payload, first); const auto secondResult = Terrain::EncodeTerrainArea(header, payload, second);
			Terrain::DecodedTerrainArea decoded{}; const auto decodeResult = Terrain::DecodeTerrainArea(first.bytes, decoded);
			context.Expect(firstResult.Succeeded() && secondResult.Succeeded() && decodeResult.Succeeded(), "A valid height channel should encode and decode through the bounded envelope.");
			context.Expect(first.bytes == second.bytes && decoded.canonicalPayload == payload, "Canonical terrain area bytes should be deterministic and lossless.");
		});

		registry.Add("Terrain.Format.RejectsBadIdentityAndUnsupportedVersion", [](TestContext& context) {
			Terrain::Format::TerrainAreaEnvelopeHeader header{}; header.assetUuid = TerraEngine::GenerateUUID(); std::vector<std::uint8_t> payload(Terrain::kTerrainAreaSampleCount / 8u, 0u); header.channelKind = Terrain::TerrainChannelKind::Holes;
			Terrain::EncodedTerrainArea encoded{}; context.Expect(Terrain::EncodeTerrainArea(header, payload, encoded).Succeeded(), "The corruption fixture must start as a valid terrain envelope.");
			const std::vector<std::uint8_t> validBytes = encoded.bytes;
			encoded.bytes.back() ^= 0x01u; Terrain::DecodedTerrainArea decoded{}; const auto badHash = Terrain::DecodeTerrainArea(encoded.bytes, decoded);
			context.Expect(badHash.code == Terrain::TerrainResultCode::HashMismatch || badHash.code == Terrain::TerrainResultCode::CorruptData, "Changed stored bytes must fail before publication.");
			std::vector<std::uint8_t> unsupportedVersion = validBytes;
			unsupportedVersion[8u] = 2u;
			unsupportedVersion[9u] = 0u;
			context.Expect(Terrain::DecodeTerrainArea(unsupportedVersion, decoded).code == Terrain::TerrainResultCode::UnsupportedVersion, "Unknown required major versions must fail before allocating or publishing payload data.");
		});

		registry.Add("Terrain.Format.RootAndDirectoryContractsValidateSignedShards", [](TestContext& context) {
			const Terrain::ContentHash rootHash{11u, 22u};
			const Terrain::ContentHash directoryHash{33u, 44u};
			const Terrain::ContentHash areaHash{55u, 66u};
			Terrain::Format::TerrainRootManifestContract root{};
			root.assetUuid = TerraEngine::GenerateUUID();
			root.persistedRootGeneration = 7u;
			root.layerStackGeneration = 1u;
			root.authoredBounds = {-512, -512, 512, 512};
			root.predecessor = Terrain::Format::TerrainPredecessorReference{6u, rootHash};
			root.directoryShards.push_back({{-2, -2, 2, 2}, directoryHash});

			Terrain::Format::TerrainDirectoryShardContract directory{};
			directory.bounds = {-2, -2, 2, 2};
			directory.identity = directoryHash;
			Terrain::Format::TerrainDirectoryEntryContract entry{};
			entry.coordinate = {-1, 1};
			entry.editGeneration = 9u;
			entry.authoredContentHash = areaHash;
			entry.heightHash = {77u, 88u};
			entry.weightGroupHashes = {{1u, {99u, 111u}}, {2u, {122u, 133u}}};
			directory.entries.push_back(entry);

			context.Expect(root.IsStructurallyValid(), "The schema-aligned root contract should accept a bounded signed shard map.");
			context.Expect(directory.IsStructurallyValid(), "The directory contract should accept sorted channel groups inside its half-open bounds.");
			directory.entries.front().coordinate.x = 2;
			context.Expect(!directory.IsStructurallyValid(), "An area on the exclusive shard edge must be rejected before lookup or publication.");
			root.version.major = 2u;
			context.Expect(!root.IsStructurallyValid(), "Unsupported root major versions must fail closed.");
		});

		registry.Add("Terrain.Format.DecodeBudgetRejectsOversizedInput", [](TestContext& context) {
			Terrain::Format::TerrainAreaEnvelopeHeader header{}; header.assetUuid = TerraEngine::GenerateUUID(); std::vector<std::uint8_t> payload(Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t), 0u); Terrain::EncodedTerrainArea encoded{};
			context.Expect(Terrain::EncodeTerrainArea(header, payload, encoded).Succeeded(), "The size-bound fixture must encode."); Terrain::DecodedTerrainArea decoded{}; Terrain::TerrainCodecLimits limit{}; limit.maximumDecodedBytes = 16u;
			context.Expect(Terrain::DecodeTerrainArea(encoded.bytes, decoded, limit).code == Terrain::TerrainResultCode::BudgetExceeded, "Declared decoded length must be checked before allocating the terrain payload.");
		});
	}
}
