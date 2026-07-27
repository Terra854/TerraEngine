// MotionDatabaseAssetTests.cpp - Format and compiler-publication contracts.

#include "TestHarness.h"

#include "Animation/Assets/MotionDatabaseAsset.h"
#include "MotionDatabaseCompiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		AnimationGuid Guid(std::uint8_t value) {
			AnimationGuid result;
			for(std::size_t index = 0U; index < result.value.size(); ++index) result.value[index] = value + index;
			return result;
		}

		MotionDatabaseCompileInput MakeInput() {
			MotionDatabaseCompileInput input;
			input.assetId = Guid(10U);
			input.compatibilityFingerprint = 0x1234U;
			input.sourceFingerprint = 0x5678U;
			for(std::uint64_t id = 1U; id <= 3U; ++id) {
				MotionDatabaseSourceSample sample;
				sample.stableSampleId = id;
				sample.sourceClipId = 100U + id;
				sample.sourceTick = static_cast<AnimationFixedTick>(id * 10U);
				sample.phaseQuantized = static_cast<std::uint16_t>(id * 100U);
				sample.continuityGroup = static_cast<std::uint32_t>(id);
				for(std::size_t feature = 0U; feature < sample.features.size(); ++feature)
					sample.features[feature] = static_cast<float>(id * 10U + feature) / 100.0F;
				input.samples.push_back(sample);
			}
			return input;
		}

		MotionDatabaseCompileInput MakeStructuredInput() {
			MotionDatabaseCompileInput input;
			input.assetId = Guid(30U);
			input.compatibilityFingerprint = 0x2222U;
			input.sourceFingerprint = 0x3333U;
			input.samplesPerBlock = 1U;
			MotionDatabaseSourceClipInput clip;
			clip.sourceClipId = 700U;
			clip.sourceClipFingerprint = 0x4444U;
			clip.baseTagMask = 1U;
			clip.contactRanges.push_back({10, 10, 1U});
			clip.tagRanges.push_back({20, 20, 4U});
			clip.exclusionRanges.push_back({20, 20, 8U});
			for(AnimationFixedTick tick : {10, 20}) {
				MotionDatabaseClipSampleInput sample;
				sample.sourceTick = tick;
				sample.continuityGroup = 5U;
				sample.phaseQuantized = static_cast<std::uint16_t>(tick * 100);
				sample.transitionFlags = tick == 20 ? 2U : 1U;
				for(std::size_t horizon = 0U; horizon < sample.trajectory.size(); ++horizon) {
					sample.trajectory[horizon].positionX = static_cast<float>(tick + horizon);
					sample.trajectory[horizon].positionZ = static_cast<float>(horizon);
				}
				sample.posePoints[0].x = static_cast<float>(tick);
				sample.velocityPoints[0].z = static_cast<float>(tick) * 0.5F;
				clip.samples.push_back(sample);
			}
			input.sourceClips.push_back(std::move(clip));
			return input;
		}

		class ManualExecutor final : public MotionDatabaseCompileExecutor {
		public:
			void Enqueue(std::function<void()> work) override { work_.push_back(std::move(work)); }
			void Run() { auto work = std::move(work_.front()); work_.erase(work_.begin()); work(); }
		private:
			std::vector<std::function<void()>> work_;
		};

		template<typename Value>
		void AppendValue(std::vector<std::uint8_t>& bytes, const Value& value) {
			static_assert(std::is_trivially_copyable_v<Value>);
			const std::size_t oldSize = bytes.size();
			bytes.resize(oldSize + sizeof(Value));
			std::memcpy(bytes.data() + oldSize, &value, sizeof(Value));
		}

		void HashLegacyBytes(std::uint64_t& hash, const void* data, std::size_t size) {
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			for(std::size_t index = 0U; index < size; ++index) {
				hash = (hash ^ bytes[index]) * 1099511628211ULL;
			}
		}

		template<typename Value>
		void HashLegacyValue(std::uint64_t& hash, const Value& value) {
			HashLegacyBytes(hash, &value, sizeof(Value));
		}

		std::uint64_t LegacySchemaFingerprint(const MotionDatabaseAsset& asset) {
			std::uint64_t hash = 14695981039346656037ULL;
			HashLegacyValue(hash, asset.formatVersion);
			HashLegacyValue(hash, asset.featureCount);
			HashLegacyBytes(hash, asset.quantizationScales.data(),
				sizeof(asset.quantizationScales));
			HashLegacyBytes(hash, asset.quantizationOffsets.data(),
				sizeof(asset.quantizationOffsets));
			return hash;
		}

		#pragma pack(push, 1)
		// This is the original DATA-only record frozen by the production reader.
		// Keeping the fixture local prevents tests from accidentally making it a
		// new public type while still proving the historical 144-byte contract.
		struct LegacyMotionDatabaseSampleDiskV1 {
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
		#pragma pack(pop)

		static_assert(sizeof(LegacyMotionDatabaseSampleDiskV1) == 144U);

		std::vector<std::uint8_t> BuildLegacyDataOnlyAsset(
			const MotionDatabaseAsset& asset) {
			std::vector<std::uint8_t> payload;
			AppendValue(payload, kMotionDatabasePayloadMagic);
			AppendValue(payload, asset.formatVersion);
			AppendValue(payload, asset.assetId);
			AppendValue(payload, asset.compatibilityFingerprint);
			AppendValue(payload, asset.sourceFingerprint);
			AppendValue(payload, LegacySchemaFingerprint(asset));
			AppendValue(payload, asset.featureCount);
			AppendValue(payload, asset.quantizationScales);
			AppendValue(payload, asset.quantizationOffsets);
			AppendValue(payload, static_cast<std::uint32_t>(asset.samples.size()));
			for(const MotionDatabaseSample& sample : asset.samples) {
				LegacyMotionDatabaseSampleDiskV1 disk;
				disk.stableSampleId = sample.stableSampleId;
				disk.sourceClipId = sample.sourceClipId;
				disk.sourceTick = sample.sourceTick;
				disk.features = sample.features;
				disk.tagMask = sample.tagMask;
				disk.contactMask = sample.contactMask;
				disk.phaseQuantized = sample.phaseQuantized;
				disk.transitionFlags = sample.transitionFlags;
				disk.continuityGroup = sample.continuityGroup;
				AppendValue(payload, disk);
			}

			AnimationAssetWriter writer;
			static_cast<void>(writer.AddChunk(kAnimationChunkPayload, true, payload));
			AnimationAssetEnvelope envelope;
			envelope.assetId = asset.assetId;
			envelope.compatibilityFingerprint = asset.compatibilityFingerprint;
			std::vector<std::uint8_t> bytes;
			static_cast<void>(writer.Finish(envelope, bytes));
			return bytes;
		}

		std::vector<std::uint8_t> RewriteStreamChunk(
			std::span<const std::uint8_t> serializedAsset,
			bool includeStream,
			std::size_t streamSize) {
			AnimationAssetReader reader;
			if(reader.Parse(serializedAsset) != AnimationStatus::Ok) return {};
			const std::span<const std::uint8_t> payload =
				reader.FindChunk(kAnimationChunkPayload);
			const std::span<const std::uint8_t> stream =
				reader.FindChunk(kAnimationChunkStreamData);
			if(payload.empty() || streamSize > stream.size()) return {};
			AnimationAssetWriter writer;
			if(!writer.AddChunk(kAnimationChunkPayload, true, payload)) return {};
			if(includeStream
				&& !writer.AddChunk(kAnimationChunkStreamData, false, stream.first(streamSize))) {
				return {};
			}
			AnimationAssetEnvelope envelope;
			envelope.assetId = reader.Envelope().assetId;
			envelope.compatibilityFingerprint =
				reader.Envelope().compatibilityFingerprint;
			std::vector<std::uint8_t> bytes;
			return writer.Finish(envelope, bytes) == AnimationStatus::Ok
				? bytes : std::vector<std::uint8_t>{};
		}

		bool CorruptStreamAndRefreshEnvelope(std::vector<std::uint8_t>& bytes) {
			if(bytes.size() < sizeof(AnimationAssetEnvelope)) return false;
			AnimationAssetEnvelope envelope{};
			std::memcpy(&envelope, bytes.data(), sizeof(envelope));
			for(std::uint32_t index = 0U; index < envelope.chunkCount; ++index) {
				const std::size_t entryOffset =
					static_cast<std::size_t>(envelope.chunkTableOffset)
					+ static_cast<std::size_t>(index) * sizeof(AnimationChunkEntry);
				if(entryOffset > bytes.size()
					|| bytes.size() - entryOffset < sizeof(AnimationChunkEntry)) {
					return false;
				}
				AnimationChunkEntry entry{};
				std::memcpy(&entry, bytes.data() + entryOffset, sizeof(entry));
				if(entry.type != kAnimationChunkStreamData || entry.size == 0U
					|| entry.offset >= bytes.size()) {
					continue;
				}
				bytes[static_cast<std::size_t>(entry.offset)] ^= 0x5aU;
				envelope.contentHash = ComputeAnimationContentHash(
					std::span<const std::uint8_t>(bytes).subspan(
						sizeof(AnimationAssetEnvelope)));
				std::memcpy(bytes.data(), &envelope, sizeof(envelope));
				return true;
			}
			return false;
		}
	}

	void RegisterMotionDatabaseAssetTests(TestRegistry& registry) {
		registry.Add("Animation.MotionDatabase.AssetRoundTripsQuantizedSchemaAndFingerprint",
			[](TestContext& context) {
				MotionDatabaseAsset asset;
				MotionDatabaseCompileReport report;
				const MotionDatabaseCompileInput input = MakeInput();
				context.Expect(CompileMotionDatabase(input, asset, report) == AnimationStatus::Ok
					&& asset.featureCount == kMotionDatabaseFeatureCount
					&& asset.featureSchemaFingerprint == ComputeMotionDatabaseSchemaFingerprint(asset)
					&& report.contentFingerprint == ComputeMotionDatabaseContentFingerprint(asset),
					"Cooking must produce a reproducible 48-feature schema and content fingerprint.");
				std::vector<std::uint8_t> bytes;
				MotionDatabaseAsset restored;
				context.Expect(SerializeMotionDatabaseAsset(asset, bytes) == AnimationStatus::Ok
					&& DeserializeMotionDatabaseAsset(bytes, input.compatibilityFingerprint, restored) == AnimationStatus::Ok
					&& restored == asset,
					"A cooked database must round-trip without runtime generation or quantized-feature drift.");
			});

		registry.Add("Animation.MotionDatabase.AssetStreamChunkMapsExactContiguousBlockRanges",
			[](TestContext& context) {
				MotionDatabaseAsset asset;
				MotionDatabaseCompileReport report;
				context.Expect(CompileMotionDatabase(MakeStructuredInput(), asset, report)
					== AnimationStatus::Ok,
					"The stream fixture must cook explicit one-sample resident blocks.");
				std::vector<std::uint8_t> firstBytes;
				std::vector<std::uint8_t> secondBytes;
				context.Expect(SerializeMotionDatabaseAsset(asset, firstBytes) == AnimationStatus::Ok
					&& SerializeMotionDatabaseAsset(asset, secondBytes) == AnimationStatus::Ok
					&& firstBytes == secondBytes,
					"Adding STRM must preserve exact deterministic bytes across identical cooks.");

				AnimationAssetReader reader;
				context.Expect(reader.Parse(firstBytes) == AnimationStatus::Ok,
					"The common envelope must accept the DATA plus optional STRM chunks.");
				const std::span<const std::uint8_t> stream =
					reader.FindChunk(kAnimationChunkStreamData);
				context.Expect(stream.size() == asset.samples.size()
						* kMotionDatabaseCpuSampleStrideBytes
					&& ValidateMotionDatabaseStreamChunk(asset, stream) == AnimationStatus::Ok,
					"STRM must contain one exact 160-byte record for every authoritative DATA sample.");

				std::uint64_t expectedAbsoluteOffset = 0U;
				for(std::uint32_t blockIndex = 0U;
					blockIndex < asset.residentBlocks.size(); ++blockIndex) {
					MotionDatabaseStreamBlockRange range;
					context.Expect(ResolveMotionDatabaseStreamBlockRange(
						firstBytes, blockIndex, range) == AnimationStatus::Ok,
						"Every DATA resident block must resolve to one checked STRM file range.");
					const MotionDatabaseResidentBlock& block = asset.residentBlocks[blockIndex];
					if(blockIndex != 0U) {
						context.Expect(range.absoluteByteOffset == expectedAbsoluteOffset,
							"Adjacent resident blocks must occupy contiguous non-overlapping file ranges.");
					}
					expectedAbsoluteOffset = range.absoluteByteOffset + range.byteSize;
					context.Expect(range.blockIndex == blockIndex
						&& range.firstSample == block.firstSample
						&& range.sampleCount == block.sampleCount
						&& range.byteSize == block.cpuByteSize
						&& range.contentFingerprint == block.contentFingerprint
						&& range.absoluteByteOffset <= firstBytes.size()
						&& range.byteSize <= firstBytes.size() - range.absoluteByteOffset,
						"Resolved ranges must retain DATA identity and remain entirely inside the file.");

					std::vector<std::uint8_t> encoded;
					std::vector<MotionDatabaseSample> decoded;
					const std::span<const std::uint8_t> fileBlock =
						std::span<const std::uint8_t>(firstBytes).subspan(
							static_cast<std::size_t>(range.absoluteByteOffset),
							static_cast<std::size_t>(range.byteSize));
					context.Expect(EncodeMotionDatabaseStreamBlock(asset, blockIndex, encoded)
							== AnimationStatus::Ok && encoded.size() == 160U
							&& std::equal(encoded.begin(), encoded.end(), fileBlock.begin(), fileBlock.end()),
						"The public encoder and serialized block must share the exact current-record bytes.");
					context.Expect(DecodeMotionDatabaseStreamBlock(
							asset, blockIndex, fileBlock, decoded) == AnimationStatus::Ok
							&& decoded.size() == block.sampleCount
							&& decoded.front() == asset.samples[block.firstSample],
						"Checked decode must reproduce the authoritative DATA sample for this block.");
				}
			});

		registry.Add("Animation.MotionDatabase.AssetKeepsLegacyAndVersionOneDataOnlyCompatibility",
			[](TestContext& context) {
				MotionDatabaseAsset asset;
				MotionDatabaseCompileReport report;
				static_cast<void>(CompileMotionDatabase(MakeInput(), asset, report));
				std::vector<std::uint8_t> currentBytes;
				static_cast<void>(SerializeMotionDatabaseAsset(asset, currentBytes));
				const std::vector<std::uint8_t> versionOneDataOnly =
					RewriteStreamChunk(currentBytes, false, 0U);

				MotionDatabaseAsset restored;
				context.Expect(!versionOneDataOnly.empty()
					&& DeserializeMotionDatabaseAsset(versionOneDataOnly,
						asset.compatibilityFingerprint, restored) == AnimationStatus::Ok
					&& restored == asset,
					"The optional STRM addition must not make the existing extended DATA payload mandatory.");
				MotionDatabaseStreamBlockRange unchanged;
				unchanged.absoluteByteOffset = 77U;
				const MotionDatabaseStreamBlockRange before = unchanged;
				context.Expect(ResolveMotionDatabaseStreamBlockRange(
						versionOneDataOnly, 0U, unchanged) == AnimationStatus::Unsupported
					&& unchanged == before,
					"A valid DATA-only v1 asset must report unavailable range streaming without changing output.");

				const std::vector<std::uint8_t> legacyBytes = BuildLegacyDataOnlyAsset(asset);
				MotionDatabaseAsset upgraded;
				context.Expect(!legacyBytes.empty()
					&& DeserializeMotionDatabaseAsset(legacyBytes,
						asset.compatibilityFingerprint, upgraded) == AnimationStatus::Ok
					&& upgraded == asset,
					"The original 144-byte DATA records must still upgrade to current samples and block metadata.");
			});

		registry.Add("Animation.MotionDatabase.AssetRejectsCorruptOrTruncatedStreamAtomically",
			[](TestContext& context) {
				MotionDatabaseAsset asset;
				MotionDatabaseCompileReport report;
				static_cast<void>(CompileMotionDatabase(MakeStructuredInput(), asset, report));
				std::vector<std::uint8_t> blockBytes;
				static_cast<void>(EncodeMotionDatabaseStreamBlock(asset, 0U, blockBytes));

				std::vector<MotionDatabaseSample> lastGood(1U);
				lastGood.front().stableSampleId = 999U;
				const std::vector<MotionDatabaseSample> before = lastGood;
				context.Expect(DecodeMotionDatabaseStreamBlock(asset, 0U,
						std::span<const std::uint8_t>(blockBytes).first(blockBytes.size() - 1U),
						lastGood) == AnimationStatus::TruncatedInput
					&& lastGood == before,
					"A short block must fail before replacing the caller's last-good decoded samples.");
				std::vector<std::uint8_t> corruptBlock = blockBytes;
				corruptBlock.back() ^= 0x01U;
				context.Expect(DecodeMotionDatabaseStreamBlock(
						asset, 0U, corruptBlock, lastGood) == AnimationStatus::ContentHashMismatch
					&& lastGood == before,
					"A same-sized block with changed bytes must fail its DATA block fingerprint atomically.");
				std::vector<std::uint8_t> oversizedBlock = blockBytes;
				oversizedBlock.push_back(0U);
				context.Expect(DecodeMotionDatabaseStreamBlock(
						asset, 0U, oversizedBlock, lastGood) == AnimationStatus::InvalidChunk
					&& lastGood == before,
					"Extra bytes may not blur the exact boundary between adjacent resident blocks.");

				std::vector<std::uint8_t> serialized;
				static_cast<void>(SerializeMotionDatabaseAsset(asset, serialized));
				AnimationAssetReader reader;
				static_cast<void>(reader.Parse(serialized));
				const std::span<const std::uint8_t> stream =
					reader.FindChunk(kAnimationChunkStreamData);
				const std::vector<std::uint8_t> truncatedAsset =
					RewriteStreamChunk(serialized, true, stream.size() - 1U);
				MotionDatabaseAsset lastGoodAsset = asset;
				lastGoodAsset.assetId = Guid(99U);
				const MotionDatabaseAsset beforeAsset = lastGoodAsset;
				context.Expect(DeserializeMotionDatabaseAsset(truncatedAsset,
						asset.compatibilityFingerprint, lastGoodAsset)
						== AnimationStatus::TruncatedInput
					&& lastGoodAsset == beforeAsset,
					"A valid envelope with a short STRM chunk must not publish partial DATA or stream state.");

				std::vector<std::uint8_t> corruptAsset = serialized;
				context.Expect(CorruptStreamAndRefreshEnvelope(corruptAsset),
					"The corruption fixture must change STRM while keeping the outer envelope hash valid.");
				context.Expect(DeserializeMotionDatabaseAsset(corruptAsset,
						asset.compatibilityFingerprint, lastGoodAsset)
						== AnimationStatus::ContentHashMismatch
					&& lastGoodAsset == beforeAsset,
					"STRM must match authoritative DATA even when the outer content hash was recomputed.");
			});

		registry.Add("Animation.MotionDatabase.AssetRejectsCorruptionBoundsAndPreservesLastGood",
			[](TestContext& context) {
				MotionDatabaseAsset asset;
				MotionDatabaseCompileReport report;
				static_cast<void>(CompileMotionDatabase(MakeInput(), asset, report));
				std::vector<std::uint8_t> bytes;
				static_cast<void>(SerializeMotionDatabaseAsset(asset, bytes));
				MotionDatabaseAsset lastGood = asset;
				lastGood.assetId = Guid(90U);
				bytes.back() ^= 0x55U;
				context.Expect(DeserializeMotionDatabaseAsset(bytes, asset.compatibilityFingerprint, lastGood)
						== AnimationStatus::ContentHashMismatch && lastGood.assetId == Guid(90U),
					"A corrupt envelope must not replace a caller's complete last-good database.");
				MotionDatabaseCompileInput oversized = MakeInput();
				oversized.samples.resize(kMotionDatabaseMaximumSamples + 1U);
				context.Expect(CompileMotionDatabase(oversized, lastGood, report) == AnimationStatus::HardLimitExceeded,
					"The compiler must reject declared sample hard limits before allocating a cooked database.");
			});

		registry.Add("Animation.MotionDatabase.CompilerCancellationAndAtomicPublication",
			[](TestContext& context) {
				auto executor = std::make_shared<ManualExecutor>();
				MotionDatabaseCompiler compiler(executor);
				const MotionDatabaseCompileInput first = MakeInput();
				context.Expect(compiler.RequestBuild(first) == AnimationStatus::Queued
					&& compiler.PollAndPublish(first.assetId, first.compatibilityFingerprint) == AnimationStatus::NotReady,
					"Polling must never wait for a database build worker.");
				executor->Run();
				context.Expect(compiler.PollAndPublish(first.assetId, first.compatibilityFingerprint) == AnimationStatus::Ok
					&& compiler.GetPublished().has_value(),
					"One completed, matching worker product must atomically publish a first immutable generation.");
				const auto lastGood = compiler.GetPublished();
				context.Expect(compiler.RequestBuild(first) == AnimationStatus::Queued,
					"A compatible rebuild should be accepted into the bounded pending queue.");
				compiler.CancelPending();
				executor->Run();
				const auto afterCancellation = compiler.GetPublished();
				context.Expect(compiler.PollAndPublish(first.assetId, first.compatibilityFingerprint) == AnimationStatus::Cancelled
					&& afterCancellation.has_value() && lastGood.has_value()
					&& afterCancellation->generation == lastGood->generation
					&& afterCancellation->contentFingerprint == lastGood->contentFingerprint,
					"Cancellation must preserve the complete previous generation rather than publishing partial normalization.");
			});

		registry.Add("Animation.MotionDatabase.CompilerExtractsNamedBlocksAndResidentLayouts",
			[](TestContext& context) {
				MotionDatabaseAsset asset;
				MotionDatabaseCompileReport report;
				const MotionDatabaseCompileInput input = MakeStructuredInput();
				context.Expect(CompileMotionDatabase(input, asset, report) == AnimationStatus::Ok
					&& report.sourceSamples == 0U && report.extractedSamples == 2U
					&& report.residentBlocks == 2U,
					"Semantic clip inputs must extract deterministic samples and build the configured resident blocks.");
				context.Expect(asset.featureBlocks == kMotionDatabaseDefaultFeatureBlocks
					&& asset.storageLayout.cpuSampleStrideBytes == sizeof(MotionDatabaseSample)
					&& asset.storageLayout.gpuSampleStrideBytes == sizeof(MotionDatabaseGpuSample)
					&& asset.residentBlocks[1].firstSample == 1U,
					"The cooked asset must publish explicit named feature ranges and matching CPU/GPU byte layouts.");
				const auto first = std::find_if(asset.samples.begin(), asset.samples.end(),
					[](const MotionDatabaseSample& sample) { return sample.sourceTick == 10; });
				const auto second = std::find_if(asset.samples.begin(), asset.samples.end(),
					[](const MotionDatabaseSample& sample) { return sample.sourceTick == 20; });
				context.Expect(first != asset.samples.end() && second != asset.samples.end()
					&& first->tagMask == 1U && first->contactMask == 1U
					&& second->tagMask == 5U && second->exclusionFlags == 8U
					&& first->continuitySuccessorId == second->stableSampleId
					&& second->continuityPredecessorId == first->stableSampleId,
					"Contact/tag/exclusion ranges and continuity edges must resolve from source ticks before quantization.");
				const MotionDatabaseGpuSample gpu = ExpandMotionDatabaseSampleForGpu(*first);
				context.Expect(gpu.stableSampleId == first->stableSampleId
					&& gpu.features[0] == first->features.values[0]
					&& gpu.transitionFlags == first->transitionFlags,
					"GPU expansion must preserve every authoritative quantized value and metadata field.");
			});

		registry.Add("Animation.MotionDatabase.DirectCancellationLeavesOutputsUntouched",
			[](TestContext& context) {
				MotionDatabaseAsset output;
				output.assetId = Guid(99U);
				MotionDatabaseCompileReport report;
				report.diagnostic = "sentinel";
				std::atomic_bool cancelled = true;
				context.Expect(CompileMotionDatabase(MakeStructuredInput(), output, report,
						MotionDatabaseCompileCancellation{&cancelled}) == AnimationStatus::Cancelled
					&& output.assetId == Guid(99U) && report.diagnostic == "sentinel",
					"A cancellation observed between compiler stages must not expose a partial asset or report.");
			});
	}
}
