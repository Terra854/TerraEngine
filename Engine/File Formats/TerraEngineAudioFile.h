// TerraEngineAudioFile.h - provider-neutral TEAF audio envelope.
//
// This format retains validated encoded source bytes. Runtime providers receive
// the immutable payload only after this reader has checked every field and range.

#pragma once

#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Audio
{
	inline constexpr std::array<std::uint8_t, 4U> kAudioFileMagic{'T', 'E', 'A', 'F'};
	inline constexpr std::uint16_t kAudioFileMajorVersion = 1U;
	inline constexpr std::uint16_t kAudioFileMinorVersion = 0U;
	inline constexpr std::uint32_t kAudioFileEndianMarker = 0x01020304U;
	inline constexpr std::size_t kAudioFileHeaderSize = 96U;
	inline constexpr std::uint64_t kAudioFileMaximumPayloadBytes = 512ULL * 1024ULL * 1024ULL;

	// Values are persisted in TEAF and deliberately never reuse a removed value.
	enum class AudioEncoding : std::uint32_t
	{
		Wave = 1U,
		OggVorbis = 2U
	};

	enum class AudioFileStatus : std::uint8_t
	{
		Ok,
		SignatureMismatch,
		TruncatedHeader,
		IncompatibleVersion,
		InvalidMetadata,
		ZeroFrames,
		RangeOverflow,
		PayloadChecksumMismatch,
		IdentityMismatch,
		UnsupportedEncoding
	};

	struct AudioFileMetadata
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		AudioEncoding encoding = AudioEncoding::Wave;
		std::uint32_t channelCount = 0U;
		std::uint32_t sampleRate = 0U;
		std::uint64_t frameCount = 0U;
		std::uint64_t sourceChecksum = 0U;
		std::uint64_t payloadChecksum = 0U;
	};

	struct AudioFileReadResult
	{
		AudioFileStatus status = AudioFileStatus::TruncatedHeader;
		AudioFileMetadata metadata;
		std::span<const std::uint8_t> payload;

		[[nodiscard]] bool Succeeded() const noexcept { return status == AudioFileStatus::Ok; }
	};

	[[nodiscard]] inline std::uint64_t ComputeAudioChecksum(std::span<const std::uint8_t> bytes) noexcept
	{
		// FNV-1a is a stable integrity fingerprint for this project-owned envelope.
		std::uint64_t hash = 14695981039346656037ULL;
		for(const std::uint8_t byte : bytes) {
			hash ^= byte;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	namespace Detail
	{
		inline void WriteU16(std::vector<std::uint8_t>& output, std::uint16_t value)
		{
			output.push_back(static_cast<std::uint8_t>(value));
			output.push_back(static_cast<std::uint8_t>(value >> 8U));
		}

		inline void WriteU32(std::vector<std::uint8_t>& output, std::uint32_t value)
		{
			for(std::uint32_t shift = 0U; shift < 32U; shift += 8U)
				output.push_back(static_cast<std::uint8_t>(value >> shift));
		}

		inline void WriteU64(std::vector<std::uint8_t>& output, std::uint64_t value)
		{
			for(std::uint32_t shift = 0U; shift < 64U; shift += 8U)
				output.push_back(static_cast<std::uint8_t>(value >> shift));
		}

		[[nodiscard]] inline bool ReadU16(std::span<const std::uint8_t> input, std::size_t offset, std::uint16_t& value)
		{
			if(offset > input.size() || input.size() - offset < 2U) return false;
			value = static_cast<std::uint16_t>(input[offset])
				| (static_cast<std::uint16_t>(input[offset + 1U]) << 8U);
			return true;
		}

		[[nodiscard]] inline bool ReadU32(std::span<const std::uint8_t> input, std::size_t offset, std::uint32_t& value)
		{
			if(offset > input.size() || input.size() - offset < 4U) return false;
			value = 0U;
			for(std::uint32_t index = 0U; index < 4U; ++index)
				value |= static_cast<std::uint32_t>(input[offset + index]) << (index * 8U);
			return true;
		}

		[[nodiscard]] inline bool ReadU64(std::span<const std::uint8_t> input, std::size_t offset, std::uint64_t& value)
		{
			if(offset > input.size() || input.size() - offset < 8U) return false;
			value = 0U;
			for(std::uint32_t index = 0U; index < 8U; ++index)
				value |= static_cast<std::uint64_t>(input[offset + index]) << (index * 8U);
			return true;
		}
	}

	[[nodiscard]] inline AudioFileStatus ValidateAudioFileMetadata(const AudioFileMetadata& metadata) noexcept
	{
		if(metadata.assetId == TerraEngine::EMPTY_UUID) return AudioFileStatus::IdentityMismatch;
		if(metadata.encoding != AudioEncoding::Wave && metadata.encoding != AudioEncoding::OggVorbis)
			return AudioFileStatus::UnsupportedEncoding;
		if(metadata.channelCount == 0U || metadata.channelCount > 8U
			|| metadata.sampleRate < 8000U || metadata.sampleRate > 192000U)
			return AudioFileStatus::InvalidMetadata;
		if(metadata.frameCount == 0U) return AudioFileStatus::ZeroFrames;
		return AudioFileStatus::Ok;
	}

	// Serialize avoids native structures, padding, pointers, and host endian layout.
	[[nodiscard]] inline AudioFileStatus SerializeAudioFile(const AudioFileMetadata& metadata,
		std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& output)
	{
		if(const AudioFileStatus status = ValidateAudioFileMetadata(metadata); status != AudioFileStatus::Ok)
			return status;
		if(payload.empty()) return AudioFileStatus::InvalidMetadata;
		if(payload.size() > kAudioFileMaximumPayloadBytes) return AudioFileStatus::RangeOverflow;
		const std::uint64_t payloadOffset = kAudioFileHeaderSize;
		const std::uint64_t payloadSize = payload.size();
		if(payloadOffset > std::numeric_limits<std::uint64_t>::max() - payloadSize) return AudioFileStatus::RangeOverflow;

		std::vector<std::uint8_t> staged;
		staged.reserve(kAudioFileHeaderSize + payload.size());
		staged.insert(staged.end(), kAudioFileMagic.begin(), kAudioFileMagic.end());
		Detail::WriteU16(staged, kAudioFileMajorVersion);
		Detail::WriteU16(staged, kAudioFileMinorVersion);
		Detail::WriteU32(staged, static_cast<std::uint32_t>(kAudioFileHeaderSize));
		Detail::WriteU32(staged, kAudioFileEndianMarker);
		staged.insert(staged.end(), metadata.assetId.begin(), metadata.assetId.end());
		Detail::WriteU32(staged, static_cast<std::uint32_t>(metadata.encoding));
		Detail::WriteU32(staged, metadata.channelCount);
		Detail::WriteU32(staged, metadata.sampleRate);
		Detail::WriteU32(staged, 0U); // Reserved for append-only v1-compatible flags.
		Detail::WriteU64(staged, metadata.frameCount);
		Detail::WriteU64(staged, metadata.sourceChecksum);
		Detail::WriteU64(staged, payloadOffset);
		Detail::WriteU64(staged, payloadSize);
		Detail::WriteU64(staged, ComputeAudioChecksum(payload));
		Detail::WriteU64(staged, 0U); // Reserved field keeps v1's checked header fixed-width.
		if(staged.size() != kAudioFileHeaderSize) return AudioFileStatus::RangeOverflow;
		staged.insert(staged.end(), payload.begin(), payload.end());
		output = std::move(staged);
		return AudioFileStatus::Ok;
	}

	[[nodiscard]] inline AudioFileReadResult ReadAudioFile(std::span<const std::uint8_t> input)
	{
		AudioFileReadResult result;
		if(input.size() < kAudioFileHeaderSize) return result;
		if(!std::equal(kAudioFileMagic.begin(), kAudioFileMagic.end(), input.begin())) {
			result.status = AudioFileStatus::SignatureMismatch;
			return result;
		}
		std::uint16_t major = 0U, minor = 0U;
		std::uint32_t headerSize = 0U, endian = 0U, encoding = 0U;
		std::uint64_t payloadOffset = 0U, payloadSize = 0U;
		if(!Detail::ReadU16(input, 4U, major) || !Detail::ReadU16(input, 6U, minor)
			|| !Detail::ReadU32(input, 8U, headerSize) || !Detail::ReadU32(input, 12U, endian)
			|| !Detail::ReadU32(input, 32U, encoding)
			|| !Detail::ReadU32(input, 36U, result.metadata.channelCount)
			|| !Detail::ReadU32(input, 40U, result.metadata.sampleRate)
			|| !Detail::ReadU64(input, 48U, result.metadata.frameCount)
			|| !Detail::ReadU64(input, 56U, result.metadata.sourceChecksum)
			|| !Detail::ReadU64(input, 64U, payloadOffset)
			|| !Detail::ReadU64(input, 72U, payloadSize)
			|| !Detail::ReadU64(input, 80U, result.metadata.payloadChecksum)) return result;
		if(major != kAudioFileMajorVersion || minor > kAudioFileMinorVersion) {
			result.status = AudioFileStatus::IncompatibleVersion;
			return result;
		}
		if(headerSize != kAudioFileHeaderSize || endian != kAudioFileEndianMarker) {
			result.status = AudioFileStatus::InvalidMetadata;
			return result;
		}
		std::memcpy(result.metadata.assetId.data(), input.data() + 16U, result.metadata.assetId.size());
		result.metadata.encoding = static_cast<AudioEncoding>(encoding);
		if(const AudioFileStatus status = ValidateAudioFileMetadata(result.metadata); status != AudioFileStatus::Ok) {
			result.status = status;
			return result;
		}
		if(payloadSize == 0U || payloadSize > kAudioFileMaximumPayloadBytes || payloadOffset < headerSize
			|| payloadOffset > input.size() || payloadSize > input.size() - payloadOffset) {
			result.status = AudioFileStatus::RangeOverflow;
			return result;
		}
		result.payload = input.subspan(static_cast<std::size_t>(payloadOffset), static_cast<std::size_t>(payloadSize));
		if(ComputeAudioChecksum(result.payload) != result.metadata.payloadChecksum) {
			result.status = AudioFileStatus::PayloadChecksumMismatch;
			result.payload = {};
			return result;
		}
		result.status = AudioFileStatus::Ok;
		return result;
	}
}
