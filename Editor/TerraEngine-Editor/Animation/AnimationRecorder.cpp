// AnimationRecorder.cpp - bounded staged capture and atomic .teanim publication.

#include "AnimationRecorder.h"

#include "AnimationClipCompiler.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <Windows.h>
#endif

namespace TerraEngine::Editor::Animation {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		constexpr std::uint32_t kRecorderChunkMagic = MakeAnimationFourCC('T', 'E', 'R', 'C');
		constexpr std::uint16_t kRecorderChunkVersion = 1U;
		constexpr std::size_t kMaximumChunkSamples = 4096U;
		constexpr std::uint64_t kMaximumChunkCount = 65536U;
		constexpr std::uint64_t kMaximumEventPayloadBytes = 64U * 1024U;
		constexpr std::size_t kMaximumDiagnosticCount = 128U;
		constexpr std::size_t kMaximumIdentityBytes = 16U * 1024U;
		constexpr std::size_t kMaximumChannelNameBytes = 4U * 1024U;
		constexpr std::uint64_t kChunkHashOffsetBasis = 1469598103934665603ULL;
		constexpr std::uint64_t kChunkHashPrime = 1099511628211ULL;

		#pragma pack(push, 1)
		struct RecorderChunkHeaderV1 {
			std::uint32_t magic = kRecorderChunkMagic;
			std::uint16_t version = kRecorderChunkVersion;
			std::uint16_t headerSize = sizeof(RecorderChunkHeaderV1);
			std::uint32_t sampleCount = 0U;
			std::uint32_t numericChannelCount = 0U;
			std::uint64_t eventCount = 0U;
			AnimationFixedTick firstTick = 0;
			AnimationFixedTick lastTick = 0;
			std::uint64_t payloadBytes = 0U;
			std::uint64_t payloadHash = 0U;
		};

		struct RecorderEventHeaderV1 {
			std::uint64_t stableChannelId = 0U;
			std::uint64_t stableEventId = 0U;
			std::uint8_t payloadType = 0U;
			std::uint8_t flags = 0U;
			std::uint16_t reserved = 0U;
			std::uint32_t payloadSize = 0U;
		};
		#pragma pack(pop)

		struct CanonicalRecordedSample {
			AnimationFixedTick tick = 0;
			std::vector<AnimationRecordedValue> values;
			std::vector<AnimationRecordedEvent> events;
		};

		struct RecorderChunkRecord {
			std::filesystem::path path;
			std::uint64_t bytes = 0U;
			std::uint32_t sampleCount = 0U;
			std::uint64_t eventCount = 0U;
			AnimationFixedTick firstTick = 0;
			AnimationFixedTick lastTick = 0;
		};

		bool IsKnownPayloadType(AnimationEventPayloadType type) noexcept {
			return type >= AnimationEventPayloadType::None
				&& type <= AnimationEventPayloadType::Boolean;
		}

		bool IsKnownClock(AnimationClipRecordingClock clock) noexcept {
			return clock >= AnimationClipRecordingClock::FixedSimulation
				&& clock <= AnimationClipRecordingClock::Manual;
		}

		bool IsKnownChannelKind(AnimationClipRecordedChannelKind kind) noexcept {
			return kind >= AnimationClipRecordedChannelKind::BoneTranslation
				&& kind <= AnimationClipRecordedChannelKind::RootYaw;
		}

		bool IsKnownInterpolation(AnimationInterpolation interpolation) noexcept {
			return interpolation >= AnimationInterpolation::Constant
				&& interpolation <= AnimationInterpolation::Cubic;
		}

		bool IsFiniteNonNegative(float value) noexcept {
			return std::isfinite(value) && value >= 0.0F;
		}

		bool IsValidReduction(const AnimationRecordingReductionSettings& settings) noexcept {
			return IsFiniteNonNegative(settings.translationToleranceMeters)
				&& IsFiniteNonNegative(settings.rotationToleranceDegrees)
				&& IsFiniteNonNegative(settings.scaleToleranceFraction)
				&& IsFiniteNonNegative(settings.morphTolerance)
				&& IsFiniteNonNegative(settings.curveTolerance);
		}

		bool IsVectorChannel(AnimationClipRecordedChannelKind kind) noexcept {
			return kind == AnimationClipRecordedChannelKind::BoneTranslation
				|| kind == AnimationClipRecordedChannelKind::BoneScale
				|| kind == AnimationClipRecordedChannelKind::RootTranslation;
		}

		bool IsRotationChannel(AnimationClipRecordedChannelKind kind) noexcept {
			return kind == AnimationClipRecordedChannelKind::BoneRotation;
		}

		bool IsScalarChannel(AnimationClipRecordedChannelKind kind) noexcept {
			return kind == AnimationClipRecordedChannelKind::MorphWeight
				|| kind == AnimationClipRecordedChannelKind::Curve
				|| kind == AnimationClipRecordedChannelKind::RootYaw;
		}

