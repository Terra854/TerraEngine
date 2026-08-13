// AudioFileFormatTests.cpp - pointer-free TEAF v1 validation.

#include "../TestHarness.h"
#include "File Formats/TerraEngineAudioFile.h"

namespace TerraEngine::Tests {
	void RegisterAudioFileFormatTests(TestRegistry& registry) {
		registry.Add("Audio.FileFormat.RoundTripsLittleEndianEnvelope", [](TestContext& context) {
			Audio::AudioFileMetadata metadata;
			metadata.assetId = TerraEngine::GenerateUUID(); metadata.encoding = Audio::AudioEncoding::Wave;
			metadata.channelCount = 1U; metadata.sampleRate = 48000U; metadata.frameCount = 4U;
			const std::vector<std::uint8_t> payload{'R','I','F','F',1U,2U,3U,4U};
			metadata.payloadChecksum = Audio::ComputeAudioChecksum(payload);
			std::vector<std::uint8_t> encoded;
			context.Expect(Audio::SerializeAudioFile(metadata, payload, encoded) == Audio::AudioFileStatus::Ok,
				"Valid metadata and retained bytes should serialize.");
			const auto parsed = Audio::ReadAudioFile(encoded);
			context.Expect(parsed.Succeeded() && parsed.metadata.assetId == metadata.assetId,
				"TEAF must round-trip without native pointers or host layout.");
		});

		registry.Add("Audio.FileFormat.RejectsChecksumAndVersionCorruption", [](TestContext& context) {
			Audio::AudioFileMetadata metadata{TerraEngine::GenerateUUID(), Audio::AudioEncoding::Wave, 1U, 44100U, 1U};
			std::vector<std::uint8_t> payload{1U, 2U}; metadata.payloadChecksum = Audio::ComputeAudioChecksum(payload);
			std::vector<std::uint8_t> encoded; (void)Audio::SerializeAudioFile(metadata, payload, encoded);
			encoded.back() ^= 0x7FU;
			context.Expect(Audio::ReadAudioFile(encoded).status == Audio::AudioFileStatus::PayloadChecksumMismatch,
				"Payload corruption must be diagnosed before provider registration.");
			encoded[4] = 2U;
			context.Expect(Audio::ReadAudioFile(encoded).status == Audio::AudioFileStatus::IncompatibleVersion,
				"Unknown major versions must fail closed.");
		});
	}
}
