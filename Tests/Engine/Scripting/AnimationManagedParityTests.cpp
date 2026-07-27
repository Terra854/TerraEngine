// AnimationManagedParityTests.cpp - Native/C# animation ABI and surface parity checks.

#include "../TestHarness.h"

#include "Scripting/AnimationScriptAbi.h"
#include "Scripting/ScriptGlue.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		std::string ReadManagedAnimationSource(const char* relativePath)
		{
			// Source-contract checks use the configure-time checkout root so the
			// focused test behaves identically from CTest, an IDE, or a shell.
			std::ifstream source{std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary};
			return {std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
		}
	}

	void RegisterAnimationManagedParityTests(TestRegistry& registry)
	{
		registry.Add("Animation.ManagedParity.CopiedAbiV1LayoutsAreStable", [](TestContext& context) {
			context.ExpectEqual(static_cast<std::uint16_t>(1U), Native_Animation_GetAbiVersion(),
				"The managed animation bridge must advertise the frozen ABI v1 contract.");
			context.ExpectEqual(static_cast<std::size_t>(8U), sizeof(NativeAnimationAbiHeader),
				"Every copied animation record must begin with the eight-byte size/version/kind header.");
			context.ExpectEqual(static_cast<std::size_t>(288U), sizeof(NativeAnimationCommandRequest),
				"Commands must retain the pointer-free v1 payload, including typed future IK fields.");
			context.ExpectEqual(static_cast<std::size_t>(88U), sizeof(NativeAnimationCommandReceipt),
				"Receipts must retain observable tick, generation, cancellation, and idempotency identity.");
			context.ExpectEqual(static_cast<std::size_t>(216U), sizeof(NativeAnimationStateSnapshot),
				"State snapshots must retain explicit source, observation, age, count, and publication metadata.");
			context.ExpectEqual(static_cast<std::size_t>(104U), sizeof(NativeAnimationBoneSnapshot),
				"Bone snapshots must copy complete local and model transforms without native pointers.");
			context.ExpectEqual(static_cast<std::size_t>(72U), sizeof(NativeAnimationSocketSnapshot),
				"Socket snapshots must copy their stable parent and complete model transform.");
			context.ExpectEqual(static_cast<std::size_t>(96U), sizeof(NativeAnimationRootMotionResult),
				"The legacy root-motion result must retain its exact 96-byte copied ABI stride.");
			context.ExpectEqual(static_cast<std::size_t>(72U), sizeof(NativeAnimationRootMotionApplicationRequest),
				"Root-motion application requests must remain pointer-free and fixed-width for managed producers.");
			context.ExpectEqual(static_cast<std::size_t>(96U), sizeof(NativeAnimationRootMotionApplicationReceipt),
				"Root-motion application receipts must carry retry identity and protected-boundary visibility metadata.");
			context.ExpectEqual(static_cast<std::size_t>(240U), sizeof(NativeAnimationRootMotionResultDetails),
				"Detailed root-motion results must carry authored policy, transforms, and source metadata.");
			context.ExpectEqual(static_cast<std::uint16_t>(14U), static_cast<std::uint16_t>(NativeAnimationRecordKind::RootMotionResult),
				"The legacy root-motion record kind must remain unchanged for older managed readers.");
			context.ExpectEqual(static_cast<std::uint16_t>(15U), static_cast<std::uint16_t>(NativeAnimationRecordKind::RootMotionApplicationRequest),
				"Application requests must use the append-only record kind 15.");
			context.ExpectEqual(static_cast<std::uint16_t>(16U), static_cast<std::uint16_t>(NativeAnimationRecordKind::RootMotionApplicationReceipt),
				"Application receipts must use the append-only record kind 16.");
			context.ExpectEqual(static_cast<std::uint16_t>(17U), static_cast<std::uint16_t>(NativeAnimationRecordKind::RootMotionResultDetails),
				"Detailed results must use the append-only record kind 17.");
			context.ExpectEqual(static_cast<std::uint32_t>(5U), static_cast<std::uint32_t>(NativeAnimationRootMotionDisposition::Deferred),
				"Deferred publication must be represented by the append-only disposition value 5.");
			context.ExpectEqual(static_cast<std::size_t>(0U), offsetof(NativeAnimationCommandRequest, header),
				"A command must validate its ABI header before reading any payload field.");
			context.ExpectEqual(static_cast<std::size_t>(248U), offsetof(NativeAnimationCommandRequest, producerSequence),
				"Deterministic producer ordering must retain its pinned aligned offset.");
		});

		registry.Add("Animation.ManagedParity.RootMotionNativeDeclarationsAreCallable", [](TestContext& context) {
			// Taking typed addresses verifies that the native declarations and their
			// pointer constness match the records consumed by the managed P/Invoke layer.
			using ApplyFunction = NativeAnimationStatus (*)(
				const NativeAnimationRootMotionApplicationRequest*,
				NativeAnimationRootMotionApplicationReceipt*);
			using DetailsFunction = NativeAnimationStatus (*)(
				const NativeAnimationAnimatorHandle*,
				NativeAnimationRootMotionResultDetails*,
				std::uint32_t,
				NativeAnimationBufferInfo*);
			using LegacyCopyFunction = NativeAnimationStatus (*)(
				const NativeAnimationAnimatorHandle*,
				NativeAnimationRootMotionResult*,
				std::uint32_t,
				NativeAnimationBufferInfo*);
			const ApplyFunction apply = &Native_Animation_ApplyRootMotion;
			const DetailsFunction details = &Native_Animation_CopyRootMotionResultDetails;
			const LegacyCopyFunction legacy = &Native_Animation_CopyRootMotionResults;
			context.Expect(apply != nullptr && details != nullptr && legacy != nullptr,
				"Native root-motion apply, details, and legacy-copy entry points must retain their typed copied-record declarations.");
		});

		registry.Add("Animation.ManagedParity.EnumsHandlesAndStatusesMatchNative", [](TestContext& context) {
			context.Expect(static_cast<std::uint16_t>(NativeAnimationStatus::Ok) == 0U
				&& static_cast<std::uint16_t>(NativeAnimationStatus::StaleHandle) == 8U
				&& static_cast<std::uint16_t>(NativeAnimationStatus::ContentHashMismatch) == 23U,
				"Managed status values must preserve the complete append-only native range.");
			context.Expect(static_cast<std::uint32_t>(NativeAnimationCommandKind::SetParameter) == 0U
				&& static_cast<std::uint32_t>(NativeAnimationCommandKind::SetIkGoal) == 12U
				&& static_cast<std::uint32_t>(NativeAnimationCommandKind::RequestRagdoll) == 14U,
				"Managed commands must preserve native numeric identities across foundational and future operations.");
			context.Expect(static_cast<std::uint32_t>(NativeAnimationParameterType::Trigger) == 0U
				&& static_cast<std::uint32_t>(NativeAnimationParameterType::Transform) == 6U,
				"Managed parameter tags must cover each copied native value alternative.");
			context.ExpectEqual(static_cast<std::size_t>(8U), sizeof(NativeAnimationAnimatorHandle),
				"Managed wrappers must retain a slot plus generation rather than a native pointer.");
			context.ExpectEqual(static_cast<std::size_t>(16U), sizeof(NativeAnimationUuid),
				"Stable asset and object identities must stay fixed-width across the ABI.");
		});

		registry.Add("Animation.ManagedParity.PublicSurfaceCoversGameplayReadsAndWrites", [](TestContext& context) {
			const std::string types = ReadManagedAnimationSource("Engine/Managed/Source/Animation/AnimationTypes.cs");
			const std::string animator = ReadManagedAnimationSource("Engine/Managed/Source/Animation/Animator.cs");
			const std::string calls = ReadManagedAnimationSource("Engine/Managed/Source/InternalCalls.cs");
			const std::string component = ReadManagedAnimationSource("Engine/Managed/Source/Component.cs");

			context.Expect(types.find("namespace TerraEngine.Animation") != std::string::npos
				&& types.find("public enum AnimationStatus : ushort") != std::string::npos
				&& types.find("public struct AnimatorHandle") != std::string::npos,
				"The public Animation namespace must expose the native status and generation-safe handle vocabulary.");
			context.Expect(types.find("SkeletonAsset") != std::string::npos
				&& types.find("AnimationClipAsset") != std::string::npos
				&& types.find("AnimationControllerAsset") != std::string::npos
				&& types.find("RagdollProfileAsset") != std::string::npos,
				"Managed gameplay must receive typed value references for every foundational animation asset family.");
			context.Expect(types.find("public long ObservationTick") != std::string::npos
				&& types.find("public long AgeTicks") != std::string::npos
				&& types.find("public ulong PublicationGeneration") != std::string::npos
				&& types.find("public AnimationControllerAsset Controller") != std::string::npos
				&& types.find("public AnimationClipAsset Clip") != std::string::npos
				&& types.find("public SkeletonAsset Skeleton") != std::string::npos,
				"Copied state must identify its observation boundary and every active asset generation.");
			context.Expect(animator.find("SetParameter(AnimationParameterId parameter, bool value") != std::string::npos
				&& animator.find("SetParameter(AnimationParameterId parameter, Vector3 value") != std::string::npos
				&& animator.find("SetIkGoal") != std::string::npos,
				"Animator writes must cover typed parameters and a behaviorally honest typed IK request.");
			context.Expect(animator.find(
					"public AnimationCommandReceipt CrossFade(ulong stateId, float transitionSeconds")
					!= std::string::npos
				&& animator.find(
					"CreateCommand(AnimationCommandKind.CrossFade, options)")
					!= std::string::npos
				&& animator.find("request.StateId = stateId;")
					!= std::string::npos
				&& animator.find(
					"request.TransitionSeconds = transitionSeconds;")
					!= std::string::npos,
				"Managed gameplay must retain the destination controller state and blend duration in a typed CrossFade command.");
			context.Expect(animator.find("public uint DefaultClipStartFrame") != std::string::npos
				&& animator.find("GetProperty<uint>(\"defaultClipStartFrame\")") != std::string::npos
				&& animator.find("SetProperty(\"defaultClipStartFrame\", value)") != std::string::npos,
				"Managed gameplay must be able to persist the same direct-clip start frame exposed by the Inspector.");
			context.Expect(animator.find("public bool PlayAutomatically") != std::string::npos
				&& animator.find("GetProperty<bool>(\"playAutomatically\")") != std::string::npos
				&& animator.find("SetProperty(\"playAutomatically\", value)") != std::string::npos,
				"Managed gameplay must be able to persist the same automatic-play switch exposed by the Inspector.");
			context.Expect(animator.find("ReadState") != std::string::npos
				&& animator.find("ReadCurves") != std::string::npos
				&& animator.find("ReadMorphs") != std::string::npos
				&& animator.find("ReadBones") != std::string::npos
				&& animator.find("ReadSockets") != std::string::npos
				&& animator.find("ReadEvents") != std::string::npos
				&& animator.find("ReadRootMotionResults") != std::string::npos,
				"Animator queries must return copied parity snapshots for every US5 gameplay observation family.");
			context.Expect(calls.find("Native_Animation_SubmitCommand") != std::string::npos
				&& calls.find("Native_Animation_QueryCapability") != std::string::npos
				&& calls.find("Native_Animation_CopySockets") != std::string::npos
				&& calls.find("Native_Animation_ApplyRootMotion") != std::string::npos
				&& calls.find("Native_Animation_CopyRootMotionResultDetails") != std::string::npos,
				"Managed methods must cross one versioned copied native declaration set rather than retain engine storage.");
			context.Expect(types.find("AnimationRootMotionApplicationMode") != std::string::npos
				&& types.find("AnimationRootMotionApplicationOptions") != std::string::npos
				&& types.find("NativeAnimationRootMotionApplicationRequest") != std::string::npos
				&& types.find("NativeAnimationRootMotionApplicationReceipt") != std::string::npos
				&& types.find("NativeAnimationRootMotionResultDetails") != std::string::npos,
				"Managed parity must expose typed root-motion application options, receipts, and detailed results.");
			context.Expect(animator.find("ApplyRootMotion(") != std::string::npos
				&& animator.find("RootMotionResultDetails") != std::string::npos,
				"Animator must provide an explicit root-motion apply method and read the additive detailed-result record.");
			context.Expect(types.find("public ulong ParentBoneStableId") != std::string::npos,
				"Managed socket records must retain the stable parent bone identity from the copied native publication.");
			context.Expect(component.find("nameof(Animation.Animator) => \"AnimatorComponent\"") != std::string::npos,
				"Generic component lookup must resolve Animator to the CRTP native component name.");
		});

		registry.Add("Animation.ManagedParity.ReceiptsPreserveVisibilityAndRetryIdentity", [](TestContext& context) {
			const std::string types = ReadManagedAnimationSource("Engine/Managed/Source/Animation/AnimationTypes.cs");
			const std::string animator = ReadManagedAnimationSource("Engine/Managed/Source/Animation/Animator.cs");
			context.Expect(types.find("public long ObservableTick") != std::string::npos
				&& types.find("public ulong IdempotencyKey") != std::string::npos
				&& types.find("public ulong CancellationToken") != std::string::npos
				&& types.find("public bool Duplicate") != std::string::npos,
				"A copied receipt must distinguish visibility, retry, cancellation, and duplicate reconciliation.");
			context.Expect(animator.find("ReadReceipt") != std::string::npos
				&& animator.find("Cancel") != std::string::npos
				&& animator.find("QueryCapability") != std::string::npos
				&& animator.find("correlationId = producerSequence") != std::string::npos
				&& animator.find("correlationId = cancellationToken") != std::string::npos
				&& animator.find("options.IdempotencyKey == 0 ? sequence") != std::string::npos
				&& animator.find("options.CancellationToken == 0 ? sequence") != std::string::npos,
				"Gameplay must be able to poll, cancel, and preflight non-blocking animation work.");
			context.Expect(animator.find("s_producerSequenceExhausted") != std::string::npos
				&& animator.find("TryReserveManagedProducerSequence") != std::string::npos
				&& animator.find("Volatile.Read(ref s_producerSequenceExhausted)") != std::string::npos,
				"Managed producer ordering must latch closed after overflow instead of eventually wrapping back to identity one.");
		});

		registry.Add("Animation.ManagedParity.AdvancedCopiedAbiExtensionsRemainFixedWidth", [](TestContext& context) {
			context.Expect(static_cast<std::uint16_t>(NativeAnimationRecordKind::RetargetRequest) == 21U
				&& static_cast<std::uint16_t>(NativeAnimationRecordKind::DeterminismSnapshot) == 32U,
				"Advanced animation ABI record kinds must append after the pinned procedural records.");
			context.Expect(sizeof(NativeAnimationRetargetRequest) == 72U
				&& sizeof(NativeAnimationMotionMatchSnapshot) == 96U
				&& sizeof(NativeAnimationRagdollSnapshot) == 80U
				&& sizeof(NativeAnimationDeterminismSnapshot) == 80U,
				"Retarget, motion-match, ragdoll, and determinism records must stay fixed-width copied values.");
			const std::string retarget = ReadManagedAnimationSource("Engine/Managed/Source/Animation/AnimationRetargeting.cs");
			const std::string motion = ReadManagedAnimationSource("Engine/Managed/Source/Animation/AnimationMotionMatching.cs");
			const std::string ragdoll = ReadManagedAnimationSource("Engine/Managed/Source/Animation/AnimationRagdoll.cs");
			const std::string determinism = ReadManagedAnimationSource("Engine/Managed/Source/Animation/AnimationDeterminism.cs");
			const std::string calls = ReadManagedAnimationSource("Engine/Managed/Source/InternalCalls.cs");
			context.Expect(retarget.find("RequestRetarget") != std::string::npos
				&& motion.find("RequestMotionMatch") != std::string::npos
				&& ragdoll.find("RequestRagdoll(AnimationRagdollOperation") != std::string::npos
				&& determinism.find("RequestDeterminism") != std::string::npos,
				"Managed advanced controls must use typed copied requests instead of runtime object aliases.");
			context.Expect(calls.find("Native_Animation_SubmitRetargetRequest") != std::string::npos
				&& calls.find("Native_Animation_CopyMotionMatchSnapshots") != std::string::npos
				&& calls.find("Native_Animation_SubmitRagdollRequest") != std::string::npos
				&& calls.find("Native_Animation_CopyDeterminismSnapshots") != std::string::npos,
				"Every advanced managed control and snapshot read must have a typed P/Invoke declaration.");
		});

		registry.Add("Animation.ManagedParity.MotionMatchV2MirrorsTypedQueryAndResult", [](TestContext& context) {
			const std::string types = ReadManagedAnimationSource(
				"Engine/Managed/Source/Animation/AnimationTypes.cs");
			const std::string motion = ReadManagedAnimationSource(
				"Engine/Managed/Source/Animation/AnimationMotionMatching.cs");
			const std::string calls = ReadManagedAnimationSource(
				"Engine/Managed/Source/InternalCalls.cs");
			context.Expect(types.find("MotionMatchQueryRequestV2 = 33") != std::string::npos
				&& types.find("MotionMatchQueryReceiptV2 = 34") != std::string::npos
				&& types.find("MotionMatchResultV2 = 35") != std::string::npos,
				"Managed record kinds must append the same three V2 identities as native ABI records.");
			context.Expect(motion.find("public const int FeatureCount = 48") != std::string::npos
				&& motion.find("public const int FeatureBlockCount = 9") != std::string::npos
				&& motion.find("public const int MaximumExcludedSamples = 256") != std::string::npos
				&& motion.find("internal fixed short Features") != std::string::npos
				&& motion.find("internal fixed ushort FeatureWeights") != std::string::npos
				&& motion.find("internal fixed ulong ExcludedSampleIds") != std::string::npos,
				"The managed request must mirror every bounded inline native lane without pointers.");
			context.Expect(motion.find("public ulong SelectedSourceClipId") != std::string::npos
				&& motion.find("public long SelectedSourceTick") != std::string::npos
				&& motion.find("public ReadOnlyMemory<ulong> FeatureBlockCosts") != std::string::npos
				&& motion.find("public AnimationMotionMatchFallbackReason FallbackReason") != std::string::npos
				&& motion.find("public AnimationMotionMatchSelectionSource SelectionSource") != std::string::npos
				&& motion.find("public AnimationStatus GpuStatus") != std::string::npos
				&& motion.find("public bool Has(AnimationMotionMatchResultFields field)") != std::string::npos,
				"Managed results must preserve typed source, cost, fallback, selection, GPU, and validity detail.");
			context.Expect(motion.find("QueryMotionMatch") != std::string::npos
				&& motion.find("ReadMotionMatchResults") != std::string::npos
				&& calls.find("Native_Animation_SubmitMotionMatchQueryV2") != std::string::npos
				&& calls.find("Native_Animation_CopyMotionMatchResultsV2") != std::string::npos,
				"The public methods and P/Invoke declarations must expose the same copied V2 bridge.");
		});
	}
}
