// AnimationFormatContractTests.cpp - Frozen v1 binary contract and safety tests.

#include "TestHarness.h"

#include "Animation/AnimationTypes.h"
#include "File Formats/TerraEngineAnimationFile.h"
#include "Resource/ResourceManager.h"
#include "model/PackModel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid MakeGuid(std::uint8_t seed) {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		template<typename Value>
		Value ReadValue(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
			Value value{};
			std::memcpy(&value, bytes.data() + offset, sizeof(Value));
			return value;
		}

		template<typename Value>
		void WriteValue(std::vector<std::uint8_t>& bytes, std::size_t offset, const Value& value) {
			std::memcpy(bytes.data() + offset, &value, sizeof(Value));
		}

		std::vector<std::uint8_t> WriteValidAsset(bool reverseInputOrder = false) {
			AnimationAssetWriter writer;
			const std::array<std::uint8_t, 3U> metadata{{1U, 2U, 3U}};
			const std::array<std::uint8_t, 4U> payload{{4U, 5U, 6U, 7U}};
			const AnimationDependencyEntry first{MakeGuid(1U), 3U, 2U};
			const AnimationDependencyEntry second{MakeGuid(9U), 2U, 7U};
			if(reverseInputOrder) {
				writer.AddDependency(second);
				writer.AddDependency(first);
				writer.AddChunk(kAnimationChunkPayload, true, payload);
				writer.AddChunk(kAnimationChunkMetadata, false, metadata);
			} else {
				writer.AddDependency(first);
				writer.AddDependency(second);
				writer.AddChunk(kAnimationChunkMetadata, false, metadata);
				writer.AddChunk(kAnimationChunkPayload, true, payload);
			}
			AnimationAssetEnvelope envelope;
			envelope.assetId = MakeGuid(42U);
			envelope.compatibilityFingerprint = 0x7a4bd19e8c205f13ULL;
			std::vector<std::uint8_t> bytes;
			if(writer.Finish(envelope, bytes) != AnimationStatus::Ok) {
				return {};
			}
			return bytes;
		}
	}

	void RegisterAnimationFormatContractTests(TestRegistry& registry) {
		registry.Add("Animation.Format.StableTypesAndAppendOnlyNumericValues", [](TestContext& context) {
			// These explicit assertions make accidental enum renumbering a test failure
			// before a cooker writes assets that no older runtime can interpret.
			static_assert(static_cast<std::uint16_t>(AnimationStatus::Ok) == 0U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::Queued) == 1U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::NotReady) == 2U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::BufferTooSmall) == 3U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::InvalidArgument) == 4U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::InvalidType) == 5U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::MissingAsset) == 6U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::IncompatibleAsset) == 7U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::StaleHandle) == 8U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::Cancelled) == 9U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::Unsupported) == 10U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::DeterminismViolation) == 11U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::RendererUnavailable) == 12U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::Rejected) == 13U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::InternalError) == 14U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::UnsupportedMajorVersion) == 15U);
			static_assert(static_cast<std::uint16_t>(AnimationStatus::HardLimitExceeded) == 20U);
			static_assert(static_cast<std::uint8_t>(AnimationLimit::SkeletonJoints) == 0U);
			static_assert(static_cast<std::uint8_t>(AnimationLimit::ConcurrentAutomationJobs) == 26U);
			context.Expect(sizeof(AnimationAssetHandle) == 20U,
				"Generation-safe asset handles must retain GUID and generation fields.");
			context.Expect(AnimationAssetHandle{MakeGuid(1U), 1U}.IsValid(),
				"Nonzero GUID and generation should form a valid handle.");
			context.Expect(!AnimationAssetHandle{MakeGuid(1U), 0U}.IsValid(),
				"Generation zero must remain invalid so stale handles cannot alias new assets.");
		});

		registry.Add("Animation.Format.FoundationAssetAndArchiveIdsAreAppendOnly", [](TestContext& context) {
			// Runtime and archive values are persisted outside the animation module.
			// Pin both the old values and each appended animation value so a future
			// edit cannot silently reinterpret an existing scene or TEPAK entry.
			static_assert(static_cast<std::uint32_t>(AssetType::Mesh) == 0U);
			static_assert(static_cast<std::uint32_t>(AssetType::CookedCollider) == 5U);
			static_assert(static_cast<std::uint32_t>(AssetType::Skeleton) == 6U);
			static_assert(static_cast<std::uint32_t>(AssetType::AnimationClip) == 7U);
			static_assert(static_cast<std::uint32_t>(AssetType::AnimationController) == 8U);
			static_assert(static_cast<std::uint32_t>(AssetType::AnimationMask) == 9U);
			static_assert(static_cast<std::uint32_t>(AssetType::RetargetProfile) == 10U);
			static_assert(static_cast<std::uint32_t>(AssetType::MotionDatabase) == 11U);
			static_assert(static_cast<std::uint32_t>(AssetType::RagdollProfile) == 12U);

			using TerraEngine::Resource::ResidencyAssetType;
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::Mesh) == 0U);
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::Other) == 6U);
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::Skeleton) == 7U);
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::AnimationClip) == 8U);
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::AnimationController) == 9U);
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::AnimationMask) == 10U);
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::RetargetProfile) == 11U);
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::MotionDatabase) == 12U);
			static_assert(static_cast<std::uint8_t>(ResidencyAssetType::RagdollProfile) == 13U);

			using tepak::model::ArchiveAssetClass;
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::Mesh) == 1U);
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::Other) == 7U);
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::Skeleton) == 8U);
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::AnimationClip) == 9U);
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::AnimationController) == 10U);
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::AnimationMask) == 11U);
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::RetargetProfile) == 12U);
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::MotionDatabase) == 13U);
			static_assert(static_cast<std::uint32_t>(ArchiveAssetClass::RagdollProfile) == 14U);
			const std::array<std::pair<AssetType, ResidencyAssetType>, 7U> residencyMappings{{
				{AssetType::Skeleton, ResidencyAssetType::Skeleton},
				{AssetType::AnimationClip, ResidencyAssetType::AnimationClip},
				{AssetType::AnimationController, ResidencyAssetType::AnimationController},
				{AssetType::AnimationMask, ResidencyAssetType::AnimationMask},
				{AssetType::RetargetProfile, ResidencyAssetType::RetargetProfile},
				{AssetType::MotionDatabase, ResidencyAssetType::MotionDatabase},
				{AssetType::RagdollProfile, ResidencyAssetType::RagdollProfile}
			}};
			for(const auto& [assetType, residencyType] : residencyMappings) {
				context.Expect(TerraEngine::Resource::ToResidencyAssetType(assetType) == residencyType,
					"Every appended animation asset must retain its distinct residency bucket.");
				context.Expect(TerraEngine::Resource::ToString(residencyType) != "other",
					"Animation residency diagnostics must expose a stable type label.");
			}
		});

		registry.Add("Animation.Format.VersionedEnvelopeAndDeterministicRoundTrip", [](TestContext& context) {
			const std::vector<std::uint8_t> forward = WriteValidAsset(false);
			const std::vector<std::uint8_t> reverse = WriteValidAsset(true);
			context.Expect(!forward.empty(), "The v1 writer should emit a complete envelope.");
			context.Expect(forward == reverse,
				"Dependency and chunk input order must not change deterministic v1 bytes.");

			AnimationAssetReader reader;
			context.Expect(reader.Parse(forward) == AnimationStatus::Ok,
				"A freshly written v1 envelope should round-trip through the bounded reader.");
			context.Expect(reader.Envelope().magic == kAnimationAssetMagic,
				"The reader must preserve the frozen animation magic value.");
			context.Expect(reader.Envelope().majorVersion == kAnimationAssetMajorVersion,
				"The reader must preserve the frozen v1 major version.");
			context.Expect(reader.Envelope().headerSize == sizeof(AnimationAssetEnvelope),
				"Header size is part of the fixed v1 binary contract.");
			context.Expect(reader.Envelope().contentHash == ComputeAnimationContentHash(
				std::span<const std::uint8_t>(forward).subspan(sizeof(AnimationAssetEnvelope))),
				"The frozen v1 content hash must identify the emitted payload bytes.");
			context.Expect(reader.Dependencies().size() == 2U && reader.Chunks().size() == 2U,
				"Round-trip data should retain dependency and chunk tables.");
			const std::array<std::uint8_t, 4U> expectedPayload{{4U, 5U, 6U, 7U}};
			const std::span<const std::uint8_t> payload = reader.FindChunk(kAnimationChunkPayload);
			context.Expect(std::equal(payload.begin(), payload.end(), expectedPayload.begin(), expectedPayload.end()),
				"Chunk payload bytes should remain unchanged through the round trip.");
		});

		registry.Add("Animation.Format.HeadersVersionsAndUnknownChunksFailClosed", [](TestContext& context) {
			std::vector<std::uint8_t> bytes = WriteValidAsset();
			AnimationAssetEnvelope envelope = ReadValue<AnimationAssetEnvelope>(bytes, 0U);
			envelope.majorVersion = kAnimationAssetMajorVersion + 1U;
			WriteValue(bytes, 0U, envelope);
			AnimationAssetReader reader;
			context.Expect(reader.Parse(bytes) == AnimationStatus::UnsupportedMajorVersion,
				"An unknown major must be rejected rather than guessed.");

			bytes = WriteValidAsset();
			envelope = ReadValue<AnimationAssetEnvelope>(bytes, 0U);
			envelope.minorVersion = kAnimationAssetMinorVersion + 1U;
			WriteValue(bytes, 0U, envelope);
			context.Expect(reader.Parse(bytes) == AnimationStatus::Ok,
				"A newer minor remains readable when every encountered required chunk is known.");

			AnimationAssetWriter optionalWriter;
			const std::array<std::uint8_t, 1U> byte{{9U}};
			optionalWriter.AddChunk(MakeAnimationFourCC('N', 'E', 'W', '!'), false, byte);
			std::vector<std::uint8_t> optionalBytes;
			context.Expect(optionalWriter.Finish({}, optionalBytes) == AnimationStatus::Ok,
				"An optional unknown chunk is valid writer input for forward compatibility testing.");
			context.Expect(reader.Parse(optionalBytes) == AnimationStatus::Ok,
				"Unknown optional chunks must be skipped by their recorded size.");

			AnimationAssetWriter requiredWriter;
			requiredWriter.AddChunk(MakeAnimationFourCC('N', 'E', 'W', '!'), true, byte);
			std::vector<std::uint8_t> requiredBytes;
			requiredWriter.Finish({}, requiredBytes);
			context.Expect(reader.Parse(requiredBytes) == AnimationStatus::UnknownRequiredChunk,
				"Unknown required chunks must reject publication without partial state.");
		});

		registry.Add("Animation.Format.CheckedRangesRejectTruncationOverflowAndMalformedOffsets", [](TestContext& context) {
			AnimationAssetReader reader;
			std::vector<std::uint8_t> truncated(sizeof(AnimationAssetEnvelope) - 1U, 0U);
			context.Expect(reader.Parse(truncated) == AnimationStatus::TruncatedInput,
				"An incomplete envelope must be rejected before reading any table.");

			std::vector<std::uint8_t> bytes = WriteValidAsset();
			AnimationAssetEnvelope envelope = ReadValue<AnimationAssetEnvelope>(bytes, 0U);
			envelope.dependencyTableOffset = std::numeric_limits<std::uint64_t>::max() - 1U;
			WriteValue(bytes, 0U, envelope);
			context.Expect(reader.Parse(bytes) == AnimationStatus::RangeOverflow,
				"Offset plus table size must use checked 64-bit arithmetic.");

			bytes = WriteValidAsset();
			envelope = ReadValue<AnimationAssetEnvelope>(bytes, 0U);
			AnimationChunkEntry chunk = ReadValue<AnimationChunkEntry>(bytes, static_cast<std::size_t>(envelope.chunkTableOffset));
			chunk.offset = bytes.size();
			chunk.size = 1U;
			WriteValue(bytes, static_cast<std::size_t>(envelope.chunkTableOffset), chunk);
			context.Expect(reader.Parse(bytes) == AnimationStatus::RangeOutOfBounds,
				"A chunk range beyond the input must be rejected before access or allocation.");

			bytes = WriteValidAsset();
			envelope = ReadValue<AnimationAssetEnvelope>(bytes, 0U);
			envelope.chunkTableOffset = 0U;
			WriteValue(bytes, 0U, envelope);
			context.Expect(reader.Parse(bytes) == AnimationStatus::InvalidEnvelope,
				"Metadata tables may never alias the fixed envelope prefix.");

			bytes = WriteValidAsset();
			envelope = ReadValue<AnimationAssetEnvelope>(bytes, 0U);
			envelope.chunkTableOffset = envelope.dependencyTableOffset;
			WriteValue(bytes, 0U, envelope);
			context.Expect(reader.Parse(bytes) == AnimationStatus::InvalidEnvelope,
				"Dependency and chunk tables must not overlap even when both ranges are in bounds.");

			bytes = WriteValidAsset();
			bytes.back() ^= 0x01U;
			context.Expect(reader.Parse(bytes) == AnimationStatus::ContentHashMismatch,
				"A changed payload byte must fail the frozen content-hash integrity check.");
		});

		registry.Add("Animation.Format.AllVersionOneHardLimitsRejectTheNextValue", [](TestContext& context) {
			constexpr std::array<std::array<std::uint64_t, 2U>, 27U> expected{{
				{{256U, 4096U}}, {{256U, 256U}}, {{4U, 4U}}, {{64U, 1024U}}, {{256U, 4096U}},
				{{1800U, 86400U}}, {{16384U, 65536U}}, {{4194304U, 16777216U}}, {{256U, 4096U}},
				{{8192U, 65536U}}, {{16U, 64U}}, {{8U, 32U}}, {{16U, 32U}}, {{1024U, 4096U}},
				{{256U, 4096U}}, {{65536U, 1048576U}}, {{1024U, 4096U}}, {{2048U, 8192U}},
				{{64U, 256U}}, {{32U, 128U}}, {{1000000U, 16777216U}}, {{32U, 256U}},
				{{1800U, 14400U}}, {{60U, 240U}}, {{240U, 4096U}}, {{600U, 36000U}}, {{16U, 64U}}
			}};
			context.Expect(kAnimationRuntimeLimits.size() == 27U,
				"The version-1 contract has exactly 27 independently queryable hard limits.");
			for(std::size_t index = 0U; index < kAnimationRuntimeLimits.size(); ++index) {
				const AnimationLimitValues& limit = kAnimationRuntimeLimits[index];
				context.Expect(limit.operationalDefault == expected[index][0U] && limit.hardMaximum == expected[index][1U],
					"Every operational and hard v1 limit must match the frozen contract exactly.");
				context.Expect(ValidateAnimationHardLimit(limit.limit, limit.hardMaximum) == AnimationStatus::Ok,
					"The documented hard maximum must remain accepted.");
				context.Expect(ValidateAnimationHardLimit(limit.limit, limit.hardMaximum + 1U) == AnimationStatus::HardLimitExceeded,
					"Every value immediately above a v1 hard limit must be a structured rejection.");
			}
			const AnimationLimit malformed = static_cast<AnimationLimit>(0xffU);
			context.Expect(TryGetAnimationRuntimeLimit(malformed) == nullptr,
				"An untrusted numeric limit ID must not index outside the frozen table.");
			context.Expect(ValidateAnimationHardLimit(malformed, 0U) == AnimationStatus::InvalidArgument,
				"An unknown limit ID must fail with a structured status rather than undefined behavior.");
		});
	}
}
