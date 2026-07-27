// AnimationAssetSerialization.cpp - Checked writer and reader for animation envelopes.

#include "File Formats/TerraEngineAnimationFile.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace TerraEngine::Animation {
	namespace {
		constexpr bool IsKnownChunkType(std::uint32_t type) noexcept {
			return type == kAnimationChunkMetadata
				|| type == kAnimationChunkPayload
				|| type == kAnimationChunkStreamData;
		}

		bool GuidLess(const AnimationGuid& left, const AnimationGuid& right) noexcept {
			return left.value < right.value;
		}

		bool DependencyLess(const AnimationDependencyEntry& left, const AnimationDependencyEntry& right) noexcept {
			if(left.assetId != right.assetId) {
				return GuidLess(left.assetId, right.assetId);
			}
			if(left.expectedAssetType != right.expectedAssetType) {
				return left.expectedAssetType < right.expectedAssetType;
			}
			return left.expectedGeneration < right.expectedGeneration;
		}

		bool IsRangeAfter(std::uint64_t offset, std::uint64_t lowerBound) noexcept {
			return offset >= lowerBound;
		}

		bool RangesOverlap(std::uint64_t firstOffset, std::uint64_t firstSize,
			std::uint64_t secondOffset, std::uint64_t secondSize) noexcept {
			if(firstSize == 0U || secondSize == 0U) {
				return false;
			}
			std::uint64_t firstEnd = 0U;
			std::uint64_t secondEnd = 0U;
			return CheckedAdd(firstOffset, firstSize, firstEnd)
				&& CheckedAdd(secondOffset, secondSize, secondEnd)
				&& firstOffset < secondEnd && secondOffset < firstEnd;
		}
	}

	AnimationStatus ValidateAnimationRange(
		std::uint64_t containerSize,
		std::uint64_t offset,
		std::uint64_t size) noexcept {
		std::uint64_t end = 0U;
		if(!CheckedAdd(offset, size, end)) {
			return AnimationStatus::RangeOverflow;
		}
		return end <= containerSize ? AnimationStatus::Ok : AnimationStatus::RangeOutOfBounds;
	}

	AnimationContentHash ComputeAnimationContentHash(std::span<const std::uint8_t> bytes) noexcept {
		// The v1 field reserves 256 bits. Four independently seeded FNV-1a lanes
		// give every producer a deterministic complete field without adding a new
		// runtime dependency. This byte order and lane arrangement are frozen for v1.
		constexpr std::array<std::uint64_t, 4U> kSeeds{{
			14695981039346656037ULL,
			1099511628211ULL,
			0x9e3779b97f4a7c15ULL,
			0xd6e8feb86659fd93ULL
		}};
		AnimationContentHash result{};
		for(std::size_t lane = 0U; lane < kSeeds.size(); ++lane) {
			std::uint64_t hash = kSeeds[lane];
			for(const std::uint8_t byte : bytes) {
				hash ^= static_cast<std::uint64_t>(byte) + lane;
				hash *= 1099511628211ULL;
			}
			std::memcpy(result.data() + lane * sizeof(hash), &hash, sizeof(hash));
		}
		return result;
	}

	bool AnimationAssetWriter::AddDependency(AnimationDependencyEntry dependency) {
		if(dependencies_.size() == std::numeric_limits<std::uint32_t>::max()) {
			return false;
		}
		dependencies_.push_back(dependency);
		return true;
	}

	bool AnimationAssetWriter::AddChunk(std::uint32_t type, bool required, std::span<const std::uint8_t> bytes) {
		if(type == 0U || chunks_.size() == std::numeric_limits<std::uint32_t>::max()) {
			return false;
		}
		PendingChunk chunk;
		chunk.type = type;
		chunk.required = required;
		chunk.bytes.assign(bytes.begin(), bytes.end());
		chunks_.push_back(std::move(chunk));
		return true;
	}

	AnimationStatus AnimationAssetWriter::Finish(AnimationAssetEnvelope envelope, std::vector<std::uint8_t>& output) const {
		std::vector<AnimationDependencyEntry> dependencies = dependencies_;
		std::vector<PendingChunk> chunks = chunks_;
		std::sort(dependencies.begin(), dependencies.end(), DependencyLess);
		std::sort(chunks.begin(), chunks.end(), [](const PendingChunk& left, const PendingChunk& right) {
			if(left.type != right.type) {
				return left.type < right.type;
			}
			if(left.required != right.required) {
				return left.required < right.required;
			}
			return left.bytes < right.bytes;
		});

		for(std::size_t index = 1U; index < chunks.size(); ++index) {
			if(chunks[index - 1U].type == chunks[index].type) {
				return AnimationStatus::InvalidChunk;
			}
		}

		std::uint64_t dependencyBytes = 0U;
		std::uint64_t chunkTableBytes = 0U;
		std::uint64_t cursor = sizeof(AnimationAssetEnvelope);
		if(!CheckedMultiply(dependencies.size(), sizeof(AnimationDependencyEntry), dependencyBytes)
			|| !CheckedMultiply(chunks.size(), sizeof(AnimationChunkEntry), chunkTableBytes)
			|| !CheckedAdd(cursor, dependencyBytes, cursor)) {
			return AnimationStatus::RangeOverflow;
		}
		const std::uint64_t chunkTableOffset = cursor;
		if(!CheckedAdd(cursor, chunkTableBytes, cursor)) {
			return AnimationStatus::RangeOverflow;
		}

		std::vector<AnimationChunkEntry> entries;
		entries.reserve(chunks.size());
		for(const PendingChunk& chunk : chunks) {
			const std::uint64_t size = chunk.bytes.size();
			std::uint64_t nextCursor = 0U;
			if(!CheckedAdd(cursor, size, nextCursor)) {
				return AnimationStatus::RangeOverflow;
			}
			entries.push_back(AnimationChunkEntry{chunk.type, chunk.required ? kAnimationChunkFlagRequired : 0U, cursor, size});
			cursor = nextCursor;
		}
		if(cursor > std::numeric_limits<std::size_t>::max()) {
			return AnimationStatus::RangeOverflow;
		}

		envelope.magic = kAnimationAssetMagic;
		envelope.majorVersion = kAnimationAssetMajorVersion;
		envelope.minorVersion = kAnimationAssetMinorVersion;
		envelope.headerSize = sizeof(AnimationAssetEnvelope);
		envelope.endianMarker = kAnimationLittleEndianMarker;
		envelope.payloadSize = cursor - sizeof(AnimationAssetEnvelope);
		envelope.dependencyTableOffset = sizeof(AnimationAssetEnvelope);
		envelope.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
		envelope.chunkTableOffset = chunkTableOffset;
		envelope.chunkCount = static_cast<std::uint32_t>(entries.size());

		std::vector<std::uint8_t> staged(static_cast<std::size_t>(cursor), 0U);
		std::memcpy(staged.data(), &envelope, sizeof(envelope));
		if(!dependencies.empty()) {
			std::memcpy(staged.data() + envelope.dependencyTableOffset, dependencies.data(), static_cast<std::size_t>(dependencyBytes));
		}
		if(!entries.empty()) {
			std::memcpy(staged.data() + envelope.chunkTableOffset, entries.data(), static_cast<std::size_t>(chunkTableBytes));
		}
		for(std::size_t index = 0U; index < chunks.size(); ++index) {
			const AnimationChunkEntry& entry = entries[index];
			if(!chunks[index].bytes.empty()) {
				std::memcpy(staged.data() + entry.offset, chunks[index].bytes.data(), chunks[index].bytes.size());
			}
		}

		// The content hash describes the emitted payload, not the self-referential
		// envelope. This makes it stable when the header is copied between tools.
		envelope.contentHash = ComputeAnimationContentHash(
			std::span<const std::uint8_t>(staged).subspan(sizeof(AnimationAssetEnvelope)));
		std::memcpy(staged.data(), &envelope, sizeof(envelope));
		output = std::move(staged);
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationAssetReader::Parse(std::span<const std::uint8_t> input) {
		if(input.size() < sizeof(AnimationAssetEnvelope)) {
			return AnimationStatus::TruncatedInput;
		}

		AnimationAssetEnvelope envelope{};
		std::memcpy(&envelope, input.data(), sizeof(envelope));
		if(envelope.magic != kAnimationAssetMagic || envelope.headerSize != sizeof(AnimationAssetEnvelope)
			|| envelope.endianMarker != kAnimationLittleEndianMarker) {
			return AnimationStatus::InvalidEnvelope;
		}
		if(envelope.majorVersion != kAnimationAssetMajorVersion) {
			return AnimationStatus::UnsupportedMajorVersion;
		}
		if(envelope.payloadSize != input.size() - sizeof(AnimationAssetEnvelope)) {
			return AnimationStatus::InvalidEnvelope;
		}

		std::uint64_t dependencyBytes = 0U;
		std::uint64_t chunkTableBytes = 0U;
		if(!CheckedMultiply(envelope.dependencyCount, sizeof(AnimationDependencyEntry), dependencyBytes)
			|| !CheckedMultiply(envelope.chunkCount, sizeof(AnimationChunkEntry), chunkTableBytes)) {
			return AnimationStatus::RangeOverflow;
		}
		if(const AnimationStatus status = ValidateAnimationRange(input.size(), envelope.dependencyTableOffset, dependencyBytes);
			status != AnimationStatus::Ok) {
			return status;
		}
		if(const AnimationStatus status = ValidateAnimationRange(input.size(), envelope.chunkTableOffset, chunkTableBytes);
			status != AnimationStatus::Ok) {
			return status;
		}

		std::uint64_t dependencyEnd = 0U;
		std::uint64_t chunkTableEnd = 0U;
		CheckedAdd(envelope.dependencyTableOffset, dependencyBytes, dependencyEnd);
		CheckedAdd(envelope.chunkTableOffset, chunkTableBytes, chunkTableEnd);
		const std::uint64_t metadataEnd = std::max<std::uint64_t>({sizeof(AnimationAssetEnvelope), dependencyEnd, chunkTableEnd});
		if(!IsRangeAfter(envelope.dependencyTableOffset, sizeof(AnimationAssetEnvelope))
			|| !IsRangeAfter(envelope.chunkTableOffset, sizeof(AnimationAssetEnvelope))) {
			return AnimationStatus::InvalidEnvelope;
		}
		if(RangesOverlap(envelope.dependencyTableOffset, dependencyBytes, envelope.chunkTableOffset, chunkTableBytes)) {
			return AnimationStatus::InvalidEnvelope;
		}

		std::vector<AnimationDependencyEntry> dependencies(envelope.dependencyCount);
		if(!dependencies.empty()) {
			std::memcpy(dependencies.data(), input.data() + envelope.dependencyTableOffset, static_cast<std::size_t>(dependencyBytes));
		}
		std::vector<AnimationChunkEntry> chunks(envelope.chunkCount);
		if(!chunks.empty()) {
			std::memcpy(chunks.data(), input.data() + envelope.chunkTableOffset, static_cast<std::size_t>(chunkTableBytes));
		}

		std::vector<AnimationChunkEntry> orderedByOffset = chunks;
		std::sort(orderedByOffset.begin(), orderedByOffset.end(), [](const AnimationChunkEntry& left, const AnimationChunkEntry& right) {
			return left.offset < right.offset;
		});
		std::uint64_t priorEnd = metadataEnd;
		for(const AnimationChunkEntry& chunk : orderedByOffset) {
			if((chunk.flags & ~kAnimationChunkFlagRequired) != 0U || chunk.type == 0U) {
				return AnimationStatus::InvalidChunk;
			}
			if(!IsRangeAfter(chunk.offset, metadataEnd)) {
				return AnimationStatus::InvalidChunk;
			}
			if(const AnimationStatus status = ValidateAnimationRange(input.size(), chunk.offset, chunk.size);
			status != AnimationStatus::Ok) {
				return status;
			}
			if(chunk.offset < priorEnd) {
				return AnimationStatus::InvalidChunk;
			}
			CheckedAdd(chunk.offset, chunk.size, priorEnd);
			if(!IsKnownChunkType(chunk.type) && (chunk.flags & kAnimationChunkFlagRequired) != 0U) {
				return AnimationStatus::UnknownRequiredChunk;
			}
		}
		for(std::size_t index = 0U; index < chunks.size(); ++index) {
			for(std::size_t other = index + 1U; other < chunks.size(); ++other) {
				if(chunks[index].type == chunks[other].type) {
					return AnimationStatus::InvalidChunk;
				}
			}
		}
		if(envelope.contentHash != ComputeAnimationContentHash(input.subspan(sizeof(AnimationAssetEnvelope)))) {
			return AnimationStatus::ContentHashMismatch;
		}

		// Assign only after all validation succeeds so malformed files cannot expose
		// a partially parsed generation to a caller that reuses this reader.
		storage_.assign(input.begin(), input.end());
		envelope_ = envelope;
		dependencies_ = std::move(dependencies);
		chunks_ = std::move(chunks);
		return AnimationStatus::Ok;
	}

	std::span<const std::uint8_t> AnimationAssetReader::FindChunk(std::uint32_t type) const noexcept {
		for(const AnimationChunkEntry& chunk : chunks_) {
			if(chunk.type == type) {
				return std::span<const std::uint8_t>(storage_).subspan(
					static_cast<std::size_t>(chunk.offset), static_cast<std::size_t>(chunk.size));
			}
		}
		return {};
	}
}
