// GpuAnimationShaderLayoutTests.cpp - Frozen GPU animation ABI and plan tests.

#include "TestHarness.h"

#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Animation/Gpu/GpuAnimationPlanner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		template<typename Enum>
		[[nodiscard]] constexpr std::uint32_t Value(Enum value) noexcept
		{
			return static_cast<std::uint32_t>(value);
		}

		template<typename Record>
		void ExpectAbiRecord(
			TestContext& context,
			std::size_t expectedSize,
			std::string_view recordName)
		{
			context.ExpectEqual(expectedSize, sizeof(Record), recordName);
			context.ExpectEqual(std::size_t{16U}, alignof(Record), recordName);
			context.Expect(std::is_standard_layout_v<Record>, recordName);
			context.Expect(std::is_trivially_copyable_v<Record>, recordName);
		}

		template<typename Record>
		[[nodiscard]] bool SameRecordVectors(
			const std::vector<Record>& left,
			const std::vector<Record>& right) noexcept
		{
			static_assert(std::is_trivially_copyable_v<Record>);
			return left.size() == right.size()
				&& (left.empty() || std::memcmp(
					left.data(),
					right.data(),
					left.size() * sizeof(Record)) == 0);
		}

		[[nodiscard]] bool SamePlans(
			const GpuAnimationPlan& left,
			const GpuAnimationPlan& right) noexcept
		{
			return left.generation == right.generation
				&& SameRecordVectors(left.generationRecords, right.generationRecords)
				&& SameRecordVectors(left.workPackets, right.workPackets)
				&& SameRecordVectors(left.dispatchRecords, right.dispatchRecords)
				&& SameRecordVectors(left.dispatchCommands, right.dispatchCommands)
				&& SameRecordVectors(left.batches, right.batches)
				&& SameRecordVectors(left.stageSpans, right.stageSpans)
				&& std::memcmp(
					&left.diagnostics,
					&right.diagnostics,
					sizeof(GpuAnimationPlanDiagnostics)) == 0;
		}

		[[nodiscard]] GpuAnimationWorkRequest MakeRequest(
			std::uint64_t stableInstanceId,
			std::uint64_t programContentId,
			std::uint32_t submissionOrdinal = 0U)
		{
			GpuAnimationWorkRequest request{};
			request.batchKey.programContentId = programContentId;
			request.batchKey.skeletonTopologyId = 0x7000U;
			request.batchKey.stageMask = GpuAnimationStageMask(GpuAnimationStage::Sample)
				| GpuAnimationStageMask(GpuAnimationStage::Blend)
				| GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Bounds)
				| GpuAnimationStageMask(GpuAnimationStage::Palette);
			request.batchKey.outputFlags = Value(GpuAnimationOutputFlag::Pose)
				| Value(GpuAnimationOutputFlag::Bounds)
				| Value(GpuAnimationOutputFlag::Palette);
			request.generations.instanceSlot = static_cast<std::uint32_t>(stableInstanceId);
			request.generations.instanceGeneration = 1U;
			request.generations.componentGeneration = 1U;
			request.generations.workGeneration = 1U;
			request.generations.programGeneration = 1U;
			request.generations.controllerGeneration = 1U;
			request.generations.skeletonGeneration = 1U;
			request.generations.clipGeneration = 1U;
			request.generations.meshGeneration = 1U;
			request.generations.resourceGeneration = 1U;
			request.generations.rendererGeneration = 1U;
			request.generations.sceneGeneration = 1U;
			request.generations.frameIdLow = 7U;
			request.stableInstanceId = stableInstanceId;
			request.stableSubmissionOrdinal = submissionOrdinal;
			request.clipResourceIndex = 2U;
			request.programResourceIndex = 3U;
			request.skeletonResourceIndex = 4U;
			request.jointCount = 4U;
			request.scratchSizeBytes = 20U;
			request.paletteEntryCount = 4U;
			request.palettePartitionCount = 1U;
			request.flags = Value(GpuAnimationWorkFlag::Looping)
				| Value(GpuAnimationWorkFlag::UseReferenceFallback);
			request.diagnosticIndex = static_cast<std::uint32_t>(stableInstanceId);
			return request;
		}

		void ConfigureArenas(
			GpuAnimationArena& scratch,
			GpuAnimationArena& palette,
			std::uint64_t capacity = 1U << 20U)
		{
			static_cast<void>(scratch.Configure(GpuAnimationArenaKind::Scratch, capacity));
			static_cast<void>(palette.Configure(GpuAnimationArenaKind::Palette, capacity));
		}

		[[nodiscard]] std::string ReadAnimationGpuAbi()
		{
			// The HLSL file is the maintained cross-language contract. Reading it in
			// this test catches a C++ change that was not mirrored in shader source.
			const std::filesystem::path path = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Shaders" / "Animation" / "AnimationGpuAbi.hlsli";
			std::ifstream stream(path, std::ios::binary);
			return std::string(
				std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>());
		}

		[[nodiscard]] std::string ReadAnimationShader(std::string_view fileName)
		{
			const std::filesystem::path path = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Shaders" / "Animation" / fileName;
			std::ifstream stream(path, std::ios::binary);
			return std::string(
				std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>());
		}

		void ExpectHlslToken(
			TestContext& context,
			std::string_view hlsl,
			std::string_view token)
		{
			context.Expect(hlsl.find(token) != std::string_view::npos, token);
		}

		template<typename ValueType>
		void WriteFixtureValue(
			std::vector<std::uint8_t>& bytes,
			std::size_t byteOffset,
			const ValueType& value)
		{
			// Atlas fixtures use the same byte-addressed records as the production
			// uploader. Copying values avoids host-alignment assumptions in tests.
			if(byteOffset <= bytes.size()
				&& sizeof(ValueType) <= bytes.size() - byteOffset) {
				std::memcpy(bytes.data() + byteOffset, &value, sizeof(ValueType));
			}
		}

		template<typename ValueType>
		[[nodiscard]] ValueType ReadFixtureValue(
			const std::vector<std::uint8_t>& bytes,
			std::size_t byteOffset)
		{
			ValueType value{};
			if(byteOffset <= bytes.size()
				&& sizeof(ValueType) <= bytes.size() - byteOffset) {
				std::memcpy(&value, bytes.data() + byteOffset, sizeof(ValueType));
			}
			return value;
		}

		[[nodiscard]] GpuAnimationRenderIdentity MakeAtlasRenderIdentity(
			std::uint8_t meshSeed,
			std::uint64_t deformablePayloadGeneration)
		{
			GpuAnimationRenderIdentity identity{};
			identity.meshId[15U] = meshSeed;
			identity.meshHeaderVersion = 9U;
			identity.deformablePayloadGeneration = deformablePayloadGeneration;
			identity.rendererGeneration = 0x100000010ULL;
			identity.sceneGeneration = 0x200000020ULL;
			identity.skeletonCompatibilityFingerprint = 0x300000030ULL;
			return identity;
		}

		[[nodiscard]] std::shared_ptr<const GpuAnimationResidentPayloads>
		MakeAtlasResidentPayload(float constantTranslationX)
		{
			GpuAnimationResidentPayloads payload{};

			// One ATD1 directory selects one valid constant track. All T/R/S lanes
			// may share it because atlas relocation is concerned with addresses,
			// while shader decoding behavior is covered independently below.
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
			// the synthetic request aligned with production deformable bindings.
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

		[[nodiscard]] GpuAnimationWorkRequest MakeAtlasRequest(
			std::uint64_t stableInstanceId,
			std::uint32_t sampleTick,
			const GpuAnimationRenderIdentity& renderIdentity)
		{
			GpuAnimationWorkRequest request = MakeRequest(
				stableInstanceId, 0x4400U);
			request.batchKey.stageMask =
				GpuAnimationStageMask(GpuAnimationStage::Sample)
				| GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Bounds)
				| GpuAnimationStageMask(GpuAnimationStage::Palette);
			request.batchKey.outputFlags = Value(GpuAnimationOutputFlag::Pose)
				| Value(GpuAnimationOutputFlag::Bounds)
				| Value(GpuAnimationOutputFlag::Palette);
			request.clipResourceIndex = 0U;
			request.programResourceIndex = 0U;
			request.skeletonResourceIndex = 0U;
			request.maskResourceIndex = kGpuAnimationInvalidResourceIndex;
			request.morphResourceIndex = 0U;
			request.jointCount = 1U;
			request.morphCount = 0U;
			request.paletteEntryCount = 1U;
			request.palettePartitionCount = 1U;
			request.sampleTick = sampleTick;
			request.renderIdentity = renderIdentity;
			return request;
		}
	}

	void RegisterGpuAnimationShaderLayoutTests(TestRegistry& registry)
	{
		registry.Add("Animation.GpuPlan.SharedResidentPayloadAvoidsPerFrameClone", [](TestContext& context) {
			GpuAnimationPlan plan;
			plan.residentPayloads.programWords = {1u};
			auto sharedPayload = std::make_shared<GpuAnimationResidentPayloads>();
			sharedPayload->programWords = {7u, 9u};
			plan.sharedResidentPayloads = sharedPayload;
			context.Expect(&plan.ResidentPayloads() == sharedPayload.get()
				&& plan.ResidentPayloads().programWords.size() == 2u,
				"A shared cooked payload should remain the exact immutable plan source instead of being cloned.");
			plan.Clear();
			context.Expect(!plan.sharedResidentPayloads
				&& plan.ResidentPayloads().programWords.empty(),
				"Clearing a plan must retire shared payload ownership and its owned fallback together.");
		});

		registry.Add("Animation.GpuShaderLayout.SharedHlslAbiMatchesFrozenConstants", [](TestContext& context) {
			const std::string hlsl = ReadAnimationGpuAbi();
			context.Expect(!hlsl.empty(), "The shared HLSL ABI file must be present in Engine/Shaders/Animation.");
			for(const std::string_view token : std::array<std::string_view, 30U>{
				"#define TE_ANIMATION_GPU_DESCRIPTOR_SET 1u",
				"#define TE_ANIMATION_GPU_DESCRIPTOR_BINDING_COUNT 15u",
				"#define TE_ANIMATION_GPU_STAGE_PROCEDURAL 3u",
				"#define TE_ANIMATION_GPU_STAGE_RAGDOLL 4u",
				"#define TE_ANIMATION_GPU_GENERATION_RECORD_BYTES 64u",
				"#define TE_ANIMATION_GPU_WORK_PACKET_BYTES 144u",
				"#define TE_ANIMATION_GPU_DISPATCH_RECORD_BYTES 64u",
				"#define TE_ANIMATION_GPU_PUSH_CONSTANT_BYTES 32u",
				"#define TE_ANIMATION_GPU_PALETTE_STRIDE_BYTES 48u",
				"#define TE_ANIMATION_GPU_WORK_BIND_POSE_FALLBACK 0x004u",
				"#define TE_ANIMATION_GPU_WORK_HOLD_LAST_FALLBACK 0x008u",
				"#define TE_ANIMATION_GPU_BARRIER_HIERARCHY_TO_PROCEDURAL 0x004u",
				"#define TE_ANIMATION_GPU_BARRIER_HIERARCHY_TO_RAGDOLL 0x008u",
				"#define TE_ANIMATION_GPU_BARRIER_PROCEDURAL_TO_RAGDOLL 0x020u",
				"#define TE_ANIMATION_GPU_BARRIER_RAGDOLL_TO_MORPH 0x080u",
				"#define TE_ANIMATION_CODEC_TRACK_MAGIC 0x31544341u",
				"#define TE_ANIMATION_CODEC_VERSION 1u",
				"#define TE_ANIMATION_CODEC_CONSTANT_TRACK 0x01u",
				"#define TE_ANIMATION_CODEC_ENCODING_SCALAR_UNORM16 0u",
				"#define TE_ANIMATION_CODEC_ENCODING_VECTOR3_UNORM16 1u",
				"#define TE_ANIMATION_CODEC_ENCODING_QUATERNION_SMALLEST_THREE16 2u",
				"#define TE_ANIMATION_CODEC_TRACK_HEADER_BYTES 88u",
				"#define TE_ANIMATION_CODEC_VECTOR_KEY_BYTES 6u",
				"#define TE_ANIMATION_CODEC_QUATERNION_KEY_BYTES 8u",
				"#define TE_ANIMATION_TRACK_DIRECTORY_MAGIC 0x31445441u",
				"#define TE_ANIMATION_TRACK_DIRECTORY_VERSION 1u",
				"#define TE_ANIMATION_TRACK_DIRECTORY_HEADER_BYTES 16u",
				"#define TE_ANIMATION_TRACK_DIRECTORY_ENTRY_BYTES 16u",
				"#define TE_ANIMATION_MAX_TRACK_SEGMENTS 4096u",
				"struct TeAnimationGpuWorkPacket"}) {
				ExpectHlslToken(context, hlsl, token);
			}
			// Inspect only the work-packet declaration so the dispatch record's
			// similarly named reserved lanes cannot satisfy this stride guard.
			const std::size_t workPacketBegin = hlsl.find(
				"struct TeAnimationGpuWorkPacket");
			const std::size_t dispatchRecordBegin = hlsl.find(
				"struct TeAnimationGpuDispatchRecord", workPacketBegin);
			const std::string_view workPacketBlock =
				workPacketBegin != std::string::npos
					&& dispatchRecordBegin != std::string::npos
					&& dispatchRecordBegin > workPacketBegin
				? std::string_view(hlsl).substr(
					workPacketBegin, dispatchRecordBegin - workPacketBegin)
				: std::string_view{};
			ExpectHlslToken(context, workPacketBlock, "uint reserved0;");
			ExpectHlslToken(context, workPacketBlock, "uint reserved1;");
		});

		registry.Add("Animation.GpuShaderLayout.ProceduralStageIsBoundedVisualOnlyAndGenerationSafe", [](TestContext& context) {
			const std::string hlsl = ReadAnimationShader("AnimationProcedural.comp.hlsl");
			context.Expect(!hlsl.empty(),
				"The installed GPU animation pipeline must include its procedural compute source.");
			for(const std::string_view token : std::array<std::string_view, 14U>{
				"#define TE_ANIMATION_PROCEDURAL_RECORD_BYTES 96u",
				"#define TE_ANIMATION_PROCEDURAL_MAX_RECORDS 64u",
				"#define TE_ANIMATION_PROCEDURAL_RETARGET_MODEL 4u",
				"TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_PROCEDURAL",
				"TeValidatePacket(dispatch, packet, joint, TE_ANIMATION_GPU_STAGE_PROCEDURAL)",
				"TE_ANIMATION_GPU_WORK_HYBRID_CPU_AUTHORITY",
				"TE_ANIMATION_GPU_WORK_DETERMINISTIC_REFERENCE",
				"TE_ANIMATION_GPU_WORK_ALLOW_VISUAL_PROCEDURAL",
				"programGeneration != generation.programGeneration",
				"skeletonGeneration != generation.skeletonGeneration",
				"componentGeneration != generation.componentGeneration",
				"TeProceduralRecordsOverlap",
				"TeSolveVisualRetarget",
				"TeValidateProceduralTable"}) {
				ExpectHlslToken(context, hlsl, token);
			}
			context.Expect(hlsl.find("gAnimationPalette.Store") == std::string::npos
				&& hlsl.find("gAnimationLocalPoses.Store") == std::string::npos,
				"Visual procedural work must remain GPU-resident and must not write a CPU/readback-facing path.");
		});

		registry.Add("Animation.GpuShaderLayout.RagdollStageUsesCopiedBoundedSharedAbi", [](TestContext& context) {
			const std::string hlsl = ReadAnimationShader("AnimationRagdoll.comp.hlsl");
			context.Expect(!hlsl.empty(),
				"The installed GPU animation pipeline must include its ragdoll compute source.");
			for(const std::string_view token : std::array<std::string_view, 14U>{
				"#include \"AnimationCodec.hlsli\"",
				"#define TE_ANIMATION_RAGDOLL_MAGIC 0x31474152u",
				"#define TE_ANIMATION_RAGDOLL_RECORD_BYTES 96u",
				"#define TE_ANIMATION_RAGDOLL_MAX_RECORDS 256u",
				"TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_RAGDOLL",
				"TeValidatePacket(dispatch, packet, joint, TE_ANIMATION_GPU_STAGE_RAGDOLL)",
				"TE_ANIMATION_GPU_WORK_ALLOW_RAGDOLL_VISUAL",
				"generation.reserved",
				"physicsGeneration != generation.resourceGeneration",
				"skeletonGeneration != generation.skeletonGeneration",
				"componentGeneration != generation.componentGeneration",
				"TeValidateRagdollTable",
				"candidateJoint > joint",
				"TeStoreModel"}) {
				ExpectHlslToken(context, hlsl, token);
			}
			context.Expect(hlsl.find("StructuredBuffer<Transform>") == std::string::npos
				&& hlsl.find("RWStructuredBuffer<Transform>") == std::string::npos,
				"Ragdoll work must use the frozen shared descriptor ABI instead of private bindings.");
		});

		registry.Add("Animation.GpuShaderLayout.DescriptorStageAndFlagValuesAreFrozen", [](TestContext& context) {
			context.ExpectEqual(0U, kGpuAnimationGlobalBindlessDescriptorSet,
				"Global bindless resources must remain at descriptor set zero.");
			context.ExpectEqual(1U, kGpuAnimationDescriptorSet,
				"Animation resources must remain at descriptor set one.");
			context.ExpectEqual(15U, kGpuAnimationDescriptorBindingCount,
				"The version-one descriptor set must expose all fifteen frozen bindings.");

			const std::array<std::uint32_t, 15U> descriptorValues{
				Value(GpuAnimationDescriptorBinding::GenerationRecords),
				Value(GpuAnimationDescriptorBinding::WorkPackets),
				Value(GpuAnimationDescriptorBinding::DispatchRecords),
				Value(GpuAnimationDescriptorBinding::ProgramWords),
				Value(GpuAnimationDescriptorBinding::PackedClipBytes),
				Value(GpuAnimationDescriptorBinding::SkeletonTables),
				Value(GpuAnimationDescriptorBinding::MaskTables),
				Value(GpuAnimationDescriptorBinding::MorphInputs),
				Value(GpuAnimationDescriptorBinding::Scratch),
				Value(GpuAnimationDescriptorBinding::LocalPoses),
				Value(GpuAnimationDescriptorBinding::ModelPoses),
				Value(GpuAnimationDescriptorBinding::PaletteMatrices),
				Value(GpuAnimationDescriptorBinding::MorphOutputs),
				Value(GpuAnimationDescriptorBinding::BoundsOutputs),
				Value(GpuAnimationDescriptorBinding::Diagnostics)};
			for(std::uint32_t index = 0U; index < descriptorValues.size(); ++index) {
				context.ExpectEqual(index, descriptorValues[index],
					"Animation descriptor bindings must remain contiguous and append-only.");
			}

			const std::array<std::uint32_t, 8U> stageValues{
				Value(GpuAnimationStage::Sample),
				Value(GpuAnimationStage::Blend),
				Value(GpuAnimationStage::Hierarchy),
				Value(GpuAnimationStage::Procedural),
				Value(GpuAnimationStage::Ragdoll),
				Value(GpuAnimationStage::Morph),
				Value(GpuAnimationStage::Bounds),
				Value(GpuAnimationStage::Palette)};
			for(std::uint32_t index = 0U; index < stageValues.size(); ++index) {
				context.ExpectEqual(index, stageValues[index],
					"GPU animation stage numbers must mirror HLSL dispatch constants.");
				context.Expect(IsGpuAnimationStageValueValid(index),
					"Every frozen stage number must pass the shared range check.");
			}
			context.Expect(!IsGpuAnimationStageValueValid(kGpuAnimationStageCount),
				"The first value beyond Palette must remain invalid in version one.");

			const std::array<std::uint32_t, 10U> workFlags{
				Value(GpuAnimationWorkFlag::Looping),
				Value(GpuAnimationWorkFlag::Reverse),
				Value(GpuAnimationWorkFlag::UseReferenceFallback),
				Value(GpuAnimationWorkFlag::UseHoldLastFallback),
				Value(GpuAnimationWorkFlag::HasMorph),
				Value(GpuAnimationWorkFlag::HasRootMotion),
				Value(GpuAnimationWorkFlag::HybridCpuAuthority),
				Value(GpuAnimationWorkFlag::DeterministicReference),
				Value(GpuAnimationWorkFlag::AllowVisualProcedural),
				Value(GpuAnimationWorkFlag::AllowRagdollVisual)};
			for(std::uint32_t index = 0U; index < workFlags.size(); ++index) {
				context.ExpectEqual(1U << index, workFlags[index],
					"Each work-policy bit must retain its exact version-one value.");
			}
			context.ExpectEqual(0x3ffU, kGpuAnimationAllWorkFlags,
				"The work flag validation mask must include exactly the frozen bits.");
			context.ExpectEqual(0x01U, Value(GpuAnimationOutputFlag::Pose), "Pose output bit.");
			context.ExpectEqual(0x02U, Value(GpuAnimationOutputFlag::MorphWeights), "Morph output bit.");
			context.ExpectEqual(0x04U, Value(GpuAnimationOutputFlag::Bounds), "Bounds output bit.");
			context.ExpectEqual(0x08U, Value(GpuAnimationOutputFlag::Palette), "Palette output bit.");
			context.ExpectEqual(0x0fU, kGpuAnimationAllOutputFlags,
				"The output validation mask must include exactly the frozen bits.");
			context.ExpectEqual(0x01U, Value(GpuAnimationDispatchFlag::WritesDiagnostics),
				"Dispatch diagnostic bit.");
			context.ExpectEqual(0x02U, Value(GpuAnimationDispatchFlag::UsesReferenceFallback),
				"Dispatch reference-fallback bit.");
			context.ExpectEqual(0x04U, Value(GpuAnimationDispatchFlag::UsesHoldLastFallback),
				"Dispatch hold-last bit.");
			context.ExpectEqual(0x07U, kGpuAnimationAllDispatchFlags,
				"The dispatch validation mask must include exactly the frozen bits.");

			const std::array<std::uint32_t, 11U> diagnosticFlags{
				Value(GpuAnimationDiagnosticFlag::InvalidRecord),
				Value(GpuAnimationDiagnosticFlag::StaleGeneration),
				Value(GpuAnimationDiagnosticFlag::RangeOverflow),
				Value(GpuAnimationDiagnosticFlag::MissingResource),
				Value(GpuAnimationDiagnosticFlag::NonFiniteOutput),
				Value(GpuAnimationDiagnosticFlag::ScratchOverflow),
				Value(GpuAnimationDiagnosticFlag::PaletteOverflow),
				Value(GpuAnimationDiagnosticFlag::OutputOverflow),
				Value(GpuAnimationDiagnosticFlag::BindPoseFallback),
				Value(GpuAnimationDiagnosticFlag::HoldLastFallback),
				Value(GpuAnimationDiagnosticFlag::RendererLost)};
			for(std::uint32_t index = 0U; index < diagnosticFlags.size(); ++index) {
				context.ExpectEqual(1U << index, diagnosticFlags[index],
					"Each diagnostic bit must remain suitable for shader atomic OR.");
			}
			context.ExpectEqual(0x7ffU, kGpuAnimationAllDiagnosticFlags,
				"The diagnostic validation mask must include exactly the frozen bits.");

			const std::array<std::uint32_t, 11U> barrierFlags{
				Value(GpuAnimationBarrierFlag::CodecToBlend),
				Value(GpuAnimationBarrierFlag::BlendToHierarchy),
				Value(GpuAnimationBarrierFlag::HierarchyToProcedural),
				Value(GpuAnimationBarrierFlag::HierarchyToRagdoll),
				Value(GpuAnimationBarrierFlag::HierarchyToMorph),
				Value(GpuAnimationBarrierFlag::ProceduralToRagdoll),
				Value(GpuAnimationBarrierFlag::ProceduralToMorph),
				Value(GpuAnimationBarrierFlag::RagdollToMorph),
				Value(GpuAnimationBarrierFlag::MorphToBounds),
				Value(GpuAnimationBarrierFlag::BoundsToPalette),
				Value(GpuAnimationBarrierFlag::PaletteToRender)};
			for(std::uint32_t index = 0U; index < barrierFlags.size(); ++index) {
				context.ExpectEqual(1U << index, barrierFlags[index],
					"Renderer-neutral barrier intent must retain its exact bit value.");
			}
			context.ExpectEqual(0x7ffU, kGpuAnimationAllBarrierFlags,
				"The barrier validation mask must cover all producer boundaries.");
		});

		registry.Add("Animation.GpuShaderLayout.CodecVersionOnePackingIsExact", [](TestContext& context) {
			context.ExpectEqual(0x31544341U, kAnimationCodecTrackMagic,
				"The packed clip magic must match the HLSL ByteAddressBuffer contract.");
			context.ExpectEqual(std::uint16_t{1U}, kAnimationCodecVersion, "Codec version one.");
			context.ExpectEqual(std::uint8_t{0x01U}, kAnimationCodecConstantTrack, "Constant-track flag.");
			context.ExpectEqual(16U, kAnimationCodecScalarBits, "Scalar codec bit count.");
			context.ExpectEqual(48U, kAnimationCodecVectorBits, "Vector codec bit count.");
			context.ExpectEqual(50U, kAnimationCodecQuaternionBits, "Quaternion codec bit count.");
			context.ExpectEqual(std::uint8_t{0U}, static_cast<std::uint8_t>(AnimationCodecValueEncoding::ScalarUnorm16),
				"Scalar encoding value.");
			context.ExpectEqual(std::uint8_t{1U}, static_cast<std::uint8_t>(AnimationCodecValueEncoding::Vector3Unorm16),
				"Vector encoding value.");
			context.ExpectEqual(std::uint8_t{2U}, static_cast<std::uint8_t>(AnimationCodecValueEncoding::QuaternionSmallestThree16),
				"Quaternion encoding value.");
			context.ExpectEqual(std::size_t{88U}, sizeof(AnimationCodecTrackHeaderV1),
				"The CPU/GPU codec track header must remain exactly 88 bytes.");
			context.ExpectEqual(std::size_t{1U}, alignof(AnimationCodecTrackHeaderV1),
				"Packed codec records must not gain platform alignment padding.");
			context.ExpectEqual(std::size_t{0U}, offsetof(AnimationCodecTrackHeaderV1, magic), "codec.magic");
			context.ExpectEqual(std::size_t{4U}, offsetof(AnimationCodecTrackHeaderV1, version), "codec.version");
			context.ExpectEqual(std::size_t{6U}, offsetof(AnimationCodecTrackHeaderV1, encoding), "codec.encoding");
			context.ExpectEqual(std::size_t{7U}, offsetof(AnimationCodecTrackHeaderV1, flags), "codec.flags");
			context.ExpectEqual(std::size_t{8U}, offsetof(AnimationCodecTrackHeaderV1, byteSize), "codec.byteSize");
			context.ExpectEqual(std::size_t{12U}, offsetof(AnimationCodecTrackHeaderV1, keyCount), "codec.keyCount");
			context.ExpectEqual(std::size_t{16U}, offsetof(AnimationCodecTrackHeaderV1, segmentStartTick), "codec.start");
			context.ExpectEqual(std::size_t{24U}, offsetof(AnimationCodecTrackHeaderV1, segmentEndTick), "codec.end");
			context.ExpectEqual(std::size_t{32U}, offsetof(AnimationCodecTrackHeaderV1, rangeMinimum), "codec.minimum");
			context.ExpectEqual(std::size_t{44U}, offsetof(AnimationCodecTrackHeaderV1, rangeExtent), "codec.extent");
			context.ExpectEqual(std::size_t{56U}, offsetof(AnimationCodecTrackHeaderV1, constantVector), "codec.vector");
			context.ExpectEqual(std::size_t{68U}, offsetof(AnimationCodecTrackHeaderV1, constantRotation), "codec.rotation");
			context.ExpectEqual(std::size_t{84U}, offsetof(AnimationCodecTrackHeaderV1, constantScalar), "codec.scalar");

			context.ExpectEqual(std::size_t{6U}, sizeof(AnimationCodecVectorKeyV1),
				"A quantized vector key must remain three uint16 lanes.");
			context.ExpectEqual(std::size_t{0U}, offsetof(AnimationCodecVectorKeyV1, x), "vector.x");
			context.ExpectEqual(std::size_t{2U}, offsetof(AnimationCodecVectorKeyV1, y), "vector.y");
			context.ExpectEqual(std::size_t{4U}, offsetof(AnimationCodecVectorKeyV1, z), "vector.z");
			context.ExpectEqual(std::size_t{8U}, sizeof(AnimationCodecQuaternionKeyV1),
				"A smallest-three quaternion key must remain eight bytes.");
			context.ExpectEqual(std::size_t{0U}, offsetof(AnimationCodecQuaternionKeyV1, component0), "quaternion.c0");
			context.ExpectEqual(std::size_t{2U}, offsetof(AnimationCodecQuaternionKeyV1, component1), "quaternion.c1");
			context.ExpectEqual(std::size_t{4U}, offsetof(AnimationCodecQuaternionKeyV1, component2), "quaternion.c2");
			context.ExpectEqual(std::size_t{6U}, offsetof(AnimationCodecQuaternionKeyV1, largestComponent), "quaternion.largest");
			context.ExpectEqual(std::size_t{7U}, offsetof(AnimationCodecQuaternionKeyV1, reserved), "quaternion.reserved");
			context.ExpectEqual(0x31445441U, kGpuAnimationTrackDirectoryMagic,
				"Segment directories must retain the shared ATD1 magic.");
			context.ExpectEqual(1U, kGpuAnimationTrackDirectoryVersion,
				"Segment directories must retain version one.");
			ExpectAbiRecord<GpuAnimationTrackDirectoryHeader>(
				context, 16U, "track directory header");
			context.ExpectEqual(std::size_t{0U},
				offsetof(GpuAnimationTrackDirectoryHeader, magic),
				"trackDirectory.magic");
			context.ExpectEqual(std::size_t{4U},
				offsetof(GpuAnimationTrackDirectoryHeader, versionAndStride),
				"trackDirectory.versionAndStride");
			context.ExpectEqual(std::size_t{8U},
				offsetof(GpuAnimationTrackDirectoryHeader, entryCount),
				"trackDirectory.entryCount");
			context.ExpectEqual(std::size_t{12U},
				offsetof(GpuAnimationTrackDirectoryHeader, reserved),
				"trackDirectory.reserved");
			ExpectAbiRecord<GpuAnimationTrackDirectoryEntry>(
				context, 16U, "track directory entry");
			context.ExpectEqual(std::size_t{0U},
				offsetof(GpuAnimationTrackDirectoryEntry, startTick),
				"trackDirectoryEntry.startTick");
			context.ExpectEqual(std::size_t{4U},
				offsetof(GpuAnimationTrackDirectoryEntry, endTick),
				"trackDirectoryEntry.endTick");
			context.ExpectEqual(std::size_t{8U},
				offsetof(GpuAnimationTrackDirectoryEntry, trackHeaderOffsetBytes),
				"trackDirectoryEntry.trackHeaderOffset");
			context.ExpectEqual(std::size_t{12U},
				offsetof(GpuAnimationTrackDirectoryEntry, reserved),
				"trackDirectoryEntry.reserved");
		});

		registry.Add("Animation.GpuShaderLayout.RuntimeSamplingHierarchyAndPaletteUseCompleteData", [](TestContext& context) {
			const std::string codec = ReadAnimationShader("AnimationCodec.hlsli");
			const std::string sample = ReadAnimationShader("AnimationSample.comp.hlsl");
			const std::string hierarchy = ReadAnimationShader("AnimationHierarchy.comp.hlsl");
			const std::string palette = ReadAnimationShader("AnimationPalette.comp.hlsl");
			context.Expect(!codec.empty() && !sample.empty()
				&& !hierarchy.empty() && !palette.empty(),
				"Every production GPU animation shader source must be available to contract tests.");

			// Both quaternion endpoints must be decoded, then hemisphere-corrected
			// before normalized interpolation just like the authoritative CPU codec.
			for(const std::string_view token : std::array<std::string_view, 4U>{
				"TeDecodeSmallestThreeQuaternionKey(",
				"valueOffset + right * valueStride, rightRotation",
				"if (dot(leftRotation, rightRotation) < 0.0f) rightRotation = -rightRotation;",
				"lerp(leftRotation, rightRotation, factor)"}) {
				ExpectHlslToken(context, codec, token);
			}

			// Sampling consumes the producer-resolved tick; hierarchy consumes one
			// explicit depth per joint instead of assuming contiguous level ranges.
			ExpectHlslToken(context, sample, "const uint tick = packet.sampleTick;");
			ExpectHlslToken(context, sample, "TeDecodeTrack(gAnimationProgram.Load(recordOffset), tick");
			ExpectHlslToken(context, hierarchy,
				"gAnimationSkeleton.Load(levelTable + joint * 4u)");
			ExpectHlslToken(context, hierarchy,
				"parentLevel + 1u != jointLevel");

			// Palette compaction follows the directory's palette lane, maps each
			// sparse entry to its source joint, and applies the cooked inverse bind.
			ExpectHlslToken(context, palette,
				"gAnimationSkeleton.Load(directory + 12u)");
			ExpectHlslToken(context, palette,
				"sourceJoint = gAnimationSkeleton.Load(paletteRecord)");
			ExpectHlslToken(context, palette,
				"inverseBind.row0 = asfloat");
			ExpectHlslToken(context, palette,
				"matrix = TeMultiplyAffine(matrix, inverseBind)");
		});

		registry.Add("Animation.GpuShaderLayout.AllFixedRecordsHaveFrozenSizesAndOffsets", [](TestContext& context) {
			ExpectAbiRecord<GpuAnimationGenerationRecord>(context, 64U, "generation record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationGenerationRecord, instanceSlot), "generation.instanceSlot");
			context.ExpectEqual(std::size_t{4U}, offsetof(GpuAnimationGenerationRecord, instanceGeneration), "generation.instance");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationGenerationRecord, componentGeneration), "generation.component");
			context.ExpectEqual(std::size_t{12U}, offsetof(GpuAnimationGenerationRecord, workGeneration), "generation.work");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationGenerationRecord, programGeneration), "generation.program");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationGenerationRecord, controllerGeneration), "generation.controller");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationGenerationRecord, skeletonGeneration), "generation.skeleton");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationGenerationRecord, clipGeneration), "generation.clip");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationGenerationRecord, meshGeneration), "generation.mesh");
			context.ExpectEqual(std::size_t{36U}, offsetof(GpuAnimationGenerationRecord, resourceGeneration), "generation.resource");
			context.ExpectEqual(std::size_t{40U}, offsetof(GpuAnimationGenerationRecord, rendererGeneration), "generation.renderer");
			context.ExpectEqual(std::size_t{44U}, offsetof(GpuAnimationGenerationRecord, sceneGeneration), "generation.scene");
			context.ExpectEqual(std::size_t{48U}, offsetof(GpuAnimationGenerationRecord, frameIdLow), "generation.frameLow");
			context.ExpectEqual(std::size_t{52U}, offsetof(GpuAnimationGenerationRecord, frameIdHigh), "generation.frameHigh");
			context.ExpectEqual(std::size_t{56U}, offsetof(GpuAnimationGenerationRecord, flags), "generation.flags");
			context.ExpectEqual(std::size_t{60U}, offsetof(GpuAnimationGenerationRecord, reserved), "generation.reserved");

			ExpectAbiRecord<GpuAnimationRagdollTableHeader>(context, 32U, "ragdoll table header");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationRagdollTableHeader, magic), "ragdollHeader.magic");
			context.ExpectEqual(std::size_t{4U}, offsetof(GpuAnimationRagdollTableHeader, versionAndStride), "ragdollHeader.version");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationRagdollTableHeader, recordCount), "ragdollHeader.count");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationRagdollTableHeader, physicsGeneration), "ragdollHeader.physics");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationRagdollTableHeader, skeletonGeneration), "ragdollHeader.skeleton");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationRagdollTableHeader, componentGeneration), "ragdollHeader.component");
			ExpectAbiRecord<GpuAnimationRagdollRecord>(context, 96U, "ragdoll record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationRagdollRecord, jointIndex), "ragdollRecord.joint");
			context.ExpectEqual(std::size_t{4U}, offsetof(GpuAnimationRagdollRecord, bodyGeneration), "ragdollRecord.body");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationRagdollRecord, profileWeight), "ragdollRecord.profileWeight");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationRagdollRecord, activationWeight), "ragdollRecord.activationWeight");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationRagdollRecord, bodyWeight), "ragdollRecord.bodyWeight");
			context.ExpectEqual(std::size_t{40U}, offsetof(GpuAnimationRagdollRecord, row0), "ragdollRecord.row0");
			context.ExpectEqual(std::size_t{56U}, offsetof(GpuAnimationRagdollRecord, row1), "ragdollRecord.row1");
			context.ExpectEqual(std::size_t{72U}, offsetof(GpuAnimationRagdollRecord, row2), "ragdollRecord.row2");

			ExpectAbiRecord<GpuAnimationWorkPacket>(context, 144U, "work packet");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationWorkPacket, stableInstanceId), "work.instance");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationWorkPacket, stableProgramId), "work.program");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationWorkPacket, inputPoseOffsetBytes), "work.inputPose");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationWorkPacket, scratchOffsetBytes), "work.scratch");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationWorkPacket, localPoseOffsetBytes), "work.local");
			context.ExpectEqual(std::size_t{40U}, offsetof(GpuAnimationWorkPacket, modelPoseOffsetBytes), "work.model");
			context.ExpectEqual(std::size_t{48U}, offsetof(GpuAnimationWorkPacket, paletteOffsetBytes), "work.palette");
			context.ExpectEqual(std::size_t{56U}, offsetof(GpuAnimationWorkPacket, morphOutputOffsetBytes), "work.morphOutput");
			context.ExpectEqual(std::size_t{64U}, offsetof(GpuAnimationWorkPacket, generationRecordIndex), "work.generationIndex");
			context.ExpectEqual(std::size_t{68U}, offsetof(GpuAnimationWorkPacket, clipResourceIndex), "work.clipIndex");
			context.ExpectEqual(std::size_t{72U}, offsetof(GpuAnimationWorkPacket, programResourceIndex), "work.programIndex");
			context.ExpectEqual(std::size_t{76U}, offsetof(GpuAnimationWorkPacket, skeletonResourceIndex), "work.skeletonIndex");
			context.ExpectEqual(std::size_t{80U}, offsetof(GpuAnimationWorkPacket, maskResourceIndex), "work.maskIndex");
			context.ExpectEqual(std::size_t{84U}, offsetof(GpuAnimationWorkPacket, morphResourceIndex), "work.morphIndex");
			context.ExpectEqual(std::size_t{88U}, offsetof(GpuAnimationWorkPacket, jointCount), "work.joints");
			context.ExpectEqual(std::size_t{92U}, offsetof(GpuAnimationWorkPacket, morphCount), "work.morphs");
			context.ExpectEqual(std::size_t{96U}, offsetof(GpuAnimationWorkPacket, paletteEntryCount), "work.paletteEntries");
			context.ExpectEqual(std::size_t{100U}, offsetof(GpuAnimationWorkPacket, palettePartitionCount), "work.palettePartitions");
			context.ExpectEqual(std::size_t{104U}, offsetof(GpuAnimationWorkPacket, scratchSizeBytes), "work.scratchSize");
			context.ExpectEqual(std::size_t{108U}, offsetof(GpuAnimationWorkPacket, stageMask), "work.stageMask");
			context.ExpectEqual(std::size_t{112U}, offsetof(GpuAnimationWorkPacket, flags), "work.flags");
			context.ExpectEqual(std::size_t{116U}, offsetof(GpuAnimationWorkPacket, diagnosticIndex), "work.diagnostic");
			context.ExpectEqual(std::size_t{120U}, offsetof(GpuAnimationWorkPacket, resourceLayoutId), "work.layout");
			context.ExpectEqual(std::size_t{124U}, offsetof(GpuAnimationWorkPacket, boundsOutputIndex), "work.bounds");
			context.ExpectEqual(std::size_t{128U}, offsetof(GpuAnimationWorkPacket, stableSubmissionOrdinal), "work.ordinal");
			context.ExpectEqual(std::size_t{132U}, offsetof(GpuAnimationWorkPacket, sampleTick), "work.sampleTick");
			context.ExpectEqual(std::size_t{136U}, offsetof(GpuAnimationWorkPacket, reserved0), "work.reserved0");
			context.ExpectEqual(std::size_t{140U}, offsetof(GpuAnimationWorkPacket, reserved1), "work.reserved1");

			ExpectAbiRecord<GpuAnimationDispatchRecord>(context, 64U, "dispatch record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationDispatchRecord, stableInstanceId), "dispatch.instance");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationDispatchRecord, elementOffset), "dispatch.offset");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationDispatchRecord, workPacketIndex), "dispatch.work");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationDispatchRecord, generationRecordIndex), "dispatch.generation");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationDispatchRecord, stage), "dispatch.stage");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationDispatchRecord, elementCount), "dispatch.count");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationDispatchRecord, flags), "dispatch.flags");
			context.ExpectEqual(std::size_t{36U}, offsetof(GpuAnimationDispatchRecord, barrierFlags), "dispatch.barrier");
			context.ExpectEqual(std::size_t{40U}, offsetof(GpuAnimationDispatchRecord, diagnosticIndex), "dispatch.diagnostic");
			context.ExpectEqual(std::size_t{44U}, offsetof(GpuAnimationDispatchRecord, resourceLayoutId), "dispatch.layout");
			context.ExpectEqual(std::size_t{48U}, offsetof(GpuAnimationDispatchRecord, batchIndex), "dispatch.batch");
			context.ExpectEqual(std::size_t{52U}, offsetof(GpuAnimationDispatchRecord, stableSubmissionOrdinal), "dispatch.ordinal");
			context.ExpectEqual(std::size_t{56U}, offsetof(GpuAnimationDispatchRecord, reserved0), "dispatch.reserved0");
			context.ExpectEqual(std::size_t{60U}, offsetof(GpuAnimationDispatchRecord, reserved1), "dispatch.reserved1");

			ExpectAbiRecord<GpuAnimationPushConstants>(context, 32U, "push constants");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationPushConstants, stage), "push.stage");
			context.ExpectEqual(std::size_t{4U}, offsetof(GpuAnimationPushConstants, firstDispatchRecord), "push.firstDispatch");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationPushConstants, dispatchRecordCount), "push.dispatchCount");
			context.ExpectEqual(std::size_t{12U}, offsetof(GpuAnimationPushConstants, firstElementLow), "push.firstLow");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationPushConstants, firstElementHigh), "push.firstHigh");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationPushConstants, elementCount), "push.elementCount");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationPushConstants, planGeneration), "push.generation");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationPushConstants, flags), "push.flags");

			ExpectAbiRecord<GpuAnimationPoseRecord>(context, 48U, "pose record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationPoseRecord, translation), "pose.translation");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationPoseRecord, rotation), "pose.rotation");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationPoseRecord, scale), "pose.scale");
			ExpectAbiRecord<GpuAnimationAffineRecord>(context, 48U, "affine record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationAffineRecord, row0), "affine.row0");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationAffineRecord, row1), "affine.row1");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationAffineRecord, row2), "affine.row2");
			ExpectAbiRecord<GpuAnimationPaletteRecord>(context, 48U, "palette record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationPaletteRecord, row0), "palette.row0");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationPaletteRecord, row1), "palette.row1");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationPaletteRecord, row2), "palette.row2");
			ExpectAbiRecord<GpuAnimationBoundsRecord>(context, 32U, "bounds record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationBoundsRecord, minimum), "bounds.minimum");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationBoundsRecord, maximum), "bounds.maximum");
			ExpectAbiRecord<GpuAnimationDiagnosticRecord>(context, 32U, "diagnostic record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationDiagnosticRecord, flags), "diagnostic.flags");
			context.ExpectEqual(std::size_t{4U}, offsetof(GpuAnimationDiagnosticRecord, stableInstanceIdLow), "diagnostic.instanceLow");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationDiagnosticRecord, stableInstanceIdHigh), "diagnostic.instanceHigh");
			context.ExpectEqual(std::size_t{12U}, offsetof(GpuAnimationDiagnosticRecord, stage), "diagnostic.stage");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationDiagnosticRecord, workPacketIndex), "diagnostic.work");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationDiagnosticRecord, generationRecordIndex), "diagnostic.generation");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationDiagnosticRecord, detail0), "diagnostic.detail0");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationDiagnosticRecord, detail1), "diagnostic.detail1");

			ExpectAbiRecord<GpuAnimationBatchKey>(context, 32U, "batch key");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationBatchKey, programContentId), "batchKey.program");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationBatchKey, skeletonTopologyId), "batchKey.skeleton");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationBatchKey, resourceLayoutId), "batchKey.layout");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationBatchKey, stageMask), "batchKey.stages");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationBatchKey, outputFlags), "batchKey.outputs");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationBatchKey, codecVersion), "batchKey.codec");
			ExpectAbiRecord<GpuAnimationBatchGenerationKey>(context, 32U, "batch generation key");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationBatchGenerationKey, programGeneration), "batchGeneration.program");
			context.ExpectEqual(std::size_t{4U}, offsetof(GpuAnimationBatchGenerationKey, controllerGeneration), "batchGeneration.controller");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationBatchGenerationKey, skeletonGeneration), "batchGeneration.skeleton");
			context.ExpectEqual(std::size_t{12U}, offsetof(GpuAnimationBatchGenerationKey, clipGeneration), "batchGeneration.clip");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationBatchGenerationKey, meshGeneration), "batchGeneration.mesh");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationBatchGenerationKey, resourceGeneration), "batchGeneration.resource");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationBatchGenerationKey, rendererGeneration), "batchGeneration.renderer");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationBatchGenerationKey, sceneGeneration), "batchGeneration.scene");

			ExpectAbiRecord<GpuAnimationDispatchCommand>(context, 48U, "dispatch command");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationDispatchCommand, pushConstants), "command.push");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationDispatchCommand, groupCountX), "command.groupsX");
			context.ExpectEqual(std::size_t{36U}, offsetof(GpuAnimationDispatchCommand, groupCountY), "command.groupsY");
			context.ExpectEqual(std::size_t{40U}, offsetof(GpuAnimationDispatchCommand, groupCountZ), "command.groupsZ");
			context.ExpectEqual(std::size_t{44U}, offsetof(GpuAnimationDispatchCommand, batchIndex), "command.batch");
			ExpectAbiRecord<GpuAnimationBatch>(context, 80U, "batch record");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationBatch, key), "batch.key");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationBatch, generationKey), "batch.generations");
			context.ExpectEqual(std::size_t{64U}, offsetof(GpuAnimationBatch, firstWorkPacket), "batch.firstWork");
			context.ExpectEqual(std::size_t{68U}, offsetof(GpuAnimationBatch, workPacketCount), "batch.workCount");
			context.ExpectEqual(std::size_t{72U}, offsetof(GpuAnimationBatch, stableBatchOrdinal), "batch.ordinal");
			context.ExpectEqual(std::size_t{76U}, offsetof(GpuAnimationBatch, reserved), "batch.reserved");
			ExpectAbiRecord<GpuAnimationStageSpan>(context, 32U, "stage span");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationStageSpan, totalElementCount), "span.total");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationStageSpan, stage), "span.stage");
			context.ExpectEqual(std::size_t{12U}, offsetof(GpuAnimationStageSpan, firstCommand), "span.firstCommand");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationStageSpan, commandCount), "span.commandCount");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationStageSpan, firstDispatchRecord), "span.firstDispatch");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationStageSpan, dispatchRecordCount), "span.dispatchCount");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationStageSpan, barrierFlags), "span.barriers");
			ExpectAbiRecord<GpuAnimationArenaAllocation>(context, 48U, "arena allocation");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationArenaAllocation, offsetBytes), "allocation.offset");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationArenaAllocation, sizeBytes), "allocation.size");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationArenaAllocation, arenaEpoch), "allocation.epoch");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationArenaAllocation, generation), "allocation.generation");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationArenaAllocation, arenaKind), "allocation.kind");
			context.ExpectEqual(std::size_t{32U}, offsetof(GpuAnimationArenaAllocation, alignmentBytes), "allocation.alignment");
			context.ExpectEqual(std::size_t{36U}, offsetof(GpuAnimationArenaAllocation, reserved), "allocation.reserved");
			ExpectAbiRecord<GpuAnimationPlanDiagnostics>(context, 32U, "plan diagnostics");
			context.ExpectEqual(std::size_t{0U}, offsetof(GpuAnimationPlanDiagnostics, flags), "planDiagnostics.flags");
			context.ExpectEqual(std::size_t{4U}, offsetof(GpuAnimationPlanDiagnostics, rejectedRequestCount), "planDiagnostics.rejected");
			context.ExpectEqual(std::size_t{8U}, offsetof(GpuAnimationPlanDiagnostics, fallbackRequestCount), "planDiagnostics.fallback");
			context.ExpectEqual(std::size_t{12U}, offsetof(GpuAnimationPlanDiagnostics, arenaFailureCount), "planDiagnostics.arena");
			context.ExpectEqual(std::size_t{16U}, offsetof(GpuAnimationPlanDiagnostics, prefixSumFailureCount), "planDiagnostics.prefix");
			context.ExpectEqual(std::size_t{20U}, offsetof(GpuAnimationPlanDiagnostics, dispatchLimitFailureCount), "planDiagnostics.dispatch");
			context.ExpectEqual(std::size_t{24U}, offsetof(GpuAnimationPlanDiagnostics, duplicateStableOrderCount), "planDiagnostics.duplicate");
			context.ExpectEqual(std::size_t{28U}, offsetof(GpuAnimationPlanDiagnostics, palettePartitionFailureCount), "planDiagnostics.palette");
		});

		registry.Add("Animation.GpuPlan.ArenasUseCheckedAlignmentGenerationAndAtomicFailure", [](TestContext& context) {
			GpuAnimationArena arena(GpuAnimationArenaKind::Scratch, 256U);
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(arena.BeginGeneration(9U)),
				"A nonzero renderer generation should begin an arena epoch.");

			GpuAnimationArenaAllocation first{};
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(arena.Allocate(1U, 1U, first)),
				"Requests below sixteen-byte alignment should be raised to the ABI minimum.");
			context.ExpectEqual(std::uint64_t{0U}, first.offsetBytes, "The first allocation should begin at zero.");
			context.ExpectEqual(16U, first.alignmentBytes, "The arena must store the effective alignment.");
			context.Expect(arena.Owns(first), "The current generation must own its published allocation.");

			GpuAnimationArenaAllocation second{};
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(arena.Allocate(1U, 64U, second)),
				"A larger plan alignment should create a checked aligned offset.");
			context.ExpectEqual(std::uint64_t{64U}, second.offsetBytes, "The second allocation must align from used byte one.");
			context.ExpectEqual(std::uint64_t{65U}, arena.UsedBytes(), "Alignment padding and payload must count toward capacity.");

			GpuAnimationArenaAllocation unchanged{};
			unchanged.offsetBytes = 999U;
			const std::uint64_t usedBeforeFailure = arena.UsedBytes();
			context.ExpectEqual(Value(GpuAnimationPlanStatus::ArenaCapacityExceeded),
				Value(arena.Allocate(256U, 16U, unchanged)),
				"An allocation beyond the bounded arena must fail closed.");
			context.ExpectEqual(std::uint64_t{999U}, unchanged.offsetBytes,
				"A rejected allocation must leave the caller's last-good record unchanged.");
			context.ExpectEqual(usedBeforeFailure, arena.UsedBytes(),
				"A rejected allocation must not consume any arena bytes.");

			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(arena.BeginGeneration(9U)),
				"Retirement should permit an externally reused renderer generation.");
			context.Expect(!arena.Owns(first),
				"A reused renderer generation must not revalidate an allocation from its retired arena epoch.");
			context.ExpectEqual(std::uint64_t{0U}, arena.UsedBytes(), "A new generation should reset only the checked offset.");
			context.ExpectEqual(Value(GpuAnimationPlanStatus::InvalidArgument),
				Value(arena.Configure(GpuAnimationArenaKind::Scratch, kGpuAnimationMaximumArenaBytesV1 + 1U)),
				"An arena larger than the version-one address contract must be rejected.");
			context.ExpectEqual(std::uint64_t{256U}, arena.CapacityBytes(),
				"A rejected reconfiguration must preserve the usable prior capacity.");
		});

		registry.Add("Animation.GpuPlan.PrefixSumsAreCheckedAndTransactional", [](TestContext& context) {
			const std::array<std::uint64_t, 3U> counts{5U, 0U, 7U};
			std::vector<std::uint64_t> offsets;
			std::uint64_t total = 0U;
			GpuAnimationPlanDiagnostics diagnostics{};
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(ComputeGpuAnimationPrefixOffsets(
				counts, 12U, offsets, total, diagnostics)),
				"A bounded prefix sum should publish deterministic offsets.");
			context.Expect(offsets == std::vector<std::uint64_t>{0U, 5U, 5U},
				"Zero-sized ranges must retain their deterministic prefix position.");
			context.ExpectEqual(std::uint64_t{12U}, total, "The checked total must include every range.");

			offsets = {99U};
			total = 88U;
			const std::array<std::uint64_t, 2U> overflowing{8U, 5U};
			context.ExpectEqual(Value(GpuAnimationPlanStatus::ArithmeticOverflow),
				Value(ComputeGpuAnimationPrefixOffsets(overflowing, 12U, offsets, total, diagnostics)),
				"A sum beyond the declared hard limit must fail before publication.");
			context.Expect(offsets == std::vector<std::uint64_t>{99U} && total == 88U,
				"Failed prefix sums must preserve both last-good outputs atomically.");
			context.ExpectEqual(1U, diagnostics.prefixSumFailureCount,
				"The bounded diagnostics value should count the rejected sum.");
			context.Expect(HasGpuAnimationDiagnostic(
				diagnostics.flags,
				GpuAnimationDiagnosticFlag::RangeOverflow),
				"Prefix rejection must expose the shared RangeOverflow bit.");
		});

		registry.Add("Animation.GpuPlan.FrameAtlasDeduplicatesDistinctExactPayloadCopies", [](TestContext& context) {
			const auto firstPayload = MakeAtlasResidentPayload(1.0F);
			const auto secondPayload = MakeAtlasResidentPayload(1.0F);
			context.Expect(firstPayload.get() != secondPayload.get(),
				"The fixture must use distinct immutable owners so pointer identity cannot satisfy deduplication.");
			const GpuAnimationRenderIdentity firstIdentity =
				MakeAtlasRenderIdentity(1U, 0x100000001ULL);
			const GpuAnimationRenderIdentity secondIdentity =
				MakeAtlasRenderIdentity(2U, 0x200000001ULL);
			const std::array<GpuAnimationFrameAtlasEntry, 2U> entries{{
				{MakeAtlasRequest(200U, 20U, firstIdentity), firstPayload},
				{MakeAtlasRequest(100U, 10U, secondIdentity), secondPayload}}};
			GpuAnimationFrameAtlas atlas{};
			const GpuAnimationLoweringStatus status =
				BuildGpuAnimationFrameAtlas(entries, atlas);
			context.ExpectEqual(
				Value(GpuAnimationLoweringStatus::Ok),
				Value(status),
				"Byte-identical payloads with distinct owners should share one checked atlas range.");
			if(status != GpuAnimationLoweringStatus::Ok) return;

			context.ExpectEqual(
				firstPayload->packedClipBytes.size(),
				atlas.residentPayloads.packedClipBytes.size(),
				"Exact clip bytes must be emitted only once.");
			context.ExpectEqual(
				firstPayload->programWords.size(),
				atlas.residentPayloads.programWords.size(),
				"Exact program words must be emitted only once.");
			context.ExpectEqual(
				firstPayload->skeletonTables.size(),
				atlas.residentPayloads.skeletonTables.size(),
				"Exact skeleton bytes must be emitted only once.");
			context.ExpectEqual(std::size_t{2U}, atlas.requests.size(),
				"Deduplication must retain both independently identified requests.");
			if(atlas.requests.size() != 2U) return;
			for(const GpuAnimationWorkRequest& request : atlas.requests) {
				context.Expect(request.clipResourceIndex == 0U
					&& request.programResourceIndex == 0U
					&& request.skeletonResourceIndex == 0U,
					"Deduplicated requests must point at the same aligned clip, program, and skeleton bases.");
			}
			context.Expect(atlas.requests[0U].renderIdentity == firstIdentity
				&& atlas.requests[1U].renderIdentity == secondIdentity,
				"Payload deduplication must not merge the requests' exact CPU-only render identities.");
		});

		registry.Add("Animation.GpuPlan.FrameAtlasRelocatesDirectoriesAndPreservesSortedTickIdentity", [](TestContext& context) {
			const auto firstPayload = MakeAtlasResidentPayload(1.0F);
			const auto secondPayload = MakeAtlasResidentPayload(2.0F);
			const GpuAnimationRenderIdentity firstIdentity =
				MakeAtlasRenderIdentity(3U, 0x1000000aaULL);
			const GpuAnimationRenderIdentity secondIdentity =
				MakeAtlasRenderIdentity(4U, 0x2000000aaULL);
			const std::array<GpuAnimationFrameAtlasEntry, 2U> entries{{
				{MakeAtlasRequest(200U, 222U, firstIdentity), firstPayload},
				{MakeAtlasRequest(100U, 111U, secondIdentity), secondPayload}}};
			GpuAnimationFrameAtlas atlas{};
			const GpuAnimationLoweringStatus atlasStatus =
				BuildGpuAnimationFrameAtlas(entries, atlas);
			context.ExpectEqual(
				Value(GpuAnimationLoweringStatus::Ok),
				Value(atlasStatus),
				"Non-identical payloads should append as independently relocated atlas ranges.");
			if(atlasStatus != GpuAnimationLoweringStatus::Ok
				|| atlas.requests.size() != 2U) {
				return;
			}

			const std::uint32_t secondClipBase = static_cast<std::uint32_t>(
				(firstPayload->packedClipBytes.size() + 3U) & ~std::size_t{3U});
			const std::uint32_t secondProgramBase = static_cast<std::uint32_t>(
				(firstPayload->programWords.size() * sizeof(std::uint32_t) + 63U)
					& ~std::size_t{63U});
			const std::uint32_t secondSkeletonBase = static_cast<std::uint32_t>(
				(firstPayload->skeletonTables.size() + 15U) & ~std::size_t{15U});
			context.Expect((secondClipBase % alignof(std::uint32_t)) == 0U
				&& (secondProgramBase % 64U) == 0U
				&& (secondSkeletonBase % 16U) == 0U,
				"Each appended descriptor payload must retain its frozen atlas alignment.");
			context.ExpectEqual(
				secondClipBase,
				atlas.requests[1U].clipResourceIndex,
				"The second request must point at its aligned clip base.");
			context.ExpectEqual(
				secondProgramBase / 64U,
				atlas.requests[1U].programResourceIndex,
				"The second request must name its relocated 64-byte program directory.");
			context.ExpectEqual(
				secondSkeletonBase / 16U,
				atlas.requests[1U].skeletonResourceIndex,
				"The second request must name its relocated 16-byte skeleton directory.");

			// Every T/R/S program lane originally points at ATD1 offset zero.
			// Relocation must add the clip base to all three lanes.
			const std::size_t secondJointWord =
				(static_cast<std::size_t>(secondProgramBase) + 64U)
					/ sizeof(std::uint32_t);
			context.Expect(secondJointWord + 2U
					< atlas.residentPayloads.programWords.size(),
				"The relocated program must retain its complete joint record.");
			if(secondJointWord + 2U
				< atlas.residentPayloads.programWords.size()) {
				context.Expect(
					atlas.residentPayloads.programWords[secondJointWord] == secondClipBase
						&& atlas.residentPayloads.programWords[secondJointWord + 1U] == secondClipBase
						&& atlas.residentPayloads.programWords[secondJointWord + 2U] == secondClipBase,
					"All T/R/S locators must point at the relocated ATD1 directory.");
			}
			const std::size_t secondDirectoryTrackLane =
				static_cast<std::size_t>(secondClipBase)
				+ sizeof(GpuAnimationTrackDirectoryHeader)
				+ offsetof(
					GpuAnimationTrackDirectoryEntry,
					trackHeaderOffsetBytes);
			const std::uint32_t relocatedTrackOffset =
				ReadFixtureValue<std::uint32_t>(
					atlas.residentPayloads.packedClipBytes,
					secondDirectoryTrackLane);
			context.ExpectEqual(
				secondClipBase
					+ static_cast<std::uint32_t>(
						sizeof(GpuAnimationTrackDirectoryHeader)
						+ sizeof(GpuAnimationTrackDirectoryEntry)),
				relocatedTrackOffset,
				"The ATD1 entry must point at its relocated track header, not the first payload's bytes.");

			// The plan sorts by stable identity after atlas construction. sampleTick
			// and the exact 64-bit render identity must follow the same permutation.
			GpuAnimationArena scratch{};
			GpuAnimationArena palette{};
			ConfigureArenas(scratch, palette);
			GpuAnimationPlan plan{};
			const GpuAnimationPlanStatus planStatus = BuildGpuAnimationPlan(
				atlas.requests, 91U, scratch, palette, plan);
			context.ExpectEqual(
				Value(GpuAnimationPlanStatus::Ok),
				Value(planStatus),
				"The relocated requests should build one deterministic frame plan.");
			if(planStatus != GpuAnimationPlanStatus::Ok
				|| plan.workPackets.size() != 2U
				|| plan.renderIdentities.size() != 2U) {
				return;
			}
			context.Expect(plan.workPackets[0U].stableInstanceId == 100U
				&& plan.workPackets[0U].sampleTick == 111U
				&& plan.renderIdentities[0U] == secondIdentity,
				"The lower stable instance must retain the second input's tick and exact identity after sorting.");
			context.Expect(plan.workPackets[1U].stableInstanceId == 200U
				&& plan.workPackets[1U].sampleTick == 222U
				&& plan.renderIdentities[1U] == firstIdentity,
				"The higher stable instance must retain the first input's tick and exact identity after sorting.");
			context.Expect(plan.workPackets[0U].clipResourceIndex == secondClipBase
				&& plan.workPackets[0U].programResourceIndex
					== secondProgramBase / 64U
				&& plan.workPackets[0U].skeletonResourceIndex
					== secondSkeletonBase / 16U,
				"Sorting must preserve every relocated descriptor association beside the packet.");
		});

		registry.Add("Animation.GpuPlan.ContentGenerationAndStableTieBreakSortingIsDeterministic", [](TestContext& context) {
			GpuAnimationWorkRequest programTwo = MakeRequest(300U, 2U, 1U);
			GpuAnimationWorkRequest newerProgramOne = MakeRequest(200U, 1U, 0U);
			newerProgramOne.generations.programGeneration = 2U;
			GpuAnimationWorkRequest ordinalFive = MakeRequest(100U, 1U, 5U);
			GpuAnimationWorkRequest ordinalThree = MakeRequest(100U, 1U, 3U);
			std::vector<GpuAnimationWorkRequest> forward{
				programTwo, newerProgramOne, ordinalFive, ordinalThree};
			std::vector<GpuAnimationWorkRequest> reverse = forward;
			std::reverse(reverse.begin(), reverse.end());

			GpuAnimationArena scratchForward;
			GpuAnimationArena paletteForward;
			GpuAnimationArena scratchReverse;
			GpuAnimationArena paletteReverse;
			ConfigureArenas(scratchForward, paletteForward);
			ConfigureArenas(scratchReverse, paletteReverse);
			GpuAnimationPlan forwardPlan;
			GpuAnimationPlan reversePlan;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(BuildGpuAnimationPlan(
				forward, 21U, scratchForward, paletteForward, forwardPlan)),
				"A valid mixed content/generation request set should build.");
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(BuildGpuAnimationPlan(
				reverse, 21U, scratchReverse, paletteReverse, reversePlan)),
				"The input permutation should build the same deterministic plan.");
			context.Expect(SamePlans(forwardPlan, reversePlan),
				"Content, generation, stable instance, and ordinal sorting must remove input-order dependence.");
			context.ExpectEqual(std::size_t{4U}, forwardPlan.workPackets.size(), "Every request should publish one packet.");
			context.ExpectEqual(std::uint64_t{100U}, forwardPlan.workPackets[0U].stableInstanceId, "First stable instance.");
			context.ExpectEqual(3U, forwardPlan.workPackets[0U].stableSubmissionOrdinal, "First stable ordinal.");
			context.ExpectEqual(5U, forwardPlan.workPackets[1U].stableSubmissionOrdinal, "Second stable ordinal.");
			context.ExpectEqual(std::uint64_t{200U}, forwardPlan.workPackets[2U].stableInstanceId, "Newer generation follows.");
			context.ExpectEqual(std::uint64_t{300U}, forwardPlan.workPackets[3U].stableInstanceId, "Different content follows.");
			context.ExpectEqual(std::size_t{3U}, forwardPlan.batches.size(),
				"Compatible content and generation requests should share exactly one batch.");
			context.ExpectEqual(2U, forwardPlan.batches[0U].workPacketCount,
				"The two ordinal-separated requests share their compatibility batch.");
			context.ExpectEqual(2U, forwardPlan.batches[1U].generationKey.programGeneration,
				"A changed content generation must begin a new batch.");
			context.ExpectEqual(std::uint64_t{2U}, forwardPlan.batches[2U].key.programContentId,
				"A changed program content ID must begin a new batch.");
			for(const GpuAnimationWorkPacket& packet : forwardPlan.workPackets) {
				context.Expect((packet.scratchOffsetBytes % 16U) == 0U,
					"Every scratch partition must retain the minimum shader alignment.");
			}
			context.ExpectEqual(scratchForward.UsedBytes(), scratchReverse.UsedBytes(),
				"Deterministic sorting must also produce deterministic scratch usage.");
			context.ExpectEqual(paletteForward.UsedBytes(), paletteReverse.UsedBytes(),
				"Deterministic sorting must also produce deterministic palette usage.");
		});

		registry.Add("Animation.GpuPlan.AggregatesCompatibleCommandsAndPreservesOptionalStageBarriers", [](TestContext& context) {
			context.ExpectEqual(
				Value(GpuAnimationBarrierFlag::CodecToBlend),
				GpuAnimationBarrierBetweenStages(
					GpuAnimationStage::Sample,
					GpuAnimationStage::Hierarchy),
				"Skipping Blend must still publish Sample writes before Hierarchy reads local poses.");
			GpuAnimationWorkRequest first = MakeRequest(10U, 8U, 1U);
			GpuAnimationWorkRequest second = MakeRequest(11U, 8U, 2U);
			for(GpuAnimationWorkRequest* request : std::array{&first, &second}) {
				request->batchKey.stageMask = GpuAnimationStageMask(GpuAnimationStage::Sample);
				request->batchKey.outputFlags = Value(GpuAnimationOutputFlag::Pose);
				request->paletteEntryCount = 0U;
				request->palettePartitionCount = 0U;
			}

			GpuAnimationArena scratch;
			GpuAnimationArena palette;
			ConfigureArenas(scratch, palette);
			GpuAnimationPlan plan;
			const std::array<GpuAnimationWorkRequest, 2U> compatible{first, second};
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(BuildGpuAnimationPlan(
				compatible, 25U, scratch, palette, plan)),
				"Compatible packets should build one renderer command rather than submit per character.");
			context.ExpectEqual(std::size_t{2U}, plan.dispatchRecords.size(), "Both work packets retain their bounded records.");
			context.ExpectEqual(std::size_t{1U}, plan.dispatchCommands.size(), "Compatible records aggregate into one command.");
			context.ExpectEqual(2U, plan.dispatchCommands[0U].pushConstants.dispatchRecordCount,
				"The command must identify both records to the shader.");
			context.ExpectEqual(2U, plan.dispatchCommands[0U].groupCountX,
				"The command must retain the combined bounded workgroup count.");

			GpuAnimationWorkRequest staged = MakeRequest(12U, 9U);
			staged.batchKey.stageMask = GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Ragdoll)
				| GpuAnimationStageMask(GpuAnimationStage::Morph);
			staged.batchKey.outputFlags = Value(GpuAnimationOutputFlag::Pose)
				| Value(GpuAnimationOutputFlag::MorphWeights);
			staged.morphCount = 1U;
			staged.morphResourceIndex = 7U;
			staged.paletteEntryCount = 0U;
			staged.palettePartitionCount = 0U;
			staged.flags |= Value(GpuAnimationWorkFlag::HasMorph)
				| Value(GpuAnimationWorkFlag::AllowRagdollVisual);
			GpuAnimationArena stagedScratch;
			GpuAnimationArena stagedPalette;
			ConfigureArenas(stagedScratch, stagedPalette);
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&staged, 1U), 26U,
				stagedScratch, stagedPalette, plan)),
				"An explicit hierarchy, ragdoll, and morph path should build.");
			context.ExpectEqual(std::size_t{3U}, plan.stageSpans.size(), "Every enabled optional stage exposes a span.");
			context.ExpectEqual(Value(GpuAnimationBarrierFlag::HierarchyToRagdoll), plan.stageSpans[0U].barrierFlags,
				"Skipping procedural must retain the distinct hierarchy-to-ragdoll boundary.");
			context.ExpectEqual(Value(GpuAnimationBarrierFlag::RagdollToMorph), plan.stageSpans[1U].barrierFlags,
				"Ragdoll output must become visible before morph evaluation.");
		});

		registry.Add("Animation.GpuPlan.GenerationValidationAndPlanFailureAreAtomic", [](TestContext& context) {
			GpuAnimationGenerationRecord expected = MakeRequest(1U, 1U).generations;
			GpuAnimationGenerationRecord actual = expected;
			actual.reserved = 42U;
			context.Expect(!GpuAnimationGenerationsMatch(expected, actual),
				"The frame-owned ragdoll table offset must invalidate a late GPU result.");
			actual.reserved = expected.reserved;
			actual.workGeneration = 2U;
			context.Expect(!GpuAnimationGenerationsMatch(expected, actual),
				"A changed work lifetime must invalidate a late GPU result.");

			GpuAnimationArena scratch;
			GpuAnimationArena palette;
			ConfigureArenas(scratch, palette, 256U);
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(scratch.BeginGeneration(77U)),
				"The sentinel scratch arena should begin its last-good generation.");
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(palette.BeginGeneration(77U)),
				"The sentinel palette arena should begin its last-good generation.");
			GpuAnimationArenaAllocation sentinelAllocation{};
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(scratch.Allocate(16U, 16U, sentinelAllocation)),
				"The sentinel arena should contain observable last-good usage.");
			GpuAnimationPlan output;
			output.generation = 99U;
			output.workPackets.push_back(GpuAnimationWorkPacket{});
			const std::uint64_t scratchUsedBefore = scratch.UsedBytes();

			GpuAnimationWorkRequest stale = MakeRequest(1U, 1U);
			stale.generations.clipGeneration = 0U;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::StaleGeneration), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&stale, 1U), 88U, scratch, palette, output)),
				"A zero resource generation must reject stale work before allocation.");
			context.ExpectEqual(99U, output.generation,
				"A rejected generation must preserve the caller's last-good plan.");
			context.ExpectEqual(std::size_t{1U}, output.workPackets.size(),
				"A rejected generation must not publish a partial packet list.");
			context.ExpectEqual(77U, scratch.Generation(),
				"A rejected build must preserve the last-good scratch generation.");
			context.ExpectEqual(scratchUsedBefore, scratch.UsedBytes(),
				"A rejected build must preserve all prior arena offsets.");

			GpuAnimationWorkRequest tooLarge = MakeRequest(2U, 1U);
			tooLarge.scratchSizeBytes = 240U;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::ArenaCapacityExceeded), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&tooLarge, 1U), 89U, scratch, palette, output)),
				"Capacity failure must reject the whole plan rather than a partial pose.");
			context.ExpectEqual(99U, output.generation,
				"Capacity failure must preserve the last-good plan generation.");
			context.ExpectEqual(77U, scratch.Generation(),
				"Capacity failure must preserve the last-good arena generation.");
			context.ExpectEqual(scratchUsedBefore, scratch.UsedBytes(),
				"Capacity failure must preserve the last-good arena usage.");
		});

		registry.Add("Animation.GpuPlan.WorkgroupsSplitBoundedStageRangesAndCarryBarriers", [](TestContext& context) {
			GpuAnimationWorkRequest request = MakeRequest(9U, 4U);
			request.batchKey.stageMask = GpuAnimationStageMask(GpuAnimationStage::Sample);
			request.batchKey.outputFlags = Value(GpuAnimationOutputFlag::Pose);
			request.jointCount = 130U;
			request.paletteEntryCount = 0U;
			request.palettePartitionCount = 0U;
			GpuAnimationPlanLimits limits{};
			limits.maximumGroupsX = 1U;
			limits.maximumElementsPerStage = 1000U;
			GpuAnimationArena scratch;
			GpuAnimationArena palette;
			ConfigureArenas(scratch, palette, 1U << 20U);
			GpuAnimationPlan plan;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&request, 1U), 31U, scratch, palette, plan, limits)),
				"A stage larger than one workgroup should split into bounded dispatch records.");
			context.ExpectEqual(std::size_t{3U}, plan.dispatchRecords.size(),
				"One 130-element stage at 64 threads and one group should split 64/64/2.");
			context.ExpectEqual(64U, plan.dispatchRecords[0U].elementCount, "First dispatch count.");
			context.ExpectEqual(64U, plan.dispatchRecords[1U].elementCount, "Second dispatch count.");
			context.ExpectEqual(2U, plan.dispatchRecords[2U].elementCount, "Tail dispatch count.");
			context.ExpectEqual(std::uint64_t{0U}, plan.dispatchRecords[0U].elementOffset, "First prefix.");
			context.ExpectEqual(std::uint64_t{64U}, plan.dispatchRecords[1U].elementOffset, "Second prefix.");
			context.ExpectEqual(std::uint64_t{128U}, plan.dispatchRecords[2U].elementOffset, "Tail prefix.");
			for(const GpuAnimationDispatchCommand& command : plan.dispatchCommands) {
				context.ExpectEqual(1U, command.groupCountX,
					"Each split command must stay within the configured one-group cap.");
				context.ExpectEqual(31U, command.pushConstants.planGeneration,
					"Every command must carry the generation used to build the plan.");
			}
			context.ExpectEqual(std::size_t{1U}, plan.stageSpans.size(),
				"Only the enabled Sample stage should publish a boundary span.");
			context.ExpectEqual(std::uint64_t{130U}, plan.stageSpans[0U].totalElementCount,
				"The span must retain the checked 64-bit total.");
			context.ExpectEqual(Value(GpuAnimationBarrierFlag::CodecToBlend), plan.stageSpans[0U].barrierFlags,
				"Sample completion must expose the codec-to-blend barrier intent.");

			GpuAnimationPlanLimits tooFewRecords = limits;
			tooFewRecords.maximumDispatchRecords = 2U;
			GpuAnimationPlan unchanged;
			unchanged.generation = 444U;
			const std::uint32_t scratchGeneration = scratch.Generation();
			const std::uint64_t scratchUsage = scratch.UsedBytes();
			context.ExpectEqual(Value(GpuAnimationPlanStatus::PlanLimitExceeded), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&request, 1U), 32U,
				scratch, palette, unchanged, tooFewRecords)),
				"A dispatch-record cap must reject the whole stage plan.");
			context.ExpectEqual(444U, unchanged.generation,
				"Dispatch-limit failure must preserve the caller's plan.");
			context.ExpectEqual(scratchGeneration, scratch.Generation(),
				"Dispatch-limit failure must preserve the arena generation.");
			context.ExpectEqual(scratchUsage, scratch.UsedBytes(),
				"Dispatch-limit failure must preserve the arena allocation state.");
		});

		registry.Add("Animation.GpuPlan.CommandsSplitAtTheShaderRecordScanLimit", [](TestContext& context) {
			std::vector<GpuAnimationWorkRequest> requests;
			requests.reserve(kGpuAnimationMaximumDispatchRecordsPerCommand + 1U);
			for(std::uint32_t index = 0U; index <= kGpuAnimationMaximumDispatchRecordsPerCommand; ++index) {
				// Every request must share one batch key; stable instance/ordinal values
				// provide deterministic per-packet identity without fragmenting the test
				// into 1,025 unrelated one-record batches.
				GpuAnimationWorkRequest request = MakeRequest(1000U + index, 1U, index);
				request.batchKey.stageMask = GpuAnimationStageMask(GpuAnimationStage::Sample);
				request.batchKey.outputFlags = Value(GpuAnimationOutputFlag::Pose);
				request.jointCount = 1U;
				request.paletteEntryCount = 0U;
				request.palettePartitionCount = 0U;
				requests.push_back(request);
			}
			GpuAnimationArena scratch;
			GpuAnimationArena palette;
			ConfigureArenas(scratch, palette, 1U << 20U);
			GpuAnimationPlan plan;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(BuildGpuAnimationPlan(
				requests, 41U, scratch, palette, plan)),
				"A stage above the shader scan bound must split rather than reject valid work.");
			context.ExpectEqual(std::size_t{2U}, plan.dispatchCommands.size(),
				"One thousand twenty-five records must become two shader-visible commands.");
			context.Expect(plan.dispatchCommands.size() == 2U
				&& plan.dispatchCommands[0U].pushConstants.firstDispatchRecord == 0U
				&& plan.dispatchCommands[0U].pushConstants.dispatchRecordCount
					== kGpuAnimationMaximumDispatchRecordsPerCommand
				&& plan.dispatchCommands[1U].pushConstants.firstDispatchRecord
					== kGpuAnimationMaximumDispatchRecordsPerCommand
				&& plan.dispatchCommands[1U].pushConstants.dispatchRecordCount == 1U,
				"Each split command must preserve the exact contiguous record range consumed by HLSL.");
		});

		registry.Add("Animation.GpuPlan.PaletteStridePartitionsAndPlanAlignmentAreBounded", [](TestContext& context) {
			context.ExpectEqual(48U, kGpuAnimationPaletteStride,
				"Palette matrices must retain the three-row 48-byte shader stride.");
			context.ExpectEqual(256U, kGpuAnimationPaletteEntriesPerPartition,
				"One palette partition must retain the frozen 256-entry limit.");
			context.ExpectEqual(256U, kGpuAnimationMaximumPalettePartitionsV1,
				"Version one must reject more than 256 palette partitions per request.");

			GpuAnimationWorkRequest request = MakeRequest(12U, 5U);
			request.jointCount = 300U;
			request.paletteEntryCount = 300U;
			request.palettePartitionCount = 2U;
			request.scratchAlignmentBytes = 16U;
			GpuAnimationPlanLimits limited{};
			limited.maximumPalettePartitions = 1U;
			limited.scratchAlignmentBytes = 64U;
			GpuAnimationArena scratch;
			GpuAnimationArena palette;
			ConfigureArenas(scratch, palette, 1U << 20U);
			GpuAnimationPlan plan;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::PalettePartitionLimitExceeded),
				Value(BuildGpuAnimationPlan(
					std::span<const GpuAnimationWorkRequest>(&request, 1U),
					51U, scratch, palette, plan, limited)),
				"A request needing two palette partitions must fail a one-partition plan limit.");
			context.ExpectEqual(0U, scratch.Generation(),
				"Partition rejection must occur before either arena publishes a generation.");

			limited.maximumPalettePartitions = 2U;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::Ok), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&request, 1U),
				51U, scratch, palette, plan, limited)),
				"Two allowed partitions should hold a 300-entry palette.");
			context.ExpectEqual(std::uint64_t{300U * 48U}, palette.UsedBytes(),
				"Palette arena usage must be entry count times the frozen stride.");
			context.Expect((plan.workPackets[0U].scratchOffsetBytes % 64U) == 0U,
				"Effective scratch alignment must be max(16, plan, request).");

			// The planner consumes the cooker's explicit partition metadata. It must
			// not recreate a partition count from palette-entry arithmetic here.
			request.palettePartitionCount = 1U;
			limited.maximumPalettePartitions = 1U;
			GpuAnimationArena metadataScratch;
			GpuAnimationArena metadataPalette;
			ConfigureArenas(metadataScratch, metadataPalette, 1U << 20U);
			context.ExpectEqual(Value(GpuAnimationPlanStatus::InvalidArgument), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&request, 1U),
				52U, metadataScratch, metadataPalette, plan, limited)),
				"Cooked metadata that places more than 256 entries in one partition must fail closed.");
			context.ExpectEqual(51U, plan.generation,
				"A rejected inconsistent partition record must preserve the last complete plan.");
		});

		registry.Add("Animation.GpuPlan.InvalidResourcesFlagsAndDuplicateOrderFailClosed", [](TestContext& context) {
			GpuAnimationArena scratch;
			GpuAnimationArena palette;
			ConfigureArenas(scratch, palette);
			GpuAnimationPlan output;
			output.generation = 900U;

			GpuAnimationWorkRequest invalidResource = MakeRequest(1U, 1U);
			invalidResource.clipResourceIndex = kGpuAnimationInvalidResourceIndex;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::InvalidArgument), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&invalidResource, 1U),
				1U, scratch, palette, output)),
				"A missing required clip resource must fail before plan publication.");
			context.ExpectEqual(900U, output.generation, "Missing resources must preserve the last-good plan.");

			GpuAnimationWorkRequest conflictingFallbacks = MakeRequest(1U, 1U);
			conflictingFallbacks.flags |= Value(GpuAnimationWorkFlag::UseHoldLastFallback);
			context.ExpectEqual(Value(GpuAnimationPlanStatus::InvalidArgument), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&conflictingFallbacks, 1U),
				1U, scratch, palette, output)),
				"A packet cannot request both bind/reference and hold-last fallback policies.");

			GpuAnimationWorkRequest unknownFlag = MakeRequest(1U, 1U);
			unknownFlag.flags |= 0x80000000U;
			context.ExpectEqual(Value(GpuAnimationPlanStatus::InvalidArgument), Value(BuildGpuAnimationPlan(
				std::span<const GpuAnimationWorkRequest>(&unknownFlag, 1U),
				1U, scratch, palette, output)),
				"Unknown work-policy bits must fail instead of reaching a shader generation.");

			GpuAnimationWorkRequest first = MakeRequest(7U, 2U, 3U);
			GpuAnimationWorkRequest duplicate = first;
			duplicate.generations.instanceSlot = 99U;
			duplicate.batchKey.programContentId = 99U;
			duplicate.generations.programGeneration = 2U;
			const std::array<GpuAnimationWorkRequest, 2U> ambiguous{first, duplicate};
			GpuAnimationPlanBuildDiagnostics rejection{};
			context.ExpectEqual(Value(GpuAnimationPlanStatus::DuplicateStableOrder), Value(BuildGpuAnimationPlan(
				ambiguous, 2U, scratch, palette, output, {}, &rejection)),
				"The final stable instance/ordinal pair must uniquely order every request.");
			context.ExpectEqual(900U, output.generation,
				"Ambiguous stable ordering must not publish a scheduler-dependent plan.");
			context.ExpectEqual(1U, rejection.counters.rejectedRequestCount,
				"A rejected transaction must publish one bounded rejection counter snapshot.");
			context.ExpectEqual(1U, rejection.counters.duplicateStableOrderCount,
				"The rejection snapshot must name duplicate stable ordering explicitly.");
			context.ExpectEqual(Value(GpuAnimationPlanStatus::DuplicateStableOrder), rejection.rejection.status,
				"The rejection detail must preserve the precise non-published failure reason.");
		});
	}
}
