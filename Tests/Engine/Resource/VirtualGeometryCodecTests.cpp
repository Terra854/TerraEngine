#include "TestHarness.h"

#include "Resource/VirtualGeometryCodec.h"
#include "VirtualGeometryCodecs.h"
#include "VirtualGeometryMeshoptCodec.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace TerraEngine::Tests
{

	void RegisterVirtualGeometryCodecTests(TestRegistry &registry)
	{
		registry.Add("Resource.VirtualGeometryCodec.ReferenceDecoderRoundTripsExpandedAndTerraBlocks",
			[](TestContext &context)
			{
				const std::array<std::uint8_t, 12u> source = { 4u, 4u, 4u, 4u, 8u, 9u, 9u, 9u, 9u, 9u, 2u, 1u };
				const TerraEngine::Editor::VirtualGeometryCodecEncodeRequest request{
					std::span<const std::uint8_t>(source), static_cast<std::uint32_t>(source.size()), 1u, 0u
				};

				for (const TerraEngine::Editor::VirtualGeometryPrototypeCodec codec :
					{ TerraEngine::Editor::VirtualGeometryPrototypeCodec::Expanded, TerraEngine::Editor::VirtualGeometryPrototypeCodec::Terra })
				{
					const TerraEngine::Editor::VirtualGeometryEncodedBlock encoded = TerraEngine::Editor::EncodeVirtualGeometryPrototypeBlock(codec, request);
					context.Expect(encoded.IsComplete(), "Prototype codecs should return a complete self-describing block for bounded valid input.");
					const TerraEngine::Resource::VirtualGeometryCodecDecodeResult decoded =
						TerraEngine::Resource::DecodeVirtualGeometryCompactBlock(encoded.bytes, {});
					context.Expect(decoded.IsComplete(), "The reference decoder should decode a valid prototype block.");
					context.Expect(decoded.bytes == std::vector<std::uint8_t>(source.begin(), source.end()),
						"Reference decoding must reproduce the exact original bytes.");
				}
			});

		registry.Add("Resource.VirtualGeometryCodec.RejectsMalformedIntegrityAndMissingCapability",
			[](TestContext &context)
			{
				const std::array<std::uint8_t, 5u> source = { 7u, 7u, 7u, 3u, 2u };
				const TerraEngine::Editor::VirtualGeometryEncodedBlock encoded =
					TerraEngine::Editor::EncodeVirtualGeometryPrototypeBlock(TerraEngine::Editor::VirtualGeometryPrototypeCodec::Terra,
						{ std::span<const std::uint8_t>(source), static_cast<std::uint32_t>(source.size()), 1u, 0u });
				std::vector<std::uint8_t> malformed = encoded.bytes;
				malformed.pop_back();
				const TerraEngine::Resource::VirtualGeometryCodecDecodeResult truncated =
					TerraEngine::Resource::DecodeVirtualGeometryCompactBlock(malformed, {});
				context.Expect(!truncated.IsComplete(), "Truncated blocks must fail before the decoder reads beyond their declared range.");

				TerraEngine::Resource::VirtualGeometryCodecDecodeLimits limited{};
				limited.availableCapabilities = kVirtualGeometryCodecCapabilityExpanded;
				const TerraEngine::Resource::VirtualGeometryCodecDecodeResult unsupported =
					TerraEngine::Resource::DecodeVirtualGeometryCompactBlock(encoded.bytes, limited);
				context.Expect(!unsupported.IsComplete(), "Required codec capabilities must be checked before a block is decoded.");
			});

		registry.Add("Resource.VirtualGeometryCodec.TerraLiteralBoundaryRoundTripsTwoByteRun",
			[](TestContext &context)
			{
				// The first 127 bytes avoid runs, then a two-byte run lands across
				// the literal boundary. The encoder must close the literal at 127
				// bytes and encode the pair in a following literal block.
				std::vector<std::uint8_t> source(130u);
				for (std::size_t index = 0u; index < 127u; ++index)
				{
					source[index] = static_cast<std::uint8_t>(index);
				}
				source[127u] = 201u;
				source[128u] = 201u;
				source[129u] = 17u;

				const TerraEngine::Editor::VirtualGeometryEncodedBlock encoded =
					TerraEngine::Editor::EncodeVirtualGeometryPrototypeBlock(
						TerraEngine::Editor::VirtualGeometryPrototypeCodec::Terra,
						{ std::span<const std::uint8_t>(source), 1u,
							static_cast<std::uint32_t>(source.size()), 0u });
				const TerraEngine::Resource::VirtualGeometryCodecDecodeResult decoded =
					TerraEngine::Resource::DecodeVirtualGeometryCompactBlock(encoded.bytes, {});

				context.Expect(encoded.IsComplete(),
					"The Terra encoder should accept a bounded literal-boundary fixture.");
				context.Expect(decoded.IsComplete() && decoded.bytes == source,
					"A two-byte run at the 128-byte literal boundary must round-trip without changing the control-byte kind.");
			});

		registry.Add("Resource.VirtualGeometryCodec.ReferenceDecoderRoundTripsMeshoptimizerTopology",
			[](TestContext &context)
			{
				const std::array<std::uint32_t, 6u> indices = { 0u, 1u, 2u, 2u, 1u, 3u };
				std::array<std::uint8_t, sizeof(indices)> source{};
				std::memcpy(source.data(), indices.data(), source.size());
				const TerraEngine::Editor::VirtualGeometryEncodedBlock encoded =
					TerraEngine::Editor::EncodeVirtualGeometryMeshoptBlock({ std::span<const std::uint8_t>(source),
						TerraEngine::Editor::VirtualGeometryMeshoptStreamKind::TriangleIndices,
						static_cast<std::uint32_t>(indices.size()),
						static_cast<std::uint32_t>(sizeof(std::uint32_t)) });
				context.Expect(encoded.IsComplete(), "Vendored meshoptimizer should encode a bounded triangle-index prototype block.");
				const TerraEngine::Resource::VirtualGeometryCodecDecodeResult decoded =
					TerraEngine::Resource::DecodeVirtualGeometryCompactBlock(encoded.bytes, {});
				context.Expect(decoded.IsComplete(), "The reference decoder should decode the meshoptimizer topology prototype.");
				context.Expect(decoded.bytes == std::vector<std::uint8_t>(source.begin(), source.end()),
					"meshoptimizer topology decoding must preserve the original index bytes.");
			});
	}

} // namespace TerraEngine::Tests
