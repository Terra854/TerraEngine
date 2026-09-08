// SavedNavigationFormat.cpp - bounded deterministic .tenav encoding and decoding.

#include "Navigation/Persistence/SavedNavigationFormat.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <tuple>

namespace TerraEngine::Navigation {
	namespace {
		constexpr std::array<std::byte, 8U> kMagic{
			std::byte{'T'}, std::byte{'E'}, std::byte{'N'}, std::byte{'A'},
			std::byte{'V'}, std::byte{'0'}, std::byte{'0'}, std::byte{'1'}};
		constexpr std::size_t kHeaderBytes = 64U;
		constexpr std::size_t kIndexEntryBytes = 64U;
		constexpr std::size_t kChecksumOffset = 56U;
		constexpr std::size_t kMaximumBackendAbiBytes = 256U;

		[[nodiscard]] bool CheckedAdd(std::size_t first, std::size_t second, std::size_t& result) noexcept {
			if(second > std::numeric_limits<std::size_t>::max() - first) return false;
			result = first + second;
			return true;
		}

		[[nodiscard]] bool CheckedMultiply(std::size_t first, std::size_t second, std::size_t& result) noexcept {
			if(first != 0U && second > std::numeric_limits<std::size_t>::max() / first) return false;
			result = first * second;
			return true;
		}

		void WriteU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
			for(std::size_t index = 0U; index < 4U; ++index)
				bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
		}

