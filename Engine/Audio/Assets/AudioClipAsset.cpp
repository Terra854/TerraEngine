// AudioClipAsset.cpp - immutable audio clip construction.

#include "AudioClipAsset.h"

#include <utility>

namespace TerraEngine::Audio
{
	std::shared_ptr<const AudioClipAsset> AudioClipAsset::Create(
		AudioFileMetadata metadata, std::vector<std::uint8_t> encodedPayload, std::uint64_t generation)
	{
		// A generation of zero is never published: it would make stale handles
		// indistinguishable from an uninitialized resource entry.
		if(generation == 0U || ValidateAudioFileMetadata(metadata) != AudioFileStatus::Ok
			|| encodedPayload.empty() || encodedPayload.size() > kAudioFileMaximumPayloadBytes
			|| ComputeAudioChecksum(encodedPayload) != metadata.payloadChecksum) return {};
		auto payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(encodedPayload));
		return std::shared_ptr<const AudioClipAsset>(new AudioClipAsset(metadata, std::move(payload), generation));
	}
}
