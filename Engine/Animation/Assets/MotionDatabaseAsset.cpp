// MotionDatabaseAsset.cpp - Checked persistence for quantized motion data.

#include "Animation/Assets/MotionDatabaseAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace TerraEngine::Animation {
	namespace {
		// FNV-1a is used only as a stable identity/fingerprint combiner. The outer
		// animation envelope still carries its complete content hash.
		constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
		constexpr std::uint64_t kMotionDatabaseMaximumSerializedBytes =
			2ULL * kMotionDatabaseMaximumPayloadBytes
			+ sizeof(AnimationAssetEnvelope) + 4ULL * sizeof(AnimationChunkEntry);

		void HashBytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for(std::size_t index = 0U; index < size; ++index) {
				hash = (hash ^ bytes[index]) * kFnvPrime;
			}
		}

		template<typename Type>
		void HashValue(std::uint64_t& hash, const Type& value) noexcept {
			static_assert(std::is_trivially_copyable_v<Type>);
			HashBytes(hash, &value, sizeof(value));
		}

		// Do not hash a whole C++ record: compiler-inserted padding is not authored
		// data and could otherwise make an identical cook differ between builds.
		void HashSample(std::uint64_t& hash, const MotionDatabaseSample& sample) noexcept {
			HashValue(hash, sample.stableSampleId);
			HashValue(hash, sample.sourceClipId);
			HashValue(hash, sample.sourceTick);
			HashBytes(hash, sample.features.values.data(), sizeof(sample.features.values));
			HashValue(hash, sample.tagMask);
			HashValue(hash, sample.contactMask);
			HashValue(hash, sample.phaseQuantized);
			HashValue(hash, sample.transitionFlags);
			HashValue(hash, sample.continuityGroup);
			HashValue(hash, sample.exclusionFlags);
			HashValue(hash, sample.continuityPredecessorId);
			HashValue(hash, sample.continuitySuccessorId);
		}

		void HashFeatureBlock(std::uint64_t& hash,
			const MotionDatabaseFeatureBlock& block) noexcept {
			HashValue(hash, block.semantic);
			HashValue(hash, block.firstFeature);
			HashValue(hash, block.featureCount);
			HashValue(hash, block.flags);
		}

		void HashStorageLayout(std::uint64_t& hash,
			const MotionDatabaseStorageLayout& layout) noexcept {
			HashValue(hash, layout.schemaVersion);
			HashValue(hash, layout.samplesPerBlock);
			HashValue(hash, layout.cpuSampleStrideBytes);
			HashValue(hash, layout.gpuSampleStrideBytes);
			HashValue(hash, layout.maximumTopK);
			HashValue(hash, layout.flags);
		}

		void HashResidentBlock(std::uint64_t& hash,
			const MotionDatabaseResidentBlock& block) noexcept {
			HashValue(hash, block.blockIndex);
			HashValue(hash, block.firstSample);
			HashValue(hash, block.sampleCount);
			HashValue(hash, block.flags);
			HashValue(hash, block.cpuByteOffset);
			HashValue(hash, block.cpuByteSize);
			HashValue(hash, block.gpuByteOffset);
			HashValue(hash, block.gpuByteSize);
			HashValue(hash, block.contentFingerprint);
		}

		std::uint64_t ComputeLegacySchemaFingerprint(
			const MotionDatabaseAsset& asset) noexcept {
			std::uint64_t hash = kFnvOffset;
			HashValue(hash, asset.formatVersion);
			HashValue(hash, asset.featureCount);
			HashBytes(hash, asset.quantizationScales.data(), sizeof(asset.quantizationScales));
			HashBytes(hash, asset.quantizationOffsets.data(), sizeof(asset.quantizationOffsets));
			return hash;
		}

		std::uint64_t ComputeSampleFingerprint(
			std::span<const MotionDatabaseSample> samples) noexcept {
			std::uint64_t hash = kFnvOffset;
			for(const MotionDatabaseSample& sample : samples) HashSample(hash, sample);
			return hash;
		}

		std::uint64_t ComputeBlockFingerprint(const MotionDatabaseAsset& asset,
			std::size_t firstSample, std::size_t sampleCount) noexcept {
			return ComputeSampleFingerprint(std::span<const MotionDatabaseSample>(asset.samples)
				.subspan(firstSample, sampleCount));
		}

		class Writer {
		public:
			template<typename Type>
			bool Put(const Type& value) {
				static_assert(std::is_trivially_copyable_v<Type>);
				if(sizeof(Type) > kMotionDatabaseMaximumPayloadBytes
					|| bytes_.size() > kMotionDatabaseMaximumPayloadBytes - sizeof(Type)) {
					return false;
				}
				const std::size_t offset = bytes_.size();
				bytes_.resize(offset + sizeof(Type));
				std::memcpy(bytes_.data() + offset, &value, sizeof(Type));
				return true;
			}

			std::vector<std::uint8_t> Take() noexcept { return std::move(bytes_); }
			std::size_t Size() const noexcept { return bytes_.size(); }

		private:
			std::vector<std::uint8_t> bytes_;
		};

		class Reader {
		public:
			explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

			template<typename Type>
			bool Get(Type& value) noexcept {
				static_assert(std::is_trivially_copyable_v<Type>);
				if(cursor_ > bytes_.size() || bytes_.size() - cursor_ < sizeof(Type)) {
					return false;
				}
				std::memcpy(&value, bytes_.data() + cursor_, sizeof(Type));
				cursor_ += sizeof(Type);
				return true;
			}

			bool Finished() const noexcept { return cursor_ == bytes_.size(); }
			std::size_t Remaining() const noexcept { return bytes_.size() - cursor_; }

		private:
			std::span<const std::uint8_t> bytes_;
			std::size_t cursor_ = 0U;
		};

		#pragma pack(push, 1)
		// The first-generation writer copied a 144-byte platform record. Keeping
		// this prefix lets the new reader upgrade those assets while all new fields
		// live in the checked extension table below.
		struct LegacyMotionDatabaseSampleV1 {
			std::uint64_t stableSampleId = 0U;
			std::uint64_t sourceClipId = 0U;
			AnimationFixedTick sourceTick = 0;
			MotionDatabaseQuantizedFeatureVector features{};
			std::uint64_t tagMask = 0U;
			std::uint32_t contactMask = 0U;
			std::uint16_t phaseQuantized = 0U;
			std::uint16_t transitionFlags = 0U;
			std::uint32_t continuityGroup = 0U;
			std::uint32_t reservedPadding = 0U;
		};

		struct MotionDatabaseSampleExtensionV1 {
			std::uint32_t exclusionFlags = 0U;
			std::uint32_t reserved = 0U;
			std::uint64_t continuityPredecessorId = 0U;
			std::uint64_t continuitySuccessorId = 0U;
		};
		#pragma pack(pop)

		static_assert(sizeof(LegacyMotionDatabaseSampleV1) == 144U);
		static_assert(sizeof(MotionDatabaseSampleExtensionV1) == 24U);

		LegacyMotionDatabaseSampleV1 MakeLegacyPrefix(
			const MotionDatabaseSample& sample) noexcept {
			LegacyMotionDatabaseSampleV1 result;
			result.stableSampleId = sample.stableSampleId;
			result.sourceClipId = sample.sourceClipId;
			result.sourceTick = sample.sourceTick;
			result.features = sample.features;
			result.tagMask = sample.tagMask;
			result.contactMask = sample.contactMask;
			result.phaseQuantized = sample.phaseQuantized;
			result.transitionFlags = sample.transitionFlags;
			result.continuityGroup = sample.continuityGroup;
			return result;
		}

		MotionDatabaseSample MakeCurrentSample(
			const LegacyMotionDatabaseSampleV1& legacy) noexcept {
			MotionDatabaseSample result;
			result.stableSampleId = legacy.stableSampleId;
			result.sourceClipId = legacy.sourceClipId;
			result.sourceTick = legacy.sourceTick;
			result.features = legacy.features;
			result.tagMask = legacy.tagMask;
			result.contactMask = legacy.contactMask;
			result.phaseQuantized = legacy.phaseQuantized;
			result.transitionFlags = legacy.transitionFlags;
			result.continuityGroup = legacy.continuityGroup;
			return result;
		}

		// STRM records deliberately write every field instead of copying the C++
		// object. The resulting byte sequence is exactly 160 bytes and can never
		// acquire compiler padding as the in-memory type evolves.
		bool PutCurrentSample(Writer& writer,
			const MotionDatabaseSample& sample) {
			const std::size_t start = writer.Size();
			return writer.Put(sample.stableSampleId)
				&& writer.Put(sample.sourceClipId)
				&& writer.Put(sample.sourceTick)
				&& writer.Put(sample.features.values)
				&& writer.Put(sample.tagMask)
				&& writer.Put(sample.contactMask)
				&& writer.Put(sample.phaseQuantized)
				&& writer.Put(sample.transitionFlags)
				&& writer.Put(sample.continuityGroup)
				&& writer.Put(sample.exclusionFlags)
				&& writer.Put(sample.continuityPredecessorId)
				&& writer.Put(sample.continuitySuccessorId)
				&& writer.Size() - start == kMotionDatabaseCpuSampleStrideBytes;
		}

		bool GetCurrentSample(Reader& reader,
			MotionDatabaseSample& sample) noexcept {
			const std::size_t before = reader.Remaining();
			return reader.Get(sample.stableSampleId)
				&& reader.Get(sample.sourceClipId)
				&& reader.Get(sample.sourceTick)
				&& reader.Get(sample.features.values)
				&& reader.Get(sample.tagMask)
				&& reader.Get(sample.contactMask)
				&& reader.Get(sample.phaseQuantized)
				&& reader.Get(sample.transitionFlags)
				&& reader.Get(sample.continuityGroup)
				&& reader.Get(sample.exclusionFlags)
				&& reader.Get(sample.continuityPredecessorId)
				&& reader.Get(sample.continuitySuccessorId)
				&& before - reader.Remaining() == kMotionDatabaseCpuSampleStrideBytes;
		}

		bool FinitePositive(float value) noexcept {
			return std::isfinite(value) && value > 0.0F;
		}

		void Canonicalize(MotionDatabaseAsset& asset) {
			std::sort(asset.samples.begin(), asset.samples.end(),
				[](const MotionDatabaseSample& left, const MotionDatabaseSample& right) {
					return left.stableSampleId < right.stableSampleId;
				});
		}

		bool HasValidContinuityReference(const MotionDatabaseAsset& asset,
			const MotionDatabaseSample& sample, std::uint64_t referencedId) noexcept {
			if(referencedId == 0U) return true;
			const auto found = std::lower_bound(asset.samples.begin(), asset.samples.end(), referencedId,
				[](const MotionDatabaseSample& candidate, std::uint64_t id) {
					return candidate.stableSampleId < id;
				});
			return found != asset.samples.end() && found->stableSampleId == referencedId
				&& found->stableSampleId != sample.stableSampleId
				&& found->continuityGroup == sample.continuityGroup;
		}

		AnimationStatus ValidateResidentBlocks(const MotionDatabaseAsset& asset) noexcept {
			if(asset.residentBlocks.empty()) {
				// Hand-authored v1 tests and old runtime producers are accepted as an
				// implicit single-layout asset. Serialization/compiler publication always
				// materializes the explicit table before it crosses a boundary.
				return AnimationStatus::Ok;
			}
			const std::size_t expectedCount = asset.samples.empty() ? 0U
				: (asset.samples.size() + asset.storageLayout.samplesPerBlock - 1U)
					/ asset.storageLayout.samplesPerBlock;
			if(asset.residentBlocks.size() != expectedCount
				|| expectedCount > kMotionDatabaseMaximumResidentBlocks) {
				return AnimationStatus::InvalidArgument;
			}
			std::uint64_t expectedCpuOffset = 0U;
			std::uint64_t expectedGpuOffset = 0U;
			std::uint32_t expectedFirstSample = 0U;
			for(std::size_t index = 0U; index < asset.residentBlocks.size(); ++index) {
				const MotionDatabaseResidentBlock& block = asset.residentBlocks[index];
				const std::uint32_t remaining = static_cast<std::uint32_t>(asset.samples.size())
					- expectedFirstSample;
				const std::uint32_t expectedSamples = std::min(
					asset.storageLayout.samplesPerBlock, remaining);
				std::uint64_t expectedCpuSize = 0U;
				std::uint64_t expectedGpuSize = 0U;
				if(!CheckedMultiply(expectedSamples, asset.storageLayout.cpuSampleStrideBytes,
					expectedCpuSize)
					|| !CheckedMultiply(expectedSamples, asset.storageLayout.gpuSampleStrideBytes,
						expectedGpuSize)) {
					return AnimationStatus::RangeOverflow;
				}
				if(block.blockIndex != index || block.firstSample != expectedFirstSample
					|| block.sampleCount != expectedSamples
					|| block.flags != (kMotionStorageCpuResident | kMotionStorageGpuResident)
					|| block.cpuByteOffset != expectedCpuOffset
					|| block.cpuByteSize != expectedCpuSize
					|| block.gpuByteOffset != expectedGpuOffset
					|| block.gpuByteSize != expectedGpuSize
					|| block.contentFingerprint != ComputeBlockFingerprint(
						asset, block.firstSample, block.sampleCount)) {
					return AnimationStatus::ContentHashMismatch;
				}
				if(!CheckedAdd(expectedCpuOffset, expectedCpuSize, expectedCpuOffset)
					|| !CheckedAdd(expectedGpuOffset, expectedGpuSize, expectedGpuOffset)) {
					return AnimationStatus::RangeOverflow;
				}
				expectedFirstSample += expectedSamples;
			}
			return expectedFirstSample == asset.samples.size()
				? AnimationStatus::Ok : AnimationStatus::InvalidArgument;
		}

		AnimationStatus EncodeValidatedStreamBlock(
			const MotionDatabaseAsset& asset,
			std::uint32_t blockIndex,
			std::vector<std::uint8_t>& output) {
			if(blockIndex >= asset.residentBlocks.size()) {
				return AnimationStatus::InvalidArgument;
			}
			const MotionDatabaseResidentBlock& block = asset.residentBlocks[blockIndex];
			Writer writer;
			for(std::uint32_t offset = 0U; offset < block.sampleCount; ++offset) {
				if(!PutCurrentSample(writer, asset.samples[block.firstSample + offset])) {
					return AnimationStatus::RangeOverflow;
				}
			}
			std::vector<std::uint8_t> staged = writer.Take();
			if(staged.size() != block.cpuByteSize
				|| ComputeSampleFingerprint(std::span<const MotionDatabaseSample>(asset.samples)
					.subspan(block.firstSample, block.sampleCount)) != block.contentFingerprint) {
				return AnimationStatus::ContentHashMismatch;
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		}

		AnimationStatus DecodeValidatedStreamBlock(
			const MotionDatabaseAsset& asset,
			std::uint32_t blockIndex,
			std::span<const std::uint8_t> encodedBlock,
			std::vector<MotionDatabaseSample>& output) {
			if(blockIndex >= asset.residentBlocks.size()) {
				return AnimationStatus::InvalidArgument;
			}
			const MotionDatabaseResidentBlock& block = asset.residentBlocks[blockIndex];
			std::uint64_t expectedByteSize = 0U;
			if(!CheckedMultiply(block.sampleCount, kMotionDatabaseCpuSampleStrideBytes,
				expectedByteSize)) {
				return AnimationStatus::RangeOverflow;
			}
			if(expectedByteSize != block.cpuByteSize) {
				return AnimationStatus::ContentHashMismatch;
			}
			if(encodedBlock.size() < expectedByteSize) {
				return AnimationStatus::TruncatedInput;
			}
			if(encodedBlock.size() > expectedByteSize) {
				return AnimationStatus::InvalidChunk;
			}

			std::vector<MotionDatabaseSample> staged;
			staged.reserve(block.sampleCount);
			Reader reader(encodedBlock);
			for(std::uint32_t index = 0U; index < block.sampleCount; ++index) {
				MotionDatabaseSample sample;
				if(!GetCurrentSample(reader, sample)) {
					return AnimationStatus::TruncatedInput;
				}
				staged.push_back(sample);
			}
			if(!reader.Finished()) {
				return AnimationStatus::InvalidChunk;
			}
			if(ComputeSampleFingerprint(staged) != block.contentFingerprint) {
				return AnimationStatus::ContentHashMismatch;
			}
			const std::span<const MotionDatabaseSample> authoritative =
				std::span<const MotionDatabaseSample>(asset.samples)
					.subspan(block.firstSample, block.sampleCount);
			if(!std::equal(staged.begin(), staged.end(), authoritative.begin(), authoritative.end())) {
				return AnimationStatus::ContentHashMismatch;
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		}

		AnimationStatus ValidateStreamChunkForValidatedAsset(
			const MotionDatabaseAsset& asset,
			std::span<const std::uint8_t> streamChunk) {
			std::uint64_t expectedByteSize = 0U;
			if(!CheckedMultiply(asset.samples.size(), kMotionDatabaseCpuSampleStrideBytes,
				expectedByteSize)) {
				return AnimationStatus::RangeOverflow;
			}
			if(streamChunk.size() < expectedByteSize) {
				return AnimationStatus::TruncatedInput;
			}
			if(streamChunk.size() > expectedByteSize) {
				return AnimationStatus::InvalidChunk;
			}
			if(asset.residentBlocks.empty()) {
				return asset.samples.empty() ? AnimationStatus::Ok
					: AnimationStatus::InvalidArgument;
			}

			std::vector<MotionDatabaseSample> decoded;
			for(const MotionDatabaseResidentBlock& block : asset.residentBlocks) {
				if(const AnimationStatus rangeStatus = ValidateAnimationRange(
					streamChunk.size(), block.cpuByteOffset, block.cpuByteSize);
					rangeStatus != AnimationStatus::Ok) {
					return rangeStatus;
				}
				const std::span<const std::uint8_t> encodedBlock = streamChunk.subspan(
					static_cast<std::size_t>(block.cpuByteOffset),
					static_cast<std::size_t>(block.cpuByteSize));
				if(const AnimationStatus decodeStatus = DecodeValidatedStreamBlock(
					asset, block.blockIndex, encodedBlock, decoded);
					decodeStatus != AnimationStatus::Ok) {
					return decodeStatus;
				}
			}
			return AnimationStatus::Ok;
		}

		const AnimationChunkEntry* FindChunkEntry(
			const AnimationAssetReader& reader,
			std::uint32_t type) noexcept {
			const std::span<const AnimationChunkEntry> chunks = reader.Chunks();
			const auto found = std::find_if(chunks.begin(), chunks.end(),
				[type](const AnimationChunkEntry& chunk) { return chunk.type == type; });
			return found == chunks.end() ? nullptr : &*found;
		}

		// DATA is parsed through one shared path so ordinary deserialization and
		// file-range resolution apply exactly the same legacy upgrade, metadata,
		// sample, and optional STRM validation rules.
		AnimationStatus DecodeMotionDatabaseFromReader(
			const AnimationAssetReader& assetReader,
			AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
			MotionDatabaseAsset& staged) {
			if(expectedCompatibilityFingerprint != 0U
				&& assetReader.Envelope().compatibilityFingerprint
					!= expectedCompatibilityFingerprint) {
				return AnimationStatus::IncompatibleAsset;
			}
			const std::span<const std::uint8_t> payload =
				assetReader.FindChunk(kAnimationChunkPayload);
			if(payload.empty()) {
				return AnimationStatus::InvalidChunk;
			}

			Reader reader(payload);
			std::uint32_t magic = 0U;
			std::uint32_t count = 0U;
			if(!reader.Get(magic) || magic != kMotionDatabasePayloadMagic
				|| !reader.Get(staged.formatVersion) || !reader.Get(staged.assetId)
				|| !reader.Get(staged.compatibilityFingerprint)
				|| !reader.Get(staged.sourceFingerprint)
				|| !reader.Get(staged.featureSchemaFingerprint)
				|| !reader.Get(staged.featureCount)
				|| !reader.Get(staged.quantizationScales)
				|| !reader.Get(staged.quantizationOffsets)
				|| !reader.Get(count) || count > kMotionDatabaseMaximumSamples) {
				return AnimationStatus::TruncatedInput;
			}
			staged.samples.reserve(count);
			for(std::uint32_t index = 0U; index < count; ++index) {
				LegacyMotionDatabaseSampleV1 legacy;
				if(!reader.Get(legacy)) return AnimationStatus::TruncatedInput;
				staged.samples.push_back(MakeCurrentSample(legacy));
			}

			if(reader.Finished()) {
				// Upgrade a legacy payload only after its original schema identity is
				// proven. The in-memory fingerprint changes at this boundary so old and
				// current query layouts cannot alias one another.
				if(staged.featureSchemaFingerprint != ComputeLegacySchemaFingerprint(staged)) {
					return AnimationStatus::ContentHashMismatch;
				}
				if(const AnimationStatus layoutStatus =
					RebuildMotionDatabaseStorageMetadata(staged);
					layoutStatus != AnimationStatus::Ok) {
					return layoutStatus;
				}
				staged.featureSchemaFingerprint =
					ComputeMotionDatabaseSchemaFingerprint(staged);
			} else {
				std::uint32_t extensionMagic = 0U;
				std::uint16_t extensionVersion = 0U;
				std::uint16_t featureBlockCount = 0U;
				std::uint32_t residentBlockCount = 0U;
				std::uint32_t sampleExtensionCount = 0U;
				if(!reader.Get(extensionMagic)
					|| extensionMagic != kMotionDatabaseExtensionMagic
					|| !reader.Get(extensionVersion)
					|| extensionVersion != kMotionDatabaseExtensionVersion
					|| !reader.Get(featureBlockCount)
					|| featureBlockCount != kMotionDatabaseFeatureBlockCount
					|| !reader.Get(staged.featureBlocks)
					|| !reader.Get(staged.storageLayout)
					|| !reader.Get(residentBlockCount)
					|| residentBlockCount > kMotionDatabaseMaximumResidentBlocks) {
					return AnimationStatus::InvalidChunk;
				}
				staged.residentBlocks.resize(residentBlockCount);
				for(MotionDatabaseResidentBlock& block : staged.residentBlocks) {
					if(!reader.Get(block)) return AnimationStatus::TruncatedInput;
				}
				if(!reader.Get(sampleExtensionCount) || sampleExtensionCount != count) {
					return AnimationStatus::InvalidChunk;
				}
				for(MotionDatabaseSample& sample : staged.samples) {
					MotionDatabaseSampleExtensionV1 extension;
					if(!reader.Get(extension) || extension.reserved != 0U) {
						return AnimationStatus::InvalidChunk;
					}
					sample.exclusionFlags = extension.exclusionFlags;
					sample.continuityPredecessorId = extension.continuityPredecessorId;
					sample.continuitySuccessorId = extension.continuitySuccessorId;
				}
				if(!reader.Finished()) return AnimationStatus::InvalidChunk;
			}

			if(staged.assetId != assetReader.Envelope().assetId
				|| staged.compatibilityFingerprint
					!= assetReader.Envelope().compatibilityFingerprint) {
				return AnimationStatus::InvalidEnvelope;
			}
			Canonicalize(staged);
			if(const AnimationStatus status = ValidateMotionDatabaseAsset(staged);
				status != AnimationStatus::Ok) {
				return status;
			}

			// A missing STRM entry is valid legacy/v1 input. Presence opts the file
			// into strict parity validation before any range becomes observable.
			if(FindChunkEntry(assetReader, kAnimationChunkStreamData) != nullptr) {
				const std::span<const std::uint8_t> streamChunk =
					assetReader.FindChunk(kAnimationChunkStreamData);
				if(const AnimationStatus streamStatus = ValidateStreamChunkForValidatedAsset(
					staged, streamChunk); streamStatus != AnimationStatus::Ok) {
					return streamStatus;
				}
			}
			return AnimationStatus::Ok;
		}
	}

	std::string_view ToString(MotionDatabaseFeatureSemantic semantic) noexcept {
		switch(semantic) {
		case MotionDatabaseFeatureSemantic::Trajectory: return "Trajectory";
		case MotionDatabaseFeatureSemantic::Pose: return "Pose";
		case MotionDatabaseFeatureSemantic::Velocity: return "Velocity";
		case MotionDatabaseFeatureSemantic::Facing: return "Facing";
		case MotionDatabaseFeatureSemantic::Contact: return "Contact";
		case MotionDatabaseFeatureSemantic::Phase: return "Phase";
		case MotionDatabaseFeatureSemantic::Tag: return "Tag";
		case MotionDatabaseFeatureSemantic::Transition: return "Transition";
		case MotionDatabaseFeatureSemantic::Continuity: return "Continuity";
		}
		return "Unknown";
	}

	const MotionDatabaseFeatureBlock* FindMotionDatabaseFeatureBlock(
		const MotionDatabaseAsset& asset, MotionDatabaseFeatureSemantic semantic) noexcept {
		const auto found = std::find_if(asset.featureBlocks.begin(), asset.featureBlocks.end(),
			[semantic](const MotionDatabaseFeatureBlock& block) {
				return block.semantic == semantic;
			});
		return found == asset.featureBlocks.end() ? nullptr : &*found;
	}

	MotionDatabaseGpuSample ExpandMotionDatabaseSampleForGpu(
		const MotionDatabaseSample& sample) noexcept {
		MotionDatabaseGpuSample result;
		result.stableSampleId = sample.stableSampleId;
		result.sourceClipId = sample.sourceClipId;
		result.sourceTick = sample.sourceTick;
		for(std::size_t feature = 0U; feature < kMotionDatabaseFeatureCount; ++feature) {
			result.features[feature] = sample.features.values[feature];
		}
		result.tagMask = sample.tagMask;
		result.contactMask = sample.contactMask;
		result.phaseQuantized = sample.phaseQuantized;
		result.transitionFlags = sample.transitionFlags;
		result.continuityGroup = sample.continuityGroup;
		result.exclusionFlags = sample.exclusionFlags;
		result.continuityPredecessorId = sample.continuityPredecessorId;
		result.continuitySuccessorId = sample.continuitySuccessorId;
		return result;
	}

	AnimationStatus RebuildMotionDatabaseStorageMetadata(MotionDatabaseAsset& asset) noexcept {
		if(asset.storageLayout.samplesPerBlock == 0U
			|| asset.storageLayout.samplesPerBlock > kMotionDatabaseSamplesPerBlockDefault
			|| asset.samples.size() > kMotionDatabaseMaximumSamples) {
			return AnimationStatus::HardLimitExceeded;
		}
		try {
			std::vector<MotionDatabaseResidentBlock> blocks;
			const std::size_t blockCount = asset.samples.empty() ? 0U
				: (asset.samples.size() + asset.storageLayout.samplesPerBlock - 1U)
					/ asset.storageLayout.samplesPerBlock;
			if(blockCount > kMotionDatabaseMaximumResidentBlocks) {
				return AnimationStatus::HardLimitExceeded;
			}
			blocks.reserve(blockCount);
			std::uint64_t cpuOffset = 0U;
			std::uint64_t gpuOffset = 0U;
			for(std::size_t blockIndex = 0U; blockIndex < blockCount; ++blockIndex) {
				const std::size_t firstSample = blockIndex * asset.storageLayout.samplesPerBlock;
				const std::size_t sampleCount = std::min<std::size_t>(
					asset.storageLayout.samplesPerBlock, asset.samples.size() - firstSample);
				std::uint64_t cpuSize = 0U;
				std::uint64_t gpuSize = 0U;
				if(!CheckedMultiply(sampleCount, asset.storageLayout.cpuSampleStrideBytes, cpuSize)
					|| !CheckedMultiply(sampleCount, asset.storageLayout.gpuSampleStrideBytes, gpuSize)) {
					return AnimationStatus::RangeOverflow;
				}
				MotionDatabaseResidentBlock block;
				block.blockIndex = static_cast<std::uint32_t>(blockIndex);
				block.firstSample = static_cast<std::uint32_t>(firstSample);
				block.sampleCount = static_cast<std::uint32_t>(sampleCount);
				block.cpuByteOffset = cpuOffset;
				block.cpuByteSize = cpuSize;
				block.gpuByteOffset = gpuOffset;
				block.gpuByteSize = gpuSize;
				block.contentFingerprint = ComputeBlockFingerprint(asset, firstSample, sampleCount);
				blocks.push_back(block);
				if(!CheckedAdd(cpuOffset, cpuSize, cpuOffset)
					|| !CheckedAdd(gpuOffset, gpuSize, gpuOffset)) {
					return AnimationStatus::RangeOverflow;
				}
			}
			// The format intentionally uses a lower operational payload budget than
			// the absolute sample-count hard limit. This keeps one failed cook from
			// allocating multi-gigabyte staging buffers; projects can shard databases.
			if(cpuOffset > kMotionDatabaseMaximumPayloadBytes
				|| gpuOffset > std::numeric_limits<std::uint32_t>::max() * 4ULL) {
				return AnimationStatus::HardLimitExceeded;
			}
			asset.residentBlocks = std::move(blocks);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	std::uint64_t ComputeMotionDatabaseSchemaFingerprint(
		const MotionDatabaseAsset& asset) noexcept {
		std::uint64_t hash = ComputeLegacySchemaFingerprint(asset);
		for(const MotionDatabaseFeatureBlock& block : asset.featureBlocks) {
			HashFeatureBlock(hash, block);
		}
		HashStorageLayout(hash, asset.storageLayout);
		return hash;
	}

	std::uint64_t ComputeMotionDatabaseContentFingerprint(
		const MotionDatabaseAsset& asset) noexcept {
		std::uint64_t hash = kFnvOffset;
		HashBytes(hash, &asset.assetId, sizeof(asset.assetId));
		HashValue(hash, asset.compatibilityFingerprint);
		HashValue(hash, asset.sourceFingerprint);
		HashValue(hash, asset.featureSchemaFingerprint);
		for(const MotionDatabaseResidentBlock& block : asset.residentBlocks) {
			HashResidentBlock(hash, block);
		}
		for(const MotionDatabaseSample& sample : asset.samples) HashSample(hash, sample);
		return hash;
	}

	AnimationStatus ValidateMotionDatabaseAsset(const MotionDatabaseAsset& asset) noexcept {
		if(!asset.assetId.IsValid() || asset.compatibilityFingerprint == 0U
			|| asset.sourceFingerprint == 0U || asset.formatVersion != kMotionDatabaseAssetCurrentVersion
			|| asset.featureCount != kMotionDatabaseFeatureCount) {
			return AnimationStatus::InvalidArgument;
		}
		if(asset.samples.size() > kMotionDatabaseMaximumSamples) {
			return AnimationStatus::HardLimitExceeded;
		}
		for(std::size_t index = 0U; index < kMotionDatabaseFeatureCount; ++index) {
			if(!FinitePositive(asset.quantizationScales[index])
				|| !std::isfinite(asset.quantizationOffsets[index])) {
				return AnimationStatus::InvalidArgument;
			}
		}
		if(asset.featureBlocks != kMotionDatabaseDefaultFeatureBlocks
			|| asset.storageLayout.schemaVersion != 1U
			|| asset.storageLayout.samplesPerBlock == 0U
			|| asset.storageLayout.samplesPerBlock > kMotionDatabaseSamplesPerBlockDefault
			|| asset.storageLayout.cpuSampleStrideBytes != kMotionDatabaseCpuSampleStrideBytes
			|| asset.storageLayout.gpuSampleStrideBytes != kMotionDatabaseGpuSampleStrideBytes
			|| asset.storageLayout.maximumTopK != kMotionDatabaseMaximumTopK
			|| asset.storageLayout.flags != (kMotionStorageCpuResident
				| kMotionStorageGpuResident | kMotionStorageStreamableBlocks)) {
			return AnimationStatus::InvalidArgument;
		}
		std::uint64_t priorId = 0U;
		for(const MotionDatabaseSample& sample : asset.samples) {
			if(sample.stableSampleId == 0U || sample.sourceClipId == 0U
				|| (priorId != 0U && sample.stableSampleId <= priorId)) {
				return AnimationStatus::InvalidArgument;
			}
			priorId = sample.stableSampleId;
			if(!HasValidContinuityReference(asset, sample, sample.continuityPredecessorId)
				|| !HasValidContinuityReference(asset, sample, sample.continuitySuccessorId)) {
				return AnimationStatus::InvalidArgument;
			}
		}
		if(const AnimationStatus blockStatus = ValidateResidentBlocks(asset);
			blockStatus != AnimationStatus::Ok) {
			return blockStatus;
		}
		return asset.featureSchemaFingerprint == ComputeMotionDatabaseSchemaFingerprint(asset)
			? AnimationStatus::Ok : AnimationStatus::ContentHashMismatch;
	}

	AnimationStatus EncodeMotionDatabaseStreamBlock(
		const MotionDatabaseAsset& authoritativeAsset,
		std::uint32_t blockIndex,
		std::vector<std::uint8_t>& output) {
		try {
			if(const AnimationStatus status = ValidateMotionDatabaseAsset(authoritativeAsset);
				status != AnimationStatus::Ok) {
				return status;
			}
			return EncodeValidatedStreamBlock(authoritativeAsset, blockIndex, output);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus DecodeMotionDatabaseStreamBlock(
		const MotionDatabaseAsset& authoritativeAsset,
		std::uint32_t blockIndex,
		std::span<const std::uint8_t> encodedBlock,
		std::vector<MotionDatabaseSample>& output) {
		try {
			if(const AnimationStatus status = ValidateMotionDatabaseAsset(authoritativeAsset);
				status != AnimationStatus::Ok) {
				return status;
			}
			return DecodeValidatedStreamBlock(
				authoritativeAsset, blockIndex, encodedBlock, output);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus ValidateMotionDatabaseStreamChunk(
		const MotionDatabaseAsset& authoritativeAsset,
		std::span<const std::uint8_t> streamChunk) noexcept {
		try {
			if(const AnimationStatus status = ValidateMotionDatabaseAsset(authoritativeAsset);
				status != AnimationStatus::Ok) {
				return status;
			}
			return ValidateStreamChunkForValidatedAsset(authoritativeAsset, streamChunk);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus SerializeMotionDatabaseAsset(
		const MotionDatabaseAsset& asset, std::vector<std::uint8_t>& output) {
		try {
			MotionDatabaseAsset staged = asset;
			staged.runtimeGeneration = 0U;
			Canonicalize(staged);
			staged.featureBlocks = kMotionDatabaseDefaultFeatureBlocks;
			if(const AnimationStatus layoutStatus = RebuildMotionDatabaseStorageMetadata(staged);
				layoutStatus != AnimationStatus::Ok) {
				return layoutStatus;
			}
			staged.featureSchemaFingerprint = ComputeMotionDatabaseSchemaFingerprint(staged);
			if(const AnimationStatus status = ValidateMotionDatabaseAsset(staged);
				status != AnimationStatus::Ok) {
				return status;
			}

			// STRM duplicates the authoritative current records in block order. DATA
			// remains complete for old readers and deterministic fallback, while this
			// contiguous copy gives async IO one independently addressable file range
			// per resident block.
			Writer stream;
			for(const MotionDatabaseResidentBlock& block : staged.residentBlocks) {
				if(stream.Size() != block.cpuByteOffset) {
					return AnimationStatus::ContentHashMismatch;
				}
				const std::size_t blockStart = stream.Size();
				for(std::uint32_t offset = 0U; offset < block.sampleCount; ++offset) {
					if(!PutCurrentSample(stream, staged.samples[block.firstSample + offset])) {
						return AnimationStatus::RangeOverflow;
					}
				}
				if(stream.Size() - blockStart != block.cpuByteSize) {
					return AnimationStatus::ContentHashMismatch;
				}
			}
			const std::vector<std::uint8_t> streamBytes = stream.Take();
			if(const AnimationStatus streamStatus = ValidateStreamChunkForValidatedAsset(
				staged, streamBytes); streamStatus != AnimationStatus::Ok) {
				return streamStatus;
			}

			Writer payload;
			const std::uint32_t count = static_cast<std::uint32_t>(staged.samples.size());
			if(!payload.Put(kMotionDatabasePayloadMagic) || !payload.Put(staged.formatVersion)
				|| !payload.Put(staged.assetId) || !payload.Put(staged.compatibilityFingerprint)
				|| !payload.Put(staged.sourceFingerprint) || !payload.Put(staged.featureSchemaFingerprint)
				|| !payload.Put(staged.featureCount) || !payload.Put(staged.quantizationScales)
				|| !payload.Put(staged.quantizationOffsets) || !payload.Put(count)) {
				return AnimationStatus::RangeOverflow;
			}
			for(const MotionDatabaseSample& sample : staged.samples) {
				if(!payload.Put(MakeLegacyPrefix(sample))) {
					return AnimationStatus::RangeOverflow;
				}
			}

			const std::uint16_t blockCount = static_cast<std::uint16_t>(
				staged.featureBlocks.size());
			const std::uint32_t residentBlockCount = static_cast<std::uint32_t>(
				staged.residentBlocks.size());
			if(!payload.Put(kMotionDatabaseExtensionMagic)
				|| !payload.Put(kMotionDatabaseExtensionVersion)
				|| !payload.Put(blockCount)
				|| !payload.Put(staged.featureBlocks)
				|| !payload.Put(staged.storageLayout)
				|| !payload.Put(residentBlockCount)) {
				return AnimationStatus::RangeOverflow;
			}
			for(const MotionDatabaseResidentBlock& block : staged.residentBlocks) {
				if(!payload.Put(block)) return AnimationStatus::RangeOverflow;
			}
			if(!payload.Put(count)) return AnimationStatus::RangeOverflow;
			for(const MotionDatabaseSample& sample : staged.samples) {
				const MotionDatabaseSampleExtensionV1 extension{
					sample.exclusionFlags, 0U,
					sample.continuityPredecessorId, sample.continuitySuccessorId};
				if(!payload.Put(extension)) return AnimationStatus::RangeOverflow;
			}

			AnimationAssetWriter writer;
			const std::vector<std::uint8_t> bytes = payload.Take();
			if(!writer.AddChunk(kAnimationChunkPayload, true, bytes)) {
				return AnimationStatus::InternalError;
			}
			// STRM is optional because existing DATA-only v1 assets and readers remain
			// valid. New readers validate it before exposing any streamed range.
			if(!writer.AddChunk(kAnimationChunkStreamData, false, streamBytes)) {
				return AnimationStatus::InternalError;
			}
			AnimationAssetEnvelope envelope{};
			envelope.assetId = staged.assetId;
			envelope.compatibilityFingerprint = staged.compatibilityFingerprint;
			return writer.Finish(envelope, output);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus ResolveMotionDatabaseStreamBlockRange(
		std::span<const std::uint8_t> serializedAsset,
		std::uint32_t blockIndex,
		MotionDatabaseStreamBlockRange& output) noexcept {
		try {
			if(serializedAsset.size() > kMotionDatabaseMaximumSerializedBytes) {
				return AnimationStatus::HardLimitExceeded;
			}
			AnimationAssetReader assetReader;
			if(const AnimationStatus status = assetReader.Parse(serializedAsset);
				status != AnimationStatus::Ok) {
				return status;
			}
			MotionDatabaseAsset authoritativeAsset;
			if(const AnimationStatus status = DecodeMotionDatabaseFromReader(
				assetReader, 0U, authoritativeAsset); status != AnimationStatus::Ok) {
				return status;
			}
			const AnimationChunkEntry* streamEntry =
				FindChunkEntry(assetReader, kAnimationChunkStreamData);
			if(streamEntry == nullptr) {
				// DATA-only legacy files remain valid and use the complete resident
				// fallback; they simply cannot provide a direct streamed file range.
				return AnimationStatus::Unsupported;
			}
			if(blockIndex >= authoritativeAsset.residentBlocks.size()) {
				return AnimationStatus::InvalidArgument;
			}

			const MotionDatabaseResidentBlock& block =
				authoritativeAsset.residentBlocks[blockIndex];
			if(const AnimationStatus status = ValidateAnimationRange(
				streamEntry->size, block.cpuByteOffset, block.cpuByteSize);
				status != AnimationStatus::Ok) {
				return status;
			}
			std::uint64_t absoluteOffset = 0U;
			if(!CheckedAdd(streamEntry->offset, block.cpuByteOffset, absoluteOffset)) {
				return AnimationStatus::RangeOverflow;
			}
			if(const AnimationStatus status = ValidateAnimationRange(
				serializedAsset.size(), absoluteOffset, block.cpuByteSize);
				status != AnimationStatus::Ok) {
				return status;
			}

			MotionDatabaseStreamBlockRange staged;
			staged.blockIndex = block.blockIndex;
			staged.firstSample = block.firstSample;
			staged.sampleCount = block.sampleCount;
			staged.absoluteByteOffset = absoluteOffset;
			staged.byteSize = block.cpuByteSize;
			staged.contentFingerprint = block.contentFingerprint;
			output = staged;
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus DeserializeMotionDatabaseAsset(std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		MotionDatabaseAsset& output) {
		try {
			if(input.size() > kMotionDatabaseMaximumSerializedBytes) {
				return AnimationStatus::HardLimitExceeded;
			}
			AnimationAssetReader assetReader;
			if(const AnimationStatus status = assetReader.Parse(input);
				status != AnimationStatus::Ok) {
				return status;
			}
			MotionDatabaseAsset staged;
			if(const AnimationStatus status = DecodeMotionDatabaseFromReader(
				assetReader, expectedCompatibilityFingerprint, staged);
				status != AnimationStatus::Ok) {
				return status;
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}
}
