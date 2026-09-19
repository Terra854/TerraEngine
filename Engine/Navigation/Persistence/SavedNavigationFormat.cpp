// SavedNavigationFormat.cpp - bounded deterministic .tenav encoding and decoding.

#include "Navigation/Persistence/SavedNavigationFormat.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>

namespace TerraEngine::Navigation {
	namespace {
		constexpr std::array<std::byte, 8U> kMagic{
			std::byte{'T'}, std::byte{'E'}, std::byte{'N'}, std::byte{'A'},
			std::byte{'V'}, std::byte{'0'}, std::byte{'0'}, std::byte{'2'}};
		constexpr std::array<std::byte, 8U> kVersionOneMagic{
			std::byte{'T'}, std::byte{'E'}, std::byte{'N'}, std::byte{'A'},
			std::byte{'V'}, std::byte{'0'}, std::byte{'0'}, std::byte{'1'}};
		constexpr std::size_t kHeaderBytes = 64U;
		constexpr std::size_t kIndexEntryBytes = 64U;
		constexpr std::size_t kChecksumOffset = 56U;
		constexpr std::size_t kMaximumBackendAbiBytes = 256U;
		constexpr std::size_t kChunkMetadataBytes = 40U;
		constexpr std::uint32_t kEmptyTileFlag = 1U << 0U;
		constexpr std::uint32_t kSupportCompleteFlag = 1U << 1U;
		constexpr std::uint32_t kClearanceCompleteFlag = 1U << 2U;
		constexpr std::uint32_t kKnownTileFlags = kEmptyTileFlag | kSupportCompleteFlag | kClearanceCompleteFlag;

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