		AnimationClipTrackKind ToTrackKind(AnimationClipRecordedChannelKind kind) noexcept {
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

		AnimationAuthoringStatus ToAuthoringStatus(AnimationStatus status) noexcept {
			switch(status) {
			case AnimationStatus::Ok:
				return AnimationAuthoringStatus::Ok;
			case AnimationStatus::Cancelled:
				return AnimationAuthoringStatus::Canceled;
			case AnimationStatus::InvalidArgument:
			case AnimationStatus::InvalidType:
				return AnimationAuthoringStatus::InvalidArgument;
			case AnimationStatus::HardLimitExceeded:
			case AnimationStatus::RangeOverflow:
			case AnimationStatus::BufferTooSmall:
				return AnimationAuthoringStatus::CapacityExceeded;
			case AnimationStatus::StaleHandle:
				return AnimationAuthoringStatus::Stale;
			default:
				return AnimationAuthoringStatus::Rejected;
			}
		}

		AnimationGuid MakeLegacyGuid(std::uint8_t seed) noexcept {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		std::uint64_t HashChunk(std::span<const std::uint8_t> payload) noexcept {
			std::uint64_t hash = kChunkHashOffsetBasis;
			for(const std::uint8_t byte : payload) {
				hash ^= byte;
				hash *= kChunkHashPrime;
			}
			return hash;
		}

		template<typename Value>
		bool AppendPod(std::vector<std::uint8_t>& bytes, const Value& value) {
			if(bytes.size() > std::numeric_limits<std::size_t>::max() - sizeof(Value)) {
				return false;
			}
			const std::size_t offset = bytes.size();
			bytes.resize(offset + sizeof(Value));
			std::memcpy(bytes.data() + offset, &value, sizeof(Value));
			return true;
		}

		template<typename Value>
		bool ReadPod(std::span<const std::uint8_t> bytes, std::size_t& cursor, Value& value) noexcept {
			if(cursor > bytes.size() || bytes.size() - cursor < sizeof(Value)) {
				return false;
			}
			std::memcpy(&value, bytes.data() + cursor, sizeof(Value));
			cursor += sizeof(Value);
			return true;
		}

		bool AppendRaw(
			std::vector<std::uint8_t>& bytes,
			std::span<const std::uint8_t> value) {
			if(value.size() > std::numeric_limits<std::size_t>::max() - bytes.size()) {
				return false;
			}
			bytes.insert(bytes.end(), value.begin(), value.end());
			return true;
		}

		std::string Lowercase(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return value;
		}

		bool WriteBinaryFile(
			const std::filesystem::path& path,
			std::span<const std::uint8_t> bytes,
			std::string& diagnostic) noexcept {
			try {
				std::ofstream stream(path, std::ios::binary | std::ios::trunc);
				if(!stream) {
					diagnostic = "The staged file could not be opened for writing.";
					return false;
				}
				if(!bytes.empty()) {
					stream.write(reinterpret_cast<const char*>(bytes.data()),
						static_cast<std::streamsize>(bytes.size()));
				}
				stream.flush();
				if(!stream) {
					diagnostic = "The staged file could not be written completely.";
					return false;
				}
				stream.close();
				if(!stream) {
					diagnostic = "The staged file could not be closed cleanly.";
					return false;
				}
				return true;
			} catch(...) {
				diagnostic = "An exception interrupted staged file writing.";
				return false;
			}
		}

		bool ReadBinaryFile(
			const RecorderChunkRecord& chunk,
			std::uint64_t maximumStagedBytes,
			std::vector<std::uint8_t>& bytes,
			std::string& diagnostic) noexcept {
			try {
				std::error_code error;
				const std::uint64_t fileSize = std::filesystem::file_size(chunk.path, error);
				if(error || fileSize != chunk.bytes || fileSize > maximumStagedBytes
					|| fileSize > std::numeric_limits<std::size_t>::max()) {
					diagnostic = "A staged recording chunk changed size or exceeded its declared bound.";
					return false;
				}
				std::ifstream stream(chunk.path, std::ios::binary);
				if(!stream) {
					diagnostic = "A staged recording chunk could not be opened.";
					return false;
				}
				bytes.resize(static_cast<std::size_t>(fileSize));
				if(!bytes.empty()) {
					stream.read(reinterpret_cast<char*>(bytes.data()),
						static_cast<std::streamsize>(bytes.size()));
				}
				if(!stream) {
					diagnostic = "A staged recording chunk was truncated while reading.";
					return false;
				}
				return true;
			} catch(...) {
				diagnostic = "An exception interrupted staged chunk reading.";
				return false;
			}
		}

		bool ReplaceFileAtomically(
			const std::filesystem::path& stagedPath,
			const std::filesystem::path& destinationPath,
			bool destinationExists,
			std::string& diagnostic) noexcept {
		#if defined(_WIN32)
			// ReplaceFileW preserves the prior destination if the filesystem cannot
			// perform the same-volume atomic swap. We intentionally do not fall back to
			// delete-then-rename because that would create a last-known-good gap.
			const BOOL replaced = destinationExists
				? ::ReplaceFileW(destinationPath.c_str(), stagedPath.c_str(), nullptr,
					REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
				: ::MoveFileExW(stagedPath.c_str(), destinationPath.c_str(), MOVEFILE_WRITE_THROUGH);
			if(replaced == FALSE) {
				diagnostic = "Windows could not atomically replace the .teanim destination (error "
					+ std::to_string(static_cast<unsigned long>(::GetLastError()))
					+ "). The prior output was preserved; the destination must support a same-volume atomic replace.";
				return false;
			}
			return true;
		#else
			std::error_code error;
			std::filesystem::rename(stagedPath, destinationPath, error);
			if(error) {
				diagnostic = "The platform could not atomically rename the validated .teanim candidate: "
					+ error.message();
				return false;
			}
			return true;
		#endif
		}
	}

	class AnimationRecorder::Impl final {
	public:
		AnimationAuthoringStatus Begin(
			const AnimationRecorderDescriptor& requestedDescriptor,
			bool legacyCompatibility) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				if(IsWorkingLocked()) {
					return AnimationAuthoringStatus::Rejected;
				}
				CleanupStagingLocked();
				ResetLocked();
				descriptor_ = requestedDescriptor;
				legacyCompatibility_ = legacyCompatibility;
				std::string validationDiagnostic;
				const AnimationAuthoringStatus validation = ValidateDescriptorLocked(validationDiagnostic);
				if(validation != AnimationAuthoringStatus::Ok) {
					FailLocked(validation, AnimationStatus::InvalidArgument,
						"RecorderDescriptorInvalid", std::move(validationDiagnostic));
					return validation;
				}

				std::sort(descriptor_.channels.begin(), descriptor_.channels.end(),
					[](const auto& left, const auto& right) {
						return std::tie(left.stableChannelId, left.stableTargetId, left.kind,
							left.interpolation, left.name)
							< std::tie(right.stableChannelId, right.stableTargetId, right.kind,
								right.interpolation, right.name);
					});
				BuildChannelLookupLocked();
				if(descriptor_.maximumCapturedSamples == 0U) {
					descriptor_.maximumCapturedSamples = DerivedMaximumCapturedSamplesLocked();
				}

				std::filesystem::path stagingRoot = descriptor_.stagingRoot;
				if(stagingRoot.empty()) {
					if(!descriptor_.destinationPath.empty() && !descriptor_.destinationPath.parent_path().empty()) {
						stagingRoot = descriptor_.destinationPath.parent_path()
							/ ".terraengine-animation-recordings";
					} else {
						stagingRoot = std::filesystem::temp_directory_path()
							/ "TerraEngine-AnimationRecordings";
					}
					// The recorder owns only its conventional private root. A caller-supplied
					// root may contain unrelated sessions and must never be removed here.
					ownedStagingRoot_ = stagingRoot;
				}
				std::error_code error;
				std::filesystem::create_directories(stagingRoot, error);
				if(error) {
					FailLocked(AnimationAuthoringStatus::Rejected, AnimationStatus::InternalError,
						"RecorderStagingRootUnavailable",
						"The private recording staging root could not be created: " + error.message());
					return AnimationAuthoringStatus::Rejected;
				}
				sessionDirectory_ = stagingRoot
					/ ("recording-" + ToHexString(GenerateUUID()));
				std::filesystem::create_directory(sessionDirectory_, error);
				if(error) {
					FailLocked(AnimationAuthoringStatus::Rejected, AnimationStatus::InternalError,
						"RecorderSessionUnavailable",
						"A unique recording staging session could not be created: " + error.message());
					return AnimationAuthoringStatus::Rejected;
				}

				pendingSamples_.reserve(descriptor_.samplesPerChunk);
				progress_.stage = AnimationRecorderStage::Capturing;
				progress_.active = true;
				progress_.codecStatus = AnimationStatus::NotReady;
				result_.destinationPath = descriptor_.destinationPath;
				result_.stagingDirectory = sessionDirectory_;
				result_.assetId = descriptor_.assetId;
				result_.skeletonAssetId = descriptor_.skeletonAssetId;
				result_.compatibilityFingerprint = descriptor_.compatibilityFingerprint;
				result_.durationTicks = descriptor_.lastTick - descriptor_.firstTick;
				result_.sampleRate = descriptor_.sampleRate;
				result_.clock = descriptor_.clock;
				AddDiagnosticLocked("RecorderCaptureStarted", AnimationRecordingDiagnosticSeverity::Info,
					AnimationRecorderStage::Capturing, descriptor_.firstTick, 0U,
					"Recording capture started with a private bounded staging session.");
				return AnimationAuthoringStatus::Ok;
			} catch(...) {
				std::scoped_lock lock(mutex_);
				FailLocked(AnimationAuthoringStatus::Rejected, AnimationStatus::InternalError,
					"RecorderBeginException", "An exception prevented the recording session from starting.");
				return AnimationAuthoringStatus::Rejected;
			}
		}

		AnimationAuthoringStatus Capture(AnimationRecordedSample sample) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				if(progress_.stage != AnimationRecorderStage::Capturing) {
					return progress_.stage == AnimationRecorderStage::Canceled
						? AnimationAuthoringStatus::Canceled : AnimationAuthoringStatus::Rejected;
				}
				if(cancellationRequested_.load(std::memory_order_relaxed)) {
					CancelLocked("Recorder cancellation was observed before accepting another sample.");
					return AnimationAuthoringStatus::Canceled;
				}
				if(sample.tick < descriptor_.firstTick || sample.tick > descriptor_.lastTick) {
					++progress_.droppedSamples;
					AddDiagnosticLocked("RecorderSampleOutOfRange", AnimationRecordingDiagnosticSeverity::Error,
						AnimationRecorderStage::Capturing, sample.tick, 0U,
						"The sample tick is outside the declared recording range.");
					return AnimationAuthoringStatus::InvalidArgument;
				}
				if(lastCapturedTick_.has_value() && sample.tick <= *lastCapturedTick_) {
					++progress_.droppedSamples;
					AddDiagnosticLocked("RecorderSampleOrderInvalid", AnimationRecordingDiagnosticSeverity::Error,
						AnimationRecorderStage::Capturing, sample.tick, 0U,
						"Recorded sample ticks must be strictly increasing.");
					return AnimationAuthoringStatus::Rejected;
				}
				if(progress_.capturedSamples >= descriptor_.maximumCapturedSamples) {
					++progress_.droppedSamples;
					AddDiagnosticLocked("RecorderSampleCapacityExceeded", AnimationRecordingDiagnosticSeverity::Error,
						AnimationRecorderStage::Capturing, sample.tick, 0U,
						"The configured bounded sample capacity has been reached.");
					return AnimationAuthoringStatus::CapacityExceeded;
				}

				CanonicalRecordedSample canonical;
				std::string validationDiagnostic;
				std::uint64_t estimatedBytes = 0U;
				const AnimationAuthoringStatus validation = CanonicalizeSampleLocked(
					std::move(sample), canonical, estimatedBytes, validationDiagnostic);
				if(validation != AnimationAuthoringStatus::Ok) {
					++progress_.droppedSamples;
					AddDiagnosticLocked("RecorderSampleInvalid", AnimationRecordingDiagnosticSeverity::Error,
						AnimationRecorderStage::Capturing, canonical.tick, 0U,
						std::move(validationDiagnostic));
					return validation;
				}
				std::uint64_t nextEstimatedBytes = 0U;
				if(!CheckedAdd(estimatedStagedBytes_, estimatedBytes, nextEstimatedBytes)
					|| nextEstimatedBytes > descriptor_.maximumStagedBytes) {
					++progress_.droppedSamples;
					AddDiagnosticLocked("RecorderStagingCapacityExceeded", AnimationRecordingDiagnosticSeverity::Error,
						AnimationRecorderStage::Capturing, canonical.tick, 0U,
						"The next sample would exceed the configured staged-byte bound.");
					return AnimationAuthoringStatus::CapacityExceeded;
				}

				for(const AnimationRecordedEvent& event : canonical.events) {
					capturedEventIds_.insert(event.stableEventId);
				}
				estimatedStagedBytes_ = nextEstimatedBytes;
				lastCapturedTick_ = canonical.tick;
				progress_.capturedSamples++;
				progress_.capturedEvents += canonical.events.size();
				progress_.sourceKeyCount = progress_.capturedSamples * numericChannels_.size();
				progress_.captureNormalized = static_cast<float>(canonical.tick - descriptor_.firstTick)
					/ static_cast<float>(descriptor_.lastTick - descriptor_.firstTick);
				progress_.overallNormalized = 0.5F * progress_.captureNormalized;
				pendingSamples_.push_back(std::move(canonical));
				progress_.bufferedSampleCount = pendingSamples_.size();
				progress_.peakBufferedSampleCount = std::max<std::uint64_t>(
					progress_.peakBufferedSampleCount, pendingSamples_.size());
				if(pendingSamples_.size() == descriptor_.samplesPerChunk) {
					if(!FlushChunkLocked(validationDiagnostic)) {
						FailLocked(AnimationAuthoringStatus::Rejected, AnimationStatus::InternalError,
							"RecorderChunkWriteFailed", std::move(validationDiagnostic));
						return AnimationAuthoringStatus::Rejected;
					}
				}
				return AnimationAuthoringStatus::Ok;
			} catch(...) {
				std::scoped_lock lock(mutex_);
				FailLocked(AnimationAuthoringStatus::Rejected, AnimationStatus::InternalError,
					"RecorderCaptureException", "An exception interrupted bounded sample capture.");
				return AnimationAuthoringStatus::Rejected;
			}
		}

		AnimationAuthoringStatus CapturePublishedSnapshot(
			const AnimationSnapshot& snapshot,
			const AnimationRecorderSnapshotBinding& binding) noexcept {
			try {
				AnimationRecordedSample sample;
				{
					std::scoped_lock lock(mutex_);
					if(progress_.stage != AnimationRecorderStage::Capturing) {
						return progress_.stage == AnimationRecorderStage::Canceled
							? AnimationAuthoringStatus::Canceled : AnimationAuthoringStatus::Rejected;
					}
					if(snapshot.animator != binding.animator
						|| (binding.expectedPublicationGeneration != 0U
							&& snapshot.publicationGeneration != binding.expectedPublicationGeneration)) {
						AddDiagnosticLocked("RecorderSnapshotStale",
							AnimationRecordingDiagnosticSeverity::Warning,
							AnimationRecorderStage::Capturing, snapshot.tick, 0U,
							"The copied AnimationSystem snapshot no longer matches this recording binding.");
						return AnimationAuthoringStatus::Stale;
					}
					if(!snapshot.pose || !snapshot.pose->IsSealed()
						|| binding.jointStableIds.size() != snapshot.pose->LocalTransforms().size()) {
						AddDiagnosticLocked("RecorderSnapshotInvalid",
							AnimationRecordingDiagnosticSeverity::Error,
							AnimationRecorderStage::Capturing, snapshot.tick, 0U,
							"The copied pose is absent, unsealed, or has no exact stable-joint binding.");
						return AnimationAuthoringStatus::InvalidArgument;
					}

					std::unordered_map<std::uint64_t, std::size_t> jointIndices;
					jointIndices.reserve(binding.jointStableIds.size());
					for(std::size_t index = 0U; index < binding.jointStableIds.size(); ++index) {
						if(binding.jointStableIds[index] == 0U
							|| !jointIndices.emplace(binding.jointStableIds[index], index).second) {
							return AnimationAuthoringStatus::InvalidArgument;
						}
					}
					sample.tick = snapshot.tick;
					sample.values.reserve(numericChannels_.size());
					const std::span<const AnimationTransform> localTransforms = snapshot.pose->LocalTransforms();
					const std::span<const AnimationNamedFloatValue> morphs = snapshot.pose->MorphValues();
					const std::span<const AnimationNamedFloatValue> curves = snapshot.pose->CurveValues();
					const std::optional<AnimationRootMotionDelta>& rootMotion = snapshot.pose->RootMotionDelta();
					for(const AnimationRecordingChannelDescriptor& channel : numericChannels_) {
						AnimationRecordedValue value;
						value.stableChannelId = channel.stableChannelId;
						// Root translation is vector-valued in the staged recording format,
						// but its stable target is a root-motion lane rather than a joint.
						// Resolve it before the generic vector branch so it cannot be
						// misread as a joint transform or assigned into rotationValue.
						if(channel.kind == AnimationClipRecordedChannelKind::RootTranslation) {
							value.vectorValue = rootMotion ? rootMotion->localTranslation : AnimationFloat3{};
						}
						else if(IsVectorChannel(channel.kind) || IsRotationChannel(channel.kind)) {
							const auto joint = jointIndices.find(channel.stableTargetId);
							if(joint == jointIndices.end()) {
								return AnimationAuthoringStatus::InvalidArgument;
							}
							const AnimationTransform& transform = localTransforms[joint->second];
							if(channel.kind == AnimationClipRecordedChannelKind::BoneTranslation) {
								value.vectorValue = transform.translation;
							}
							else if(channel.kind == AnimationClipRecordedChannelKind::BoneScale) {
								value.vectorValue = transform.scale;
							}
							else {
								value.rotationValue = transform.rotation;
							}
						}
						else if(channel.kind == AnimationClipRecordedChannelKind::MorphWeight
							|| channel.kind == AnimationClipRecordedChannelKind::Curve) {
							const std::span<const AnimationNamedFloatValue> values =
								channel.kind == AnimationClipRecordedChannelKind::MorphWeight ? morphs : curves;
							const auto found = std::find_if(values.begin(), values.end(), [&channel](const auto& item) {
								return item.stableId == channel.stableTargetId;
							});
							if(found == values.end()) return AnimationAuthoringStatus::InvalidArgument;
							value.scalarValue = found->value;
						}
						else if(channel.kind == AnimationClipRecordedChannelKind::RootYaw) {
							value.scalarValue = rootMotion ? rootMotion->localYawRadians : 0.0F;
						}
						sample.values.push_back(value);
					}
					for(const AnimationRecordingChannelDescriptor& channel : descriptor_.channels) {
						if(channel.kind != AnimationClipRecordedChannelKind::Event) continue;
						for(const AnimationEventRecord& event : snapshot.pose->Events()) {
							sample.events.push_back({channel.stableChannelId, event.identity.eventId,
								event.payloadType, event.payload,
								event.direction != AnimationPlaybackDirection::Reverse,
								event.direction != AnimationPlaybackDirection::Forward});
						}
					}
				}
				// Reuse Capture's checked range/order/budget/chunk path rather than
				// duplicating state changes for a system-derived sample.
				return Capture(std::move(sample));
			} catch(...) {
				return AnimationAuthoringStatus::Rejected;
			}
		}

		AnimationAuthoringStatus Cancel() noexcept {
			std::scoped_lock lock(mutex_);
			if(progress_.stage == AnimationRecorderStage::Canceled) {
				return AnimationAuthoringStatus::Canceled;
			}
			if(!IsWorkingLocked()) {
				return AnimationAuthoringStatus::Rejected;
			}
			cancellationRequested_.store(true, std::memory_order_relaxed);
			progress_.cancellationRequested = true;
			if(progress_.stage == AnimationRecorderStage::Capturing) {
				CancelLocked("Recording was cancelled before finalization.");
			}
			// A worker already inside compression observes the same atomic token at its
			// next compiler checkpoint and performs staging cleanup itself.
			return AnimationAuthoringStatus::Canceled;
		}

		AnimationAuthoringStatus Finalize(bool publish, std::string* candidateOutput) noexcept {
			try {
				AnimationRecorderDescriptor descriptor;
				std::vector<RecorderChunkRecord> chunks;
				std::vector<AnimationRecordingChannelDescriptor> numericChannels;
				{
					std::scoped_lock lock(mutex_);
					if(progress_.stage == AnimationRecorderStage::Canceled) {
						return AnimationAuthoringStatus::Canceled;
					}
					if(progress_.stage != AnimationRecorderStage::Capturing) {
						return AnimationAuthoringStatus::Rejected;
					}
					if(progress_.capturedSamples == 0U) {
						FailLocked(AnimationAuthoringStatus::Rejected, AnimationStatus::InvalidArgument,
							"RecorderHasNoSamples",
							"A recording with no valid samples cannot produce an editable clip.");
						return AnimationAuthoringStatus::Rejected;
					}
					if(publish && descriptor_.destinationPath.empty()) {
						FailLocked(AnimationAuthoringStatus::InvalidArgument, AnimationStatus::InvalidArgument,
							"RecorderDestinationMissing",
							"Atomic publication requires a .teanim destination path.");
						return AnimationAuthoringStatus::InvalidArgument;
					}
					std::string flushDiagnostic;
					if(!FlushChunkLocked(flushDiagnostic)) {
						FailLocked(AnimationAuthoringStatus::Rejected, AnimationStatus::InternalError,
							"RecorderChunkWriteFailed", std::move(flushDiagnostic));
						return AnimationAuthoringStatus::Rejected;
					}
					progress_.stage = AnimationRecorderStage::LoadingChunks;
					progress_.overallNormalized = 0.5F;
					descriptor = descriptor_;
					chunks = chunks_;
					numericChannels = numericChannels_;
				}

				AnimationClipCompileInput compileInput;
				compileInput.assetId = descriptor.assetId;
				compileInput.skeletonAssetId = descriptor.skeletonAssetId;
				compileInput.compatibilityFingerprint = descriptor.compatibilityFingerprint;
				compileInput.durationTicks = descriptor.lastTick - descriptor.firstTick;
				compileInput.sampleRate = descriptor.sampleRate;
				compileInput.segmentDurationTicks = descriptor.segmentDurationTicks;
				compileInput.namedChannels.reserve(descriptor.channels.size());
				for(const AnimationRecordingChannelDescriptor& channel : descriptor.channels) {
					compileInput.namedChannels.push_back({channel.stableChannelId, channel.name});
				}
				compileInput.tracks.reserve(numericChannels.size());
				for(const AnimationRecordingChannelDescriptor& channel : numericChannels) {
					AnimationSourceTrack track;
					track.stableTargetId = channel.stableTargetId;
					track.kind = ToTrackKind(channel.kind);
					track.interpolation = channel.interpolation;
					track.keys.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
						descriptor.maximumCapturedSamples,
						std::numeric_limits<std::size_t>::max())));
					compileInput.tracks.push_back(std::move(track));
				}

				std::optional<AnimationFixedTick> priorTick;
				for(std::size_t chunkIndex = 0U; chunkIndex < chunks.size(); ++chunkIndex) {
					if(cancellationRequested_.load(std::memory_order_relaxed)) {
						return CompleteCanceled("Recording was cancelled while staged chunks were loading.");
					}
					std::string readDiagnostic;
					if(!ReadChunk(chunks[chunkIndex], descriptor, numericChannels,
						compileInput, priorTick, readDiagnostic)) {
						return CompleteFailure(AnimationAuthoringStatus::Rejected,
							AnimationStatus::InvalidChunk, "RecorderChunkInvalid", std::move(readDiagnostic));
					}
					std::scoped_lock lock(mutex_);
					progress_.processedChunkCount = chunkIndex + 1U;
					progress_.overallNormalized = 0.5F + 0.15F
						* static_cast<float>(chunkIndex + 1U) / static_cast<float>(chunks.size());
				}

				{
					std::scoped_lock lock(mutex_);
					progress_.stage = AnimationRecorderStage::ReducingAndCompressing;
					progress_.overallNormalized = 0.7F;
					AddDiagnosticLocked("RecorderCompressionStarted", AnimationRecordingDiagnosticSeverity::Info,
						AnimationRecorderStage::ReducingAndCompressing, descriptor.firstTick, 0U,
						"Staged chunks loaded; deterministic key reduction and compression started.");
				}
				AnimationClipCompilerSettings compilerSettings;
				compilerSettings.translationToleranceMeters = descriptor.reduction.translationToleranceMeters;
				compilerSettings.rotationToleranceDegrees = descriptor.reduction.rotationToleranceDegrees;
				compilerSettings.scaleToleranceFraction = descriptor.reduction.scaleToleranceFraction;
				compilerSettings.morphTolerance = descriptor.reduction.morphTolerance;
				compilerSettings.curveTolerance = descriptor.reduction.curveTolerance;
				AnimationClipAsset candidate;
				AnimationClipCompileReport report;
				const AnimationStatus compileStatus = CompileAnimationClip(
					compileInput, compilerSettings, candidate, report, &cancellationRequested_);
				if(compileStatus == AnimationStatus::Cancelled) {
					return CompleteCanceled("Recording was cancelled during deterministic compression.");
				}
				if(compileStatus != AnimationStatus::Ok) {
					return CompleteFailure(ToAuthoringStatus(compileStatus), compileStatus,
						"RecorderCompressionFailed", report.diagnostic.empty()
							? "The staged recording could not be compressed within its declared tolerances."
							: report.diagnostic);
				}

				AnimationClipRecordingProvenance provenance;
				provenance.sourceAnimatorId = descriptor.provenance.sourceAnimatorId;
				provenance.sourceControllerAssetId = descriptor.provenance.sourceControllerAssetId;
				provenance.sourceSceneIdentity = descriptor.provenance.sourceSceneIdentity;
				provenance.sourceSessionIdentity = descriptor.provenance.sourceSessionIdentity;
				provenance.sourceFirstTick = descriptor.firstTick;
				provenance.sourceLastTick = descriptor.lastTick;
				provenance.sourceSampleRate = descriptor.sampleRate;
				provenance.clock = descriptor.clock;
				provenance.capturedSampleCount = CapturedSampleCount();
				provenance.capturedEventCount = compileInput.events.size();
				provenance.sourceKeyCount = report.sourceKeyCount;
				provenance.retainedKeyCount = report.retainedKeyCount;
				provenance.removedKeyCount = report.removedKeyCount;
				provenance.reduction = {
					descriptor.reduction.translationToleranceMeters,
					descriptor.reduction.rotationToleranceDegrees,
					descriptor.reduction.scaleToleranceFraction,
					descriptor.reduction.morphTolerance,
					descriptor.reduction.curveTolerance};
				provenance.channels = descriptor.channels;
				candidate.recordingProvenance = std::move(provenance);

				{
					std::scoped_lock lock(mutex_);
					progress_.stage = AnimationRecorderStage::ValidatingCandidate;
					progress_.overallNormalized = 0.85F;
					progress_.sourceKeyCount = report.sourceKeyCount;
					progress_.retainedKeyCount = report.retainedKeyCount;
					progress_.removedKeyCount = report.removedKeyCount;
				}
				if(cancellationRequested_.load(std::memory_order_relaxed)) {
					return CompleteCanceled("Recording was cancelled before codec validation.");
				}
				std::vector<std::uint8_t> serialized;
				AnimationStatus codecStatus = SerializeAnimationClipAsset(candidate, serialized);
				if(codecStatus != AnimationStatus::Ok) {
					return CompleteFailure(ToAuthoringStatus(codecStatus), codecStatus,
						"RecorderSerializationFailed",
						"The compressed recording failed checked .teanim serialization.");
				}
				AnimationClipAsset decoded;
				codecStatus = DeserializeAnimationClipAsset(serialized,
					descriptor.compatibilityFingerprint, decoded);
				if(codecStatus != AnimationStatus::Ok || !decoded.recordingProvenance.has_value()
					|| decoded.assetId != descriptor.assetId
					|| decoded.skeletonAssetId != descriptor.skeletonAssetId
					|| decoded.durationTicks != descriptor.lastTick - descriptor.firstTick
					|| decoded.recordingProvenance->channels != descriptor.channels) {
					return CompleteFailure(AnimationAuthoringStatus::Rejected,
						codecStatus == AnimationStatus::Ok ? AnimationStatus::InvalidChunk : codecStatus,
						"RecorderValidationFailed",
						"The serialized candidate did not round-trip through the production AnimationClipAsset codec.");
				}

				if(!publish) {
					if(candidateOutput == nullptr) {
						return CompleteFailure(AnimationAuthoringStatus::InvalidArgument,
							AnimationStatus::InvalidArgument, "RecorderCandidateOutputMissing",
							"Candidate-only finalization requires caller-owned output storage.");
					}
					candidateOutput->assign(reinterpret_cast<const char*>(serialized.data()), serialized.size());
					return CompleteSuccess(false, false, serialized.size(), report,
						"A validated binary .teanim candidate is ready; no destination was published.");
				}

				{
					std::scoped_lock lock(mutex_);
					progress_.stage = AnimationRecorderStage::Publishing;
					progress_.overallNormalized = 0.95F;
				}
				if(cancellationRequested_.load(std::memory_order_relaxed)) {
					return CompleteCanceled("Recording was cancelled before atomic publication.");
				}
				std::error_code error;
				const std::filesystem::path parent = descriptor.destinationPath.parent_path();
				if(!parent.empty()) {
					std::filesystem::create_directories(parent, error);
				}
				if(error) {
					return CompleteFailure(AnimationAuthoringStatus::Rejected,
						AnimationStatus::InternalError, "RecorderDestinationUnavailable",
						"The destination directory could not be prepared: " + error.message());
				}
				const bool destinationExists = std::filesystem::exists(descriptor.destinationPath, error);
				if(error || (destinationExists && std::filesystem::is_directory(descriptor.destinationPath, error))) {
					return CompleteFailure(AnimationAuthoringStatus::Rejected,
						AnimationStatus::InvalidArgument, "RecorderDestinationInvalid",
						"The .teanim destination could not be inspected or names a directory.");
				}
				const std::filesystem::path publicationStagingPath = descriptor.destinationPath.parent_path()
					/ (descriptor.destinationPath.filename().string() + ".recording-"
						+ ToHexString(GenerateUUID()) + ".tmp");
				std::string publicationDiagnostic;
				if(!WriteBinaryFile(publicationStagingPath, serialized, publicationDiagnostic)) {
					std::filesystem::remove(publicationStagingPath, error);
					return CompleteFailure(AnimationAuthoringStatus::Rejected,
						AnimationStatus::InternalError, "RecorderPublicationWriteFailed",
						std::move(publicationDiagnostic));
				}
				if(cancellationRequested_.load(std::memory_order_relaxed)) {
					std::filesystem::remove(publicationStagingPath, error);
					return CompleteCanceled("Recording was cancelled after candidate writing and before replacement.");
				}
				if(!ReplaceFileAtomically(publicationStagingPath, descriptor.destinationPath,
					destinationExists, publicationDiagnostic)) {
					std::filesystem::remove(publicationStagingPath, error);
					return CompleteFailure(AnimationAuthoringStatus::Rejected,
						AnimationStatus::InternalError, "RecorderAtomicReplaceFailed",
						std::move(publicationDiagnostic));
				}
				return CompleteSuccess(true, destinationExists, serialized.size(), report,
					"The validated recording was atomically published as a loadable .teanim asset.");
			} catch(...) {
				return CompleteFailure(AnimationAuthoringStatus::Rejected,
					AnimationStatus::InternalError, "RecorderFinalizeException",
					"An exception interrupted recording finalization before publication completed.");
			}
		}

		AnimationRecorderProgress Progress() const noexcept {
			try {
				std::scoped_lock lock(mutex_);
				return progress_;
			} catch(...) {
				return {};
			}
		}

		AnimationRecorderResult Result() const noexcept {
			try {
				std::scoped_lock lock(mutex_);
				return result_;
			} catch(...) {
				return {};
			}
		}

		std::vector<AnimationRecordingDiagnostic> Diagnostics() const noexcept {
			try {
				std::scoped_lock lock(mutex_);
				return diagnostics_;
			} catch(...) {
				return {};
			}
		}

		~Impl() {
			cancellationRequested_.store(true, std::memory_order_relaxed);
			std::scoped_lock lock(mutex_);
			CleanupStagingLocked();
		}

	private:
		bool IsWorkingLocked() const noexcept {
			return progress_.stage >= AnimationRecorderStage::Capturing
				&& progress_.stage <= AnimationRecorderStage::Publishing;
		}

		void ResetLocked() {
			descriptor_ = {};
			legacyCompatibility_ = false;
			cancellationRequested_.store(false, std::memory_order_relaxed);
			progress_ = {};
			result_ = {};
			diagnostics_.clear();
			numericChannels_.clear();
			numericIndexByChannel_.clear();
			eventChannelIds_.clear();
			capturedEventIds_.clear();
			pendingSamples_.clear();
			chunks_.clear();
			sessionDirectory_.clear();
			ownedStagingRoot_.clear();
			lastCapturedTick_.reset();
			estimatedStagedBytes_ = 0U;
		}

		AnimationAuthoringStatus ValidateDescriptorLocked(std::string& diagnostic) const noexcept {
			if(!descriptor_.assetId.IsValid() || !descriptor_.skeletonAssetId.IsValid()
				|| descriptor_.compatibilityFingerprint == 0U
				|| descriptor_.firstTick < 0 || descriptor_.lastTick <= descriptor_.firstTick
				|| descriptor_.sampleRate == 0U || !IsKnownClock(descriptor_.clock)
				|| descriptor_.channels.empty() || !descriptor_.provenance.sourceAnimatorId.IsValid()
				|| descriptor_.provenance.sourceSceneIdentity.empty()
				|| descriptor_.provenance.sourceSessionIdentity.empty()
				|| descriptor_.provenance.sourceSceneIdentity.size() > kMaximumIdentityBytes
				|| descriptor_.provenance.sourceSessionIdentity.size() > kMaximumIdentityBytes
				|| !IsValidReduction(descriptor_.reduction)
				|| descriptor_.segmentDurationTicks <= 0
				|| descriptor_.samplesPerChunk == 0U
				|| descriptor_.samplesPerChunk > kMaximumChunkSamples
				|| descriptor_.maximumStagedBytes < sizeof(RecorderChunkHeaderV1)) {
				diagnostic = "Recorder identity, range, clock, channels, provenance, tolerances, or bounds are invalid.";
				return AnimationAuthoringStatus::InvalidArgument;
			}
			if(ValidateAnimationHardLimit(AnimationLimit::RecordingSampleRateHz,
				descriptor_.sampleRate) != AnimationStatus::Ok
				|| ValidateAnimationHardLimit(AnimationLimit::ClipChannels,
				descriptor_.channels.size()) != AnimationStatus::Ok) {
				diagnostic = "Recording sample rate or channel count exceeds the frozen version-1 hard limit.";
				return AnimationAuthoringStatus::CapacityExceeded;
			}
			const std::uint64_t maximumDurationTicks =
				GetAnimationRuntimeLimit(AnimationLimit::RecordingDurationSeconds).hardMaximum
					* static_cast<std::uint64_t>(kAnimationFixedTicksPerSecond);
			if(static_cast<std::uint64_t>(descriptor_.lastTick - descriptor_.firstTick)
				> maximumDurationTicks) {
				diagnostic = "Recording duration exceeds the frozen four-hour hard limit.";
				return AnimationAuthoringStatus::CapacityExceeded;
			}
			if(!descriptor_.destinationPath.empty()
				&& Lowercase(descriptor_.destinationPath.extension().string()) != ".teanim") {
				diagnostic = "Recording destinations must use the .teanim extension.";
				return AnimationAuthoringStatus::InvalidArgument;
			}

			std::set<std::uint64_t> channelIds;
			std::set<std::pair<AnimationClipTrackKind, std::uint64_t>> numericTargets;
			std::uint64_t numericCount = 0U;
			for(const AnimationRecordingChannelDescriptor& channel : descriptor_.channels) {
				if(channel.stableChannelId == 0U || channel.stableTargetId == 0U
					|| channel.name.empty() || channel.name.size() > kMaximumChannelNameBytes
					|| !IsKnownChannelKind(channel.kind)
					|| !IsKnownInterpolation(channel.interpolation)
					|| !channelIds.insert(channel.stableChannelId).second) {
					diagnostic = "Recorded channels require unique stable IDs, targets, valid kinds, names, and interpolation.";
					return AnimationAuthoringStatus::InvalidArgument;
				}
				if(channel.kind == AnimationClipRecordedChannelKind::Event) {
					continue;
				}
				++numericCount;
				if(!numericTargets.emplace(ToTrackKind(channel.kind), channel.stableTargetId).second) {
					diagnostic = "Two recorded numeric channels target the same runtime track kind and stable target.";
					return AnimationAuthoringStatus::InvalidArgument;
				}
			}
			if(numericCount == 0U) {
				diagnostic = "Version-1 recorder cooking requires at least one numeric bone, morph, curve, or root channel.";
				return AnimationAuthoringStatus::InvalidArgument;
			}
			const std::uint64_t derivedMaximum = DerivedMaximumCapturedSamples(numericCount);
			const std::uint64_t effectiveMaximum = descriptor_.maximumCapturedSamples == 0U
				? derivedMaximum : descriptor_.maximumCapturedSamples;
			if(effectiveMaximum == 0U || effectiveMaximum > derivedMaximum
				|| (effectiveMaximum + descriptor_.samplesPerChunk - 1U)
					/ descriptor_.samplesPerChunk > kMaximumChunkCount) {
				diagnostic = "Configured sample/chunk bounds exceed the declared range, clip-key limit, or maximum chunk count.";
				return AnimationAuthoringStatus::CapacityExceeded;
			}
			return AnimationAuthoringStatus::Ok;
		}

		std::uint64_t DerivedMaximumCapturedSamples(std::uint64_t numericCount) const noexcept {
			const std::uint64_t rangeSamples = static_cast<std::uint64_t>(
				descriptor_.lastTick - descriptor_.firstTick) + 1U;
			const std::uint64_t keyBound = GetAnimationRuntimeLimit(AnimationLimit::ClipKeys).hardMaximum
				/ std::max<std::uint64_t>(1U, numericCount);
			return std::min(rangeSamples, keyBound);
		}

		std::uint64_t DerivedMaximumCapturedSamplesLocked() const noexcept {
			return DerivedMaximumCapturedSamples(numericChannels_.size());
		}

		void BuildChannelLookupLocked() {
			for(const AnimationRecordingChannelDescriptor& channel : descriptor_.channels) {
				if(channel.kind == AnimationClipRecordedChannelKind::Event) {
					eventChannelIds_.insert(channel.stableChannelId);
					continue;
				}
				numericIndexByChannel_.emplace(channel.stableChannelId, numericChannels_.size());
				numericChannels_.push_back(channel);
			}
		}

		AnimationAuthoringStatus CanonicalizeSampleLocked(
			AnimationRecordedSample sample,
			CanonicalRecordedSample& canonical,
			std::uint64_t& estimatedBytes,
			std::string& diagnostic) const {
			canonical.tick = sample.tick;
			canonical.values.resize(numericChannels_.size());
			std::vector<bool> seen(numericChannels_.size(), false);
			if(!sample.channels.empty()) {
				if(!legacyCompatibility_ || !sample.values.empty()
					|| sample.channels.size() != numericChannels_.size()) {
					diagnostic = "Legacy scalar samples cannot be mixed with typed values or a typed recorder descriptor.";
					return AnimationAuthoringStatus::InvalidArgument;
				}
				for(std::size_t index = 0U; index < sample.channels.size(); ++index) {
					if(!std::isfinite(sample.channels[index])) {
						diagnostic = "A legacy scalar channel contains a non-finite value.";
						return AnimationAuthoringStatus::InvalidArgument;
					}
					canonical.values[index].stableChannelId = numericChannels_[index].stableChannelId;
					canonical.values[index].scalarValue = sample.channels[index];
					seen[index] = true;
				}
			} else {
				if(sample.values.size() != numericChannels_.size()) {
					diagnostic = "Every selected numeric channel must provide exactly one typed value per sample.";
					return AnimationAuthoringStatus::InvalidArgument;
				}
				for(AnimationRecordedValue& value : sample.values) {
					const auto found = numericIndexByChannel_.find(value.stableChannelId);
					if(found == numericIndexByChannel_.end() || seen[found->second]) {
						diagnostic = "A typed sample references an unknown or duplicate stable channel ID.";
						return AnimationAuthoringStatus::InvalidArgument;
					}
					const AnimationRecordingChannelDescriptor& channel = numericChannels_[found->second];
					if(IsVectorChannel(channel.kind) && !IsFinite(value.vectorValue)) {
						diagnostic = "A recorded vector channel contains a non-finite component.";
						return AnimationAuthoringStatus::InvalidArgument;
					}
					if(IsRotationChannel(channel.kind)) {
						const float lengthSquared = value.rotationValue.x * value.rotationValue.x
							+ value.rotationValue.y * value.rotationValue.y
							+ value.rotationValue.z * value.rotationValue.z
							+ value.rotationValue.w * value.rotationValue.w;
						if(!IsFinite(value.rotationValue) || !std::isfinite(lengthSquared)
							|| lengthSquared <= std::numeric_limits<float>::epsilon()) {
							diagnostic = "A recorded rotation channel is non-finite or has zero length.";
							return AnimationAuthoringStatus::InvalidArgument;
						}
					}
					if(IsScalarChannel(channel.kind) && !std::isfinite(value.scalarValue)) {
						diagnostic = "A recorded scalar channel contains a non-finite value.";
						return AnimationAuthoringStatus::InvalidArgument;
					}
					canonical.values[found->second] = std::move(value);
					seen[found->second] = true;
				}
			}

			if(sample.events.size() > GetAnimationRuntimeLimit(
				AnimationLimit::AnimatorEventsPerUpdate).hardMaximum) {
				diagnostic = "One recording sample exceeds the per-update event hard limit.";
				return AnimationAuthoringStatus::CapacityExceeded;
			}
			std::unordered_set<std::uint64_t> sampleEventIds;
			for(AnimationRecordedEvent& event : sample.events) {
				if(eventChannelIds_.find(event.stableChannelId) == eventChannelIds_.end()
					|| event.stableEventId == 0U || !IsKnownPayloadType(event.payloadType)
					|| event.payload.size() > kMaximumEventPayloadBytes
					|| capturedEventIds_.find(event.stableEventId) != capturedEventIds_.end()
					|| !sampleEventIds.insert(event.stableEventId).second) {
					diagnostic = "A recorded event has an unknown channel, duplicate/zero identity, invalid payload type, or oversized payload.";
					return AnimationAuthoringStatus::InvalidArgument;
				}
				canonical.events.push_back(std::move(event));
			}

			estimatedBytes = sizeof(AnimationFixedTick) + sizeof(std::uint32_t);
			for(const AnimationRecordingChannelDescriptor& channel : numericChannels_) {
				const std::uint64_t valueBytes = IsVectorChannel(channel.kind)
					? sizeof(AnimationFloat3)
					: IsRotationChannel(channel.kind) ? sizeof(AnimationQuaternion) : sizeof(float);
				if(!CheckedAdd(estimatedBytes, valueBytes, estimatedBytes)) {
					return AnimationAuthoringStatus::CapacityExceeded;
				}
			}
			for(const AnimationRecordedEvent& event : canonical.events) {
				if(!CheckedAdd(estimatedBytes, sizeof(RecorderEventHeaderV1), estimatedBytes)
					|| !CheckedAdd(estimatedBytes, event.payload.size(), estimatedBytes)) {
					return AnimationAuthoringStatus::CapacityExceeded;
				}
			}
			if(pendingSamples_.empty()
				&& !CheckedAdd(estimatedBytes, sizeof(RecorderChunkHeaderV1), estimatedBytes)) {
				return AnimationAuthoringStatus::CapacityExceeded;
			}
			return AnimationAuthoringStatus::Ok;
		}

		bool FlushChunkLocked(std::string& diagnostic) {
			if(pendingSamples_.empty()) {
				return true;
			}
			if(chunks_.size() >= kMaximumChunkCount) {
				diagnostic = "The recording exceeded the bounded staged chunk count.";
				return false;
			}
			try {
				std::vector<std::uint8_t> payload;
				std::uint64_t eventCount = 0U;
				for(const CanonicalRecordedSample& sample : pendingSamples_) {
					if(!AppendPod(payload, sample.tick)) {
						diagnostic = "A sample tick overflowed the chunk byte range.";
						return false;
					}
					for(std::size_t index = 0U; index < numericChannels_.size(); ++index) {
						const AnimationRecordingChannelDescriptor& channel = numericChannels_[index];
						const AnimationRecordedValue& value = sample.values[index];
						const bool appended = IsVectorChannel(channel.kind)
							? AppendPod(payload, value.vectorValue)
							: IsRotationChannel(channel.kind)
								? AppendPod(payload, value.rotationValue)
								: AppendPod(payload, value.scalarValue);
						if(!appended) {
							diagnostic = "A typed channel value overflowed the chunk byte range.";
							return false;
						}
					}
					const std::uint32_t sampleEventCount = static_cast<std::uint32_t>(sample.events.size());
					if(!AppendPod(payload, sampleEventCount)) {
						diagnostic = "An event count overflowed the chunk byte range.";
						return false;
					}
					eventCount += sample.events.size();
					for(const AnimationRecordedEvent& event : sample.events) {
						const RecorderEventHeaderV1 header{event.stableChannelId, event.stableEventId,
							static_cast<std::uint8_t>(event.payloadType),
							static_cast<std::uint8_t>((event.fireForward ? 1U : 0U)
								| (event.fireReverse ? 2U : 0U)), 0U,
							static_cast<std::uint32_t>(event.payload.size())};
						const std::span<const std::uint8_t> payloadBytes(
							reinterpret_cast<const std::uint8_t*>(event.payload.data()),
							event.payload.size());
						if(!AppendPod(payload, header) || !AppendRaw(payload, payloadBytes)) {
							diagnostic = "An event payload overflowed the chunk byte range.";
							return false;
						}
					}
				}
				RecorderChunkHeaderV1 header;
				header.sampleCount = static_cast<std::uint32_t>(pendingSamples_.size());
				header.numericChannelCount = static_cast<std::uint32_t>(numericChannels_.size());
				header.eventCount = eventCount;
				header.firstTick = pendingSamples_.front().tick;
				header.lastTick = pendingSamples_.back().tick;
				header.payloadBytes = payload.size();
				header.payloadHash = HashChunk(payload);
				std::vector<std::uint8_t> bytes;
				bytes.reserve(sizeof(header) + payload.size());
				if(!AppendPod(bytes, header) || !AppendRaw(bytes, payload)) {
					diagnostic = "A completed recording chunk overflowed its byte range.";
					return false;
				}
				std::ostringstream name;
				name << "chunk-" << std::setw(8) << std::setfill('0') << chunks_.size();
				const std::filesystem::path temporary = sessionDirectory_ / (name.str() + ".tmp");
				const std::filesystem::path completed = sessionDirectory_ / (name.str() + ".bin");
				if(!WriteBinaryFile(temporary, bytes, diagnostic)) {
					return false;
				}
				std::error_code error;
				std::filesystem::rename(temporary, completed, error);
				if(error) {
					std::filesystem::remove(temporary, error);
					diagnostic = "A completed recording chunk could not be committed inside staging: "
						+ error.message();
					return false;
				}
				chunks_.push_back({completed, bytes.size(), header.sampleCount, eventCount,
					header.firstTick, header.lastTick});
				progress_.stagedChunkCount = chunks_.size();
				progress_.stagedBytes += bytes.size();
				progress_.bufferedSampleCount = 0U;
				pendingSamples_.clear();
				return true;
			} catch(...) {
				diagnostic = "An exception interrupted bounded chunk staging.";
				return false;
			}
		}

		bool ReadChunk(
			const RecorderChunkRecord& chunk,
			const AnimationRecorderDescriptor& descriptor,
			const std::vector<AnimationRecordingChannelDescriptor>& numericChannels,
			AnimationClipCompileInput& output,
			std::optional<AnimationFixedTick>& priorTick,
			std::string& diagnostic) const noexcept {
			std::vector<std::uint8_t> bytes;
			if(!ReadBinaryFile(chunk, descriptor.maximumStagedBytes, bytes, diagnostic)
				|| bytes.size() < sizeof(RecorderChunkHeaderV1)) {
				if(diagnostic.empty()) {
					diagnostic = "A staged recording chunk is shorter than its fixed header.";
				}
				return false;
			}
			RecorderChunkHeaderV1 header{};
			std::memcpy(&header, bytes.data(), sizeof(header));
			if(header.magic != kRecorderChunkMagic || header.version != kRecorderChunkVersion
				|| header.headerSize != sizeof(header) || header.sampleCount != chunk.sampleCount
				|| header.numericChannelCount != numericChannels.size()
				|| header.eventCount != chunk.eventCount || header.firstTick != chunk.firstTick
				|| header.lastTick != chunk.lastTick
				|| header.payloadBytes != bytes.size() - sizeof(header)) {
				diagnostic = "A staged recording chunk header does not match its immutable session index.";
				return false;
			}
			const std::span<const std::uint8_t> payload(bytes.data() + sizeof(header),
				bytes.size() - sizeof(header));
			if(HashChunk(payload) != header.payloadHash) {
				diagnostic = "A staged recording chunk failed its payload integrity hash.";
				return false;
			}
			std::size_t cursor = 0U;
			std::uint64_t observedEvents = 0U;
			for(std::uint32_t sampleIndex = 0U; sampleIndex < header.sampleCount; ++sampleIndex) {
				AnimationFixedTick tick = 0;
				if(!ReadPod(payload, cursor, tick) || tick < descriptor.firstTick
					|| tick > descriptor.lastTick || (priorTick.has_value() && tick <= *priorTick)) {
					diagnostic = "A staged sample tick is truncated, out of range, or out of order.";
					return false;
				}
				priorTick = tick;
				for(std::size_t channelIndex = 0U; channelIndex < numericChannels.size(); ++channelIndex) {
					AnimationSourceKey key;
					key.tick = tick - descriptor.firstTick;
					const AnimationClipRecordedChannelKind kind = numericChannels[channelIndex].kind;
					const bool read = IsVectorChannel(kind)
						? ReadPod(payload, cursor, key.vectorValue)
						: IsRotationChannel(kind)
							? ReadPod(payload, cursor, key.rotationValue)
							: ReadPod(payload, cursor, key.scalarValue);
					if(!read) {
						diagnostic = "A staged typed channel value is truncated.";
						return false;
					}
					output.tracks[channelIndex].keys.push_back(key);
				}
				std::uint32_t sampleEventCount = 0U;
				if(!ReadPod(payload, cursor, sampleEventCount)
					|| sampleEventCount > GetAnimationRuntimeLimit(
						AnimationLimit::AnimatorEventsPerUpdate).hardMaximum) {
					diagnostic = "A staged per-sample event count is truncated or exceeds its hard limit.";
					return false;
				}
				for(std::uint32_t eventIndex = 0U; eventIndex < sampleEventCount; ++eventIndex) {
					RecorderEventHeaderV1 eventHeader{};
					if(!ReadPod(payload, cursor, eventHeader)
						|| eventHeader.reserved != 0U || (eventHeader.flags & ~3U) != 0U
						|| eventHeader.payloadSize > kMaximumEventPayloadBytes
						|| cursor > payload.size() || payload.size() - cursor < eventHeader.payloadSize
						|| eventChannelIds_.find(eventHeader.stableChannelId) == eventChannelIds_.end()
						|| eventHeader.stableEventId == 0U
						|| !IsKnownPayloadType(static_cast<AnimationEventPayloadType>(eventHeader.payloadType))) {
						diagnostic = "A staged event record is malformed, oversized, or references an unknown channel.";
						return false;
					}
					AnimationClipEvent event;
					event.stableEventId = eventHeader.stableEventId;
					event.tick = tick - descriptor.firstTick;
					event.payloadType = static_cast<AnimationEventPayloadType>(eventHeader.payloadType);
					event.fireForward = (eventHeader.flags & 1U) != 0U;
					event.fireReverse = (eventHeader.flags & 2U) != 0U;
					event.payload.assign(reinterpret_cast<const char*>(payload.data() + cursor),
						eventHeader.payloadSize);
					cursor += eventHeader.payloadSize;
					output.events.push_back(std::move(event));
					++observedEvents;
				}
			}
			if(cursor != payload.size() || observedEvents != header.eventCount) {
				diagnostic = "A staged recording chunk contains trailing bytes or an inconsistent event count.";
				return false;
			}
			return true;
		}

		std::uint64_t CapturedSampleCount() const noexcept {
			std::scoped_lock lock(mutex_);
			return progress_.capturedSamples;
		}

		AnimationAuthoringStatus CompleteSuccess(
			bool published,
			bool replacedPriorOutput,
			std::uint64_t outputBytes,
			const AnimationClipCompileReport& report,
			std::string diagnostic) noexcept {
			std::scoped_lock lock(mutex_);
			progress_.stage = published ? AnimationRecorderStage::Completed
				: AnimationRecorderStage::CandidateReady;
			progress_.active = false;
			progress_.overallNormalized = 1.0F;
			progress_.codecStatus = AnimationStatus::Ok;
			progress_.latestDiagnostic = diagnostic;
			result_.outcome = published ? AnimationRecorderOutcome::Published
				: AnimationRecorderOutcome::CandidateReady;
			result_.authoringStatus = AnimationAuthoringStatus::Ok;
			result_.codecStatus = AnimationStatus::Ok;
			result_.candidateValidated = true;
			result_.published = published;
			result_.replacedPriorOutput = replacedPriorOutput;
			result_.capturedSamples = progress_.capturedSamples;
			result_.capturedEvents = progress_.capturedEvents;
			result_.stagedChunkCount = progress_.stagedChunkCount;
			result_.stagedBytes = progress_.stagedBytes;
			result_.outputBytes = outputBytes;
			result_.sourceKeyCount = report.sourceKeyCount;
			result_.retainedKeyCount = report.retainedKeyCount;
			result_.removedKeyCount = report.removedKeyCount;
			result_.peakBufferedSampleCount = progress_.peakBufferedSampleCount;
			result_.measuredTranslationErrorMeters = report.measuredTranslationErrorMeters;
			result_.measuredRotationErrorDegrees = report.measuredRotationErrorDegrees;
			result_.measuredScaleErrorFraction = report.measuredScaleErrorFraction;
			result_.measuredMorphError = report.measuredMorphError;
			result_.measuredCurveError = report.measuredCurveError;
			result_.diagnostic = diagnostic;
			AddDiagnosticLocked(published ? "RecorderPublished" : "RecorderCandidateReady",
				AnimationRecordingDiagnosticSeverity::Info, progress_.stage,
				descriptor_.lastTick, 0U, std::move(diagnostic));
			CleanupStagingLocked();
			return AnimationAuthoringStatus::Ok;
		}

		AnimationAuthoringStatus CompleteFailure(
			AnimationAuthoringStatus authoringStatus,
			AnimationStatus codecStatus,
			std::string code,
			std::string diagnostic) noexcept {
			std::scoped_lock lock(mutex_);
			FailLocked(authoringStatus, codecStatus, std::move(code), std::move(diagnostic));
			return authoringStatus;
		}

		AnimationAuthoringStatus CompleteCanceled(std::string diagnostic) noexcept {
			std::scoped_lock lock(mutex_);
			CancelLocked(std::move(diagnostic));
			return AnimationAuthoringStatus::Canceled;
		}

		void FailLocked(
			AnimationAuthoringStatus authoringStatus,
			AnimationStatus codecStatus,
			std::string code,
			std::string diagnostic) {
			progress_.stage = AnimationRecorderStage::Failed;
			progress_.active = false;
			progress_.codecStatus = codecStatus;
			progress_.latestDiagnostic = diagnostic;
			result_.outcome = AnimationRecorderOutcome::Failed;
			result_.authoringStatus = authoringStatus;
			result_.codecStatus = codecStatus;
			result_.candidateValidated = false;
			result_.published = false;
			result_.capturedSamples = progress_.capturedSamples;
			result_.capturedEvents = progress_.capturedEvents;
			result_.stagedChunkCount = progress_.stagedChunkCount;
			result_.stagedBytes = progress_.stagedBytes;
			result_.peakBufferedSampleCount = progress_.peakBufferedSampleCount;
			result_.diagnostic = diagnostic;
			AddDiagnosticLocked(std::move(code), AnimationRecordingDiagnosticSeverity::Error,
				AnimationRecorderStage::Failed, lastCapturedTick_.value_or(descriptor_.firstTick),
				0U, std::move(diagnostic));
			CleanupStagingLocked();
		}

		void CancelLocked(std::string diagnostic) {
			cancellationRequested_.store(true, std::memory_order_relaxed);
			progress_.stage = AnimationRecorderStage::Canceled;
			progress_.active = false;
			progress_.cancellationRequested = true;
			progress_.canceled = true;
			progress_.codecStatus = AnimationStatus::Cancelled;
			progress_.latestDiagnostic = diagnostic;
			result_.outcome = AnimationRecorderOutcome::Canceled;
			result_.authoringStatus = AnimationAuthoringStatus::Canceled;
			result_.codecStatus = AnimationStatus::Cancelled;
			result_.candidateValidated = false;
			result_.published = false;
			result_.capturedSamples = progress_.capturedSamples;
			result_.capturedEvents = progress_.capturedEvents;
			result_.stagedChunkCount = progress_.stagedChunkCount;
			result_.stagedBytes = progress_.stagedBytes;
			result_.peakBufferedSampleCount = progress_.peakBufferedSampleCount;
			result_.diagnostic = diagnostic;
			AddDiagnosticLocked("RecorderCanceled", AnimationRecordingDiagnosticSeverity::Info,
				AnimationRecorderStage::Canceled, lastCapturedTick_.value_or(descriptor_.firstTick),
				0U, std::move(diagnostic));
			CleanupStagingLocked();
		}

		void AddDiagnosticLocked(
			std::string code,
			AnimationRecordingDiagnosticSeverity severity,
			AnimationRecorderStage stage,
			AnimationFixedTick tick,
			std::uint64_t stableChannelId,
			std::string message) {
			if(diagnostics_.size() == kMaximumDiagnosticCount) {
				diagnostics_.erase(diagnostics_.begin());
			}
			progress_.latestDiagnostic = message;
			diagnostics_.push_back({std::move(code), severity, stage, tick,
				stableChannelId, std::move(message)});
		}

		void CleanupStagingLocked() noexcept {
			pendingSamples_.clear();
			std::error_code ignored;
			if(!sessionDirectory_.empty()) {
				// sessionDirectory is a UUID-named child created by Begin. Never widen this
				// recursive cleanup to stagingRoot or a caller-owned destination directory.
				std::filesystem::remove_all(sessionDirectory_, ignored);
			}
			if(!ownedStagingRoot_.empty()) {
				// remove() is deliberately non-recursive. It removes the recorder-owned
				// conventional root only when no concurrent/private session remains.
				ignored.clear();
				std::filesystem::remove(ownedStagingRoot_, ignored);
			}
		}

		mutable std::mutex mutex_;
		std::atomic_bool cancellationRequested_{false};
		AnimationRecorderDescriptor descriptor_{};
		bool legacyCompatibility_ = false;
		AnimationRecorderProgress progress_{};
		AnimationRecorderResult result_{};
		std::vector<AnimationRecordingDiagnostic> diagnostics_;
		std::vector<AnimationRecordingChannelDescriptor> numericChannels_;
		std::unordered_map<std::uint64_t, std::size_t> numericIndexByChannel_;
		std::unordered_set<std::uint64_t> eventChannelIds_;
		std::unordered_set<std::uint64_t> capturedEventIds_;
		std::vector<CanonicalRecordedSample> pendingSamples_;
		std::vector<RecorderChunkRecord> chunks_;
		std::filesystem::path sessionDirectory_;
		std::filesystem::path ownedStagingRoot_;
		std::optional<AnimationFixedTick> lastCapturedTick_;
		std::uint64_t estimatedStagedBytes_ = 0U;
	};

	AnimationRecorder::AnimationRecorder()
		: impl_(std::make_unique<Impl>()) {}

	AnimationRecorder::~AnimationRecorder() = default;

	AnimationAuthoringStatus AnimationRecorder::Begin(
		const AnimationRecorderDescriptor& descriptor) noexcept {
		return impl_->Begin(descriptor, false);
	}

	AnimationAuthoringStatus AnimationRecorder::Begin(
		std::uint64_t firstTick,
		std::uint64_t lastTick,
		std::uint32_t channelCount) noexcept {
		if(firstTick > static_cast<std::uint64_t>(std::numeric_limits<AnimationFixedTick>::max())
			|| lastTick > static_cast<std::uint64_t>(std::numeric_limits<AnimationFixedTick>::max())
			|| channelCount == 0U) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		try {
			AnimationRecorderDescriptor descriptor;
			descriptor.assetId = MakeLegacyGuid(1U);
			descriptor.skeletonAssetId = MakeLegacyGuid(33U);
			descriptor.compatibilityFingerprint = 0x5445525245434F52ULL;
			descriptor.firstTick = static_cast<AnimationFixedTick>(firstTick);
			descriptor.lastTick = static_cast<AnimationFixedTick>(lastTick);
			descriptor.clock = AnimationClipRecordingClock::Manual;
			descriptor.provenance.sourceAnimatorId = MakeLegacyGuid(65U);
			descriptor.provenance.sourceSceneIdentity = "LegacyAnimationRecorder";
			descriptor.provenance.sourceSessionIdentity = "candidate-only";
			descriptor.maximumCapturedSamples = std::min<std::uint64_t>(
				65536U, lastTick >= firstTick ? lastTick - firstTick + 1U : 0U);
			descriptor.channels.reserve(channelCount);
			for(std::uint32_t index = 0U; index < channelCount; ++index) {
				descriptor.channels.push_back({static_cast<std::uint64_t>(index) + 1U,
					static_cast<std::uint64_t>(index) + 1U,
					"Legacy Curve " + std::to_string(index),
					AnimationClipRecordedChannelKind::Curve, AnimationInterpolation::Linear});
			}
			return impl_->Begin(descriptor, true);
		} catch(...) {
			return AnimationAuthoringStatus::Rejected;
		}
	}

	AnimationAuthoringStatus AnimationRecorder::Capture(AnimationRecordedSample sample) noexcept {
		return impl_->Capture(std::move(sample));
	}

	AnimationAuthoringStatus AnimationRecorder::CapturePublishedSnapshot(
		const AnimationSnapshot& snapshot, const AnimationRecorderSnapshotBinding& binding) noexcept {
		return impl_->CapturePublishedSnapshot(snapshot, binding);
	}

	AnimationAuthoringStatus AnimationRecorder::Cancel() noexcept {
		return impl_->Cancel();
	}

	AnimationAuthoringStatus AnimationRecorder::Finalize() noexcept {
		return impl_->Finalize(true, nullptr);
	}

	AnimationAuthoringStatus AnimationRecorder::Finalize(std::string& outTeanimBytes) noexcept {
		return impl_->Finalize(false, &outTeanimBytes);
	}

	AnimationRecorderProgress AnimationRecorder::Progress() const noexcept {
		return impl_->Progress();
	}

	AnimationRecorderResult AnimationRecorder::Result() const noexcept {
		return impl_->Result();
	}

	std::vector<AnimationRecordingDiagnostic> AnimationRecorder::Diagnostics() const noexcept {
		return impl_->Diagnostics();
	}
}