		void WriteU64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
			for(std::size_t index = 0U; index < 8U; ++index)
				bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
		}

		[[nodiscard]] bool ReadU32(std::span<const std::byte> bytes, std::size_t offset, std::uint32_t& value) noexcept {
			if(offset > bytes.size() || bytes.size() - offset < 4U) return false;
			value = 0U;
			for(std::size_t index = 0U; index < 4U; ++index)
				value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
			return true;
		}

		[[nodiscard]] bool ReadU64(std::span<const std::byte> bytes, std::size_t offset, std::uint64_t& value) noexcept {
			if(offset > bytes.size() || bytes.size() - offset < 8U) return false;
			value = 0U;
			for(std::size_t index = 0U; index < 8U; ++index)
				value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
			return true;
		}

		[[nodiscard]] std::uint64_t HashBytes(std::span<const std::byte> bytes) noexcept {
			std::uint64_t hash = 14695981039346656037ULL;
			for(const std::byte value : bytes) {
				hash ^= std::to_integer<unsigned char>(value);
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		[[nodiscard]] bool LessChunk(const SavedNavigationChunk& left, const SavedNavigationChunk& right) noexcept {
			return std::tie(left.key.profileBuildKey, left.key.verticalBand, left.key.shardZ, left.key.shardX)
				< std::tie(right.key.profileBuildKey, right.key.verticalBand, right.key.shardZ, right.key.shardX);
		}
	}

	SavedNavigationWriteResult EncodeSavedNavigation(
		const SavedNavigationDescriptor& descriptor,
		std::span<const SavedNavigationChunk> chunks) {
		SavedNavigationWriteResult result{};
		if(descriptor.formatVersion != kSavedNavigationFormatVersion || descriptor.backendAbi.empty()
			|| descriptor.backendAbi.size() > kMaximumBackendAbiBytes || descriptor.sourceFingerprint == 0U
			|| descriptor.settingsFingerprint == 0U || chunks.size() > kMaximumSavedNavigationChunks) {
			result.reason = "SavedNavigationInvalidDescriptor";
			return result;
		}

		std::vector<SavedNavigationChunk> ordered(chunks.begin(), chunks.end());
		std::sort(ordered.begin(), ordered.end(), LessChunk);
		for(std::size_t index = 0U; index < ordered.size(); ++index) {
			if(ordered[index].generation == 0U || ordered[index].bytes.empty()) {
				result.reason = "SavedNavigationInvalidChunk";
				return result;
			}
			if(index > 0U && ordered[index - 1U].key == ordered[index].key) {
				result.reason = "SavedNavigationDuplicateChunk";
				return result;
			}
		}

		std::size_t indexBytes = 0U;
		std::size_t indexOffset = 0U;
		std::size_t payloadOffset = 0U;
		std::size_t totalBytes = 0U;
		if(!CheckedMultiply(ordered.size(), kIndexEntryBytes, indexBytes)
			|| !CheckedAdd(kHeaderBytes, descriptor.backendAbi.size(), indexOffset)
			|| !CheckedAdd(indexOffset, indexBytes, payloadOffset)) {
			result.reason = "SavedNavigationSizeOverflow";
			return result;
		}
		totalBytes = payloadOffset;
		for(const SavedNavigationChunk& chunk : ordered) {
			if(!CheckedAdd(totalBytes, chunk.bytes.size(), totalBytes)
				|| totalBytes > kMaximumSavedNavigationBytes) {
				result.reason = "SavedNavigationCapacityExceeded";
				return result;
			}
		}

		result.bytes.assign(totalBytes, std::byte{0});
		std::copy(kMagic.begin(), kMagic.end(), result.bytes.begin());
		WriteU32(result.bytes, 8U, descriptor.formatVersion);
		WriteU32(result.bytes, 12U, static_cast<std::uint32_t>(kHeaderBytes));
		WriteU32(result.bytes, 16U, static_cast<std::uint32_t>(descriptor.backendAbi.size()));
		WriteU32(result.bytes, 20U, static_cast<std::uint32_t>(ordered.size()));
		WriteU64(result.bytes, 24U, descriptor.sourceFingerprint);
		WriteU64(result.bytes, 32U, descriptor.settingsFingerprint);
		WriteU64(result.bytes, 40U, indexOffset);
		WriteU64(result.bytes, 48U, payloadOffset);
		std::memcpy(result.bytes.data() + kHeaderBytes, descriptor.backendAbi.data(), descriptor.backendAbi.size());

		std::size_t payloadCursor = payloadOffset;
		for(std::size_t index = 0U; index < ordered.size(); ++index) {
			const SavedNavigationChunk& chunk = ordered[index];
			const std::size_t entry = indexOffset + index * kIndexEntryBytes;
			WriteU64(result.bytes, entry, static_cast<std::uint64_t>(chunk.key.shardX));
			WriteU64(result.bytes, entry + 8U, static_cast<std::uint64_t>(chunk.key.shardZ));
			WriteU32(result.bytes, entry + 16U, static_cast<std::uint32_t>(chunk.key.verticalBand));
			WriteU64(result.bytes, entry + 24U, chunk.key.profileBuildKey);
			WriteU64(result.bytes, entry + 32U, chunk.generation);
			WriteU64(result.bytes, entry + 40U, payloadCursor);
			WriteU64(result.bytes, entry + 48U, chunk.bytes.size());
			WriteU64(result.bytes, entry + 56U, HashBytes(chunk.bytes));
			std::copy(chunk.bytes.begin(), chunk.bytes.end(), result.bytes.begin() + static_cast<std::ptrdiff_t>(payloadCursor));
			payloadCursor += chunk.bytes.size();
		}
		result.checksum = HashBytes(result.bytes);
		WriteU64(result.bytes, kChecksumOffset, result.checksum);
		result.accepted = true;
		return result;
	}

	SavedNavigationReadResult DecodeSavedNavigation(
		std::span<const std::byte> bytes,
		const SavedNavigationDescriptor& expected) {
		SavedNavigationReadResult result{};
		if(bytes.size() < kHeaderBytes || bytes.size() > kMaximumSavedNavigationBytes
			|| !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
			result.reason = "SavedNavigationInvalidHeader";
			return result;
		}
		std::uint32_t version = 0U, headerBytes = 0U, abiLength = 0U, chunkCount = 0U;
		std::uint64_t sourceFingerprint = 0U, settingsFingerprint = 0U, indexOffset = 0U,
			payloadOffset = 0U, checksum = 0U;
		if(!ReadU32(bytes, 8U, version) || !ReadU32(bytes, 12U, headerBytes)
			|| !ReadU32(bytes, 16U, abiLength) || !ReadU32(bytes, 20U, chunkCount)
			|| !ReadU64(bytes, 24U, sourceFingerprint) || !ReadU64(bytes, 32U, settingsFingerprint)
			|| !ReadU64(bytes, 40U, indexOffset) || !ReadU64(bytes, 48U, payloadOffset)
			|| !ReadU64(bytes, kChecksumOffset, checksum) || headerBytes != kHeaderBytes
			|| abiLength > kMaximumBackendAbiBytes || chunkCount > kMaximumSavedNavigationChunks) {
			result.reason = "SavedNavigationMalformedHeader";
			return result;
		}
		std::vector<std::byte> checksumInput(bytes.begin(), bytes.end());
		WriteU64(checksumInput, kChecksumOffset, 0U);
		if(HashBytes(checksumInput) != checksum) {
			result.reason = "SavedNavigationChecksumMismatch";
			return result;
		}
		std::size_t expectedIndexOffset = 0U, indexBytes = 0U, expectedPayloadOffset = 0U;
		if(!CheckedAdd(kHeaderBytes, abiLength, expectedIndexOffset)
			|| !CheckedMultiply(chunkCount, kIndexEntryBytes, indexBytes)
			|| !CheckedAdd(expectedIndexOffset, indexBytes, expectedPayloadOffset)
			|| indexOffset != expectedIndexOffset || payloadOffset != expectedPayloadOffset
			|| payloadOffset > bytes.size()) {
			result.reason = "SavedNavigationIndexOutOfBounds";
			return result;
		}

		result.descriptor = {version,
			std::string(reinterpret_cast<const char*>(bytes.data() + kHeaderBytes), abiLength),
			sourceFingerprint, settingsFingerprint};
		if(result.descriptor != expected) {
			result.reason = "SavedNavigationCompatibilityMismatch";
			return result;
		}
		std::size_t nextPayloadOffset = static_cast<std::size_t>(payloadOffset);
		for(std::size_t index = 0U; index < chunkCount; ++index) {
			const std::size_t entry = static_cast<std::size_t>(indexOffset) + index * kIndexEntryBytes;
			std::uint64_t shardX = 0U, shardZ = 0U, profile = 0U, generation = 0U,
				chunkOffset = 0U, chunkLength = 0U, chunkChecksum = 0U;
			std::uint32_t verticalBand = 0U;
			if(!ReadU64(bytes, entry, shardX) || !ReadU64(bytes, entry + 8U, shardZ)
				|| !ReadU32(bytes, entry + 16U, verticalBand) || !ReadU64(bytes, entry + 24U, profile)
				|| !ReadU64(bytes, entry + 32U, generation) || !ReadU64(bytes, entry + 40U, chunkOffset)
				|| !ReadU64(bytes, entry + 48U, chunkLength) || !ReadU64(bytes, entry + 56U, chunkChecksum)
				|| generation == 0U || chunkOffset != nextPayloadOffset || chunkLength == 0U
				|| chunkLength > bytes.size() - static_cast<std::size_t>(chunkOffset)) {
				result.reason = "SavedNavigationChunkOutOfBounds";
				result.chunks.clear();
				return result;
			}
			const auto payload = bytes.subspan(static_cast<std::size_t>(chunkOffset), static_cast<std::size_t>(chunkLength));
			if(HashBytes(payload) != chunkChecksum) {
				result.reason = "SavedNavigationChunkChecksumMismatch";
				result.chunks.clear();
				return result;
			}
			SavedNavigationChunk chunk{};
			chunk.key = {static_cast<std::int64_t>(shardX), static_cast<std::int64_t>(shardZ),
				static_cast<std::int32_t>(verticalBand), profile};
			chunk.generation = generation;
			chunk.bytes.assign(payload.begin(), payload.end());
			if(!result.chunks.empty() && !LessChunk(result.chunks.back(), chunk)) {
				result.reason = "SavedNavigationNonCanonicalIndex";
				result.chunks.clear();
				return result;
			}
			result.chunks.push_back(std::move(chunk));
			nextPayloadOffset += static_cast<std::size_t>(chunkLength);
		}
		if(nextPayloadOffset != bytes.size()) {
			result.reason = "SavedNavigationTrailingBytes";
			result.chunks.clear();
			return result;
		}
		result.accepted = true;
		result.matchesFreshRuntimeGeneration = true;
		return result;
	}
}