		void WriteF32(std::vector<std::byte>& bytes, std::size_t offset, float value) {
			WriteU32(bytes, offset, std::bit_cast<std::uint32_t>(value));
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

		[[nodiscard]] bool ReadF32(std::span<const std::byte> bytes, std::size_t offset, float& value) noexcept {
			std::uint32_t encoded = 0U;
			if(!ReadU32(bytes, offset, encoded)) return false;
			value = std::bit_cast<float>(encoded);
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

		[[nodiscard]] bool IsValidChunk(const SavedNavigationChunk& chunk) noexcept {
			// A cache cannot stand in for fresh coverage until it proves both support
			// and clearance. An empty tile is authoritative only after that work.
			return chunk.generation != 0U
				&& !chunk.sourceFingerprints.empty()
				&& chunk.sourceFingerprints.size() <= kMaximumSavedNavigationSourceFingerprints
				&& std::is_sorted(chunk.sourceFingerprints.begin(), chunk.sourceFingerprints.end())
				&& std::isfinite(chunk.bounds.minimum.x) && std::isfinite(chunk.bounds.minimum.y)
				&& std::isfinite(chunk.bounds.minimum.z) && std::isfinite(chunk.bounds.maximum.x)
				&& std::isfinite(chunk.bounds.maximum.y) && std::isfinite(chunk.bounds.maximum.z)
				&& chunk.bounds.minimum.x <= chunk.bounds.maximum.x
				&& chunk.bounds.minimum.y <= chunk.bounds.maximum.y
				&& chunk.bounds.minimum.z <= chunk.bounds.maximum.z
				&& chunk.supportComplete && chunk.clearanceComplete
				&& (chunk.empty ? chunk.bytes.empty() : !chunk.bytes.empty())
				&& chunk.checksum == HashBytes(chunk.bytes);
		}

		[[nodiscard]] bool EncodeChunkPayload(const SavedNavigationChunk& chunk, std::vector<std::byte>& payload) {
			if(!IsValidChunk(chunk)) return false;
			std::size_t fingerprintBytes = 0U;
			std::size_t payloadBytes = 0U;
			if(!CheckedMultiply(chunk.sourceFingerprints.size(), sizeof(std::uint64_t), fingerprintBytes)
				|| !CheckedAdd(kChunkMetadataBytes, fingerprintBytes, payloadBytes)
				|| !CheckedAdd(payloadBytes, chunk.bytes.size(), payloadBytes)
				|| payloadBytes > kMaximumSavedNavigationBytes) return false;
			payload.assign(payloadBytes, std::byte{0});
			std::uint32_t flags = kSupportCompleteFlag | kClearanceCompleteFlag;
			if(chunk.empty) flags |= kEmptyTileFlag;
			WriteU32(payload, 0U, flags);
			WriteU32(payload, 4U, static_cast<std::uint32_t>(chunk.sourceFingerprints.size()));
			WriteU64(payload, 8U, chunk.checksum);
			WriteF32(payload, 16U, chunk.bounds.minimum.x);
			WriteF32(payload, 20U, chunk.bounds.minimum.y);
			WriteF32(payload, 24U, chunk.bounds.minimum.z);
			WriteF32(payload, 28U, chunk.bounds.maximum.x);
			WriteF32(payload, 32U, chunk.bounds.maximum.y);
			WriteF32(payload, 36U, chunk.bounds.maximum.z);
			std::size_t cursor = kChunkMetadataBytes;
			for(const std::uint64_t fingerprint : chunk.sourceFingerprints) {
				WriteU64(payload, cursor, fingerprint);
				cursor += sizeof(fingerprint);
			}
			std::copy(chunk.bytes.begin(), chunk.bytes.end(), payload.begin() + static_cast<std::ptrdiff_t>(cursor));
			return true;
		}

		[[nodiscard]] bool DecodeChunkPayload(
			std::span<const std::byte> payload, SavedNavigationChunk& chunk, std::string& reason) {
			std::uint32_t flags = 0U;
			std::uint32_t fingerprintCount = 0U;
			std::uint64_t checksum = 0U;
			if(payload.size() < kChunkMetadataBytes || !ReadU32(payload, 0U, flags)
				|| !ReadU32(payload, 4U, fingerprintCount) || !ReadU64(payload, 8U, checksum)
				|| !ReadF32(payload, 16U, chunk.bounds.minimum.x) || !ReadF32(payload, 20U, chunk.bounds.minimum.y)
				|| !ReadF32(payload, 24U, chunk.bounds.minimum.z) || !ReadF32(payload, 28U, chunk.bounds.maximum.x)
				|| !ReadF32(payload, 32U, chunk.bounds.maximum.y) || !ReadF32(payload, 36U, chunk.bounds.maximum.z)
				|| (flags & ~kKnownTileFlags) != 0U
				|| fingerprintCount > kMaximumSavedNavigationSourceFingerprints) {
				reason = "SavedNavigationChunkMetadataMalformed";
				return false;
			}
			std::size_t fingerprintBytes = 0U;
			std::size_t byteOffset = 0U;
			if(!CheckedMultiply(fingerprintCount, sizeof(std::uint64_t), fingerprintBytes)
				|| !CheckedAdd(kChunkMetadataBytes, fingerprintBytes, byteOffset)
				|| byteOffset > payload.size()) {
				reason = "SavedNavigationChunkMetadataOutOfBounds";
				return false;
			}
			chunk.sourceFingerprints.resize(fingerprintCount);
			for(std::size_t index = 0U; index < chunk.sourceFingerprints.size(); ++index) {
				if(!ReadU64(payload, kChunkMetadataBytes + index * sizeof(std::uint64_t), chunk.sourceFingerprints[index])) {
					reason = "SavedNavigationChunkFingerprintOutOfBounds";
					return false;
				}
			}
			chunk.bytes.assign(payload.begin() + static_cast<std::ptrdiff_t>(byteOffset), payload.end());
			chunk.checksum = checksum;
			chunk.empty = (flags & kEmptyTileFlag) != 0U;
			chunk.supportComplete = (flags & kSupportCompleteFlag) != 0U;
			chunk.clearanceComplete = (flags & kClearanceCompleteFlag) != 0U;
			if(!IsValidChunk(chunk)) {
				reason = "SavedNavigationChunkMetadataIncompatible";
				return false;
			}
			return true;
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
		std::vector<std::vector<std::byte>> payloads;
		payloads.reserve(ordered.size());
		for(std::size_t index = 0U; index < ordered.size(); ++index) {
			if(!EncodeChunkPayload(ordered[index], payloads.emplace_back())) {
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
		for(const std::vector<std::byte>& payload : payloads) {
			if(!CheckedAdd(totalBytes, payload.size(), totalBytes) || totalBytes > kMaximumSavedNavigationBytes) {
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
			const std::vector<std::byte>& payload = payloads[index];
			const std::size_t entry = indexOffset + index * kIndexEntryBytes;
			WriteU64(result.bytes, entry, static_cast<std::uint64_t>(chunk.key.shardX));
			WriteU64(result.bytes, entry + 8U, static_cast<std::uint64_t>(chunk.key.shardZ));
			WriteU32(result.bytes, entry + 16U, static_cast<std::uint32_t>(chunk.key.verticalBand));
			WriteU64(result.bytes, entry + 24U, chunk.key.profileBuildKey);
			WriteU64(result.bytes, entry + 32U, chunk.generation);
			WriteU64(result.bytes, entry + 40U, payloadCursor);
			WriteU64(result.bytes, entry + 48U, payload.size());
			WriteU64(result.bytes, entry + 56U, HashBytes(payload));
			std::copy(payload.begin(), payload.end(), result.bytes.begin() + static_cast<std::ptrdiff_t>(payloadCursor));
			payloadCursor += payload.size();
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
		if(bytes.size() < kHeaderBytes || bytes.size() > kMaximumSavedNavigationBytes) {
			result.reason = "SavedNavigationInvalidHeader";
			return result;
		}
		if(std::equal(kVersionOneMagic.begin(), kVersionOneMagic.end(), bytes.begin())) {
			// V1 lacked empty/complete/source metadata. Treat it as a normal optional
			// cache miss so the caller can schedule its ordinary fresh build.
			result.reason = "SavedNavigationUnsupportedVersion";
			return result;
		}
		if(!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
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
		if(version != kSavedNavigationFormatVersion) {
			result.reason = "SavedNavigationUnsupportedVersion";
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
				|| generation == 0U || chunkOffset != nextPayloadOffset || chunkLength < kChunkMetadataBytes
				|| chunkOffset > bytes.size()
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
			if(!DecodeChunkPayload(payload, chunk, result.reason)) {
				result.chunks.clear();
				return result;
			}
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
