// Actual rollback, clip serialization and checked legacy decoding contracts.
#include "TestHarness.h"
#include "AnimationAuthorityTestHelpers.h"
#include "Animation/AnimationSystem.h"
#include "File Formats/TerraEngineAnimationFile.h"
#include "Animation/Evaluation/CpuClipEvaluator.h"
#include "Animation/Graph/EvaluationProgram.h"
#include "Editor/AssetCompilers/AnimationClipCompiler.h"
#include "Editor/AssetCompilers/MotionDatabaseCompiler.h"
#include "Core/Scheduling/FrameWorkload.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
namespace {
using namespace Animation;
using namespace AssetPipeline;
		AnimationGuid MakeIntegrationGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		template<typename Record>
		Record MakeAdvancedRecord(NativeAnimationRecordKind kind)
		{
			Record record{};
			record.header = {
				static_cast<std::uint32_t>(sizeof(Record)),
				kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(kind)};
			return record;
		}

		NativeAnimationUuid MakeNativeUuid(const AnimationGuid& value) noexcept
		{
			NativeAnimationUuid result{};
			std::memcpy(&result.first, value.value.data(), sizeof(result.first));
			std::memcpy(&result.second,
				value.value.data() + sizeof(result.first), sizeof(result.second));
			return result;
		}

		AnimationMatrix4x4 MakeIdentityMatrix()
		{
			AnimationMatrix4x4 matrix;
			matrix.values[0U] = 1.0F;
			matrix.values[5U] = 1.0F;
			matrix.values[10U] = 1.0F;
			matrix.values[15U] = 1.0F;
			return matrix;
		}

		std::shared_ptr<const SkeletonAsset> MakeIntegrationSkeleton()
		{
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId = MakeIntegrationGuid(40U);
			skeleton->compatibilityFingerprint = 0x4387aabbccdd1122ULL;
			skeleton->rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = 101U;
			root.parentIndex = -1;
			root.normalizedSourcePath = "/root";
			root.localBindTransform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
			root.localBindTransform.scale = {1.0F, 1.0F, 1.0F};
			skeleton->joints.push_back(root);
			skeleton->inverseBindMatrices.push_back(MakeIdentityMatrix());
			return skeleton;
		}

		std::shared_ptr<const AnimationClipAsset> MakeIntegrationClip(
			TestContext& context,
			const SkeletonAsset& skeleton,
			std::uint8_t assetSeed = 20U,
			float beginTranslationX = 0.0F,
			float endTranslationX = 1.0F,
			AnimationFixedTick durationTicks = 1)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeIntegrationGuid(assetSeed);
			input.skeletonAssetId = skeleton.assetId;
			input.compatibilityFingerprint = skeleton.compatibilityFingerprint;
			input.durationTicks = durationTicks;
			input.sampleRate = 60U;
			input.segmentDurationTicks = durationTicks;

			AnimationSourceTrack translation;
			translation.stableTargetId = skeleton.joints[0U].stableId;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey begin;
			begin.tick = 0;
			begin.vectorValue = {beginTranslationX, 0.0F, 0.0F};
			AnimationSourceKey end;
			end.tick = durationTicks;
			end.vectorValue = {endTranslationX, 0.0F, 0.0F};
			translation.keys = {begin, end};
			input.tracks.push_back(std::move(translation));
			input.events = {{1000U + assetSeed, 0, AnimationEventPayloadType::String, "ClipEvent", true, true}};

