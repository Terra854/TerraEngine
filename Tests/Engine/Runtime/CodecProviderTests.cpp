#include "TestHarness.h"

#include "Runtime/ContentMount/codec/CodecProvider.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		// Repeated byte patterns make compression choices deterministic without
		// depending on large fixture files checked into the repository.
		tepak::ByteBuffer MakeRepeatedBytes(std::string_view pattern, std::size_t repeatCount) {
			tepak::ByteBuffer bytes;
			bytes.reserve(pattern.size() * repeatCount);
			for(std::size_t repeatIndex = 0U; repeatIndex < repeatCount; ++repeatIndex) {
				bytes.insert(bytes.end(), pattern.begin(), pattern.end());
			}
			return bytes;
		}
	}

	void RegisterCodecProviderTests(TestRegistry& registry) {
		registry.Add("Engine.CodecProvider.HashBytesIsStableAcrossBufferAndString", [](TestContext& context) {
			const tepak::codec::CodecProvider provider;
			const std::string text = "TerraEngine deterministic hash input";
			const tepak::ByteBuffer bytes(text.begin(), text.end());

			context.ExpectEqual(provider.HashBytes(text), provider.HashBytes(bytes), "String and byte-buffer hashing should use the same byte sequence.");
			context.ExpectEqual(provider.HashBytes(bytes), provider.HashBytes(bytes), "Hashing the same buffer twice should be deterministic.");
		});

		registry.Add("Engine.CodecProvider.SmallOrDisallowedPayloadsStayRaw", [](TestContext& context) {
			const tepak::codec::CodecProvider provider;
			const tepak::ByteBuffer smallBytes = MakeRepeatedBytes("abc", 8U);
			const tepak::ByteBuffer largeBytes = MakeRepeatedBytes("TerraEngine raw compression guard ", 128U);

			const tepak::codec::CompressionDecision smallDecision = provider.ChooseCompression(smallBytes, {}, true, 9);
			context.Expect(smallDecision.mode == tepak::StorageMode::Raw, "Payloads smaller than the codec threshold should stay raw.");
			context.Expect(smallDecision.storedBytes == smallBytes, "Raw small payloads should keep the original bytes exactly.");

			const tepak::codec::CompressionDecision disabledDecision = provider.ChooseCompression(largeBytes, {}, false, 9);
			context.Expect(disabledDecision.mode == tepak::StorageMode::Raw, "Compression-disabled payloads should stay raw.");
			context.Expect(disabledDecision.storedBytes == largeBytes, "Raw disabled-compression payloads should keep the original bytes exactly.");
		});

		registry.Add("Engine.CodecProvider.CompressiblePayloadRoundTripsThroughZstd", [](TestContext& context) {
			const tepak::codec::CodecProvider provider;
			const tepak::ByteBuffer bytes = MakeRepeatedBytes("AAAAAABBBBBBCCCCCCDDDDDD", 512U);

			const tepak::codec::CompressionDecision decision = provider.ChooseCompression(bytes, {}, true, 9);
			context.Expect(decision.mode == tepak::StorageMode::Zstd, "Highly repetitive payloads should choose plain zstd compression.");
			context.Expect(decision.storedBytes.size() < bytes.size(), "Compressed payloads should store fewer bytes than the original input.");

			const tepak::ByteBuffer decoded = provider.Decompress(decision.storedBytes, decision.mode, bytes.size(), {}, nullptr);
			context.Expect(decoded == bytes, "Zstd-compressed payloads should decode back to the original bytes.");
		});

		registry.Add("Engine.CodecProvider.EstimatesAndTinyDictionariesAreSafe", [](TestContext& context) {
			const tepak::codec::CodecProvider provider;
			const tepak::ByteBuffer tinyDictionary{1U, 2U, 3U, 4U};

			context.Expect(provider.EstimateCompressionContextBytes(4096U, 9, 0U) > 0U, "Compression context estimates should be non-zero for valid source sizes.");
			context.ExpectEqual(0ULL, provider.EstimatePreparedCompressionDictionaryBytes(4096U, 9, tinyDictionary.size()), "Tiny dictionaries should not allocate prepared compression state.");

			const tepak::codec::PreparedCompressionDictionary compressionDictionary = provider.PrepareCompressionDictionary(tinyDictionary, 9);
			const tepak::codec::PreparedDecompressionDictionary decompressionDictionary = provider.PrepareDecompressionDictionary(tinyDictionary);
			context.Expect(!compressionDictionary.IsAvailable(), "Tiny compression dictionaries should be rejected without throwing.");
			context.Expect(!decompressionDictionary.IsAvailable(), "Tiny decompression dictionaries should be rejected without throwing.");

			const tepak::DictionaryProfile profile = provider.TrainDictionary({}, 4096U);
			context.Expect(!profile.dictionaryPresent, "Empty dictionary training input should not produce a dictionary.");
			context.ExpectEqual(0ULL, profile.dictionarySize, "Empty dictionary training input should report a zero-byte dictionary.");
		});

		registry.Add("Engine.CodecProvider.DecompressRejectsSizeMismatchForCompressedPayload", [](TestContext& context) {
			const tepak::codec::CodecProvider provider;
			const tepak::ByteBuffer bytes = MakeRepeatedBytes("size-check-payload-", 1024U);
			const tepak::codec::CompressionDecision decision = provider.ChooseCompression(bytes, {}, true, 9);

			context.Expect(decision.mode != tepak::StorageMode::Raw, "The mismatch test needs a compressed payload so the decoder validates the output size.");

			bool rejectedMismatch = false;
			try {
				(void)provider.Decompress(decision.storedBytes, decision.mode, bytes.size() + 1U, {}, nullptr);
			} catch(const std::runtime_error&) {
				rejectedMismatch = true;
			}
			context.Expect(rejectedMismatch, "Compressed decode should reject an expected-size mismatch.");
		});
	}
}
