// AudioClipCompiler.cpp - bounded source probing and staging publication.

#include "AudioClipCompiler.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace TerraEngine::AssetPipeline
{
	namespace
	{
		[[nodiscard]] bool Cancelled(const std::atomic_bool* value)
		{
			return value != nullptr && value->load(std::memory_order_relaxed);
		}

		[[nodiscard]] std::uint16_t ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset)
		{
			return static_cast<std::uint16_t>(bytes[offset]) | (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
		}

		[[nodiscard]] std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset)
		{
			return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
				| (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
		}

		[[nodiscard]] AudioClipCompileStatus ProbeWave(std::span<const std::uint8_t> source,
			TerraEngine::Audio::AudioFileMetadata& metadata)
		{
			if(source.size() < 12U) return AudioClipCompileStatus::TruncatedHeader;
			if(!std::equal(source.begin(), source.begin() + 4U, "RIFF") || !std::equal(source.begin() + 8U, source.begin() + 12U, "WAVE"))
				return AudioClipCompileStatus::SignatureMismatch;
			std::uint16_t channels = 0U;
			std::uint32_t sampleRate = 0U, byteRate = 0U;
			std::uint16_t blockAlign = 0U, bitsPerSample = 0U, formatTag = 0U;
			std::uint64_t dataBytes = 0U;
			bool formatFound = false, dataFound = false;
			std::size_t offset = 12U;
			while(offset <= source.size() && source.size() - offset >= 8U) {
				const std::uint32_t chunkSize = ReadU32(source, offset + 4U);
				const std::size_t payloadOffset = offset + 8U;
				if(chunkSize > source.size() - payloadOffset) return AudioClipCompileStatus::TruncatedHeader;
				if(std::equal(source.begin() + static_cast<std::ptrdiff_t>(offset), source.begin() + static_cast<std::ptrdiff_t>(offset + 4U), "fmt ")) {
					if(chunkSize < 16U) return AudioClipCompileStatus::InvalidMetadata;
					formatTag = ReadU16(source, payloadOffset);
					channels = ReadU16(source, payloadOffset + 2U);
					sampleRate = ReadU32(source, payloadOffset + 4U);
					byteRate = ReadU32(source, payloadOffset + 8U);
					blockAlign = ReadU16(source, payloadOffset + 12U);
					bitsPerSample = ReadU16(source, payloadOffset + 14U);
					formatFound = true;
				} else if(std::equal(source.begin() + static_cast<std::ptrdiff_t>(offset), source.begin() + static_cast<std::ptrdiff_t>(offset + 4U), "data")) {
					dataBytes = chunkSize;
					dataFound = true;
				}
				const std::uint64_t next = static_cast<std::uint64_t>(payloadOffset) + chunkSize + (chunkSize & 1U);
				if(next > source.size()) return AudioClipCompileStatus::TruncatedHeader;
				offset = static_cast<std::size_t>(next);
			}
			if(!formatFound || !dataFound) return AudioClipCompileStatus::InvalidMetadata;
			// Runtime decoders accept PCM or IEEE float WAV. Reject malformed block
			// layout before deriving a frame count, rather than trusting byteRate.
			if((formatTag != 1U && formatTag != 3U) || channels == 0U || blockAlign == 0U || bitsPerSample == 0U
				|| byteRate == 0U || dataBytes % blockAlign != 0U) return AudioClipCompileStatus::InvalidMetadata;
			metadata.encoding = TerraEngine::Audio::AudioEncoding::Wave;
			metadata.channelCount = channels;
			metadata.sampleRate = sampleRate;
			metadata.frameCount = dataBytes / blockAlign;
			if(metadata.frameCount == 0U) return AudioClipCompileStatus::ZeroFrames;
			return TerraEngine::Audio::ValidateAudioFileMetadata(metadata) == TerraEngine::Audio::AudioFileStatus::Ok
				? AudioClipCompileStatus::Ok : AudioClipCompileStatus::InvalidMetadata;
		}

		[[nodiscard]] AudioClipCompileStatus ProbeOggVorbis(std::span<const std::uint8_t> source,
			TerraEngine::Audio::AudioFileMetadata& metadata)
		{
			// The Vorbis identification packet establishes all durable metadata. The
			// final Ogg page's granule position proves there is at least one frame.
			if(source.size() < 27U || !std::equal(source.begin(), source.begin() + 4U, "OggS")) return AudioClipCompileStatus::SignatureMismatch;
			std::size_t offset = 0U;
			bool identificationFound = false;
			std::uint64_t terminalGranule = 0U;
			while(offset <= source.size() && source.size() - offset >= 27U) {
				if(!std::equal(source.begin() + static_cast<std::ptrdiff_t>(offset), source.begin() + static_cast<std::ptrdiff_t>(offset + 4U), "OggS")) return AudioClipCompileStatus::DecodeProbeFailed;
				const std::uint8_t segmentCount = source[offset + 26U];
				if(source.size() - offset < 27U + segmentCount) return AudioClipCompileStatus::TruncatedHeader;
				std::size_t payloadSize = 0U;
				for(std::uint8_t index = 0U; index < segmentCount; ++index) payloadSize += source[offset + 27U + index];
				const std::size_t payloadOffset = offset + 27U + segmentCount;
				if(payloadSize > source.size() - payloadOffset) return AudioClipCompileStatus::TruncatedHeader;
				const std::uint64_t granule = static_cast<std::uint64_t>(ReadU32(source, offset + 6U))
					| (static_cast<std::uint64_t>(ReadU32(source, offset + 10U)) << 32U);
				if(granule != std::numeric_limits<std::uint64_t>::max()) terminalGranule = std::max(terminalGranule, granule);
				if(!identificationFound && payloadSize >= 30U && source[payloadOffset] == 1U
					&& std::equal(source.begin() + static_cast<std::ptrdiff_t>(payloadOffset + 1U), source.begin() + static_cast<std::ptrdiff_t>(payloadOffset + 7U), "vorbis")) {
					metadata.encoding = TerraEngine::Audio::AudioEncoding::OggVorbis;
					metadata.channelCount = source[payloadOffset + 11U];
					metadata.sampleRate = ReadU32(source, payloadOffset + 12U);
					identificationFound = true;
				}
				offset = payloadOffset + payloadSize;
			}
			if(!identificationFound) return AudioClipCompileStatus::UnsupportedEncoding;
			metadata.frameCount = terminalGranule;
			if(metadata.frameCount == 0U) return AudioClipCompileStatus::ZeroFrames;
			return TerraEngine::Audio::ValidateAudioFileMetadata(metadata) == TerraEngine::Audio::AudioFileStatus::Ok
				? AudioClipCompileStatus::Ok : AudioClipCompileStatus::InvalidMetadata;
		}

		[[nodiscard]] bool Publish(const std::filesystem::path& staged, const std::filesystem::path& destination)
		{
			std::error_code error;
			std::filesystem::create_directories(destination.parent_path(), error);
			if(error) return false;
			const std::filesystem::path backup = destination.string() + ".previous";
			std::filesystem::remove(backup, error);
			error.clear();
			const bool hadPrevious = std::filesystem::exists(destination, error) && !error;
			if(hadPrevious) std::filesystem::rename(destination, backup, error);
			if(error) return false;
			std::filesystem::rename(staged, destination, error);
			if(error && hadPrevious) {
				std::error_code ignored;
				std::filesystem::rename(backup, destination, ignored);
			}
			if(!error && hadPrevious) std::filesystem::remove(backup, error);
			return !error;
		}
	}

	AudioClipCompileReport CompileAudioClip(const AudioClipCompileInput& input, const std::atomic_bool* cancellationRequested)
	{
		AudioClipCompileReport report;
		report.metadata.assetId = input.assetId;
		if(Cancelled(cancellationRequested)) { report.status = AudioClipCompileStatus::Cancelled; return report; }
		if(input.assetId == TerraEngine::EMPTY_UUID || input.sourcePath.empty() || input.cookedPath.extension() != ".teaudio") {
			report.status = AudioClipCompileStatus::InvalidMetadata;
			report.diagnostic = "Audio cook requires a stable UUID and a .teaudio destination.";
			return report;
		}
		std::ifstream stream(input.sourcePath, std::ios::binary | std::ios::ate);
		if(!stream) { report.status = AudioClipCompileStatus::MissingSource; return report; }
		const std::streamoff length = stream.tellg();
		if(length <= 0 || static_cast<std::uint64_t>(length) > std::min(input.maximumPayloadBytes, TerraEngine::Audio::kAudioFileMaximumPayloadBytes)) {
			report.status = length == 0 ? AudioClipCompileStatus::ZeroFrames : AudioClipCompileStatus::RangeOverflow;
			return report;
		}
		stream.seekg(0, std::ios::beg);
		std::vector<std::uint8_t> source(static_cast<std::size_t>(length));
		stream.read(reinterpret_cast<char*>(source.data()), length);
		if(!stream) { report.status = AudioClipCompileStatus::IoError; return report; }
		report.sourceBytes = source.size();
		if(Cancelled(cancellationRequested)) { report.status = AudioClipCompileStatus::Cancelled; return report; }
		if(source.size() < 4U) { report.status = AudioClipCompileStatus::TruncatedHeader; return report; }
		if(std::equal(source.begin(), source.begin() + 4U, "RIFF")) report.status = ProbeWave(source, report.metadata);
		else if(std::equal(source.begin(), source.begin() + 4U, "OggS")) report.status = ProbeOggVorbis(source, report.metadata);
		else report.status = AudioClipCompileStatus::SignatureMismatch;
		if(!report.Succeeded()) return report;
		report.metadata.sourceChecksum = TerraEngine::Audio::ComputeAudioChecksum(source);
		report.metadata.payloadChecksum = report.metadata.sourceChecksum;
		std::vector<std::uint8_t> envelope;
		if(TerraEngine::Audio::SerializeAudioFile(report.metadata, source, envelope) != TerraEngine::Audio::AudioFileStatus::Ok) {
			report.status = AudioClipCompileStatus::InvalidMetadata;
			return report;
		}
		const std::filesystem::path staged = input.cookedPath.string() + ".staged";
		std::error_code error;
		std::filesystem::remove(staged, error);
		{
			std::ofstream output(staged, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(envelope.data()), static_cast<std::streamsize>(envelope.size()));
			if(!output) { report.status = AudioClipCompileStatus::IoError; std::filesystem::remove(staged, error); return report; }
		}
		const TerraEngine::Audio::AudioFileReadResult checked = TerraEngine::Audio::ReadAudioFile(envelope);
		if(!checked.Succeeded() || checked.metadata.assetId != input.assetId) {
			report.status = AudioClipCompileStatus::DecodeProbeFailed;
			std::filesystem::remove(staged, error);
			return report;
		}
		if(Cancelled(cancellationRequested)) { report.status = AudioClipCompileStatus::Cancelled; std::filesystem::remove(staged, error); return report; }
		if(!Publish(staged, input.cookedPath)) { report.status = AudioClipCompileStatus::PublishFailed; std::filesystem::remove(staged, error); return report; }
		report.diagnostic = "Validated and published provider-neutral TEAF audio clip.";
		return report;
	}

	std::string_view ToString(AudioClipCompileStatus status) noexcept
	{
		switch(status) {
		case AudioClipCompileStatus::Ok: return "Ok"; case AudioClipCompileStatus::MissingSource: return "MissingSource";
		case AudioClipCompileStatus::UnsupportedEncoding: return "UnsupportedEncoding"; case AudioClipCompileStatus::SignatureMismatch: return "SignatureMismatch";
		case AudioClipCompileStatus::TruncatedHeader: return "TruncatedHeader"; case AudioClipCompileStatus::InvalidMetadata: return "InvalidMetadata";
		case AudioClipCompileStatus::ZeroFrames: return "ZeroFrames"; case AudioClipCompileStatus::RangeOverflow: return "RangeOverflow";
		case AudioClipCompileStatus::DecodeProbeFailed: return "DecodeProbeFailed"; case AudioClipCompileStatus::Cancelled: return "Cancelled";
		case AudioClipCompileStatus::IoError: return "IoError"; case AudioClipCompileStatus::PublishFailed: return "PublishFailed";
		}
		return "Unknown";
	}
}