			auto clip = std::make_shared<AnimationClipAsset>();
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, *clip, report) == AnimationStatus::Ok,
				"The production integration fixture must compile one valid immutable clip.");
			return clip;
		}

		AnimatorInstanceDescriptor MakeIntegrationDescriptor(std::uint8_t seed)
		{
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0U] = seed;
			descriptor.componentGeneration = 3U;
			return descriptor;
		}

		// Every integration case owns a live runtime handle. Automatic cleanup keeps a
		// failed expectation from contaminating later all-or-none publication tests.
		class IntegrationAnimatorGuard final {
		public:
			IntegrationAnimatorGuard(
				AnimationSystem& system,
				AnimatorInstanceHandle handle) noexcept
				: system_(system), handle_(handle)
			{
			}

			~IntegrationAnimatorGuard()
			{
				if(handle_.IsValid()) (void)system_.InvalidateAnimatorInstance(handle_);
			}

			IntegrationAnimatorGuard(const IntegrationAnimatorGuard&) = delete;
			IntegrationAnimatorGuard& operator=(const IntegrationAnimatorGuard&) = delete;

		private:
			AnimationSystem& system_;
			AnimatorInstanceHandle handle_;
		};

		AnimatorPlaybackBinding MakeCrossfadeIntegrationBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeIntegrationSkeleton();
			auto idleClip = MakeIntegrationClip(
				context, *binding.skeleton, 24U, 0.0F, 0.0F);
			auto stopClip = MakeIntegrationClip(
				context, *binding.skeleton, 25U, 10.0F, 10.0F);
			auto reverseClip = MakeIntegrationClip(
				context, *binding.skeleton, 27U, -10.0F, -10.0F);
			binding.clip = idleClip;
			binding.clipGeneration = 5U;
			binding.residentSegments.assign(idleClip->segments.size(), 1U);

			auto program = std::make_shared<EvaluationProgram>();
			program->controllerAssetId = MakeIntegrationGuid(26U);
			program->compatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			program->states = {
				{100U, 0U, true, false, false, 5001U,
					EvaluationOpcode::SampleClip},
				{200U, 0U, false, false, false, 5002U,
					EvaluationOpcode::SampleClip},
				{300U, 0U, false, false, false, 5003U,
					EvaluationOpcode::SampleClip}};
			program->stableHash = ComputeEvaluationProgramStableHash(*program);
			binding.evaluationProgram = std::move(program);
			binding.controllerProgramRequired = true;
			binding.programGeneration = 4U;
			binding.stateId = 100U;
			binding.controllerClips = {
				{5001U, idleClip,
					std::vector<std::uint8_t>(idleClip->segments.size(), 1U), 5U},
				{5002U, stopClip,
					std::vector<std::uint8_t>(stopClip->segments.size(), 1U), 6U},
				{5003U, reverseClip,
					std::vector<std::uint8_t>(reverseClip->segments.size(), 1U), 7U}};
			binding.playback.loop = true;
			binding.playOnBind = true;
			binding.requestedExecutionPolicy =
				AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			for(const SkeletonJoint& joint : binding.skeleton->joints) {
				binding.jointStableIds.push_back(joint.stableId);
				binding.jointParents.push_back(joint.parentIndex);
			}
			return binding;
		}

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

		MotionDatabaseCompileInput MakeInput() {
			MotionDatabaseCompileInput input;
			input.assetId = MakeIntegrationGuid(10U);
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

void Frame(TestContext& context, AnimationSystem& system) {
	const auto result = Scheduling::ExecuteFrameWorkloadsCompatibility(system.DeclareFrameWorkloads(1.0F / 60.0F), "review-rollback-clips");
	context.Expect(result.valid && result.completed, "The actual animation publication stages must complete.");
}
NativeAnimationDeterminismSnapshot LatestDeterminism(TestContext& context, AnimationSystem& system, AnimatorInstanceHandle handle) {
	std::uint32_t count = 0U; (void)system.CopyDeterminismSnapshots(handle, {}, count);
	std::vector<NativeAnimationDeterminismSnapshot> values(count);
	context.Expect(count > 0U && system.CopyDeterminismSnapshots(handle, values, count) == AnimationStatus::Ok,
		"The actual protected determinism operation must publish a copied terminal snapshot.");
	return values.back();
}
}
void RegisterAnimationAssetRollbackParityTests(TestRegistry& registry) {
	registry.Add("Animation.Review.AssetRollback.ControllerClipRestoreReplacesSampledViewAtomically", [](TestContext& context) {
		// Both entry points use exact retained ticks and the same strict current
		// dependency qualification; exercising both catches a replay-only bypass.
		// Producer receipt identity outlives an individual animator. Each distinct
		// command needs its own sequence even when the fixture recreates its handle.
		std::uint64_t producerSequence = 1U;
		for(const auto restoreOperation : {NativeAnimationDeterminismOperation::Restore, NativeAnimationDeterminismOperation::ReplaySeek}) {
			auto& system = AnimationSystem::Instance(); system.Initialize();
			const auto handle = system.CreateAnimatorInstance(MakeIntegrationDescriptor(119U));
			IntegrationAnimatorGuard guard(system, handle);
			auto binding = MakeCrossfadeIntegrationBinding(context);
			const auto firstClip = binding.clip; const auto firstGeneration = binding.clipGeneration;
			context.Expect(system.BindAnimatorPlayback(handle, binding) == AnimationStatus::Ok, "The real controller fixture must bind all three immutable clip publications.");
			Frame(context, system);
			auto capture = MakeAdvancedRecord<NativeAnimationDeterminismRequest>(NativeAnimationRecordKind::DeterminismRequest);
			capture.animator = {handle.slotIndex, handle.generation}; capture.operation = NativeAnimationDeterminismOperation::Capture; capture.correlationId = 11901U;
			context.Expect(system.SubmitDeterminismRequest(capture).status == NativeAnimationStatus::Queued, "Capture must enter the typed protected request path.");
			Frame(context, system); const auto captured = LatestDeterminism(context, system, handle);
			context.Expect(captured.status == NativeAnimationStatus::Ok && captured.poseChecksum != 0U, "A restorable controller-A snapshot must exist.");
			AnimationCommand change; change.target = handle; change.kind = AnimationCommandKind::CrossFade; change.stateId = 200U;
			change.transitionTicks = 0; change.producerId = 119U; change.producerSequence = producerSequence++; change.correlationId = 11902U;
			const auto changeReceipt = system.SubmitCommand(change);
			context.Expect(changeReceipt.status == AnimationStatus::Queued,
				"Controller-B selection must use the real typed command queue. Status=" + std::to_string(static_cast<int>(changeReceipt.status))
				+ " RestoreOperation=" + std::to_string(static_cast<int>(restoreOperation)) + " ProducerSequence=" + std::to_string(change.producerSequence));
			Frame(context, system); Frame(context, system);
			const auto changed = PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			context.Expect(changed && changed->snapshot && changed->snapshot->pose && changed->snapshot->playback.currentStateId == 200U
				&& changed->snapshot->pose->LocalTransforms().front().translation.x > 9.0F, "The fixture must actually sample controller clip B before restoration.");
			auto restore = capture; restore.operation = restoreOperation; restore.sourceTick = captured.tick;
			restore.targetTick = captured.tick; restore.expectedChecksum = captured.poseChecksum; restore.correlationId = 11903U;
			context.Expect(system.SubmitDeterminismRequest(restore).status == NativeAnimationStatus::Queued, "Restoration must remain queued until its declared boundary.");
			Frame(context, system); const auto receipt = LatestDeterminism(context, system, handle);
			const auto restored = PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			context.Expect(receipt.status == NativeAnimationStatus::Ok && receipt.poseChecksum == captured.poseChecksum
				&& receipt.eventWatermark == captured.eventWatermark && restored && restored->snapshot && restored->snapshot->pose
				&& restored->clip.assetId == firstClip->assetId && restored->clip.generation == firstGeneration
				&& restored->snapshot->playback.currentStateId == 100U
				&& std::abs(restored->snapshot->pose->LocalTransforms().front().translation.x) < 0.0001F,
				"Rollback must restore clip identity/generation, exact pose and event cursor together. Status=" + std::to_string(static_cast<int>(receipt.status))
				+ " CapturedChecksum=" + std::to_string(captured.poseChecksum) + " RestoredChecksum=" + std::to_string(receipt.poseChecksum)
				+ " CapturedEvents=" + std::to_string(captured.eventWatermark) + " RestoredEvents=" + std::to_string(receipt.eventWatermark)
				+ " RestoredClipGeneration=" + std::to_string(restored ? restored->clip.generation : 0U)
				+ " RestoredX=" + std::to_string(restored && restored->snapshot && restored->snapshot->pose ? restored->snapshot->pose->LocalTransforms().front().translation.x : -999.0F));
			context.Expect(changed->snapshot->pose->LocalTransforms().front().translation.x > 9.0F, "A pinned pre-restore authority root must remain immutable.");
		}
	});
	registry.Add("Animation.Review.AssetRollback.ChangedClipPublicationRejectsHistoricalRestore", [](TestContext& context) {
		auto& system = AnimationSystem::Instance(); system.Initialize();
		const auto handle = system.CreateAnimatorInstance(MakeIntegrationDescriptor(120U)); IntegrationAnimatorGuard guard(system, handle);
		auto binding = MakeCrossfadeIntegrationBinding(context);
		context.Expect(system.BindAnimatorPlayback(handle, binding) == AnimationStatus::Ok, "The changed-asset control must begin with a genuine bound controller.");
		Frame(context, system);
		auto capture = MakeAdvancedRecord<NativeAnimationDeterminismRequest>(NativeAnimationRecordKind::DeterminismRequest);
		capture.animator = {handle.slotIndex, handle.generation}; capture.operation = NativeAnimationDeterminismOperation::Capture; capture.correlationId = 12001U;
		context.Expect(system.SubmitDeterminismRequest(capture).status == NativeAnimationStatus::Queued, "The old clip publication must be captured before replacement.");
		Frame(context, system); const auto captured = LatestDeterminism(context, system, handle);
		context.Expect(captured.status == NativeAnimationStatus::Ok, "The negative control needs a real historical snapshot.");
		// This is a genuine new payload and generation under the same clip UUID.
		// Replace its authored controller reference too; keeping an external pin
		// alive must not authorize old bytes to reenter the new binding.
		const auto obsoleteOwner = binding.clip; // Keep old bytes alive outside the current dependency set.

		auto replacement = MakeIntegrationClip(context, *binding.skeleton, 24U, 123.0F, 123.0F);
		binding.clip = replacement; binding.clipGeneration = 6U;
		binding.residentSegments.assign(replacement->segments.size(), 1U);
		binding.controllerClips[0U] = {5001U, replacement, std::vector<std::uint8_t>(replacement->segments.size(), 1U), 6U};
		context.Expect(system.BindAnimatorPlayback(handle, binding) == AnimationStatus::Ok, "The actual binding boundary must accept the changed complete payload.");
		Frame(context, system);
		const auto before = PinnedAnimationAuthorityRow::ForAnimator(system, handle);
		context.Expect(before && before->snapshot && before->snapshot->pose && before->snapshot->pose->LocalTransforms().front().translation.x > 122.0F,
			"The replacement must actually become the authoritative sampled clip before rejection is qualified.");
		auto restore = capture; restore.operation = NativeAnimationDeterminismOperation::Restore; restore.sourceTick = captured.tick;
		restore.targetTick = captured.tick; restore.expectedChecksum = captured.poseChecksum; restore.correlationId = 12002U;
		context.Expect(system.SubmitDeterminismRequest(restore).status == NativeAnimationStatus::Queued, "The stale correction must receive a real terminal decision at the next frame boundary.");
		Frame(context, system); const auto receipt = LatestDeterminism(context, system, handle);
		const auto after = PinnedAnimationAuthorityRow::ForAnimator(system, handle);
		context.Expect(obsoleteOwner && obsoleteOwner != replacement && obsoleteOwner->assetId == replacement->assetId,
			"The obsolete same-UUID owner must remain alive externally throughout the rejection check.");
		context.Expect(receipt.status != NativeAnimationStatus::Ok && after && after->snapshot && after->snapshot->pose
			&& after->clip.assetId == replacement->assetId && after->clip.generation == 6U
			&& after->snapshot->pose->LocalTransforms().front().translation.x > 122.0F,
			"Relaxing transient controller-clip selection checks must not restore changed/retired content or partially replace the new authoritative pose.");
	});

	registry.Add("Animation.Review.AssetCodec.ShuffledTracksPreserveRootMotionMeaning", [](TestContext& context) {
		AnimationClipCompileInput input; input.assetId = MakeIntegrationGuid(51U); input.skeletonAssetId = MakeIntegrationGuid(52U);
		input.compatibilityFingerprint = 77U; input.durationTicks = 4; input.segmentDurationTicks = 4;
		AnimationSourceTrack ordinary; ordinary.stableTargetId = 1U; ordinary.kind = AnimationClipTrackKind::Translation;
		ordinary.keys = {{0, {15.0F, 0.0F, 0.0F}}, {4, {15.0F, 0.0F, 0.0F}}};
		AnimationSourceTrack translation; translation.stableTargetId = 101U; translation.kind = AnimationClipTrackKind::RootTranslation;
		translation.keys = {{0, {}}, {4, {4.0F, 0.0F, 0.0F}}};
		AnimationSourceTrack yaw; yaw.stableTargetId = 101U; yaw.kind = AnimationClipTrackKind::RootYaw;
		AnimationSourceKey yawEnd; yawEnd.tick = 4; yawEnd.scalarValue = 0.4F; yaw.keys = {AnimationSourceKey{}, yawEnd};
		input.tracks = {ordinary, translation, yaw};
		AnimationClipAsset asset; AnimationClipCompileReport report;
		context.Expect(CompileAnimationClip(input, {}, asset, report) == AnimationStatus::Ok && asset.tracks.size() == 3U && asset.rootMotion.enabled,
			"The actual clip compiler must establish distinct ordinary/root channels in one checked segment.");
		const auto count = static_cast<std::uint32_t>(asset.tracks.size());
		std::reverse(asset.tracks.begin(), asset.tracks.end());
		asset.rootMotion.translationTrackIndex = count - 1U - asset.rootMotion.translationTrackIndex;
		asset.rootMotion.yawTrackIndex = count - 1U - asset.rootMotion.yawTrackIndex;
		std::vector<std::uint8_t> bytes;
		context.Expect(SerializeAnimationClipAsset(asset, bytes) == AnimationStatus::Ok, "A valid reordered asset must serialize through the production codec.");
		AnimationClipAsset decoded; const auto status = DeserializeAnimationClipAsset(bytes, 77U, decoded);
		context.Expect(status == AnimationStatus::Ok, "Sorted serialization must remap root indices so its own output remains a valid clip.");
		AnimationClipView view; context.Expect(view.Open(decoded) == AnimationStatus::Ok, "The decoded checked clip must open for actual sampling.");
		CpuClipEvaluationRequest request; request.tick = 2; CpuClipEvaluationResult sample;
		context.Expect(CpuClipEvaluator{}.Evaluate(view, request, sample) == AnimationStatus::Ok && sample.rootMotion.enabled
			&& std::abs(sample.rootMotion.translation.x - 2.0F) < 0.001F && std::abs(sample.rootMotion.yawRadians - 0.2F) < 0.001F,
			"Root translation and yaw must retain their independently authored meaning after sorting and decode.");
	});
	registry.Add("Animation.Review.AssetCodec.LegacyUnsortedSamplesProduceCanonicalStorageHashes", [](TestContext& context) {
		MotionDatabaseAsset canonical; MotionDatabaseCompileReport report;
		context.Expect(CompileMotionDatabase(MakeInput(), canonical, report) == AnimationStatus::Ok, "The actual compiler must establish the logical sample set.");
		auto unsorted = canonical; std::reverse(unsorted.samples.begin(), unsorted.samples.end());
		const auto sortedBytes = BuildLegacyDataOnlyAsset(canonical), unsortedBytes = BuildLegacyDataOnlyAsset(unsorted);
		MotionDatabaseAsset first, second;
		const auto firstStatus = DeserializeMotionDatabaseAsset(sortedBytes, canonical.compatibilityFingerprint, first);
		const auto secondStatus = DeserializeMotionDatabaseAsset(unsortedBytes, canonical.compatibilityFingerprint, second);
		context.Expect(firstStatus == AnimationStatus::Ok, "The canonical checked legacy envelope must decode before permutation qualification. Status=" + std::to_string(static_cast<int>(firstStatus)));
		context.Expect(secondStatus == AnimationStatus::Ok,
			"A checked legacy sample permutation must upgrade before canonical hashes are built. Status=" + std::to_string(static_cast<int>(secondStatus)));
		context.Expect(first.samples == second.samples && first.residentBlocks == second.residentBlocks
			&& first.featureSchemaFingerprint == second.featureSchemaFingerprint
			&& ComputeMotionDatabaseContentFingerprint(first) == ComputeMotionDatabaseContentFingerprint(second),
			"Equivalent legacy sample permutations must canonicalize before storage hashes and schema fingerprints are built.");
	});
}
}
