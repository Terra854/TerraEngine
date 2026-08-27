// TerrainFormatTests.cpp -- Deterministic .tearea envelope validation tests.

#include "TestHarness.h"
#include "File Formats/TerraEngineTerrainFile.h"
#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainAssetStore.h"

#include <algorithm>
#include <array>
#include <locale>

namespace TerraEngine::Tests
{
	void RegisterTerrainFormatTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Format.TERRAROOT1RoundTripsCanonicalBytesWithoutSelfHash", [](TestContext& context) {
			Terrain::TerrainAssetRoot root{};
			root.assetUuid = TerraEngine::GenerateUUID();
			root.publicationGeneration = 3U;
			root.persistedRootGeneration = 3U;
			root.sampleSpacingMetres = 0.25;
			root.authoredBounds = {-256, -256, 256, 256};
			root.layerStackGeneration = 1U;
			const auto encoded = Terrain::EncodeTerrainRootManifest(root);
			const auto decoded = Terrain::DecodeTerrainRootManifest(encoded.canonicalBytes);
			const auto repeated = decoded.root ? Terrain::EncodeTerrainRootManifest(*decoded.root) : Terrain::TerrainRootCodecResult{};
			context.Expect(encoded.Succeeded() && decoded.Succeeded() && repeated.Succeeded()
				&& encoded.canonicalBytes.starts_with("TERRAROOT1\n")
				&& encoded.canonicalBytes == repeated.canonicalBytes
				&& encoded.contentHash == decoded.contentHash,
				"Canonical TERRAROOT1 bytes must round-trip exactly and derive identity from the complete document.");
			context.Expect(encoded.canonicalBytes.find("ROOT_HASH") == std::string::npos,
				"TERRAROOT1 must not serialize a self-referential root hash field.");
		});

		registry.Add("Terrain.Format.TERRAROOT1RejectsLegacyTrailingAndOversizedInput", [](TestContext& context) {
			const auto legacy = Terrain::DecodeTerrainRootManifest("TERRAINROOT2\n");
			const auto json = Terrain::DecodeTerrainRootManifest("{\"format\":\"TerraEngineTerrain\"}\n");
			Terrain::TerrainAssetRoot root{};
			root.assetUuid = TerraEngine::GenerateUUID(); root.publicationGeneration = 1U; root.persistedRootGeneration = 1U;
			root.authoredBounds = {0, 0, 256, 256}; root.layerStackGeneration = 1U;
			const auto encoded = Terrain::EncodeTerrainRootManifest(root);
			const auto trailing = Terrain::DecodeTerrainRootManifest(encoded.canonicalBytes + "TRAILING\n");
			std::string oversized("TERRAROOT1\n");
			oversized.resize(Terrain::Format::kTerrainRootMaximumBytes + 1U, 'x');
			const auto bounded = Terrain::DecodeTerrainRootManifest(oversized);
			context.Expect(legacy.diagnostic == Terrain::TerrainRootDiagnosticCode::UnsupportedTerrainRoot
				&& json.diagnostic == Terrain::TerrainRootDiagnosticCode::UnsupportedTerrainRoot
				&& trailing.diagnostic == Terrain::TerrainRootDiagnosticCode::TerrainRootMalformed
				&& bounded.code == Terrain::TerrainResultCode::BudgetExceeded,
				"Legacy, JSON, trailing, and over-budget roots must fail closed with stable recreate/recook diagnostics.");
		});

		registry.Add("Terrain.Format.TERRAROOT1RejectsLegacyRootsWithoutPartialLayerState", [](TestContext& context) {
			// Every pre-TERRAROOT1 spelling is intentionally incompatible. A failed
			// decode must return no root at all so callers cannot accidentally retain
			// layer metadata parsed before the incompatible identity was noticed.
			constexpr std::array<std::string_view, 4u> legacyRoots = {
				"TERRAINROOT1\n",
				"TERRAINROOT2\n",
				"TERRAROOT0\n",
				"{\"format\":\"TerraEngineTerrain\"}\n"
			};
			bool rejectedEveryLegacyRoot = true;
			for(const std::string_view legacyRoot : legacyRoots)
			{
				const Terrain::TerrainRootCodecResult decoded = Terrain::DecodeTerrainRootManifest(legacyRoot);
				rejectedEveryLegacyRoot = rejectedEveryLegacyRoot
					&& decoded.code == Terrain::TerrainResultCode::UnsupportedVersion
					&& decoded.diagnostic == Terrain::TerrainRootDiagnosticCode::UnsupportedTerrainRoot
					&& !decoded.root.has_value()
					&& decoded.canonicalBytes.empty()
					&& (decoded.message.find("recreate") != std::string::npos
						|| decoded.message.find("recook") != std::string::npos);
			}
			context.Expect(rejectedEveryLegacyRoot,
				"Every legacy terrain root must fail closed, expose recreate-or-recook guidance, and return no partial root or layer stack.");
		});

		registry.Add("Terrain.Format.TERRAROOT1MalformedLayerRejectsTheCompleteRoot", [](TestContext& context) {
			Terrain::TerrainLayerStack layers{};
			layers.generation = 4u;
			layers.layers = {
				{1u, "Grass", TerraEngine::GenerateUUID()},
				{2u, "Rock", TerraEngine::GenerateUUID()}
			};
			Terrain::TerrainAssetRoot root{};
			root.assetUuid = TerraEngine::GenerateUUID();
			root.publicationGeneration = 7u;
			root.persistedRootGeneration = 6u;
			root.authoredBounds = {-1, -1, 2, 2};
			root.defaultLayerId = 1u;
			root.layerStackGeneration = layers.generation;
			root.layerStack = std::make_shared<const Terrain::TerrainLayerStack>(layers);
			const Terrain::TerrainRootCodecResult encoded = Terrain::EncodeTerrainRootManifest(root);

			std::string malformed = encoded.canonicalBytes;
			const std::string uniqueLayerName = "\"Rock\"";
			const std::size_t nameOffset = malformed.find(uniqueLayerName);
			if(nameOffset != std::string::npos)
				malformed.replace(nameOffset, uniqueLayerName.size(), "\"Grass\"");
			const Terrain::TerrainRootCodecResult decoded = Terrain::DecodeTerrainRootManifest(malformed);

			context.Expect(encoded.Succeeded() && nameOffset != std::string::npos,
				"The malformed-layer fixture must start from a valid canonical two-layer TERRAROOT1 root.");
			context.Expect(!decoded.Succeeded() && !decoded.root.has_value() && decoded.canonicalBytes.empty()
				&& decoded.diagnostic == Terrain::TerrainRootDiagnosticCode::TerrainLayerIdentityInvalid,
				"A duplicate layer identity discovered late in decode must reject the complete root without exposing earlier root or layer records.");
		});

		registry.Add("Terrain.Format.TERRAROOT1EncodingIgnoresProcessLocale", [](TestContext& context) {
			struct CommaDecimal final : std::numpunct<char> { char do_decimal_point() const override { return ','; } };
			Terrain::TerrainAssetRoot root{};
			root.assetUuid = TerraEngine::GenerateUUID(); root.publicationGeneration = 1U; root.persistedRootGeneration = 1U;
			root.sampleSpacingMetres = 0.125; root.authoredBounds = {0, 0, 1, 1}; root.layerStackGeneration = 1U;
			const auto before = Terrain::EncodeTerrainRootManifest(root);
			const std::locale previous = std::locale();
			std::locale::global(std::locale(previous, new CommaDecimal));
			const auto after = Terrain::EncodeTerrainRootManifest(root);
			std::locale::global(previous);
			context.Expect(before.Succeeded() && after.Succeeded() && before.canonicalBytes == after.canonicalBytes
				&& after.canonicalBytes.find("0.125") != std::string::npos,
				"TERRAROOT1 numeric spelling must remain invariant under a comma-decimal process locale.");
		});
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
