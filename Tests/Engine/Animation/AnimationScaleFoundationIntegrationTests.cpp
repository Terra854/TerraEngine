// AnimationScaleFoundationIntegrationTests.cpp - production ImplementationCheck-ImplementationCheck wiring checks.

#include "TestHarness.h"

#include "AnimationAuthorityTestHelpers.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Graph/EvaluationProgram.h"
#include "Animation/Retargeting/RetargetValidator.h"
#include "AnimationClipCompiler.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Core/Object/ObjectManager.h"
#include "Scripting/ScriptGlue.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

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

		AnimatorPlaybackBinding MakeIntegrationBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeIntegrationSkeleton();
			binding.clip = MakeIntegrationClip(context, *binding.skeleton);
			binding.updatePolicy = AnimatorUpdatePolicy::Manual;
			binding.requestedExecutionPolicy =
				AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			AnimationEvaluationExternalGenerations& generations =
				binding.evaluationGenerations;
			generations.consumerGeneration = 11U;
			generations.morphGeneration = 12U;
			generations.graphGeneration = 13U;
			generations.viewGeneration = 14U;
			generations.assetGeneration = 15U;
			generations.residencyGeneration = 16U;
			generations.materialGeneration = 17U;
			generations.rendererGeneration = 18U;
			generations.physicsGeneration = 19U;
			generations.deterministicGeneration = 20U;
			generations.unchangedInputReuseEligible = true;
			return binding;
		}

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

		std::shared_ptr<MotionDatabaseAsset> MakeMotionIntegrationDatabase(
			AnimationCompatibilityFingerprint compatibility);

		AnimatorPlaybackBinding MakeMotionSelectionIntegrationBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeIntegrationSkeleton();
			auto firstClip = MakeIntegrationClip(
				context, *binding.skeleton, 21U, 0.0F, 1.0F);
			auto selectedClip = MakeIntegrationClip(
				context, *binding.skeleton, 22U, 8.0F, 9.0F);
			binding.clip = firstClip;
			binding.residentSegments.assign(firstClip->segments.size(), 1U);

			auto program = std::make_shared<EvaluationProgram>();
			program->controllerAssetId = MakeIntegrationGuid(23U);
			program->compatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
			program->states = {
				{100U, 0U, true, false, false, 5001U, EvaluationOpcode::SampleClip},
				{200U, 0U, false, false, false, 5002U, EvaluationOpcode::SampleClip}};
			program->stableHash = ComputeEvaluationProgramStableHash(*program);
			binding.evaluationProgram = std::move(program);
			binding.programGeneration = 4U;
			binding.stateId = 100U;
			binding.controllerClips = {
				{5001U, firstClip, std::vector<std::uint8_t>(firstClip->segments.size(), 1U), 5U},
				{5002U, selectedClip, std::vector<std::uint8_t>(selectedClip->segments.size(), 1U), 6U}};

			auto database = MakeMotionIntegrationDatabase(
				binding.skeleton->compatibilityFingerprint);
			database->samples.front().sourceClipId = 5002U;
			database->samples.front().sourceTick = 1;
			database->featureSchemaFingerprint =
				ComputeMotionDatabaseSchemaFingerprint(*database);
			binding.motionDatabase = std::move(database);
			binding.motionDatabaseGeneration = 12U;
			binding.updatePolicy = AnimatorUpdatePolicy::Manual;
			binding.requestedExecutionPolicy =
				AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			for(const SkeletonJoint& joint : binding.skeleton->joints) {
				binding.jointStableIds.push_back(joint.stableId);
				binding.jointParents.push_back(joint.parentIndex);
			}
			return binding;
		}

		std::shared_ptr<SkeletonAsset> MakeRetargetIntegrationSkeleton(
			std::uint8_t guidSeed,
			AnimationCompatibilityFingerprint compatibility,
			std::uint64_t referencePoseHash,
			std::uint64_t rootId,
			std::uint64_t hipsId,
			float height)
		{
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId = MakeIntegrationGuid(guidSeed);
			skeleton->compatibilityFingerprint = compatibility;
			skeleton->referencePoseHash = referencePoseHash;
			skeleton->rootJointIndex = 0U;
			SkeletonJoint root;
			root.stableId = rootId;
			root.parentIndex = -1;
			root.displayName = "Root";
			root.normalizedSourcePath = "/root";
			root.semanticRole = "Root";
			root.deformsMesh = true;
			SkeletonJoint hips;
			hips.stableId = hipsId;
			hips.parentIndex = 0;
			hips.localBindTransform.translation = {0.0F, height, 0.0F};
			hips.displayName = "Hips";
			hips.normalizedSourcePath = "/root/hips";
			hips.semanticRole = "Hips";
			hips.deformsMesh = true;
			skeleton->joints = {root, hips};
			skeleton->inverseBindMatrices = {
				MakeIdentityMatrix(), MakeIdentityMatrix()};
			return skeleton;
		}

		std::shared_ptr<AnimationClipAsset> MakeRetargetIntegrationClip(
			TestContext& context,
			const SkeletonAsset& source)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeIntegrationGuid(151U);
			input.skeletonAssetId = source.assetId;
			input.compatibilityFingerprint = source.compatibilityFingerprint;
			input.durationTicks = 60;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 60;

			AnimationSourceTrack rootTranslation;
			rootTranslation.stableTargetId = source.joints[0U].stableId;
			rootTranslation.kind = AnimationClipTrackKind::Translation;
			rootTranslation.interpolation = AnimationInterpolation::Linear;
			AnimationSourceKey begin;
			begin.tick = 0;
			begin.vectorValue = {1.0F, 0.0F, 0.0F};
			AnimationSourceKey end;
			end.tick = 60;
			end.vectorValue = {2.0F, 0.0F, 0.0F};
			rootTranslation.keys = {begin, end};
			input.tracks.push_back(std::move(rootTranslation));

			auto clip = std::make_shared<AnimationClipAsset>();
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, *clip, report)
				== AnimationStatus::Ok,
				"The production retarget fixture must compile source-rig motion through the real cooker.");
			return clip;
		}

		std::shared_ptr<RetargetProfileAsset> MakeRetargetIntegrationProfile(
			const SkeletonAsset& source,
			const SkeletonAsset& target)
		{
			auto profile = std::make_shared<RetargetProfileAsset>();
			profile->assetId = MakeIntegrationGuid(171U);
			profile->runtimeGeneration = 9U;
			profile->compatibilityFingerprint = 0x51726774696e7465ULL;
			profile->sourceSkeletonAssetId = source.assetId;
			profile->targetSkeletonAssetId = target.assetId;
			profile->sourceSkeletonFingerprint = source.compatibilityFingerprint;
			profile->targetSkeletonFingerprint = target.compatibilityFingerprint;
			profile->sourceReferencePoseHash = source.referencePoseHash;
			profile->targetReferencePoseHash = target.referencePoseHash;
			profile->semanticMappings = {
				{1U, 0U, 0U, 0U, source.joints[0U].stableId,
					target.joints[0U].stableId, "Root", "Root", true},
				{2U, 1U, 0U, 0U, source.joints[1U].stableId,
					target.joints[1U].stableId, "Hips", "Hips", true}};
			profile->proportion.sourceReferenceHeightMeters = 1.0F;
			profile->proportion.targetReferenceHeightMeters = 2.0F;
			profile->root.sourceRootJointId = source.joints[0U].stableId;
			profile->root.targetRootJointId = target.joints[0U].stableId;
			profile->quality.authoredCoverage = 1.0F;
			profile->quality.reviewReason = "Production integration fixture";
			return profile;
		}

		std::shared_ptr<MotionDatabaseAsset> MakeMotionIntegrationDatabase(
			AnimationCompatibilityFingerprint compatibility)
		{
			auto database = std::make_shared<MotionDatabaseAsset>();
			database->assetId = MakeIntegrationGuid(191U);
			database->runtimeGeneration = 12U;
			database->compatibilityFingerprint = compatibility;
			database->sourceFingerprint = 0x6d6f74696f6e6462ULL;
			database->quantizationScales.fill(1.0F);
			MotionDatabaseSample sample;
			sample.stableSampleId = 7001U;
			sample.sourceClipId = 5001U;
			sample.sourceTick = 10;
			database->samples.push_back(sample);
			database->featureSchemaFingerprint =
				ComputeMotionDatabaseSchemaFingerprint(*database);
			return database;
		}

		AnimatorPlaybackBinding MakeAdvancedIntegrationBinding(TestContext& context)
		{
			AnimatorPlaybackBinding binding;
			auto source = MakeRetargetIntegrationSkeleton(
				111U, 0x1111222233334444ULL, 0x1111000011110000ULL,
				1U, 2U, 1.0F);
			auto target = MakeRetargetIntegrationSkeleton(
				131U, 0x5555666677778888ULL, 0x2222000022220000ULL,
				101U, 102U, 2.0F);
			auto profile = MakeRetargetIntegrationProfile(*source, *target);
			RetargetValidationResult validation;
			RetargetCompiledProfile compiled;
			context.Expect(CompileRetargetProfile(
				*profile, *source, *target, validation, compiled) == AnimationStatus::Ok,
				"The minimal source/target mapping must satisfy the production retarget validator.");

			binding.clip = MakeRetargetIntegrationClip(context, *source);
			binding.skeleton = std::move(target);
			binding.retargetSourceSkeleton = std::move(source);
			binding.retargetProfile = std::move(profile);
			binding.motionDatabase = MakeMotionIntegrationDatabase(
				binding.skeleton->compatibilityFingerprint);
			binding.clipGeneration = 7U;
			binding.skeletonGeneration = 8U;
			binding.retargetProfileGeneration = 9U;
			binding.retargetSourceSkeletonGeneration = 10U;
			binding.motionDatabaseGeneration = 12U;
			binding.residentSegments.assign(binding.clip->segments.size(), 1U);
			for(const SkeletonJoint& joint : binding.skeleton->joints) {
				binding.jointStableIds.push_back(joint.stableId);
				binding.jointParents.push_back(joint.parentIndex);
			}
			binding.requestedExecutionPolicy =
				AnimationExecutionPolicy::ForceCpuDeterministicFallback;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
			binding.updatePolicy = AnimatorUpdatePolicy::Manual;
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			return binding;
		}

		template<typename Value>
		void WriteFixtureValue(std::vector<std::uint8_t>& bytes, std::size_t offset, const Value& value) {
			if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset) throw std::out_of_range("GPU fixture byte range");
			std::memcpy(bytes.data() + offset, &value, sizeof(Value));
		}
		[[nodiscard]] std::shared_ptr<const GpuAnimationResidentPayloads>
		MakeCommandResidentPayload(float constantTranslationX)
		{
			GpuAnimationResidentPayloads payload{};

			// One ATD1 directory selects one valid constant track. All T/R/S lanes
			// may share it because atlas relocation is concerned with addresses,
			// while these cases exercise CPU planning and ownership, not shader execution.
			GpuAnimationTrackDirectoryHeader directory{};
			directory.entryCount = 1U;
			GpuAnimationTrackDirectoryEntry entry{};
			entry.startTick = 0U;
			entry.endTick = 60U;
			entry.trackHeaderOffsetBytes =
				sizeof(GpuAnimationTrackDirectoryHeader)
				+ sizeof(GpuAnimationTrackDirectoryEntry);
			AnimationCodecTrackHeaderV1 track{};
			track.encoding = AnimationCodecValueEncoding::Vector3Unorm16;
			track.flags = kAnimationCodecConstantTrack;
			track.byteSize = sizeof(AnimationCodecTrackHeaderV1);
			track.keyCount = 1U;
			track.segmentStartTick = 0;
			track.segmentEndTick = 60;
			track.constantVector.x = constantTranslationX;
			payload.packedClipBytes.resize(
				entry.trackHeaderOffsetBytes + sizeof(track), 0U);
			WriteFixtureValue(payload.packedClipBytes, 0U, directory);
			WriteFixtureValue(
				payload.packedClipBytes,
				sizeof(directory),
				entry);
			WriteFixtureValue(
				payload.packedClipBytes,
				entry.trackHeaderOffsetBytes,
				track);

			// The frozen program layout is a 64-byte directory followed by one
			// 16-byte joint record containing T/R/S locators and a reserved lane.
			payload.programWords.assign(20U, 0U);
			payload.programWords[16U] = 0U;
			payload.programWords[17U] = 0U;
			payload.programWords[18U] = 0U;

			// The one-joint skeleton contains a directory, parent table, explicit
			// per-joint depth, and one 64-byte palette/inverse-bind record.
			payload.skeletonTables.resize(112U, 0U);
			const std::uint32_t parentTableOffset = 16U;
			const std::uint32_t depthTableOffset = 32U;
			const std::uint32_t levelCount = 1U;
			const std::uint32_t paletteTableOffset = 48U;
			const std::int32_t rootParent = -1;
			WriteFixtureValue(payload.skeletonTables, 0U, parentTableOffset);
			WriteFixtureValue(payload.skeletonTables, 4U, depthTableOffset);
			WriteFixtureValue(payload.skeletonTables, 8U, levelCount);
			WriteFixtureValue(payload.skeletonTables, 12U, paletteTableOffset);
			WriteFixtureValue(payload.skeletonTables, parentTableOffset, rootParent);
			const std::uint32_t rootDepth = 0U;
			WriteFixtureValue(payload.skeletonTables, depthTableOffset, rootDepth);
			const std::uint32_t paletteJoint = 0U;
			WriteFixtureValue(payload.skeletonTables, paletteTableOffset, paletteJoint);
			const std::array<float, 12U> identityRows{
				1.0F, 0.0F, 0.0F, 0.0F,
				0.0F, 1.0F, 0.0F, 0.0F,
				0.0F, 0.0F, 1.0F, 0.0F};
			WriteFixtureValue(
				payload.skeletonTables,
				paletteTableOffset + 16U,
				identityRows);

			// Bounds always consume the morph resource, even when this atlas-only
			// fixture has no animated targets. A checked zero-count AMT1 table keeps
			// the validated request aligned with production deformable bindings.
			GpuAnimationMorphTableHeader morphHeader{};
			GpuAnimationMorphBoundsRecord morphBounds{};
			morphBounds.minimum = {-1.0F, -1.0F, -1.0F, 0.0F};
			morphBounds.maximum = {1.0F, 1.0F, 1.0F, 0.0F};
			payload.morphInputs.resize(
				sizeof(morphHeader) + sizeof(morphBounds), 0U);
			WriteFixtureValue(payload.morphInputs, 0U, morphHeader);
			WriteFixtureValue(
				payload.morphInputs,
				sizeof(morphHeader),
				morphBounds);
			return std::make_shared<const GpuAnimationResidentPayloads>(
				std::move(payload));
		}



		AnimatorPlaybackBinding MakeCompactCommandBinding(TestContext& context) {
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeIntegrationSkeleton();
			binding.clip = MakeIntegrationClip(context, *binding.skeleton, 20U, 1.0F, 1.0F, 60);
			binding.residentSegments.assign(binding.clip->segments.size(), 1U);
			binding.jointStableIds = {101U}; binding.jointParents = {-1};
			binding.requestedExecutionPolicy = AnimationExecutionPolicy::ForceGpuVisual;
			binding.updatePolicy = AnimatorUpdatePolicy::EveryFrame;
			binding.eventPolicy = AnimatorEventPolicy::Suppress;
			binding.playback.loop = false; binding.playOnBind = false;
			binding.rootMotionPolicy = AnimatorRootMotionPolicy::Ignore;
			binding.evaluationGenerations.residencyGeneration = 1U;
			// The same checked direct-clip program shape is authored by the real
			// component coordinator. No controller transition or CPU consumer is added.
			auto program = std::make_shared<EvaluationProgram>();
			program->controllerAssetId = MakeIntegrationGuid(80U);
			program->compatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
			program->scratchPlan = {64U, 16U};
			EvaluationInstruction instruction;
			instruction.sourceStableId = 1U; instruction.opcode = EvaluationOpcode::SampleClip;
			instruction.dependencyAssetId = binding.clip->assetId;
			instruction.dependencyGeneration = 1U;
			instruction.dependencyFingerprint = binding.skeleton->compatibilityFingerprint;
			program->instructions.push_back(instruction);
			program->bytecode.push_back(static_cast<std::uint8_t>(EvaluationOpcode::SampleClip));
			program->states.push_back({1U, 0U, true, false, false, 1U, EvaluationOpcode::SampleClip});
			EvaluationLayer layer; layer.stableId = 2U; layer.rootStateId = 1U; layer.defaultWeight = 1.0F;
			program->layers.push_back(layer);
			program->stableHash = ComputeEvaluationProgramStableHash(*program);
			EvaluationDispatchState checked;
			context.Expect(DispatchEvaluationProgramCpu(*program, checked) == AnimationStatus::Ok && checked.dispatchedInstructionCount == 1U,
				"The real direct-clip evaluation program must pass production validation.");
			binding.evaluationProgram = program;
			binding.controllerClips.push_back({1U, binding.clip, binding.residentSegments, 1U});
			binding.stateId = 1U;
			binding.gpuResidentPayloads = MakeCommandResidentPayload(1.0F);
			binding.gpuProgramResourceIndex = 0U;
			binding.gpuVisual.skeletonTopologyId = 1U;
			binding.gpuVisual.compatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
			binding.gpuVisual.clipResourceIndex = 0U; binding.gpuVisual.skeletonResourceIndex = 0U;
			binding.gpuVisual.morphResourceIndex = 0U;
			binding.gpuVisual.jointCount = 1U; binding.gpuVisual.paletteEntryCount = 1U;
			binding.gpuVisual.palettePartitionCount = 1U; binding.gpuVisual.looping = true;
			binding.gpuVisual.generations.resourceGeneration = 1U;
			binding.gpuVisual.renderIdentity.meshId[0] = 1U;
			binding.gpuVisual.renderIdentity.meshHeaderVersion = 1U;
			binding.gpuVisual.renderIdentity.deformablePayloadGeneration = 1U;
			binding.gpuVisual.renderIdentity.rendererGeneration = 1U;
			binding.gpuVisual.renderIdentity.sceneGeneration = ObjectManager::instance().GetSceneGeneration();
			binding.gpuVisual.renderIdentity.skeletonCompatibilityFingerprint = binding.skeleton->compatibilityFingerprint;
			auto fast = std::make_shared<AnimatorPlaybackBinding::GpuFastBinding>();
			fast->programResourceIndex = 0U; fast->visual = binding.gpuVisual;
			fast->residentPayloads = binding.gpuResidentPayloads;
			fast->residentByteCount = fast->residentPayloads->programWords.size() * sizeof(std::uint32_t)
				+ fast->residentPayloads->packedClipBytes.size() + fast->residentPayloads->skeletonTables.size() + fast->residentPayloads->morphInputs.size();
			binding.gpuFastBinding = fast;
			binding.gpuExecution = {true, true, true, true, static_cast<double>(fast->residentByteCount), 1.0e9};
			// Use the checked default path, never the planner's prevalidated flags.
			GpuAnimationLoweringInput lowering;
			lowering.program = {program, program->stableHash, 0U}; lowering.visual = binding.gpuVisual;
			lowering.visual.stableInstanceId = 1U;
			lowering.visual.generations.instanceGeneration = 1U;
			lowering.visual.generations.componentGeneration = 1U;
			lowering.visual.generations.workGeneration = 1U;
			lowering.visual.generations.programGeneration = 1U;
			lowering.visual.generations.controllerGeneration = 1U;
			lowering.visual.generations.clipGeneration = 1U;
			lowering.visual.generations.skeletonGeneration = 1U;
			lowering.visual.generations.meshGeneration = 1U;
			lowering.visual.generations.rendererGeneration = 1U;
			lowering.visual.generations.sceneGeneration = static_cast<std::uint32_t>(ObjectManager::instance().GetSceneGeneration());
			lowering.visual.generations.frameIdLow = 1U;
			lowering.effectiveMode = AnimationEffectiveExecutionMode::GpuVisual;
			lowering.residentPayloads = binding.gpuResidentPayloads;
			GpuAnimationWorkRequest lowered;
			context.Expect(LowerGpuAnimationWorkRequest(lowering, lowered) == GpuAnimationLoweringStatus::Ok
				&& ValidateGpuAnimationResidentPayloads(lowered, *binding.gpuResidentPayloads),
				"The production GPU lowerer must accept the exact fixture program and resident bytes.");
			return binding;
		}

		bool ExecuteIntegrationFrame(
			AnimationSystem& system,
			const char* frameId)
		{
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				system.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan plan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), frameId);
			return plan.valid && plan.completed;
		}
	}

	void RegisterAnimationScaleFoundationIntegrationTests(TestRegistry& registry)
	{
		registry.Add("Animation.ScaleFoundation.CompactSeekCommitsExactReceipt", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance(); system.Initialize();
			const auto animator = system.CreateAnimatorInstance(MakeIntegrationDescriptor(111U));
			IntegrationAnimatorGuard cleanup(system, animator);
			context.Expect(system.BindAnimatorPlayback(animator, MakeCompactCommandBinding(context)) == AnimationStatus::Ok,
				"The checked resident direct clip must bind through the native GPU-visual path.");
			for(int frame = 0; frame < 3; ++frame) context.Expect(ExecuteIntegrationFrame(system, "compact-command-warmup"), "The real declared compact graph must publish before command admission.");
			context.Expect(system.ReadProductionIntegrationSnapshot().itemCount == 0 && system.LoadPublishedAuthorityFrame(),
				"Warmup must establish the compact path with no complete CPU work items.");
			// Explicit commands must cross the compact-to-complete boundary and
			// settle their exact receipt only after the runtime/authority exchange.
			for(const auto sequence : {1ULL, 2ULL}) {
				AnimationCommand command; command.target = animator; command.kind = AnimationCommandKind::Seek;
				command.playbackTick = sequence == 1 ? 0 : 30; command.producerId = 111U;
				command.producerSequence = sequence; command.correlationId = 11100 + sequence;
				context.Expect(system.SubmitCommandBatch(std::span(&command, 1)).status == AnimationStatus::Queued, "Native seek admission must be queued atomically.");
				const auto prior = system.LoadPublishedAuthorityFrame();
				context.Expect(ExecuteIntegrationFrame(system, "compact-command-seek"), "The command-bearing graph must execute all declared phases.");
				const auto receipt = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
				const auto diagnostics = system.ReadProductionIntegrationSnapshot();
				context.Expect(receipt.status == AnimationStatus::Ok && receipt.publicationGeneration != 0,
					"A compact owner seek must receive a real terminal Ok publication rather than an orphaned Queued receipt: status="
					+ std::string(ToString(receipt.status)) + " plan=" + std::string(ToString(diagnostics.planStatus))
					+ " reason=" + std::string(ToString(diagnostics.rejectionReason)) + " item=" + std::to_string(diagnostics.rejectionItemIndex));
				const auto published = system.LoadPublishedAuthorityFrame();
				context.Expect(published && published != prior && published->authorityGeneration >= receipt.publicationGeneration
					&& published->Find(animator) && system.LoadAnimatorPlaybackState(animator).absoluteTick == command.playbackTick,
					"The accepted command receipt must name the actual committed authority and exact stopped playback tick.");
			}
		});
		registry.Add("Animation.ScaleFoundation.FailedCommandFrameSettlesWithoutPublication", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance(); system.Initialize();
			const auto animator = system.CreateAnimatorInstance(MakeIntegrationDescriptor(112U));
			IntegrationAnimatorGuard cleanup(system, animator);
			context.Expect(system.BindAnimatorPlayback(animator, MakeIntegrationBinding(context)) == AnimationStatus::Ok
				&& ExecuteIntegrationFrame(system, "failed-command-baseline"), "A real baseline authority must exist before cancellation.");
			const auto prior = system.LoadPublishedAuthorityFrame(); const auto priorTick = system.LoadAnimatorPlaybackState(animator).absoluteTick;
			AnimationCommand command; command.target = animator; command.kind = AnimationCommandKind::Seek; command.playbackTick = 1;
			command.producerId = 112U; command.producerSequence = 1; command.correlationId = 11201;
			context.Expect(system.SubmitCommand(command).status == AnimationStatus::Queued, "The cancellation fixture must admit one actual native command.");
			auto workloads = system.DeclareFrameWorkloads(0);
			// Cancel the seal phase after capture/planning/evaluation have consumed
			// the command. The protected finalizer must still settle the failed batch.
			workloads.at(5).work = [](TerraEngine::Scheduling::JobGraphContext&) {};
			const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(workloads), "failed-command-seal");
			const auto receipt = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
			context.Expect(plan.valid && plan.completed && (receipt.status == AnimationStatus::Rejected || receipt.status == AnimationStatus::Cancelled),
				"A drained command whose seal did not run must settle terminally without inventing Ok.");
			context.Expect(receipt.publicationGeneration == 0 && system.LoadPublishedAuthorityFrame() == prior
				&& system.LoadAnimatorPlaybackState(animator).absoluteTick == priorTick,
				"Failed command settlement must preserve the exact authority root and playback state without a partial commit.");
			// Once authority and runtime roots really commit, a later specialist
			// finalizer omission cannot turn that accepted seek into a rejection.
			command.producerSequence = 2; command.correlationId = 11202;
			context.Expect(system.SubmitCommand(command).status == AnimationStatus::Queued, "A fresh exact command must be admissible after failed-frame settlement.");
			workloads = system.DeclareFrameWorkloads(0); workloads.at(8).work = [](TerraEngine::Scheduling::JobGraphContext&) {};
			const auto committedPlan = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(workloads), "committed-command-finalizer-omitted");
			const auto committedAuthority = system.LoadPublishedAuthorityFrame();
			context.Expect(committedPlan.valid && committedPlan.completed && committedAuthority != prior
				&& system.LoadAnimatorPlaybackState(animator).absoluteTick == command.playbackTick,
				"The omitted-finalizer fixture must first prove a genuine authority/runtime commit.");
			context.Expect(ExecuteIntegrationFrame(system, "committed-command-recovery"), "The next actual capture must settle the committed historical receipt before replacing its command vector.");
			const auto committed = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
			context.Expect(committed.status == AnimationStatus::Ok && committedAuthority
				&& committed.publicationGeneration == committedAuthority->authorityGeneration,
				"An actually committed command must retain its true Ok publication even when subsequent specialist work does not run.");
			// Lifecycle retirement may happen before another capture. A committed
			// command still names its historical root, while an unsealed command
			// must cancel without applying any of its pending playback changes.
			command.producerSequence = 3; command.correlationId = 11203; command.playbackTick = 0;
			context.Expect(system.SubmitCommand(command).status == AnimationStatus::Queued, "Lifecycle recovery must admit an exact fresh command.");
			workloads = system.DeclareFrameWorkloads(0); workloads.at(8).work = [](TerraEngine::Scheduling::JobGraphContext&) {};
			const auto lifecyclePlan = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(workloads), "committed-command-lifecycle");
			const auto lifecycleAuthority = system.LoadPublishedAuthorityFrame();
			context.Expect(lifecyclePlan.valid && lifecyclePlan.completed && lifecycleAuthority
				&& system.LoadAnimatorPlaybackState(animator).absoluteTick == 0, "The lifecycle fixture must first prove its actual protected runtime commit.");
			(void)system.NotifyLifecycleInvalidation(AnimationLifecycleBoundary::Renderer);
			const auto lifecycleReceipt = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
			context.Expect(lifecycleReceipt.status == AnimationStatus::Ok && lifecycleAuthority
				&& lifecycleReceipt.publicationGeneration == lifecycleAuthority->authorityGeneration,
				"Lifecycle retirement must preserve the exact historical committed receipt before discarding its token.");
			context.Expect(ExecuteIntegrationFrame(system, "lifecycle-command-baseline"), "A fresh graph must establish the post-lifecycle baseline.");
			const auto beforeFailedLifecycleTick = system.LoadAnimatorPlaybackState(animator).absoluteTick;
			command.producerSequence = 4; command.correlationId = 11204; command.playbackTick = 1;
			context.Expect(system.SubmitCommand(command).status == AnimationStatus::Queued, "The uncommitted lifecycle fixture must admit its exact native command.");
			workloads = system.DeclareFrameWorkloads(0);
			workloads.at(5).work = [](TerraEngine::Scheduling::JobGraphContext&) {};
			workloads.at(8).work = [](TerraEngine::Scheduling::JobGraphContext&) {};
			const auto failedLifecyclePlan = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(std::move(workloads), "uncommitted-command-lifecycle");
			(void)system.NotifyLifecycleInvalidation(AnimationLifecycleBoundary::Renderer);
			const auto failedLifecycleReceipt = system.ReadCommandReceipt(command.producerId, command.producerSequence, command.correlationId);
			context.Expect(failedLifecyclePlan.valid && failedLifecyclePlan.completed
				&& (failedLifecycleReceipt.status == AnimationStatus::Rejected || failedLifecycleReceipt.status == AnimationStatus::Cancelled)
				&& failedLifecycleReceipt.publicationGeneration == 0
				&& system.LoadAnimatorPlaybackState(animator).absoluteTick == beforeFailedLifecycleTick
				&& !system.LoadPublishedAuthorityFrame(),
				"Lifecycle retirement must cancel the drained uncommitted batch, retain playback, and clear the retired public root.");
		});
		registry.Add("Animation.ScaleFoundation.ZeroChannelShellAdvancesAuthority", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(90U));
			IntegrationAnimatorGuard cleanup(system, animator);
			const std::uint64_t priorAuthority = system.AuthorityPublicationGeneration();

			context.Expect(animator.IsValid()
				&& ExecuteIntegrationFrame(system, "animation-scale-zero-channel"),
				"A never-bound zero-channel animator must complete through the production scheduler.");
			const PinnedAnimationAuthorityRow published =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			context.Expect(published && published->safeFallback && published->snapshot
				&& published->snapshot->pose
				&& published->snapshot->pose->LocalTransforms().empty()
				&& system.AuthorityPublicationGeneration() > priorAuthority,
				"The typed zero-channel safe pose must advance authority without fabricating skeleton channels.");
		});

		registry.Add("Animation.ScaleFoundation.ProductionPlanCacheAndMemoryAreConnected", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(91U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			context.Expect(animator.IsValid()
				&& system.BindAnimatorPlayback(animator, binding) == AnimationStatus::Ok,
				"A valid manual CPU animator must enter the production scheduler path.");

			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-first"),
				"The first production frame must evaluate and publish a complete pose.");
			const PinnedAnimationAuthorityRow first =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			const AnimationProductionIntegrationSnapshot firstDiagnostics =
				system.ReadProductionIntegrationSnapshot();
			context.Expect(first && firstDiagnostics.planStatus == AnimationStatus::Ok
				&& firstDiagnostics.itemCount == 1U && firstDiagnostics.rangeCount == 1U
				&& firstDiagnostics.scratchBytes != 0U
				&& firstDiagnostics.deterministicPublicationValidated,
				"Production capture must validate one deterministic plan and its disjoint scratch range.");

			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-reuse"),
				"An unchanged eligible manual animator must complete through cache reuse.");
			const PinnedAnimationAuthorityRow second =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			const AnimationProductionIntegrationSnapshot reusedDiagnostics =
				system.ReadProductionIntegrationSnapshot();
			const std::size_t posePoolIndex = static_cast<std::size_t>(
				AnimationMemoryClass::PosePool);
			context.Expect(second == first && reusedDiagnostics.reusedItemCount == 1U
				&& reusedDiagnostics.cacheEntryCount == 1U
				&& reusedDiagnostics.memory.classes[posePoolIndex].bytesInUse != 0U
				&& reusedDiagnostics.memory.classes[posePoolIndex].highWaterBytes
					>= reusedDiagnostics.memory.classes[posePoolIndex].bytesInUse,
				"A truthful unchanged cache hit must retain the exact pose and its bounded lifetime lease.");
		});

		registry.Add("Animation.ScaleFoundation.SynchronizedVisualClonesReuseCpuEvaluation",
			[](TestContext& context) {
				AnimationSystem& system = AnimationSystem::Instance();
				system.Initialize();
				const AnimatorInstanceHandle firstAnimator =
					system.CreateAnimatorInstance(MakeIntegrationDescriptor(92U));
				const AnimatorInstanceHandle secondAnimator =
					system.CreateAnimatorInstance(MakeIntegrationDescriptor(93U));
				IntegrationAnimatorGuard firstCleanup(system, firstAnimator);
				IntegrationAnimatorGuard secondCleanup(system, secondAnimator);
				AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
				binding.evaluationGenerations.unchangedInputReuseEligible = false;
				context.Expect(firstAnimator.IsValid() && secondAnimator.IsValid()
					&& system.BindAnimatorPlayback(firstAnimator, binding)
						== AnimationStatus::Ok
					&& system.BindAnimatorPlayback(secondAnimator, std::move(binding))
						== AnimationStatus::Ok
					&& ExecuteIntegrationFrame(
						system, "animation-scale-synchronized-clones"),
					"Two synchronized visual-only CPU animators must complete through one scheduler range.");

				const PinnedAnimationAuthorityRow first =
					PinnedAnimationAuthorityRow::ForAnimator(system, firstAnimator);
				const PinnedAnimationAuthorityRow second =
					PinnedAnimationAuthorityRow::ForAnimator(system, secondAnimator);
				const AnimationProductionIntegrationSnapshot diagnostics =
					system.ReadProductionIntegrationSnapshot();
				context.Expect(first && second && first != second
					&& first->key.Handle() == firstAnimator
					&& second->key.Handle() == secondAnimator
					&& first->snapshot && second->snapshot
					&& first->snapshot->pose && second->snapshot->pose
					&& first->snapshot->pose->Generation().animator == firstAnimator
					&& second->snapshot->pose->Generation().animator == secondAnimator
					&& first->visualPoseContentIdLow != 0U
					&& first->visualPoseContentIdHigh != 0U
					&& first->visualPoseContentIdLow
						== second->visualPoseContentIdLow
					&& first->visualPoseContentIdHigh
						== second->visualPoseContentIdHigh
					&& std::ranges::equal(
						first->snapshot->pose->LocalTransforms(),
						second->snapshot->pose->LocalTransforms())
					&& diagnostics.reusedItemCount == 1U,
					"A shared visual evaluation must preserve distinct instance generations while publishing identical pose channels.");
			});

		registry.Add("Animation.ScaleFoundation.ScriptCrossfadePublishesContinuousPose",
			[](TestContext& context) {
				AnimationSystem& system = AnimationSystem::Instance();
				system.Initialize();
				const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
					MakeIntegrationDescriptor(99U));
				IntegrationAnimatorGuard cleanup(system, animator);
				AnimatorPlaybackBinding binding =
					MakeCrossfadeIntegrationBinding(context);
				context.Expect(animator.IsValid()
					&& system.BindAnimatorPlayback(animator, std::move(binding))
						== AnimationStatus::Ok
					&& ExecuteIntegrationFrame(
						system, "animation-crossfade-source"),
					"The script-crossfade fixture must publish its source state before the command.");

				auto publishedX = [&system, animator]() {
					const PinnedAnimationAuthorityRow publication =
						PinnedAnimationAuthorityRow::ForAnimator(system, animator);
					if(!publication || !publication->snapshot
						|| !publication->snapshot->pose
						|| publication->snapshot->pose->LocalTransforms().empty()) {
						return std::numeric_limits<float>::quiet_NaN();
					}
					return publication->snapshot->pose
						->LocalTransforms().front().translation.x;
				};
				context.Expect(std::abs(publishedX()) <= 0.01F,
					"The source clip must begin at its authored zero-translation pose.");

				NativeAnimationCommandRequest crossfade =
					MakeAdvancedRecord<NativeAnimationCommandRequest>(
						NativeAnimationRecordKind::CommandRequest);
				crossfade.animator = {
					animator.slotIndex,
					animator.generation};
				crossfade.command = NativeAnimationCommandKind::CrossFade;
				crossfade.stateId = 200U;
				crossfade.transitionSeconds =
					4.0F / static_cast<float>(kAnimationFixedTicksPerSecond);
				crossfade.producerId = 99U;
				crossfade.producerSequence = 1U;
				crossfade.correlationId = 9901U;
				crossfade.idempotencyKey = 99001U;
				crossfade.cancellationToken = 990001U;
				NativeAnimationCommandReceipt crossfadeReceipt =
					MakeAdvancedRecord<NativeAnimationCommandReceipt>(
						NativeAnimationRecordKind::CommandReceipt);
				context.Expect(Native_Animation_SubmitCommand(
						&crossfade, &crossfadeReceipt)
						== NativeAnimationStatus::Queued,
					"The same copied native boundary used by C# and exported builds must queue a typed four-tick crossfade.");

				std::array<float, 5U> samples{};
				std::array<float, 5U> progress{};
				std::array<std::uint64_t, 5U> states{};
				for(std::size_t frame = 0U; frame < samples.size(); ++frame) {
					const std::string frameName =
						"animation-script-crossfade-" + std::to_string(frame);
					context.Expect(ExecuteIntegrationFrame(
						system, frameName.c_str()),
						"Every transition tick must complete through the production scheduler.");
					samples[frame] = publishedX();
					const PinnedAnimationAuthorityRow publication =
						PinnedAnimationAuthorityRow::ForAnimator(system, animator);
					if(publication && publication->snapshot) {
						progress[frame] =
							publication->snapshot->playback.transitionProgress;
						states[frame] =
							publication->snapshot->playback.currentStateId;
					}
				}
				context.Expect(std::abs(samples[0U] - 0.0F) <= 0.01F
					&& std::abs(samples[1U] - 2.5F) <= 0.02F
					&& std::abs(samples[2U] - 5.0F) <= 0.02F
					&& std::abs(samples[3U] - 7.5F) <= 0.02F
					&& std::abs(samples[4U] - 10.0F) <= 0.02F,
					std::string(
						"A scripted crossfade must publish the blended source/destination pose on every fixed tick instead of holding then snapping. samples=")
						+ std::to_string(samples[0U]) + ","
						+ std::to_string(samples[1U]) + ","
						+ std::to_string(samples[2U]) + ","
						+ std::to_string(samples[3U]) + ","
						+ std::to_string(samples[4U])
						+ " progress=" + std::to_string(progress[0U]) + ","
						+ std::to_string(progress[1U]) + ","
						+ std::to_string(progress[2U]) + ","
						+ std::to_string(progress[3U]) + ","
						+ std::to_string(progress[4U])
						+ " states=" + std::to_string(states[0U]) + ","
						+ std::to_string(states[1U]) + ","
						+ std::to_string(states[2U]) + ","
						+ std::to_string(states[3U]) + ","
						+ std::to_string(states[4U])
						+ " receipt="
						+ std::to_string(static_cast<std::uint32_t>(
							system.ReadCommandReceipt(
								crossfade.producerId,
								crossfade.producerSequence,
								crossfade.correlationId).status)));
				const PinnedAnimationAuthorityRow completed =
					PinnedAnimationAuthorityRow::ForAnimator(system, animator);
				context.Expect(completed && completed->snapshot
					&& completed->snapshot->playback.currentStateId == 200U
					&& completed->snapshot->playback.previousStateId == 0U,
					"The final blended tick must promote the destination state atomically.");
			});

		registry.Add("Animation.ScaleFoundation.InterruptedCrossfadeCapturesPublishedPose",
			[](TestContext& context) {
				AnimationSystem& system = AnimationSystem::Instance();
				system.Initialize();
				const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
					MakeIntegrationDescriptor(100U));
				IntegrationAnimatorGuard cleanup(system, animator);
				AnimatorPlaybackBinding binding =
					MakeCrossfadeIntegrationBinding(context);
				context.Expect(animator.IsValid()
					&& system.BindAnimatorPlayback(animator, std::move(binding))
						== AnimationStatus::Ok
					&& ExecuteIntegrationFrame(
						system, "animation-interruption-source"),
					"The interruption fixture must publish its source pose.");

				auto submitCrossfade = [&system, animator](
					std::uint64_t stateId,
					std::uint64_t sequence) {
					AnimationCommand command;
					command.target = animator;
					command.kind = AnimationCommandKind::CrossFade;
					command.stateId = stateId;
					command.transitionTicks = 4;
					command.producerId = 100U;
					command.producerSequence = sequence;
					command.correlationId = 10000U + sequence;
					command.idempotencyKey = 100000U + sequence;
					command.cancellationToken = 1000000U + sequence;
					return system.SubmitCommand(command).status;
				};
				auto publishedX = [&system, animator]() {
					const PinnedAnimationAuthorityRow publication =
						PinnedAnimationAuthorityRow::ForAnimator(system, animator);
					if(!publication || !publication->snapshot
						|| !publication->snapshot->pose
						|| publication->snapshot->pose->LocalTransforms().empty()) {
						return std::numeric_limits<float>::quiet_NaN();
					}
					return publication->snapshot->pose
						->LocalTransforms().front().translation.x;
				};

				context.Expect(submitCrossfade(200U, 1U)
						== AnimationStatus::Queued
					&& ExecuteIntegrationFrame(
						system, "animation-interruption-first-0")
					&& ExecuteIntegrationFrame(
						system, "animation-interruption-first-1")
					&& ExecuteIntegrationFrame(
						system, "animation-interruption-first-2")
					&& std::abs(publishedX() - 5.0F) <= 0.02F,
					"The first transition must reach its deterministic halfway pose.");
				context.Expect(submitCrossfade(300U, 2U)
						== AnimationStatus::Queued,
					"An active gameplay transition must accept a replacement destination.");

				std::array<float, 5U> samples{};
				for(std::size_t frame = 0U; frame < samples.size(); ++frame) {
					const std::string frameName =
						"animation-interruption-replacement-"
						+ std::to_string(frame);
					context.Expect(ExecuteIntegrationFrame(
						system, frameName.c_str()),
						"The replacement transition must complete through the production scheduler.");
					samples[frame] = publishedX();
				}
				context.Expect(std::abs(samples[0U] - 5.0F) <= 0.02F
					&& std::abs(samples[1U] - 1.25F) <= 0.02F
					&& std::abs(samples[2U] + 2.5F) <= 0.02F
					&& std::abs(samples[3U] + 6.25F) <= 0.02F
					&& std::abs(samples[4U] + 10.0F) <= 0.02F,
					"Replacing a crossfade must use the last complete blended pose as its immutable source, avoiding a jump back to either old endpoint.");
			});

		registry.Add("Animation.ScaleFoundation.LifecycleCancellationRejectsCapturedProductionWork", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(92U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			(void)system.BindAnimatorPlayback(animator, binding);
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-cancel-baseline"),
				"The cancellation fixture must first establish one complete publication.");
			const PinnedAnimationAuthorityRow prior =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			const std::uint64_t priorAuthority = system.AuthorityPublicationGeneration();
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> stale =
				system.DeclareFrameWorkloads(1.0F / 60.0F);

			context.Expect(system.NotifyLifecycleInvalidation(
				AnimationLifecycleBoundary::Scene) == AnimationStatus::Ok,
				"A protected scene change must advance the live CPU and GPU generation latches.");
			const TerraEngine::Scheduling::FrameExecutionPlan stalePlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(stale), "animation-scale-cancelled");
			const PinnedAnimationAuthorityRow current =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			context.Expect(stalePlan.valid && stalePlan.completed
				&& system.AuthorityPublicationGeneration() > priorAuthority
				&& current == nullptr
				&& prior,
				"Cancelled captured work must not republish after lifecycle retirement; new readers see no stale authority while an already pinned immutable root remains valid.");
		});

		registry.Add("Animation.ScaleFoundation.RecoverableFailureHoldsTypedPriorGeneration", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(93U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			(void)system.BindAnimatorPlayback(animator, binding);
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-fallback-baseline"),
				"The fallback fixture must establish one typed last-known-good generation.");
			const PinnedAnimationAuthorityRow prior =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);

			AnimatorPlaybackBinding missing = binding;
			missing.clip.reset();
			context.Expect(system.BindAnimatorPlayback(animator, std::move(missing))
				== AnimationStatus::MissingAsset,
				"A compatible missing clip must enter explicit recoverable fallback.");
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-hold-last"),
				"The failed generation must still complete through typed hold-last selection.");
			const AnimationProductionIntegrationSnapshot diagnostics =
				system.ReadProductionIntegrationSnapshot();
			context.Expect(PinnedAnimationAuthorityRow::ForAnimator(system, animator) == prior
				&& diagnostics.fallbackItemCount == 1U,
				"Recoverable failure must preserve the exact compatible prior pose without republishing root motion.");
		});

		registry.Add("Animation.ScaleFoundation.SkeletonLossHoldsExactCompatiblePriorPose", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(94U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			(void)system.BindAnimatorPlayback(animator, binding);
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-skeleton-loss-baseline"),
				"The skeleton-loss fixture must first publish one compatible complete pose.");
			const PinnedAnimationAuthorityRow prior =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);

			AnimatorPlaybackBinding missingSkeleton = binding;
			missingSkeleton.skeleton.reset();
			// BindAnimatorPlayback takes an owned binding value, so the successful
			// baseline bind cannot mutate this caller-side copy. A loss notification
			// must therefore retain the copied channel identities it needs to prove
			// that the previous pose is still skeleton-compatible.
			for(const SkeletonJoint& joint : binding.skeleton->joints) {
				missingSkeleton.jointStableIds.push_back(joint.stableId);
			}
			context.Expect(!missingSkeleton.jointStableIds.empty()
				&& missingSkeleton.fallbackCompatibilityFingerprint != 0U
				&& system.BindAnimatorPlayback(animator, std::move(missingSkeleton))
					== AnimationStatus::MissingAsset,
				"Skeleton loss must retain copied channel and compatibility identities for fallback validation.");
			context.Expect(ExecuteIntegrationFrame(system, "animation-scale-skeleton-loss-hold"),
				"A skeleton-loss generation must complete through compatible hold-last selection.");
			const AnimationProductionIntegrationSnapshot diagnostics =
				system.ReadProductionIntegrationSnapshot();
			context.Expect(prior && PinnedAnimationAuthorityRow::ForAnimator(system, animator) == prior
				&& diagnostics.fallbackItemCount == 1U,
				"Retained channels without a skeleton must not publish manufactured identity transforms before hold-last.");
		});

		registry.Add("Animation.ScaleFoundation.AdvancedRequestsReachProductionWorkers", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(95U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeAdvancedIntegrationBinding(context);
			const AnimationGuid profileId = binding.retargetProfile->assetId;
			const AnimationGuid databaseId = binding.motionDatabase->assetId;
			context.Expect(animator.IsValid()
				&& system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok,
				"A source-rig clip and its target-rig profile must bind as one immutable production bundle.");

			NativeAnimationRetargetRequest activate =
				MakeAdvancedRecord<NativeAnimationRetargetRequest>(
					NativeAnimationRecordKind::RetargetRequest);
			activate.animator = {animator.slotIndex, animator.generation};
			activate.operation = NativeAnimationRetargetOperation::ActivateProfile;
			activate.profileAssetId = MakeNativeUuid(profileId);
			activate.profileGeneration = 9U;
			activate.correlationId = 9501U;

			NativeAnimationMotionMatchRequest setDatabase =
				MakeAdvancedRecord<NativeAnimationMotionMatchRequest>(
					NativeAnimationRecordKind::MotionMatchRequest);
			setDatabase.animator = {animator.slotIndex, animator.generation};
			setDatabase.operation = NativeAnimationMotionMatchOperation::SetDatabase;
			setDatabase.databaseAssetId = MakeNativeUuid(databaseId);
			setDatabase.databaseGeneration = 12U;
			setDatabase.featureCount = static_cast<std::uint32_t>(
				kMotionDatabaseFeatureCount);
			setDatabase.correlationId = 9502U;
			context.Expect(system.SubmitRetargetRequest(activate).status
					== NativeAnimationStatus::Queued
				&& system.SubmitMotionMatchRequest(setDatabase).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-advanced-activate"),
				"Profile and database activation must become visible only after one protected publication.");

			NativeAnimationMotionMatchRequest query = setDatabase;
			query.operation = NativeAnimationMotionMatchOperation::Query;
			query.queryId = 9503U;
			query.correlationId = 9503U;
			context.Expect(system.SubmitMotionMatchRequest(query).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-advanced-evaluate"),
				"The following worker frame must evaluate both the active retarget and motion query.");
			context.Expect(ExecuteIntegrationFrame(system, "animation-advanced-retain-query"),
				"A retained query on a direct-clip animator must remain diagnostic-only instead of poisoning a later frame that has no authored motion node.");

			const PinnedAnimationAuthorityRow published =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			std::uint32_t retargetCount = 0U;
			std::uint32_t motionCount = 0U;
			(void)system.CopyRetargetSnapshots(animator, {}, retargetCount);
			(void)system.CopyMotionMatchSnapshots(animator, {}, motionCount);
			std::vector<NativeAnimationRetargetSnapshot> retarget(retargetCount);
			std::vector<NativeAnimationMotionMatchSnapshot> motion(motionCount);
			const AnimationStatus retargetCopy = system.CopyRetargetSnapshots(
				animator, retarget, retargetCount);
			const AnimationStatus motionCopy = system.CopyMotionMatchSnapshots(
				animator, motion, motionCount);
			const auto measuredRetarget = std::find_if(
				retarget.rbegin(), retarget.rend(), [&published](const auto& value) {
					return published && published->snapshot
						&& value.poseGeneration
							== published->snapshot->publicationGeneration;
				});
			const auto selectedMotion = std::find_if(
				motion.rbegin(), motion.rend(), [&query](const auto& value) {
					return value.queryId == query.queryId;
				});
			const auto local = published && published->snapshot
				&& published->snapshot->pose
				? published->snapshot->pose->LocalTransforms()
				: std::span<const AnimationTransform>{};
			context.Expect(retargetCopy == AnimationStatus::Ok
				&& motionCopy == AnimationStatus::Ok,
				"Advanced observations must remain readable through the bounded copy protocol.");
			context.Expect(static_cast<bool>(published),
				"Advanced worker evaluation must retain an immutable publication.");
			context.Expect(published->status == AnimationStatus::Ok,
				std::string("The active retarget and motion query must complete without selecting a safe-pose status; actual status=")
					+ std::to_string(static_cast<std::uint32_t>(published->status))
					+ ", retarget status="
					+ std::to_string(measuredRetarget == retarget.rend()
						? UINT32_MAX
						: static_cast<std::uint32_t>(measuredRetarget->status))
					+ ", latest retarget status="
					+ std::to_string(retarget.empty() ? UINT32_MAX
						: static_cast<std::uint32_t>(retarget.back().status)));
			context.Expect(local.size() == 2U,
				"The production publication must contain every target-skeleton joint.");
			context.Expect(local.size() == 2U && local.front().translation.x > 1.5F,
				"The active source-rig clip must be evaluated into a visibly scaled target-rig root transform.");
			context.Expect(measuredRetarget != retarget.rend()
				&& measuredRetarget->status == NativeAnimationStatus::Ok
				&& measuredRetarget->sourceSkeletonGeneration == 10U
				&& measuredRetarget->targetSkeletonGeneration == 8U,
				"The retarget observation must report evaluator-measured pose and skeleton generations.");
			context.Expect(selectedMotion != motion.rend()
				&& selectedMotion->status == NativeAnimationStatus::Ok
				&& selectedMotion->selectedSampleId == 7001U,
				"The worker-side motion query must publish the selected database sample rather than NotReady.");
		});

		registry.Add("Animation.ScaleFoundation.RetargetRebuildUsesPreEvaluationGeneration", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(96U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeAdvancedIntegrationBinding(context);
			// The regression requires this frame to replace the live publication. The
			// shared fixture is manual by default, so opt into ordinary frame advancement
			// instead of accidentally testing an unchanged-cache reuse path.
			binding.updatePolicy = AnimatorUpdatePolicy::EveryFrame;
			// The fixture normally opts into immutable unchanged-input reuse. Disable it
			// here so the protected boundary must replace the publication this request
			// targeted, even when the test host supplies a paused simulation clock.
			binding.evaluationGenerations.unchangedInputReuseEligible = false;
			const AnimationGuid profileId = binding.retargetProfile->assetId;
			const AnimationGeneration profileGeneration = binding.retargetProfileGeneration;
			NativeAnimationRetargetRequest activate =
				MakeAdvancedRecord<NativeAnimationRetargetRequest>(
					NativeAnimationRecordKind::RetargetRequest);
			activate.animator = {animator.slotIndex, animator.generation};
			activate.operation = NativeAnimationRetargetOperation::ActivateProfile;
			activate.profileAssetId = MakeNativeUuid(profileId);
			activate.profileGeneration = profileGeneration;
			activate.correlationId = 9600U;
			context.Expect(animator.IsValid()
				&& system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok
				&& system.SubmitRetargetRequest(activate).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-retarget-rebuild-activate")
				&& ExecuteIntegrationFrame(system, "animation-retarget-rebuild-baseline"),
				"A retarget rebuild must start from an active profile's immutable baseline pose.");

			const PinnedAnimationAuthorityRow baseline =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			context.Expect(baseline && baseline->snapshot,
				"The baseline publication must expose the generations a producer can target.");
			if(!baseline || !baseline->snapshot) return;

			NativeAnimationRetargetRequest rebuild =
				MakeAdvancedRecord<NativeAnimationRetargetRequest>(
					NativeAnimationRecordKind::RetargetRequest);
			rebuild.animator = {animator.slotIndex, animator.generation};
			rebuild.operation = NativeAnimationRetargetOperation::RequestRebuild;
			rebuild.profileAssetId = MakeNativeUuid(profileId);
			rebuild.profileGeneration = profileGeneration;
			rebuild.expectedPoseGeneration =
				baseline->snapshot->publicationGeneration;
			rebuild.expectedPublicationGeneration =
				baseline->authorityPublicationGeneration;
			rebuild.correlationId = 9601U;
			context.Expect(system.SubmitRetargetRequest(rebuild).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-retarget-rebuild"),
				"A rebuild targeting the publication captured before this frame must queue and execute.");

			const PinnedAnimationAuthorityRow rebuilt =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			std::uint32_t retargetCount = 0U;
			(void)system.CopyRetargetSnapshots(animator, {}, retargetCount);
			std::vector<NativeAnimationRetargetSnapshot> retarget(retargetCount);
			const AnimationStatus retargetCopy = system.CopyRetargetSnapshots(
				animator, retarget, retargetCount);
			const auto successfulRebuild = std::find_if(
				retarget.rbegin(), retarget.rend(), [&rebuild](const auto& value) {
					return value.profileGeneration == rebuild.profileGeneration
						&& value.status == NativeAnimationStatus::Ok;
				});
			context.Expect(retargetCopy == AnimationStatus::Ok
				&& rebuilt && rebuilt->snapshot
				&& rebuilt->snapshot->publicationGeneration
					> baseline->snapshot->publicationGeneration
				&& successfulRebuild != retarget.rend()
				&& successfulRebuild->poseGeneration
					== rebuilt->snapshot->publicationGeneration,
				std::string("Completion must validate the pre-evaluation publication, then report the newly published pose. baseline pose=")
					+ std::to_string(baseline->snapshot->publicationGeneration)
					+ ", baseline authority="
					+ std::to_string(baseline->authorityPublicationGeneration)
					+ ", rebuilt pose="
					+ std::to_string(rebuilt && rebuilt->snapshot
						? rebuilt->snapshot->publicationGeneration : 0U)
					+ ", copy status=" + std::to_string(static_cast<std::uint32_t>(retargetCopy))
					+ ", latest retarget status="
					+ std::to_string(retarget.empty() ? UINT32_MAX
						: static_cast<std::uint32_t>(retarget.back().status)));

			// Reusing the older baseline after a successful rebuild is intentionally stale.
			// This proves the fix preserves optimistic-concurrency rejection rather than
			// merely removing generation validation from the completion boundary.
			NativeAnimationRetargetRequest staleRebuild = rebuild;
			staleRebuild.correlationId = 9602U;
			context.Expect(system.SubmitRetargetRequest(staleRebuild).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-retarget-rebuild-stale"),
				"A later rebuild request must still traverse the protected completion boundary.");

			retargetCount = 0U;
			(void)system.CopyRetargetSnapshots(animator, {}, retargetCount);
			retarget.assign(retargetCount, {});
			const AnimationStatus staleCopy = system.CopyRetargetSnapshots(
				animator, retarget, retargetCount);
			const auto staleResult = std::find_if(
				retarget.rbegin(), retarget.rend(), [&staleRebuild](const auto& value) {
					return value.profileGeneration == staleRebuild.profileGeneration
						&& value.status == NativeAnimationStatus::StaleHandle;
				});
			context.Expect(staleCopy == AnimationStatus::Ok
				&& staleResult != retarget.rend(),
				"A request whose expected generations predate this frame's input must remain stale.");
		});

		registry.Add("Animation.ScaleFoundation.MotionSelectionDrivesNextAuthoritativeFrame", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(97U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeMotionSelectionIntegrationBinding(context);
			const AnimationGuid databaseId = binding.motionDatabase->assetId;
			context.Expect(system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok,
				"The motion-selection fixture must bind two controller clips and one compatible database.");

			NativeAnimationMotionMatchRequest setDatabase =
				MakeAdvancedRecord<NativeAnimationMotionMatchRequest>(
					NativeAnimationRecordKind::MotionMatchRequest);
			setDatabase.animator = {animator.slotIndex, animator.generation};
			setDatabase.operation = NativeAnimationMotionMatchOperation::SetDatabase;
			setDatabase.databaseAssetId = MakeNativeUuid(databaseId);
			setDatabase.databaseGeneration = 12U;
			setDatabase.featureCount = static_cast<std::uint32_t>(
				kMotionDatabaseFeatureCount);
			setDatabase.correlationId = 9701U;
			context.Expect(system.SubmitMotionMatchRequest(setDatabase).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-motion-activate"),
				"Database activation and controller entry initialization must publish atomically.");

			NativeAnimationMotionMatchRequest query = setDatabase;
			query.operation = NativeAnimationMotionMatchOperation::Query;
			query.queryId = 9702U;
			query.correlationId = 9702U;
			context.Expect(system.SubmitMotionMatchRequest(query).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-motion-query")
				&& ExecuteIntegrationFrame(system, "animation-motion-apply"),
				"The CPU-authoritative result must be retained and applied on the next worker frame without a current-frame wait.");

			const PinnedAnimationAuthorityRow published =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			const auto local = published && published->snapshot
				&& published->snapshot->pose
				? published->snapshot->pose->LocalTransforms()
				: std::span<const AnimationTransform>{};
			context.Expect(published && published->status == AnimationStatus::Ok
				&& published->snapshot->playback.currentStateId == 200U
				&& local.size() == 1U && local.front().translation.x > 8.5F,
				"The retained selection must choose the matching clip state, seek its source tick, and feed the ordinary authoritative pose pipeline.");
		});

		registry.Add("Animation.ScaleFoundation.MotionMatchV2PublishesDetailsAndDrivesNextFrame", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(98U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeMotionSelectionIntegrationBinding(context);

			// Give the selected sample nonzero costs and add one sample that exercises
			// every hard-exclusion reason. The exact result proves each V2 field group
			// reached the deterministic scorer instead of being accepted then ignored.
			auto database = std::make_shared<MotionDatabaseAsset>(*binding.motionDatabase);
			MotionDatabaseSample& selected = database->samples.front();
			selected.features.values[0U] = 2;
			selected.tagMask = 1U;
			selected.contactMask = 1U;
			selected.phaseQuantized = 100U;
			selected.transitionFlags = 1U;
			selected.continuityGroup = 7U;
			MotionDatabaseSample excluded = selected;
			excluded.stableSampleId = 7002U;
			excluded.sourceClipId = 5001U;
			excluded.tagMask = 4U;
			excluded.contactMask = 0U;
			excluded.phaseQuantized = 300U;
			excluded.transitionFlags = 4U;
			excluded.exclusionFlags = 8U;
			database->samples.push_back(excluded);
			database->featureSchemaFingerprint =
				ComputeMotionDatabaseSchemaFingerprint(*database);
			const AnimationGuid databaseId = database->assetId;
			const std::uint64_t schemaFingerprint = database->featureSchemaFingerprint;
			binding.motionDatabase = std::move(database);
			context.Expect(system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok,
				"The detailed-query fixture must bind a controller and immutable database together.");

			NativeAnimationMotionMatchQueryRequestV2 query =
				MakeAdvancedRecord<NativeAnimationMotionMatchQueryRequestV2>(
					NativeAnimationRecordKind::MotionMatchQueryRequestV2);
			query.animator = {animator.slotIndex, animator.generation};
			query.databaseAssetId = MakeNativeUuid(databaseId);
			query.databaseGeneration = 12U;
			query.featureCount = static_cast<std::uint32_t>(kMotionDatabaseFeatureCount);
			query.schemaFingerprint = schemaFingerprint;
			query.queryId = 9801U;
			query.correlationId = 9801U;
			query.queryFields = kNativeAnimationMotionMatchAllQueryFields;
			query.features[0U] = 5;
			std::fill(std::begin(query.featureWeights),
				std::end(query.featureWeights), static_cast<std::uint16_t>(1U));
			query.requiredTags = 1U;
			query.excludedTags = 4U;
			query.preferredTags = 8U;
			query.requiredContacts = 1U;
			query.preferredContacts = 3U;
			query.preferredPhase = 105U;
			query.maximumPhaseDistance = 10U;
			query.requiredTransitionFlags = 1U;
			query.excludedTransitionFlags = 4U;
			query.preferredTransitionFlags = 3U;
			query.preferredContinuityGroup = 9U;
			query.lastSampleId = 999U;
			query.lastSourceClipId = 5002U;
			query.excludedRegionFlags = 8U;
			query.phaseWeight = 13U;
			query.continuityPenalty = 17U;
			query.continuityBreakPenalty = 19U;
			query.sameClipPenalty = 29U;
			query.missingPreferredTagPenalty = 7U;
			query.contactMismatchPenalty = 11U;
			query.transitionMismatchPenalty = 23U;
			query.maximumCandidates = 1U;
			query.maximumSamplesToVisit = 2U;
			query.maximumDiagnosticExclusions = 1U;
			query.excludedSampleCount = 1U;
			query.excludedSampleIds[0U] = excluded.stableSampleId;

			const NativeAnimationMotionMatchQueryReceiptV2 receipt =
				system.SubmitMotionMatchQueryV2(query);
			context.Expect(receipt.status == NativeAnimationStatus::Queued
				&& receipt.validatedFields == query.queryFields
				&& receipt.scheduledFields == query.queryFields
				&& (receipt.flags & static_cast<std::uint32_t>(
					NativeAnimationMotionMatchQueryReceiptFlag::PayloadRetained)) != 0U
				&& ExecuteIntegrationFrame(system, "animation-motion-v2-query")
				&& ExecuteIntegrationFrame(system, "animation-motion-v2-apply"),
				"The complete copied V2 query must score on a worker and apply only on the next authoritative frame.");

			std::uint32_t resultCount = 0U;
			context.Expect(system.CopyMotionMatchResultsV2(animator, {}, resultCount)
					== AnimationStatus::BufferTooSmall && resultCount == 1U,
				"The V2 result history must expose the same atomic two-call copy protocol as V1.");
			std::vector<NativeAnimationMotionMatchResultV2> results(resultCount);
			const AnimationStatus copyStatus = system.CopyMotionMatchResultsV2(
				animator, results, resultCount);
			const NativeAnimationMotionMatchResultV2 result = results.empty()
				? NativeAnimationMotionMatchResultV2{} : results.back();
			const std::uint32_t expectedResultFields = (1U << 12U) - 1U;
			const std::uint32_t expectedFlags = static_cast<std::uint32_t>(
				NativeAnimationMotionMatchResultFlag::UsedFallback)
				| static_cast<std::uint32_t>(
					NativeAnimationMotionMatchResultFlag::UsedCpuFallback);
			context.Expect(copyStatus == AnimationStatus::Ok
				&& result.status == NativeAnimationStatus::Ok
				&& result.gpuStatus == NativeAnimationStatus::RendererUnavailable
				&& result.fallbackReason
					== NativeAnimationMotionMatchFallbackReason::GpuUnavailable
				&& result.selectionSource
					== NativeAnimationMotionMatchSelectionSource::CpuFullSearch
				&& result.populatedFields == expectedResultFields
				&& result.flags == expectedFlags,
				"CPU-authoritative V2 publication must report its populated fields, GPU absence, and fallback source truthfully.");
			context.Expect(result.selectedSampleId == 7001U
				&& result.selectedSourceClipId == 5002U
				&& result.selectedSourceTick == 1
				&& result.selectedClipGeneration == 6U
				&& result.featureCost == 3U
				&& result.featureBlockCosts[0U] == 3U
				&& result.phaseCost == 65U
				&& result.tagCost == 7U
				&& result.contactCost == 11U
				&& result.continuityCost == 36U
				&& result.transitionCost == 52U
				&& result.totalCost == 174U,
				"The detailed result must preserve the selected source and every exact integer cost group.");
			context.Expect(result.visitedSamples == 2U
				&& result.evaluatedSamples == 1U
				&& result.excludedSamples == 1U
				&& result.exclusionReasonMask == 0xffU
				&& result.gpuCandidateCount == 0U
				&& result.observationTick > 0
				&& result.publicationGeneration > receipt.publicationGeneration,
				"V2 publication must expose bounded candidate, exclusion, and protected-generation diagnostics.");

			const PinnedAnimationAuthorityRow published =
				PinnedAnimationAuthorityRow::ForAnimator(system, animator);
			const auto local = published && published->snapshot
				&& published->snapshot->pose
				? published->snapshot->pose->LocalTransforms()
				: std::span<const AnimationTransform>{};
			context.Expect(published && published->status == AnimationStatus::Ok
				&& published->snapshot->playback.currentStateId == 200U
				&& local.size() == 1U && local.front().translation.x > 8.5F,
				"The V2 candidate must feed the ordinary next-frame controller and pose publication path.");
		});

		registry.Add("Animation.ScaleFoundation.DeterminismCaptureRestoresProductionPlayback", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle animator = system.CreateAnimatorInstance(
				MakeIntegrationDescriptor(96U));
			IntegrationAnimatorGuard cleanup(system, animator);
			AnimatorPlaybackBinding binding = MakeIntegrationBinding(context);
			context.Expect(system.BindAnimatorPlayback(animator, std::move(binding))
					== AnimationStatus::Ok
				&& ExecuteIntegrationFrame(system, "animation-rollback-baseline"),
				"The rollback fixture must establish a bound, fingerprinted production pose.");

			NativeAnimationDeterminismRequest capture =
				MakeAdvancedRecord<NativeAnimationDeterminismRequest>(
					NativeAnimationRecordKind::DeterminismRequest);
			capture.animator = {animator.slotIndex, animator.generation};
			capture.operation = NativeAnimationDeterminismOperation::Capture;
			capture.correlationId = 9601U;
			context.Expect(system.SubmitDeterminismRequest(capture).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-rollback-capture"),
				"Capture must store the just-published immutable state before returning success.");
			std::uint32_t captureCount = 0U;
			(void)system.CopyDeterminismSnapshots(animator, {}, captureCount);
			std::vector<NativeAnimationDeterminismSnapshot> snapshots(captureCount);
			(void)system.CopyDeterminismSnapshots(animator, snapshots, captureCount);
			const NativeAnimationDeterminismSnapshot captured = snapshots.empty()
				? NativeAnimationDeterminismSnapshot{} : snapshots.back();
			const AnimatorPlaybackStateSnapshot capturedPlayback =
				system.LoadAnimatorPlaybackState(animator);

			AnimationCommand seek;
			seek.target = animator;
			seek.kind = AnimationCommandKind::Seek;
			seek.playbackTick = 1;
			seek.producerId = 96U;
			seek.producerSequence = 1U;
			seek.correlationId = 9602U;
			context.Expect(system.SubmitCommand(seek).status == AnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-rollback-diverge")
				&& system.LoadAnimatorPlaybackState(animator).absoluteTick
					!= capturedPlayback.absoluteTick,
				"A later protected command must move live playback away from the retained state.");

			NativeAnimationDeterminismRequest restore = capture;
			restore.operation = NativeAnimationDeterminismOperation::Restore;
			restore.sourceTick = captured.tick;
			restore.targetTick = captured.tick;
			restore.expectedChecksum = captured.poseChecksum;
			restore.correlationId = 9603U;
			context.Expect(captured.status == NativeAnimationStatus::Ok
				&& captured.poseChecksum != 0U
				&& system.SubmitDeterminismRequest(restore).status
					== NativeAnimationStatus::Queued
				&& ExecuteIntegrationFrame(system, "animation-rollback-restore"),
				"An in-window correction must restore only after exact fingerprint and generation validation.");
			std::uint32_t restoredCount = 0U;
			(void)system.CopyDeterminismSnapshots(animator, {}, restoredCount);
			snapshots.resize(restoredCount);
			const AnimationStatus restoredCopy = system.CopyDeterminismSnapshots(
				animator, snapshots, restoredCount);
			const NativeAnimationDeterminismSnapshot restored = snapshots.empty()
				? NativeAnimationDeterminismSnapshot{} : snapshots.back();
			const AnimatorPlaybackStateSnapshot restoredPlayback =
				system.LoadAnimatorPlaybackState(animator);
			context.Expect(restoredCopy == AnimationStatus::Ok
				&& restored.status == NativeAnimationStatus::Ok
				&& restored.poseChecksum == captured.poseChecksum
				&& restoredPlayback.absoluteTick == capturedPlayback.absoluteTick,
				"The first post-restore publication must reproduce the retained pose checksum and exact fixed-point playback cursor.");
		});
	}
}
