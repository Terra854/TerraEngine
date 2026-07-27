#include "TestHarness.h"

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Codec/AnimationCodec.h"
#include "Editor/TerraEngine-Editor/Animation/AnimationRecorder.h"
#include "File Formats/TerraEngineAnimationFile.h"
#include "Utilities/UUID.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::Editor::Animation;

		class RecordingTestDirectory final {
		public:
			RecordingTestDirectory() {
				path_ = std::filesystem::temp_directory_path()
					/ ("TerraEngine-AnimationRecordingTests-" + ToHexString(GenerateUUID()));
				std::filesystem::create_directories(path_);
			}

			~RecordingTestDirectory() {
				// Every test owns a unique directory below the process temp root. Removing
				// only that resolved directory keeps cleanup isolated and recoverable.
				std::error_code ignored;
				std::filesystem::remove_all(path_, ignored);
			}

			const std::filesystem::path& Path() const noexcept { return path_; }

		private:
			std::filesystem::path path_;
		};

		AnimationGuid MakeGuid(std::uint8_t seed) {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if(!stream) {
				return {};
			}
			const std::streamoff size = stream.tellg();
			if(size < 0) {
				return {};
			}
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
			stream.seekg(0, std::ios::beg);
			if(!bytes.empty()) {
				stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}
			return stream ? bytes : std::vector<std::uint8_t>{};
		}

		void WriteText(const std::filesystem::path& path, const std::string& text) {
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
		}

		AnimationRecorderDescriptor MakeDescriptor(
			const std::filesystem::path& root,
			std::string destinationName = "Recorded.teanim") {
			AnimationRecorderDescriptor descriptor;
			descriptor.assetId = MakeGuid(1U);
			descriptor.skeletonAssetId = MakeGuid(21U);
			descriptor.compatibilityFingerprint = 0xA11CE1234ULL;
			descriptor.firstTick = 100;
			descriptor.lastTick = 110;
			descriptor.sampleRate = 60U;
			descriptor.clock = AnimationClipRecordingClock::FixedSimulation;
			descriptor.channels = {
				{11U, 101U, "Hip Translation", AnimationClipRecordedChannelKind::BoneTranslation,
					AnimationInterpolation::Linear},
				{12U, 101U, "Hip Rotation", AnimationClipRecordedChannelKind::BoneRotation,
					AnimationInterpolation::Linear},
				{13U, 201U, "Smile", AnimationClipRecordedChannelKind::MorphWeight,
					AnimationInterpolation::Linear},
				{14U, 301U, "Speed", AnimationClipRecordedChannelKind::Curve,
					AnimationInterpolation::Linear},
				{15U, 401U, "Footstep", AnimationClipRecordedChannelKind::Event,
					AnimationInterpolation::Step}
			};
			descriptor.provenance.sourceAnimatorId = MakeGuid(41U);
			descriptor.provenance.sourceControllerAssetId = MakeGuid(61U);
			descriptor.provenance.sourceSceneIdentity = "Tests/AnimationRecording.scene";
			descriptor.provenance.sourceSessionIdentity = "recording-session-001";
			descriptor.reduction.translationToleranceMeters = 0.0005F;
			descriptor.reduction.rotationToleranceDegrees = 0.1F;
			descriptor.reduction.scaleToleranceFraction = 0.0005F;
			descriptor.reduction.morphTolerance = 0.0005F;
			descriptor.reduction.curveTolerance = 0.0005F;
			descriptor.samplesPerChunk = 2U;
			descriptor.maximumCapturedSamples = 11U;
			descriptor.maximumStagedBytes = 1024U * 1024U;
			descriptor.segmentDurationTicks = 10;
			descriptor.stagingRoot = root / "staging";
			descriptor.destinationPath = root / std::move(destinationName);
			return descriptor;
		}

		AnimationRecordedSample MakeSample(AnimationFixedTick tick, float value, bool emitEvent = false) {
			AnimationRecordedSample sample;
			sample.tick = tick;
			// Values are intentionally not in descriptor order. Stable channel identity,
			// not caller vector position, must determine the cooked track association.
			sample.values = {
				{14U, {}, {}, value},
				{13U, {}, {}, value * 0.1F},
				{11U, {value, value * 2.0F, value * 3.0F}, {}, 0.0F},
				{12U, {}, {0.0F, 0.0F, 0.0F, 1.0F}, 0.0F}
			};
			if(emitEvent) {
				sample.events.push_back({15U, 501U, AnimationEventPayloadType::String,
					"left-foot", true, false});
			}
			return sample;
		}

		void CaptureThreeLinearSamples(TestContext& context, AnimationRecorder& recorder) {
			context.Expect(recorder.Capture(MakeSample(100, 0.0F)) == AnimationAuthoringStatus::Ok,
				"The first complete typed sample should enter the recorder.");
			context.Expect(recorder.Capture(MakeSample(105, 0.5F, true)) == AnimationAuthoringStatus::Ok,
				"The middle sample and its named event should enter the recorder.");
			context.Expect(recorder.Capture(MakeSample(110, 1.0F)) == AnimationAuthoringStatus::Ok,
				"The final in-range sample should enter the recorder.");
		}

		AnimationClipAsset LoadPublishedClip(
			TestContext& context,
			const AnimationRecorderDescriptor& descriptor,
			std::vector<std::uint8_t>& bytes) {
			bytes = ReadBytes(descriptor.destinationPath);
			context.Expect(!bytes.empty(), "A successful recorder result must publish non-empty binary bytes.");
			AnimationClipAsset clip;
			context.Expect(DeserializeAnimationClipAsset(bytes, descriptor.compatibilityFingerprint, clip)
				== AnimationStatus::Ok,
				"Published recorder bytes must deserialize through the production AnimationClipAsset codec.");
			return clip;
		}

		AnimationSnapshot MakePublishedSnapshot(
			AnimationFixedTick tick, float value, bool emitEvent = false,
			AnimationGeneration publicationGeneration = 1U) {
			auto pose = std::make_shared<PoseBuffer>();
			AnimatorInstanceHandle animator{7U, 3U};
			AnimationPoseGeneration generation;
			generation.animator = animator;
			generation.poseGeneration = 1U;
			generation.skeleton = {MakeGuid(21U), 1U};
			generation.tick = tick;
			const PoseBufferLayout layout{1U, 1U, 1U, 1U, 0U};
			if(pose->BeginWrite(layout, generation) != AnimationStatus::Ok) return {};
			pose->MutableLocalTransforms()[0U].translation = {value, value * 2.0F, value * 3.0F};
			pose->MutableMorphValues()[0U] = {201U, value * 0.1F};
			pose->MutableCurveValues()[0U] = {301U, value};
			if(emitEvent) {
				AnimationEventRecord event;
				event.identity.animator = animator;
				event.identity.stateInstanceId = 1U;
				event.identity.clipAssetId = MakeGuid(81U);
				event.identity.eventId = 501U;
				event.payloadType = AnimationEventPayloadType::String;
				event.payload = "left-foot";
				event.direction = AnimationPlaybackDirection::Forward;
				(void)pose->AppendEvent(std::move(event));
			}
			if(pose->Seal() != AnimationStatus::Ok) return {};
			AnimationSnapshot snapshot;
			snapshot.animator = animator;
			snapshot.publicationGeneration = publicationGeneration;
			snapshot.tick = tick;
			snapshot.pose = std::move(pose);
			return snapshot;
		}

		AnimationSnapshot MakePublishedRootMotionSnapshot(
			AnimationFixedTick tick,
			AnimationFloat3 rootTranslation,
			float rootYawRadians,
			AnimationGeneration publicationGeneration) {
			auto pose = std::make_shared<PoseBuffer>();
			const AnimatorInstanceHandle animator{7U, 3U};
			AnimationPoseGeneration generation;
			generation.animator = animator;
			generation.poseGeneration = publicationGeneration;
			generation.skeleton = {MakeGuid(21U), 1U};
			generation.tick = tick;
			// The second joint deliberately shares the root channel's stable target
			// and carries a recognizable rotation. RootTranslation must still read
			// the copied root-motion delta, never this joint transform.
			const PoseBufferLayout layout{2U, 0U, 0U, 0U, 0U};
			if(pose->BeginWrite(layout, generation) != AnimationStatus::Ok) return {};
			pose->MutableLocalTransforms()[0U].rotation = {0.0F, 0.6F, 0.0F, 0.8F};
			pose->MutableLocalTransforms()[1U].rotation = {0.0F, 0.0F, 0.6F, 0.8F};
			AnimationRootMotionDelta rootMotion;
			rootMotion.animator = animator;
			rootMotion.sourceBeginTick = tick - 1;
			rootMotion.sourceEndTick = tick;
			rootMotion.localTranslation = rootTranslation;
			rootMotion.worldTranslation = rootTranslation;
			rootMotion.localYawRadians = rootYawRadians;
			rootMotion.worldYawRadians = rootYawRadians;
			rootMotion.generation = publicationGeneration;
			if(pose->SetRootMotion(rootMotion) != AnimationStatus::Ok
				|| pose->Seal() != AnimationStatus::Ok) {
				return {};
			}
			AnimationSnapshot snapshot;
			snapshot.animator = animator;
			snapshot.publicationGeneration = publicationGeneration;
			snapshot.tick = tick;
			snapshot.pose = std::move(pose);
			return snapshot;
		}
	}

	void RegisterAnimationRecordingTests(TestRegistry& registry) {
		registry.Add("Editor.Animation.Recording.CopiedPublishedSnapshotCapture", [](TestContext& context) {
			RecordingTestDirectory directory;
			const AnimationRecorderDescriptor descriptor = MakeDescriptor(directory.Path());
			AnimationRecorder recorder;
			AnimationRecorderSnapshotBinding binding{{7U, 3U}, 1U, {101U}};
			context.Expect(recorder.Begin(descriptor) == AnimationAuthoringStatus::Ok
				&& recorder.CapturePublishedSnapshot(MakePublishedSnapshot(100, 0.0F), binding)
					== AnimationAuthoringStatus::Ok
				&& (binding.expectedPublicationGeneration = 2U, recorder.CapturePublishedSnapshot(
					MakePublishedSnapshot(105, 0.5F, true, 2U), binding))
					== AnimationAuthoringStatus::Ok
				&& (binding.expectedPublicationGeneration = 3U, recorder.CapturePublishedSnapshot(
					MakePublishedSnapshot(110, 1.0F, false, 3U), binding))
					== AnimationAuthoringStatus::Ok
				&& recorder.Finalize() == AnimationAuthoringStatus::Ok,
				"A recorder must copy selected channels only from sealed AnimationSystem publications.");
			std::vector<std::uint8_t> bytes;
			const AnimationClipAsset clip = LoadPublishedClip(context, descriptor, bytes);
			context.Expect(clip.events.size() == 1U && clip.events.front().stableEventId == 501U
				&& clip.events.front().payload == "left-foot",
				"Snapshot capture must preserve copied event identity and payload through publication.");
			AnimationRecorder staleRecorder;
			AnimationRecorderSnapshotBinding staleBinding = binding;
			staleBinding.expectedPublicationGeneration = 2U;
			context.Expect(staleRecorder.Begin(descriptor) == AnimationAuthoringStatus::Ok
				&& staleRecorder.CapturePublishedSnapshot(MakePublishedSnapshot(100, 0.0F), staleBinding)
					== AnimationAuthoringStatus::Stale,
				"A stale published generation must not enter a recording chunk.");
		});

		registry.Add("Editor.Animation.Recording.RootTranslationUsesPublishedRootMotion", [](TestContext& context) {
			RecordingTestDirectory directory;
			AnimationRecorderDescriptor descriptor = MakeDescriptor(directory.Path(), "RootMotion.teanim");
			descriptor.channels = {
				{16U, 601U, "Root Translation", AnimationClipRecordedChannelKind::RootTranslation,
					AnimationInterpolation::Linear},
				{17U, 601U, "Root Yaw", AnimationClipRecordedChannelKind::RootYaw,
					AnimationInterpolation::Linear},
				{12U, 101U, "Hip Rotation", AnimationClipRecordedChannelKind::BoneRotation,
					AnimationInterpolation::Linear}
			};
			AnimationRecorder recorder;
			AnimationRecorderSnapshotBinding binding{{7U, 3U}, 1U, {101U, 601U}};
			context.Expect(recorder.Begin(descriptor) == AnimationAuthoringStatus::Ok
				&& recorder.CapturePublishedSnapshot(
					MakePublishedRootMotionSnapshot(100, {1.0F, 2.0F, 3.0F}, 0.1F, 1U), binding)
					== AnimationAuthoringStatus::Ok
				&& (binding.expectedPublicationGeneration = 2U, recorder.CapturePublishedSnapshot(
					MakePublishedRootMotionSnapshot(105, {4.0F, 5.0F, 6.0F}, 0.4F, 2U), binding))
					== AnimationAuthoringStatus::Ok
				&& (binding.expectedPublicationGeneration = 3U, recorder.CapturePublishedSnapshot(
					MakePublishedRootMotionSnapshot(110, {7.0F, 8.0F, 9.0F}, 0.7F, 3U), binding))
					== AnimationAuthoringStatus::Ok
				&& recorder.Finalize() == AnimationAuthoringStatus::Ok,
				"Root translation, root yaw, and bone rotation should capture from their distinct published lanes.");

			std::vector<std::uint8_t> bytes;
			const AnimationClipAsset clip = LoadPublishedClip(context, descriptor, bytes);
			AnimationCodecView view;
			context.Expect(view.Open(clip) == AnimationStatus::Ok,
				"The runtime decoder should open the recorded root-motion clip.");
			AnimationDecodedValue decodedRootTranslation;
			context.Expect(view.Decode(601U, AnimationClipTrackKind::RootTranslation, 5, false,
				decodedRootTranslation) == AnimationStatus::Ok
				&& std::abs(decodedRootTranslation.vectorValue.x - 4.0F) < 0.001F
				&& std::abs(decodedRootTranslation.vectorValue.y - 5.0F) < 0.001F
				&& std::abs(decodedRootTranslation.vectorValue.z - 6.0F) < 0.001F
				&& decodedRootTranslation.rotationValue == AnimationQuaternion{},
				"RootTranslation must serialize the runtime local translation rather than a joint quaternion.");
			AnimationDecodedValue decodedRootYaw;
			context.Expect(view.Decode(601U, AnimationClipTrackKind::RootYaw, 5, false,
				decodedRootYaw) == AnimationStatus::Ok
				&& std::abs(decodedRootYaw.scalarValue - 0.4F) < 0.001F,
				"Root yaw sampling must remain independent from the RootTranslation repair.");
			AnimationDecodedValue decodedBoneRotation;
			context.Expect(view.Decode(101U, AnimationClipTrackKind::Rotation, 5, false,
				decodedBoneRotation) == AnimationStatus::Ok
				&& std::abs(std::abs(decodedBoneRotation.rotationValue.y) - 0.6F) < 0.001F
				&& std::abs(std::abs(decodedBoneRotation.rotationValue.w) - 0.8F) < 0.001F,
				"Bone rotation sampling must continue reading the selected joint transform.");
			bool rootTargetHasRotationTrack = false;
			for(const AnimationClipTrack& track : clip.tracks) {
				rootTargetHasRotationTrack = rootTargetHasRotationTrack
					|| (track.stableTargetId == 601U && track.kind == AnimationClipTrackKind::Rotation);
			}
			context.Expect(clip.rootMotion.enabled && !rootTargetHasRotationTrack,
				"The cooked root-motion target must not gain a rotation track from RootTranslation capture.");
		});

		registry.Add("Editor.Animation.Recording.LoadableRoundTripAndChannelIdentity", [](TestContext& context) {
			RecordingTestDirectory directory;
			const AnimationRecorderDescriptor descriptor = MakeDescriptor(directory.Path());
			AnimationRecorder recorder;
			context.Expect(recorder.Begin(descriptor) == AnimationAuthoringStatus::Ok,
				"A complete range, clock, channel set, provenance record, and destination should begin capture.");
			CaptureThreeLinearSamples(context, recorder);
			context.Expect(recorder.Finalize() == AnimationAuthoringStatus::Ok,
				"Finalization should report success only after codec validation and atomic publication.");

			std::vector<std::uint8_t> bytes;
			const AnimationClipAsset clip = LoadPublishedClip(context, descriptor, bytes);
			context.Expect(clip.durationTicks == 10 && clip.sampleRate == 60U,
				"The published clip should rebase the explicit source range without changing its duration or clock rate.");
			context.Expect(clip.namedChannels.size() == descriptor.channels.size(),
				"Every selected bone, morph, curve, and event channel should retain a collision-safe name entry.");
			context.Expect(clip.recordingProvenance.has_value(),
				"A recorded clip must carry its durable source and capture provenance.");
			context.Expect(clip.recordingProvenance->channels == descriptor.channels,
				"Typed channel IDs, target IDs, kinds, names, and interpolation should survive publication.");
			context.Expect(clip.recordingProvenance->sourceFirstTick == 100
				&& clip.recordingProvenance->sourceLastTick == 110
				&& clip.recordingProvenance->sourceSessionIdentity == "recording-session-001",
				"The original sampling range and session identity should remain inspectable.");
			context.Expect(clip.events.size() == 1U && clip.events.front().tick == 5
				&& clip.events.front().stableEventId == 501U
				&& clip.events.front().payload == "left-foot"
				&& clip.events.front().fireForward && !clip.events.front().fireReverse,
				"Recorded event identity, relative time, payload, and direction policy should survive cooking.");

			AnimationCodecView view;
			context.Expect(view.Open(clip) == AnimationStatus::Ok,
				"The runtime decoder should open the exact clip produced by recording.");
			AnimationDecodedValue decoded;
			context.Expect(view.Decode(101U, AnimationClipTrackKind::Translation, 5, false, decoded)
				== AnimationStatus::Ok && std::abs(decoded.vectorValue.x - 0.5F) < 0.001F,
				"Sample order and stable bone target identity should decode at the rebased middle tick.");

			std::vector<std::uint8_t> roundTrip;
			context.Expect(SerializeAnimationClipAsset(clip, roundTrip) == AnimationStatus::Ok
				&& roundTrip == bytes,
				"Deserializing and reserializing a recorded clip should produce deterministic identical bytes.");
			const AnimationRecorderResult result = recorder.Result();
			context.Expect(result.outcome == AnimationRecorderOutcome::Published && result.published
				&& result.candidateValidated && result.destinationPath == descriptor.destinationPath,
				"Durable result metadata must distinguish validated publication from candidate-only work.");
		});

		registry.Add("Editor.Animation.Recording.BoundedChunksAndDeterministicReduction", [](TestContext& context) {
			RecordingTestDirectory firstDirectory;
			RecordingTestDirectory secondDirectory;
			AnimationRecorderDescriptor firstDescriptor = MakeDescriptor(firstDirectory.Path());
			AnimationRecorderDescriptor secondDescriptor = MakeDescriptor(secondDirectory.Path());
			AnimationRecorder first;
			AnimationRecorder second;
			context.Expect(first.Begin(firstDescriptor) == AnimationAuthoringStatus::Ok
				&& second.Begin(secondDescriptor) == AnimationAuthoringStatus::Ok,
				"Equivalent recorder sessions should begin independently.");
			CaptureThreeLinearSamples(context, first);
			CaptureThreeLinearSamples(context, second);

			const AnimationRecorderProgress captureProgress = first.Progress();
			context.Expect(captureProgress.stagedChunkCount == 1U
				&& captureProgress.bufferedSampleCount == 1U
				&& captureProgress.peakBufferedSampleCount <= firstDescriptor.samplesPerChunk,
				"Full chunks should stream to staging while only one bounded partial chunk remains in memory.");
			context.Expect(first.Finalize() == AnimationAuthoringStatus::Ok
				&& second.Finalize() == AnimationAuthoringStatus::Ok,
				"Equivalent sessions should both finish through the real deterministic compiler.");
			context.Expect(ReadBytes(firstDescriptor.destinationPath) == ReadBytes(secondDescriptor.destinationPath),
				"Staging locations and destination names must not perturb deterministic .teanim bytes.");
			const AnimationRecorderResult result = first.Result();
			context.Expect(result.stagedChunkCount == 2U && result.sourceKeyCount == 12U
				&& result.removedKeyCount > 0U && result.retainedKeyCount < result.sourceKeyCount,
				"Result evidence should expose chunk use and deterministic key reduction counts.");
			context.Expect(result.measuredTranslationErrorMeters <= firstDescriptor.reduction.translationToleranceMeters
				&& result.measuredRotationErrorDegrees <= firstDescriptor.reduction.rotationToleranceDegrees
				&& result.measuredMorphError <= firstDescriptor.reduction.morphTolerance
				&& result.measuredCurveError <= firstDescriptor.reduction.curveTolerance,
				"Measured compression error must remain within every requested channel tolerance.");
		});

		registry.Add("Editor.Animation.Recording.CancellationPreservesLastKnownGood", [](TestContext& context) {
			RecordingTestDirectory directory;
			AnimationRecorderDescriptor descriptor = MakeDescriptor(directory.Path());
			// Exercise the production default beside the destination. Caller-owned
			// staging roots are intentionally preserved even when they become empty.
			descriptor.stagingRoot.clear();
			const std::string prior = "last-known-good-recording";
			WriteText(descriptor.destinationPath, prior);
			AnimationRecorder recorder;
			context.Expect(recorder.Begin(descriptor) == AnimationAuthoringStatus::Ok
				&& recorder.Capture(MakeSample(100, 0.0F)) == AnimationAuthoringStatus::Ok,
				"A cancellable recording should stage without touching its destination.");
			context.Expect(recorder.Cancel() == AnimationAuthoringStatus::Canceled
				&& recorder.Finalize() == AnimationAuthoringStatus::Canceled,
				"Cancellation should remain observable and must never become a successful result.");
			const std::vector<std::uint8_t> after = ReadBytes(descriptor.destinationPath);
			context.Expect(std::string(after.begin(), after.end()) == prior,
				"Cancellation must preserve the exact prior last-known-good destination bytes.");
			const AnimationRecorderResult result = recorder.Result();
			context.Expect(result.outcome == AnimationRecorderOutcome::Canceled && !result.published,
				"Durable status should identify cancellation without claiming publication.");
			context.Expect(!std::filesystem::exists(result.stagingDirectory),
				"Cancellation should remove private staged chunks after capture stops.");
			context.Expect(!std::filesystem::exists(result.stagingDirectory.parent_path()),
				"Cancellation should remove the empty recorder-owned staging root without touching authored content.");
		});

		registry.Add("Editor.Animation.Recording.InvalidInputAndFailureAreAtomic", [](TestContext& context) {
			RecordingTestDirectory directory;
			const AnimationRecorderDescriptor descriptor = MakeDescriptor(directory.Path());
			const std::string prior = "known-good-before-failure";
			WriteText(descriptor.destinationPath, prior);
			AnimationRecorder recorder;
			context.Expect(recorder.Begin(descriptor) == AnimationAuthoringStatus::Ok,
				"The valid session should begin before malformed samples are tested.");
			AnimationRecordedSample invalid = MakeSample(100, 0.0F);
			invalid.values.front().scalarValue = std::numeric_limits<float>::quiet_NaN();
			context.Expect(recorder.Capture(std::move(invalid)) == AnimationAuthoringStatus::InvalidArgument,
				"A non-finite typed channel value must be rejected before it reaches a staging chunk.");
			context.Expect(recorder.Capture(MakeSample(99, 0.0F)) == AnimationAuthoringStatus::InvalidArgument,
				"A sample outside the declared source range must be rejected.");
			context.Expect(recorder.Finalize() == AnimationAuthoringStatus::Rejected,
				"A session with no valid samples must fail rather than publish an empty fake clip.");
			const std::vector<std::uint8_t> after = ReadBytes(descriptor.destinationPath);
			context.Expect(std::string(after.begin(), after.end()) == prior,
				"Validation failure must leave the prior destination byte-for-byte unchanged.");
			context.Expect(recorder.Result().outcome == AnimationRecorderOutcome::Failed
				&& !recorder.Result().candidateValidated && !recorder.Result().published,
				"Failure metadata must not claim codec validation or publication.");
		});

		registry.Add("Editor.Animation.Recording.AtomicOverwriteAndMalformedMetadataRejection", [](TestContext& context) {
			RecordingTestDirectory directory;
			const AnimationRecorderDescriptor descriptor = MakeDescriptor(directory.Path());
			WriteText(descriptor.destinationPath, "older-good-generation");
			AnimationRecorder recorder;
			context.Expect(recorder.Begin(descriptor) == AnimationAuthoringStatus::Ok,
				"A replacement recording should stage beside an existing destination.");
			CaptureThreeLinearSamples(context, recorder);
			context.Expect(recorder.Finalize() == AnimationAuthoringStatus::Ok
				&& recorder.Result().replacedPriorOutput,
				"Atomic publication should explicitly report replacing a prior good generation.");

			std::vector<std::uint8_t> bytes;
			AnimationClipAsset decoded = LoadPublishedClip(context, descriptor, bytes);
			AnimationClipAsset priorOutput = decoded;
			AnimationAssetEnvelope envelope{};
			std::memcpy(&envelope, bytes.data(), sizeof(envelope));
			std::vector<AnimationChunkEntry> chunks(envelope.chunkCount);
			std::memcpy(chunks.data(), bytes.data() + static_cast<std::size_t>(envelope.chunkTableOffset),
				chunks.size() * sizeof(AnimationChunkEntry));
			bool corruptedMetadata = false;
			for(const AnimationChunkEntry& chunk : chunks) {
				if(chunk.type == kAnimationChunkMetadata && chunk.size != 0U) {
					bytes[static_cast<std::size_t>(chunk.offset)] ^= 0x7FU;
					corruptedMetadata = true;
					break;
				}
			}
			context.Expect(corruptedMetadata,
				"Recorded clips should serialize their provenance into the optional metadata chunk.");
			// Repair the common-envelope hash so this test reaches the clip metadata
			// parser instead of stopping at the earlier content-integrity boundary.
			envelope.contentHash = ComputeAnimationContentHash(
				std::span<const std::uint8_t>(bytes).subspan(sizeof(AnimationAssetEnvelope)));
			std::memcpy(bytes.data(), &envelope, sizeof(envelope));
			context.Expect(DeserializeAnimationClipAsset(bytes, descriptor.compatibilityFingerprint, decoded)
				!= AnimationStatus::Ok,
				"Malformed optional recorder metadata must reject the candidate atomically.");
			context.Expect(decoded == priorOutput,
				"A failed metadata decode must not partially overwrite the caller's last-known-good clip object.");
		});

		registry.Add("Editor.Animation.Recording.LegacyCandidateIsNotFakePublication", [](TestContext& context) {
			AnimationRecorder recorder;
			std::string candidate;
			context.Expect(recorder.Begin(0U, 2U, 1U) == AnimationAuthoringStatus::Ok
				&& recorder.Capture({0U, {0.0F}}) == AnimationAuthoringStatus::Ok
				&& recorder.Capture({2U, {1.0F}}) == AnimationAuthoringStatus::Ok,
				"The compatibility scalar-channel wrapper should continue accepting bounded captures.");
			context.Expect(recorder.Finalize(candidate) == AnimationAuthoringStatus::Ok
				&& !candidate.empty(),
				"The compatibility wrapper should return validated binary candidate bytes.");
			context.Expect(recorder.Result().outcome == AnimationRecorderOutcome::CandidateReady
				&& recorder.Result().candidateValidated && !recorder.Result().published,
				"Candidate-only compatibility calls must never masquerade as atomic publication.");
			AnimationClipAsset clip;
			const std::span<const std::uint8_t> bytes(
				reinterpret_cast<const std::uint8_t*>(candidate.data()), candidate.size());
			context.Expect(DeserializeAnimationClipAsset(bytes, 0U, clip) == AnimationStatus::Ok,
				"Legacy callers should still receive a real loadable .teanim candidate, not the former text stub.");
		});
	}
}
