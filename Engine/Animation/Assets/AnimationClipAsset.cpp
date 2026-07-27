// AnimationClipAsset.cpp - Checked deterministic .teanim table serialization.

#include "Animation/Assets/AnimationClipAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::uint32_t kRootMotionEnabled = 1U << 0U;
		constexpr std::uint32_t kRecordingProvenanceMagic = MakeAnimationFourCC('R', 'E', 'C', 'D');
		constexpr std::uint16_t kRecordingProvenanceVersion = 1U;
		constexpr std::uint64_t kMaximumRecordingMetadataBytes = 4U * 1024U * 1024U;
		constexpr std::uint64_t kMaximumRecordingIdentityBytes = 16U * 1024U;
		constexpr std::uint64_t kMaximumRecordedChannelNameBytes = 4U * 1024U;

		#pragma pack(push, 1)
		struct ClipSegmentDiskV1 {
			AnimationFixedTick startTick;
			AnimationFixedTick endTick;
			std::uint64_t streamOffset;
			std::uint64_t streamSize;
			std::uint32_t firstTrack;
			std::uint32_t trackCount;
		};

		struct ClipTrackDiskV1 {
			std::uint64_t stableTargetId;
			std::uint8_t kind;
			std::uint8_t interpolation;
			std::uint16_t reserved;
			std::uint32_t segmentIndex;
			std::uint64_t sampleOffset;
			std::uint32_t sampleCount;
			std::uint32_t encodedValueBits;
		};

		struct NamedChannelDiskV1 {
			std::uint64_t stableId;
			std::uint64_t nameOffset;
			std::uint64_t nameSize;
		};

		struct ClipEventDiskV1 {
			std::uint64_t stableEventId;
			AnimationFixedTick tick;
			std::uint8_t payloadType;
			std::uint8_t flags;
			std::uint16_t reserved;
			std::uint32_t reserved2;
			std::uint64_t payloadOffset;
			std::uint64_t payloadSize;
		};

		struct RecordingProvenanceDiskV1 {
			std::uint32_t magic;
			std::uint16_t version;
			std::uint16_t headerSize;
			AnimationGuid sourceAnimatorId;
			AnimationGuid sourceControllerAssetId;
			AnimationFixedTick sourceFirstTick;
			AnimationFixedTick sourceLastTick;
			std::uint32_t sourceSampleRate;
			std::uint8_t clock;
			std::uint8_t reserved[3];
			std::uint64_t capturedSampleCount;
			std::uint64_t capturedEventCount;
			std::uint64_t sourceKeyCount;
			std::uint64_t retainedKeyCount;
			std::uint64_t removedKeyCount;
			AnimationClipRecordingReductionSettings reduction;
			std::uint32_t channelCount;
			std::uint32_t reserved2;
			std::uint64_t channelTableOffset;
			std::uint64_t stringTableOffset;
			std::uint64_t stringTableSize;
			std::uint64_t sourceSceneOffset;
			std::uint64_t sourceSceneSize;
			std::uint64_t sourceSessionOffset;
			std::uint64_t sourceSessionSize;
		};

		struct RecordedChannelDiskV1 {
			std::uint64_t stableChannelId;
			std::uint64_t stableTargetId;
			std::uint8_t kind;
			std::uint8_t interpolation;
			std::uint16_t reserved;
			std::uint32_t reserved2;
			std::uint64_t nameOffset;
			std::uint64_t nameSize;
		};
		#pragma pack(pop)

		bool IsKnownTrackKind(AnimationClipTrackKind kind) noexcept {
			return kind >= AnimationClipTrackKind::Translation
				&& kind <= AnimationClipTrackKind::RootYaw;
		}

		bool IsKnownInterpolation(AnimationInterpolation interpolation) noexcept {
			return interpolation >= AnimationInterpolation::Constant
				&& interpolation <= AnimationInterpolation::Cubic;
		}

		bool IsKnownPayloadType(AnimationEventPayloadType type) noexcept {
			return type >= AnimationEventPayloadType::None
				&& type <= AnimationEventPayloadType::Boolean;
		}

		bool IsKnownRecordingClock(AnimationClipRecordingClock clock) noexcept {
			return clock >= AnimationClipRecordingClock::FixedSimulation
				&& clock <= AnimationClipRecordingClock::Manual;
		}

		bool IsKnownRecordedChannelKind(AnimationClipRecordedChannelKind kind) noexcept {
			return kind >= AnimationClipRecordedChannelKind::BoneTranslation
				&& kind <= AnimationClipRecordedChannelKind::RootYaw;
		}

		bool IsFiniteNonNegative(float value) noexcept {
			return std::isfinite(value) && value >= 0.0F;
		}

		bool IsFiniteReductionSettings(
			const AnimationClipRecordingReductionSettings& settings) noexcept {
			for(const float tolerance : {
				settings.translationToleranceMeters,
				settings.rotationToleranceDegrees,
				settings.scaleToleranceFraction,
				settings.morphTolerance,
				settings.curveTolerance}) {
				if(!IsFiniteNonNegative(tolerance)) {
					return false;
				}
			}
			return true;
		}

		AnimationClipTrackKind TrackKindForRecordedChannel(
			AnimationClipRecordedChannelKind kind) noexcept {
			switch(kind) {
			case AnimationClipRecordedChannelKind::BoneTranslation:
				return AnimationClipTrackKind::Translation;
			case AnimationClipRecordedChannelKind::BoneRotation:
				return AnimationClipTrackKind::Rotation;
			case AnimationClipRecordedChannelKind::BoneScale:
				return AnimationClipTrackKind::Scale;
			case AnimationClipRecordedChannelKind::MorphWeight:
				return AnimationClipTrackKind::MorphWeight;
			case AnimationClipRecordedChannelKind::Curve:
				return AnimationClipTrackKind::Curve;
			case AnimationClipRecordedChannelKind::RootTranslation:
				return AnimationClipTrackKind::RootTranslation;
			case AnimationClipRecordedChannelKind::RootYaw:
				return AnimationClipTrackKind::RootYaw;
			case AnimationClipRecordedChannelKind::Event:
				break;
			}
			return AnimationClipTrackKind::Curve;
		}

		template<typename Record>
		AnimationStatus AppendRecords(
			std::vector<std::uint8_t>& bytes,
			std::span<const Record> records,
			std::uint64_t& offset) {
			offset = bytes.size();
			std::uint64_t byteCount = 0U;
			std::uint64_t finalSize = 0U;
			if(!CheckedMultiply(records.size(), sizeof(Record), byteCount)
				|| !CheckedAdd(bytes.size(), byteCount, finalSize)
				|| finalSize > std::numeric_limits<std::size_t>::max()) {
				return AnimationStatus::RangeOverflow;
			}
			const std::size_t oldSize = bytes.size();
			bytes.resize(static_cast<std::size_t>(finalSize));
			if(!records.empty()) {
				std::memcpy(bytes.data() + oldSize, records.data(), static_cast<std::size_t>(byteCount));
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus AppendString(
			std::vector<std::uint8_t>& strings,
			const std::string& value,
			std::uint64_t& offset,
			std::uint64_t& size) {
			offset = strings.size();
			size = value.size();
			std::uint64_t finalSize = 0U;
			if(!CheckedAdd(strings.size(), value.size(), finalSize)
				|| finalSize > std::numeric_limits<std::size_t>::max()) {
				return AnimationStatus::RangeOverflow;
			}
			strings.insert(strings.end(), value.begin(), value.end());
			return AnimationStatus::Ok;
		}

		template<typename Record>
		AnimationStatus ReadRecords(
			std::span<const std::uint8_t> payload,
			std::uint64_t offset,
			std::uint32_t count,
			std::vector<Record>& output) {
			std::uint64_t byteCount = 0U;
			if(!CheckedMultiply(count, sizeof(Record), byteCount)) {
				return AnimationStatus::RangeOverflow;
			}
			if(const AnimationStatus status = ValidateAnimationRange(payload.size(), offset, byteCount);
				status != AnimationStatus::Ok) {
				return status;
			}
			output.resize(count);
			if(byteCount != 0U) {
				std::memcpy(output.data(), payload.data() + static_cast<std::size_t>(offset),
					static_cast<std::size_t>(byteCount));
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus ReadString(
			std::span<const std::uint8_t> strings,
			std::uint64_t offset,
			std::uint64_t size,
			std::string& output) {
			if(const AnimationStatus status = ValidateAnimationRange(strings.size(), offset, size);
				status != AnimationStatus::Ok) {
				return status;
			}
			output.assign(
				reinterpret_cast<const char*>(strings.data() + static_cast<std::size_t>(offset)),
				static_cast<std::size_t>(size));
			return AnimationStatus::Ok;
		}

		AnimationStatus ValidateRecordingProvenance(
			const AnimationClipAsset& asset,
			const AnimationClipRecordingProvenance& provenance) noexcept {
			if(!provenance.sourceAnimatorId.IsValid()
				|| provenance.sourceSceneIdentity.empty()
				|| provenance.sourceSessionIdentity.empty()
				|| provenance.sourceSceneIdentity.size() > kMaximumRecordingIdentityBytes
				|| provenance.sourceSessionIdentity.size() > kMaximumRecordingIdentityBytes
				|| provenance.sourceFirstTick < 0
				|| provenance.sourceLastTick < provenance.sourceFirstTick
				|| provenance.sourceLastTick - provenance.sourceFirstTick != asset.durationTicks
				|| provenance.sourceSampleRate != asset.sampleRate
				|| !IsKnownRecordingClock(provenance.clock)
				|| provenance.capturedSampleCount == 0U
				|| provenance.capturedEventCount != asset.events.size()
				|| provenance.channels.empty()
				|| !IsFiniteReductionSettings(provenance.reduction)) {
				return AnimationStatus::InvalidArgument;
			}
			if(ValidateAnimationHardLimit(AnimationLimit::RecordingSampleRateHz,
				provenance.sourceSampleRate) != AnimationStatus::Ok
				|| ValidateAnimationHardLimit(AnimationLimit::ClipChannels,
				provenance.channels.size()) != AnimationStatus::Ok
				|| ValidateAnimationHardLimit(AnimationLimit::ClipKeys,
				provenance.sourceKeyCount) != AnimationStatus::Ok) {
				return AnimationStatus::HardLimitExceeded;
			}
			std::uint64_t retainedAndRemoved = 0U;
			if(!CheckedAdd(provenance.retainedKeyCount, provenance.removedKeyCount,
				retainedAndRemoved) || retainedAndRemoved != provenance.sourceKeyCount) {
				return AnimationStatus::InvalidArgument;
			}

			std::set<std::uint64_t> channelIds;
			std::set<std::pair<AnimationClipTrackKind, std::uint64_t>> numericTargets;
			std::uint64_t numericChannelCount = 0U;
			for(const AnimationClipRecordedChannel& channel : provenance.channels) {
				if(channel.stableChannelId == 0U || channel.stableTargetId == 0U
					|| channel.name.empty() || channel.name.size() > kMaximumRecordedChannelNameBytes
					|| !IsKnownRecordedChannelKind(channel.kind)
					|| !IsKnownInterpolation(channel.interpolation)
					|| !channelIds.insert(channel.stableChannelId).second) {
					return AnimationStatus::InvalidArgument;
				}
				const bool hasNamedChannel = std::any_of(asset.namedChannels.begin(),
					asset.namedChannels.end(), [&](const AnimationClipNamedChannel& named) {
						return named.stableId == channel.stableChannelId && named.name == channel.name;
					});
				if(!hasNamedChannel) {
					return AnimationStatus::InvalidArgument;
				}
				if(channel.kind == AnimationClipRecordedChannelKind::Event) {
					continue;
				}
				const AnimationClipTrackKind trackKind = TrackKindForRecordedChannel(channel.kind);
				if(!numericTargets.emplace(trackKind, channel.stableTargetId).second) {
					return AnimationStatus::InvalidArgument;
				}
				++numericChannelCount;
				const bool hasTrack = std::any_of(asset.tracks.begin(), asset.tracks.end(),
					[&](const AnimationClipTrack& track) {
						return track.kind == trackKind && track.stableTargetId == channel.stableTargetId;
					});
				if(!hasTrack) {
					return AnimationStatus::InvalidArgument;
				}
			}
			std::uint64_t expectedSourceKeys = 0U;
			if(!CheckedMultiply(provenance.capturedSampleCount, numericChannelCount,
				expectedSourceKeys) || expectedSourceKeys != provenance.sourceKeyCount) {
				return AnimationStatus::InvalidArgument;
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus SerializeRecordingProvenance(
			const AnimationClipRecordingProvenance& provenance,
			std::vector<std::uint8_t>& output) {
			std::vector<AnimationClipRecordedChannel> channels = provenance.channels;
			std::sort(channels.begin(), channels.end(), [](const auto& left, const auto& right) {
				return std::tie(left.stableChannelId, left.stableTargetId, left.kind,
					left.interpolation, left.name)
					< std::tie(right.stableChannelId, right.stableTargetId, right.kind,
						right.interpolation, right.name);
			});

			RecordingProvenanceDiskV1 header{};
			header.magic = kRecordingProvenanceMagic;
			header.version = kRecordingProvenanceVersion;
			header.headerSize = sizeof(header);
			header.sourceAnimatorId = provenance.sourceAnimatorId;
			header.sourceControllerAssetId = provenance.sourceControllerAssetId;
			header.sourceFirstTick = provenance.sourceFirstTick;
			header.sourceLastTick = provenance.sourceLastTick;
			header.sourceSampleRate = provenance.sourceSampleRate;
			header.clock = static_cast<std::uint8_t>(provenance.clock);
			header.capturedSampleCount = provenance.capturedSampleCount;
			header.capturedEventCount = provenance.capturedEventCount;
			header.sourceKeyCount = provenance.sourceKeyCount;
			header.retainedKeyCount = provenance.retainedKeyCount;
			header.removedKeyCount = provenance.removedKeyCount;
			header.reduction = provenance.reduction;
			header.channelCount = static_cast<std::uint32_t>(channels.size());

			std::vector<std::uint8_t> strings;
			if(AnimationStatus status = AppendString(strings, provenance.sourceSceneIdentity,
				header.sourceSceneOffset, header.sourceSceneSize); status != AnimationStatus::Ok) {
				return status;
			}
			if(AnimationStatus status = AppendString(strings, provenance.sourceSessionIdentity,
				header.sourceSessionOffset, header.sourceSessionSize); status != AnimationStatus::Ok) {
				return status;
			}
			std::vector<RecordedChannelDiskV1> channelRecords;
			channelRecords.reserve(channels.size());
			for(const AnimationClipRecordedChannel& channel : channels) {
				RecordedChannelDiskV1 record{channel.stableChannelId, channel.stableTargetId,
					static_cast<std::uint8_t>(channel.kind),
					static_cast<std::uint8_t>(channel.interpolation), 0U, 0U, 0U, 0U};
				if(AnimationStatus status = AppendString(strings, channel.name,
					record.nameOffset, record.nameSize); status != AnimationStatus::Ok) {
					return status;
				}
				channelRecords.push_back(record);
			}

			std::vector<std::uint8_t> staged(sizeof(header), 0U);
			if(AnimationStatus status = AppendRecords(staged,
				std::span<const RecordedChannelDiskV1>(channelRecords),
				header.channelTableOffset); status != AnimationStatus::Ok) {
				return status;
			}
			header.stringTableOffset = staged.size();
			header.stringTableSize = strings.size();
			std::uint64_t finalSize = 0U;
			if(!CheckedAdd(staged.size(), strings.size(), finalSize)
				|| finalSize > kMaximumRecordingMetadataBytes
				|| finalSize > std::numeric_limits<std::size_t>::max()) {
				return AnimationStatus::HardLimitExceeded;
			}
			staged.insert(staged.end(), strings.begin(), strings.end());
			std::memcpy(staged.data(), &header, sizeof(header));
			output = std::move(staged);
			return AnimationStatus::Ok;
		}

		AnimationStatus DeserializeRecordingProvenance(
			std::span<const std::uint8_t> input,
			AnimationClipRecordingProvenance& output) {
			if(input.size() < sizeof(RecordingProvenanceDiskV1)) {
				return AnimationStatus::TruncatedInput;
			}
			if(input.size() > kMaximumRecordingMetadataBytes) {
				return AnimationStatus::HardLimitExceeded;
			}
			RecordingProvenanceDiskV1 header{};
			std::memcpy(&header, input.data(), sizeof(header));
			if(header.magic != kRecordingProvenanceMagic
				|| header.version != kRecordingProvenanceVersion
				|| header.headerSize != sizeof(header)
				|| header.reserved[0] != 0U || header.reserved[1] != 0U
				|| header.reserved[2] != 0U || header.reserved2 != 0U) {
				return AnimationStatus::InvalidChunk;
			}
			if(ValidateAnimationHardLimit(AnimationLimit::ClipChannels, header.channelCount)
				!= AnimationStatus::Ok) {
				return AnimationStatus::HardLimitExceeded;
			}
			std::uint64_t channelBytes = 0U;
			std::uint64_t expectedStringOffset = 0U;
			if(!CheckedMultiply(header.channelCount, sizeof(RecordedChannelDiskV1), channelBytes)
				|| !CheckedAdd(sizeof(header), channelBytes, expectedStringOffset)
				|| header.channelTableOffset != sizeof(header)
				|| header.stringTableOffset != expectedStringOffset) {
				return AnimationStatus::InvalidChunk;
			}
			if(ValidateAnimationRange(input.size(), header.stringTableOffset,
				header.stringTableSize) != AnimationStatus::Ok
				|| header.stringTableOffset + header.stringTableSize != input.size()) {
				return AnimationStatus::InvalidChunk;
			}

			std::vector<RecordedChannelDiskV1> channelRecords;
			AnimationStatus status = ReadRecords(input, header.channelTableOffset,
				header.channelCount, channelRecords);
			if(status != AnimationStatus::Ok) {
				return status;
			}
			const std::span<const std::uint8_t> strings = input.subspan(
				static_cast<std::size_t>(header.stringTableOffset),
				static_cast<std::size_t>(header.stringTableSize));
			AnimationClipRecordingProvenance staged;
			staged.sourceAnimatorId = header.sourceAnimatorId;
			staged.sourceControllerAssetId = header.sourceControllerAssetId;
			staged.sourceFirstTick = header.sourceFirstTick;
			staged.sourceLastTick = header.sourceLastTick;
			staged.sourceSampleRate = header.sourceSampleRate;
			staged.clock = static_cast<AnimationClipRecordingClock>(header.clock);
			staged.capturedSampleCount = header.capturedSampleCount;
			staged.capturedEventCount = header.capturedEventCount;
			staged.sourceKeyCount = header.sourceKeyCount;
			staged.retainedKeyCount = header.retainedKeyCount;
			staged.removedKeyCount = header.removedKeyCount;
			staged.reduction = header.reduction;
			if((status = ReadString(strings, header.sourceSceneOffset,
				header.sourceSceneSize, staged.sourceSceneIdentity)) != AnimationStatus::Ok
				|| (status = ReadString(strings, header.sourceSessionOffset,
					header.sourceSessionSize, staged.sourceSessionIdentity)) != AnimationStatus::Ok) {
				return status;
			}
			staged.channels.reserve(channelRecords.size());
			for(const RecordedChannelDiskV1& record : channelRecords) {
				if(record.reserved != 0U || record.reserved2 != 0U) {
					return AnimationStatus::InvalidChunk;
				}
				AnimationClipRecordedChannel channel;
				channel.stableChannelId = record.stableChannelId;
				channel.stableTargetId = record.stableTargetId;
				channel.kind = static_cast<AnimationClipRecordedChannelKind>(record.kind);
				channel.interpolation = static_cast<AnimationInterpolation>(record.interpolation);
				if((status = ReadString(strings, record.nameOffset, record.nameSize,
					channel.name)) != AnimationStatus::Ok) {
					return status;
				}
				staged.channels.push_back(std::move(channel));
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		}

		bool TrackLess(const AnimationClipTrack& left, const AnimationClipTrack& right) noexcept {
			return std::tie(left.segmentIndex, left.kind, left.stableTargetId,
				left.sampleOffset, left.interpolation, left.sampleCount, left.encodedValueBits)
				< std::tie(right.segmentIndex, right.kind, right.stableTargetId,
					right.sampleOffset, right.interpolation, right.sampleCount, right.encodedValueBits);
		}
	}

	AnimationStatus ValidateAnimationClipAsset(const AnimationClipAsset& asset) noexcept {
		if(!asset.assetId.IsValid() || !asset.skeletonAssetId.IsValid()
			|| asset.compatibilityFingerprint == 0U
			|| asset.formatVersion > kAnimationClipAssetCurrentVersion
			|| asset.durationTicks < 0 || asset.sampleRate == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		const std::uint64_t maximumDurationTicks =
			GetAnimationRuntimeLimit(AnimationLimit::ClipDurationSeconds).hardMaximum
				* static_cast<std::uint64_t>(kAnimationFixedTicksPerSecond);
		if(static_cast<std::uint64_t>(asset.durationTicks) > maximumDurationTicks) {
			// The public asset validator is the shared fail-closed boundary for files,
			// runtime views, and compiler output. Enforce the real time limit here.
			return AnimationStatus::HardLimitExceeded;
		}
		if(ValidateAnimationHardLimit(AnimationLimit::ClipChannels, asset.tracks.size())
			!= AnimationStatus::Ok) {
			return AnimationStatus::HardLimitExceeded;
		}
		if(ValidateAnimationHardLimit(AnimationLimit::ClipAuthoredEvents, asset.events.size())
			!= AnimationStatus::Ok) {
			return AnimationStatus::HardLimitExceeded;
		}

		AnimationFixedTick priorSegmentEnd = 0;
		for(std::size_t index = 0U; index < asset.segments.size(); ++index) {
			const AnimationClipSegment& segment = asset.segments[index];
			if(segment.startTick < 0 || segment.endTick < segment.startTick
				|| segment.endTick > asset.durationTicks
				|| (index != 0U && segment.startTick < priorSegmentEnd)) {
				return AnimationStatus::InvalidArgument;
			}
			if(const AnimationStatus status = ValidateAnimationRange(
				asset.streamData.size(), segment.streamOffset, segment.streamSize);
				status != AnimationStatus::Ok) {
				return status;
			}
			priorSegmentEnd = segment.endTick;
		}

		std::uint64_t totalKeys = 0U;
		std::set<std::tuple<std::uint32_t, AnimationClipTrackKind, std::uint64_t>> trackKeys;
		for(const AnimationClipTrack& track : asset.tracks) {
			if(track.stableTargetId == 0U || !IsKnownTrackKind(track.kind)
				|| !IsKnownInterpolation(track.interpolation)
				|| track.segmentIndex >= asset.segments.size()
				|| track.sampleOffset > asset.streamData.size()
				|| !trackKeys.emplace(track.segmentIndex, track.kind, track.stableTargetId).second) {
				return AnimationStatus::InvalidArgument;
			}
			if(!CheckedAdd(totalKeys, track.sampleCount, totalKeys)) {
				return AnimationStatus::RangeOverflow;
			}
		}
		if(ValidateAnimationHardLimit(AnimationLimit::ClipKeys, totalKeys) != AnimationStatus::Ok) {
			return AnimationStatus::HardLimitExceeded;
		}

		std::set<std::uint64_t> channelIds;
		for(const AnimationClipNamedChannel& channel : asset.namedChannels) {
			if(channel.stableId == 0U || channel.name.empty() || !channelIds.insert(channel.stableId).second) {
				return AnimationStatus::InvalidArgument;
			}
		}
		std::set<std::uint64_t> eventIds;
		for(const AnimationClipEvent& event : asset.events) {
			if(event.stableEventId == 0U || event.tick < 0 || event.tick > asset.durationTicks
				|| !IsKnownPayloadType(event.payloadType)
				|| !eventIds.insert(event.stableEventId).second) {
				return AnimationStatus::InvalidArgument;
			}
		}
		if(asset.rootMotion.enabled
			&& (asset.rootMotion.translationTrackIndex >= asset.tracks.size()
				|| asset.rootMotion.yawTrackIndex >= asset.tracks.size())) {
			return AnimationStatus::InvalidArgument;
		}
		if(!IsFinite(asset.rootMotion.accumulatedCycleTranslation)
			|| !std::isfinite(asset.rootMotion.accumulatedCycleYawRadians)
			|| asset.compression.decoderVersion == 0U
			|| !IsFiniteNonNegative(asset.compression.translationErrorMeters)
			|| !IsFiniteNonNegative(asset.compression.rotationErrorDegrees)
			|| !IsFiniteNonNegative(asset.compression.scaleErrorFraction)
			|| !IsFiniteNonNegative(asset.compression.morphError)) {
			return AnimationStatus::InvalidArgument;
		}
		if(asset.recordingProvenance.has_value()) {
			if(const AnimationStatus status = ValidateRecordingProvenance(
				asset, *asset.recordingProvenance); status != AnimationStatus::Ok) {
				return status;
			}
		}
		return AnimationStatus::Ok;
	}

	AnimationStatus SerializeAnimationClipAsset(
		const AnimationClipAsset& asset,
		std::vector<std::uint8_t>& output) {
		if(const AnimationStatus status = ValidateAnimationClipAsset(asset);
			status != AnimationStatus::Ok) {
			return status;
		}

		std::vector<AnimationClipTrack> tracks = asset.tracks;
		std::sort(tracks.begin(), tracks.end(), TrackLess);
		std::vector<AnimationClipNamedChannel> namedChannels = asset.namedChannels;
		std::sort(namedChannels.begin(), namedChannels.end(), [](const auto& left, const auto& right) {
			return std::tie(left.stableId, left.name) < std::tie(right.stableId, right.name);
		});
		std::vector<AnimationClipEvent> events = asset.events;
		std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
			return std::tie(left.tick, left.stableEventId, left.payloadType, left.payload)
				< std::tie(right.tick, right.stableEventId, right.payloadType, right.payload);
		});

		std::vector<AnimationClipSegment> segments = asset.segments;
		for(AnimationClipSegment& segment : segments) {
			segment.firstTrack = 0U;
			segment.trackCount = 0U;
		}
		for(std::size_t index = 0U; index < tracks.size(); ++index) {
			AnimationClipSegment& segment = segments[tracks[index].segmentIndex];
			if(segment.trackCount == 0U) {
				segment.firstTrack = static_cast<std::uint32_t>(index);
			}
			++segment.trackCount;
		}

		std::vector<ClipSegmentDiskV1> segmentRecords;
		segmentRecords.reserve(segments.size());
		for(const AnimationClipSegment& segment : segments) {
			segmentRecords.push_back({segment.startTick, segment.endTick, segment.streamOffset,
				segment.streamSize, segment.firstTrack, segment.trackCount});
		}
		std::vector<ClipTrackDiskV1> trackRecords;
		trackRecords.reserve(tracks.size());
		for(const AnimationClipTrack& track : tracks) {
			trackRecords.push_back({track.stableTargetId, static_cast<std::uint8_t>(track.kind),
				static_cast<std::uint8_t>(track.interpolation), 0U, track.segmentIndex,
				track.sampleOffset, track.sampleCount, track.encodedValueBits});
		}

		std::vector<std::uint8_t> strings;
		std::vector<NamedChannelDiskV1> namedRecords;
		namedRecords.reserve(namedChannels.size());
		for(const AnimationClipNamedChannel& channel : namedChannels) {
			NamedChannelDiskV1 record{channel.stableId, 0U, 0U};
			if(const AnimationStatus status = AppendString(strings, channel.name, record.nameOffset, record.nameSize);
				status != AnimationStatus::Ok) {
				return status;
			}
			namedRecords.push_back(record);
		}
		std::vector<ClipEventDiskV1> eventRecords;
		eventRecords.reserve(events.size());
		for(const AnimationClipEvent& event : events) {
			ClipEventDiskV1 record{event.stableEventId, event.tick,
				static_cast<std::uint8_t>(event.payloadType),
				static_cast<std::uint8_t>((event.fireForward ? 1U : 0U) | (event.fireReverse ? 2U : 0U)),
				0U, 0U, 0U, 0U};
			if(const AnimationStatus status = AppendString(strings, event.payload, record.payloadOffset, record.payloadSize);
				status != AnimationStatus::Ok) {
				return status;
			}
			eventRecords.push_back(record);
		}

		AnimationClipAssetFileHeaderV1 header;
		header.headerSize = sizeof(header);
		header.skeletonAssetId = asset.skeletonAssetId;
		header.durationTicks = asset.durationTicks;
		header.sampleRate = asset.sampleRate;
		header.segmentCount = static_cast<std::uint32_t>(segments.size());
		header.trackCount = static_cast<std::uint32_t>(tracks.size());
		header.namedChannelCount = static_cast<std::uint32_t>(namedChannels.size());
		header.eventCount = static_cast<std::uint32_t>(events.size());
		header.rootMotionFlags = asset.rootMotion.enabled ? kRootMotionEnabled : 0U;
		header.rootTranslationTrackIndex = asset.rootMotion.translationTrackIndex;
		header.rootYawTrackIndex = asset.rootMotion.yawTrackIndex;
		header.accumulatedCycleTranslation = asset.rootMotion.accumulatedCycleTranslation;
		header.accumulatedCycleYawRadians = asset.rootMotion.accumulatedCycleYawRadians;
		header.compression = asset.compression;

		std::vector<std::uint8_t> payload(sizeof(header), 0U);
		AnimationStatus status = AppendRecords(payload, std::span<const ClipSegmentDiskV1>(segmentRecords), header.segmentTableOffset);
		if(status == AnimationStatus::Ok) {
			status = AppendRecords(payload, std::span<const ClipTrackDiskV1>(trackRecords), header.trackTableOffset);
		}
		if(status == AnimationStatus::Ok) {
			status = AppendRecords(payload, std::span<const NamedChannelDiskV1>(namedRecords), header.namedChannelTableOffset);
		}
		if(status == AnimationStatus::Ok) {
			status = AppendRecords(payload, std::span<const ClipEventDiskV1>(eventRecords), header.eventTableOffset);
		}
		if(status != AnimationStatus::Ok) {
			return status;
		}
		header.stringTableOffset = payload.size();
		header.stringTableSize = strings.size();
		payload.insert(payload.end(), strings.begin(), strings.end());
		header.streamDataOffset = payload.size();
		header.streamDataSize = asset.streamData.size();
		payload.insert(payload.end(), asset.streamData.begin(), asset.streamData.end());
		std::memcpy(payload.data(), &header, sizeof(header));

		AnimationAssetWriter writer;
		if(!writer.AddChunk(kAnimationChunkPayload, true, payload)) {
			return AnimationStatus::RangeOverflow;
		}
		if(asset.recordingProvenance.has_value()) {
			std::vector<std::uint8_t> metadata;
			if(const AnimationStatus metadataStatus = SerializeRecordingProvenance(
				*asset.recordingProvenance, metadata); metadataStatus != AnimationStatus::Ok) {
				return metadataStatus;
			}
			// META is optional by envelope contract, so an older clip reader may skip
			// this append-only editor provenance without changing runtime clip decode.
			if(!writer.AddChunk(kAnimationChunkMetadata, false, metadata)) {
				return AnimationStatus::RangeOverflow;
			}
		}
		AnimationAssetEnvelope envelope;
		envelope.assetId = asset.assetId;
		envelope.compatibilityFingerprint = asset.compatibilityFingerprint;
		std::vector<std::uint8_t> staged;
		if(const AnimationStatus finishStatus = writer.Finish(envelope, staged);
			finishStatus != AnimationStatus::Ok) {
			return finishStatus;
		}
		output = std::move(staged);
		return AnimationStatus::Ok;
	}

	AnimationStatus DeserializeAnimationClipAsset(
		std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		AnimationClipAsset& output) {
		AnimationAssetReader reader;
		if(const AnimationStatus status = reader.Parse(input); status != AnimationStatus::Ok) {
			return status;
		}
		if(expectedCompatibilityFingerprint != 0U
			&& reader.Envelope().compatibilityFingerprint != expectedCompatibilityFingerprint) {
			return AnimationStatus::IncompatibleAsset;
		}
		const std::span<const std::uint8_t> payload = reader.FindChunk(kAnimationChunkPayload);
		if(payload.size() < sizeof(AnimationClipAssetFileHeaderV1)) {
			return AnimationStatus::TruncatedInput;
		}
		AnimationClipAssetFileHeaderV1 header{};
		std::memcpy(&header, payload.data(), sizeof(header));
		if(header.magic != kAnimationClipPayloadMagic || header.headerSize != sizeof(header)) {
			return AnimationStatus::InvalidEnvelope;
		}
		if(header.formatVersion != kAnimationClipAssetLegacyVersion
			&& header.formatVersion != kAnimationClipAssetCurrentVersion) {
			return AnimationStatus::UnsupportedMajorVersion;
		}
		if(ValidateAnimationHardLimit(AnimationLimit::ClipChannels, header.trackCount)
			!= AnimationStatus::Ok
			|| ValidateAnimationHardLimit(AnimationLimit::ClipAuthoredEvents, header.eventCount)
			!= AnimationStatus::Ok) {
			return AnimationStatus::HardLimitExceeded;
		}

		std::vector<ClipSegmentDiskV1> segmentRecords;
		std::vector<ClipTrackDiskV1> trackRecords;
		std::vector<NamedChannelDiskV1> namedRecords;
		std::vector<ClipEventDiskV1> eventRecords;
		AnimationStatus status = ReadRecords(payload, header.segmentTableOffset, header.segmentCount, segmentRecords);
		if(status == AnimationStatus::Ok) {
			status = ReadRecords(payload, header.trackTableOffset, header.trackCount, trackRecords);
		}
		if(status == AnimationStatus::Ok) {
			status = ReadRecords(payload, header.namedChannelTableOffset, header.namedChannelCount, namedRecords);
		}
		if(status == AnimationStatus::Ok) {
			status = ReadRecords(payload, header.eventTableOffset, header.eventCount, eventRecords);
		}
		if(status != AnimationStatus::Ok) {
			return status;
		}
		if((status = ValidateAnimationRange(payload.size(), header.stringTableOffset, header.stringTableSize))
			!= AnimationStatus::Ok) {
			return status;
		}
		if((status = ValidateAnimationRange(payload.size(), header.streamDataOffset, header.streamDataSize))
			!= AnimationStatus::Ok) {
			return status;
		}
		const std::span<const std::uint8_t> strings = payload.subspan(
			static_cast<std::size_t>(header.stringTableOffset), static_cast<std::size_t>(header.stringTableSize));

		AnimationClipAsset staged;
		staged.assetId = reader.Envelope().assetId;
		staged.skeletonAssetId = header.skeletonAssetId;
		staged.compatibilityFingerprint = reader.Envelope().compatibilityFingerprint;
		staged.formatVersion = kAnimationClipAssetCurrentVersion;
		staged.durationTicks = header.durationTicks;
		staged.sampleRate = header.sampleRate;
		staged.compression = header.compression;
		staged.rootMotion.enabled = (header.rootMotionFlags & kRootMotionEnabled) != 0U;
		staged.rootMotion.translationTrackIndex = header.rootTranslationTrackIndex;
		staged.rootMotion.yawTrackIndex = header.rootYawTrackIndex;
		staged.rootMotion.accumulatedCycleTranslation = header.accumulatedCycleTranslation;
		staged.rootMotion.accumulatedCycleYawRadians = header.accumulatedCycleYawRadians;
		for(const ClipSegmentDiskV1& record : segmentRecords) {
			staged.segments.push_back({record.startTick, record.endTick, record.streamOffset,
				record.streamSize, record.firstTrack, record.trackCount});
		}
		for(const ClipTrackDiskV1& record : trackRecords) {
			staged.tracks.push_back({record.stableTargetId,
				static_cast<AnimationClipTrackKind>(record.kind),
				static_cast<AnimationInterpolation>(record.interpolation),
				record.segmentIndex, record.sampleOffset, record.sampleCount, record.encodedValueBits});
		}
		for(const NamedChannelDiskV1& record : namedRecords) {
			AnimationClipNamedChannel channel;
			channel.stableId = record.stableId;
			if((status = ReadString(strings, record.nameOffset, record.nameSize, channel.name))
				!= AnimationStatus::Ok) {
				return status;
			}
			staged.namedChannels.push_back(std::move(channel));
		}
		for(const ClipEventDiskV1& record : eventRecords) {
			AnimationClipEvent event;
			event.stableEventId = record.stableEventId;
			event.tick = record.tick;
			event.payloadType = static_cast<AnimationEventPayloadType>(record.payloadType);
			event.fireForward = (record.flags & 1U) != 0U;
			event.fireReverse = (record.flags & 2U) != 0U;
			if((status = ReadString(strings, record.payloadOffset, record.payloadSize, event.payload))
				!= AnimationStatus::Ok) {
				return status;
			}
			staged.events.push_back(std::move(event));
		}
		for(const AnimationChunkEntry& chunk : reader.Chunks()) {
			if(chunk.type != kAnimationChunkMetadata) {
				continue;
			}
			const std::span<const std::uint8_t> metadata = reader.FindChunk(kAnimationChunkMetadata);
			if(metadata.empty()) {
				return AnimationStatus::InvalidChunk;
			}
			AnimationClipRecordingProvenance provenance;
			if((status = DeserializeRecordingProvenance(metadata, provenance))
				!= AnimationStatus::Ok) {
				return status;
			}
			staged.recordingProvenance = std::move(provenance);
			break;
		}
		staged.streamData.assign(
			payload.begin() + static_cast<std::size_t>(header.streamDataOffset),
			payload.begin() + static_cast<std::size_t>(header.streamDataOffset + header.streamDataSize));
		if((status = ValidateAnimationClipAsset(staged)) != AnimationStatus::Ok) {
			return status;
		}

		// Publication happens only after every untrusted range and semantic rule
		// passes, preserving the caller's prior immutable generation on failure.
		output = std::move(staged);
		return AnimationStatus::Ok;
	}
}
