// AudioClipAsset.h - immutable runtime representation of a validated TEAF clip.

#pragma once

#include "Core/Export.h"
#include "File Formats/TerraEngineAudioFile.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Audio
{
	class TERRAENGINE_API AudioClipAsset final
	{
	public:
		[[nodiscard]] static std::shared_ptr<const AudioClipAsset> Create(
			AudioFileMetadata metadata, std::vector<std::uint8_t> encodedPayload, std::uint64_t generation = 1U);

		[[nodiscard]] const AudioFileMetadata& Metadata() const noexcept { return m_metadata; }
		[[nodiscard]] const TerraEngine::UUID& AssetId() const noexcept { return m_metadata.assetId; }
		[[nodiscard]] AudioEncoding Encoding() const noexcept { return m_metadata.encoding; }
		[[nodiscard]] std::uint32_t Channels() const noexcept { return m_metadata.channelCount; }
		[[nodiscard]] std::uint32_t SampleRate() const noexcept { return m_metadata.sampleRate; }
		[[nodiscard]] std::uint64_t FrameCount() const noexcept { return m_metadata.frameCount; }
		[[nodiscard]] std::uint64_t Generation() const noexcept { return m_generation; }
		[[nodiscard]] std::uint64_t EncodedBytes() const noexcept { return m_payload->size(); }
		[[nodiscard]] std::span<const std::uint8_t> EncodedPayload() const noexcept { return *m_payload; }
		[[nodiscard]] const std::shared_ptr<const std::vector<std::uint8_t>>& RetainedPayload() const noexcept { return m_payload; }

	private:
		AudioClipAsset(AudioFileMetadata metadata, std::shared_ptr<const std::vector<std::uint8_t>> payload,
			std::uint64_t generation) : m_metadata(metadata), m_payload(std::move(payload)), m_generation(generation) {}

		AudioFileMetadata m_metadata;
		std::shared_ptr<const std::vector<std::uint8_t>> m_payload;
		std::uint64_t m_generation = 1U;
	};
}
