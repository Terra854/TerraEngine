// AudioResourceTests.cpp - state-based resource ownership checks; no device is required.

#include "TestHarness.h"

#include "Audio/Assets/AudioClipAsset.h"
#include "File Formats/TerraEngineAudioFile.h"

#include <array>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterAudioResourceTests(TestRegistry& registry)
	{
		registry.Add("Engine.AudioResource.ImmutablePayloadAndChecksum", [](TestContext& context) {
			TerraEngine::Audio::AudioFileMetadata metadata;
			metadata.assetId = TerraEngine::UUID{1U};
			metadata.encoding = TerraEngine::Audio::AudioEncoding::Wave;
			metadata.channelCount = 1U;
			metadata.sampleRate = 48000U;
			metadata.frameCount = 4U;
			std::vector<std::uint8_t> payload{0x52U, 0x49U, 0x46U, 0x46U};
			metadata.sourceChecksum = TerraEngine::Audio::ComputeAudioChecksum(payload);
			metadata.payloadChecksum = metadata.sourceChecksum;
			const auto asset = TerraEngine::Audio::AudioClipAsset::Create(metadata, payload, 3U);
			context.Expect(asset != nullptr && asset->Generation() == 3U
				&& asset->EncodedBytes() == payload.size() && asset->EncodedPayload()[0] == payload[0],
				"A published clip retains one immutable encoded payload generation.");
			payload[0] = 0U;
			context.Expect(asset != nullptr && asset->EncodedPayload()[0] == 0x52U,
				"Caller mutation after publication must not alter retained clip bytes.");
		});

		registry.Add("Engine.AudioResource.TEAFRoundTripAndCorruptionRejection", [](TestContext& context) {
			TerraEngine::Audio::AudioFileMetadata metadata;
			metadata.assetId = TerraEngine::UUID{2U};
			metadata.encoding = TerraEngine::Audio::AudioEncoding::OggVorbis;
			metadata.channelCount = 2U;
			metadata.sampleRate = 44100U;
			metadata.frameCount = 99U;
			const std::vector<std::uint8_t> payload{0x4fU, 0x67U, 0x67U, 0x53U, 1U};
			metadata.sourceChecksum = TerraEngine::Audio::ComputeAudioChecksum(payload);
			metadata.payloadChecksum = metadata.sourceChecksum;
			std::vector<std::uint8_t> serialized;
			context.Expect(TerraEngine::Audio::SerializeAudioFile(metadata, payload, serialized)
				== TerraEngine::Audio::AudioFileStatus::Ok
				&& TerraEngine::Audio::ReadAudioFile(serialized).Succeeded(),
				"A checked TEAF envelope must round-trip its provider-neutral metadata and bytes.");
			serialized.back() ^= 0xffU;
			context.Expect(TerraEngine::Audio::ReadAudioFile(serialized).status
				== TerraEngine::Audio::AudioFileStatus::PayloadChecksumMismatch,
				"Corrupt payload bytes must reject before a runtime provider can retain them.");
		});
	}
}
