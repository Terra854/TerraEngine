// AnimationGameplayCommandTests.cpp - Gameplay ordering, receipts, and capabilities.

#include "TestHarness.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/AnimationAttachments.h"
#include "Animation/Runtime/AdvancedAnimationCommandStore.h"
#include "Animation/Runtime/AnimationCommandQueue.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"
#include "Animation/Ragdoll/RagdollPhysicsBridge.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Scripting/AnimationScriptAbi.h"
#include "Scripting/ScriptGlue.h"

#include <algorithm>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimatorInstanceDescriptor Descriptor(std::uint8_t seed)
		{
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0] = seed;
			descriptor.componentGeneration = 1U;
			return descriptor;
		}

		AnimationCommand Command(
			AnimatorInstanceHandle target,
			std::uint32_t producer,
			std::uint64_t sequence,
			std::uint64_t correlation,
			std::uint64_t idempotency,
			std::uint64_t cancellation)
		{
			AnimationCommand command;
			command.target = target;
			command.kind = AnimationCommandKind::SetParameter;
			command.parameterType = AnimationParameterType::Float;
			command.parameterId = 5U;
			command.value = 0.25F;
			command.targetTick = 8;
			command.producerId = producer;
			command.producerSequence = sequence;
			command.correlationId = correlation;
			command.idempotencyKey = idempotency;
			command.cancellationToken = cancellation;
			return command;
		}

		template<typename Record>
		Record AbiRecord(NativeAnimationRecordKind kind)
		{
			Record record{};
			record.header = {static_cast<std::uint32_t>(sizeof(Record)), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(kind)};
			return record;
		}

		bool ExecuteGameplayAnimationFrame(AnimationSystem& system)
		{
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				system.DeclareFrameWorkloads(1.0F / 60.0F);
			const TerraEngine::Scheduling::FrameExecutionPlan plan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), "animation-gameplay-advanced");
			return plan.valid && plan.completed;
		}
	}

	void RegisterAnimationGameplayCommandTests(TestRegistry& registry)
	{
		registry.Add("Animation.Gameplay.CommandsMergeDeterministicallyAcrossProducers", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(Descriptor(1U));
			AnimationCommandQueue queue;
			AnimationCommand lateProducer = Command(target, 9U, 2U, 90U, 900U, 9000U);
			AnimationCommand earlyProducer = Command(target, 2U, 3U, 20U, 200U, 2000U);
			AnimationCommand future = Command(target, 1U, 1U, 10U, 100U, 1000U);
			future.targetTick = 9;
			context.Expect(queue.Enqueue(lateProducer, store).status == AnimationStatus::Queued
				&& queue.Enqueue(earlyProducer, store).status == AnimationStatus::Queued
				&& queue.Enqueue(future, store).status == AnimationStatus::Queued,
				"Independent copied producers should enqueue without sharing mutable state.");
			std::vector<AnimationCommand> drained;
			queue.DrainThrough(8, drained, store);
			context.Expect(drained.size() == 2U && drained[0].producerId == 2U && drained[1].producerId == 9U,
				"Due commands must merge by tick, producer, sequence, then correlation.");
			context.ExpectEqual(static_cast<std::size_t>(1U), queue.PendingCount(),
				"A future command must remain isolated from the current deterministic batch.");
		});

		registry.Add("Animation.Gameplay.TriggerAndStructuredParametersStayTyped", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(Descriptor(2U));
			AnimationCommandQueue queue;
			AnimationCommand trigger = Command(target, 1U, 1U, 1U, 11U, 101U);
			trigger.kind = AnimationCommandKind::FireTrigger;
			trigger.parameterType = AnimationParameterType::Trigger;
			trigger.value = std::monostate{};
			context.Expect(queue.Enqueue(trigger, store).status == AnimationStatus::Queued,
				"A trigger must use its edge-only monostate payload.");
			AnimationCommand invalidTrigger = trigger;
			invalidTrigger.producerSequence = 2U;
			invalidTrigger.correlationId = 2U;
			invalidTrigger.idempotencyKey = 12U;
			invalidTrigger.cancellationToken = 102U;
			invalidTrigger.value = true;
			context.Expect(queue.Enqueue(invalidTrigger, store).status == AnimationStatus::InvalidArgument,
				"A persistent boolean must never be reinterpreted as a trigger edge.");
			AnimationCommand vector = Command(target, 1U, 3U, 3U, 13U, 103U);
			vector.parameterType = AnimationParameterType::Vector3;
			vector.value = AnimationFloat3{1.0F, 2.0F, 3.0F};
			context.Expect(queue.Enqueue(vector, store).status == AnimationStatus::Queued,
				"Vector parameters must remain typed through the native command queue.");
		});

		registry.Add("Animation.Gameplay.ControllerCommandsRetainTypedPayloads", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(Descriptor(22U));
			AnimationCommandQueue queue;
			AnimationCommand crossFade = Command(target, 22U, 1U, 2201U, 22001U, 220001U);
			crossFade.kind = AnimationCommandKind::CrossFade;
			crossFade.stateId = 9001U;
			crossFade.transitionTicks = 15;
			context.Expect(queue.Enqueue(crossFade, store).status == AnimationStatus::Queued,
				"Cross-fade state and fixed-tick duration must remain a typed queue payload.");

			AnimationCommand layer = Command(target, 22U, 2U, 2202U, 22002U, 220002U);
			layer.kind = AnimationCommandKind::SetLayerWeight;
			layer.layerIndex = 3U;
			layer.layerWeight = 0.375F;
			context.Expect(queue.Enqueue(layer, store).status == AnimationStatus::Queued,
				"Layer index and bounded weight must remain explicit controller fields.");
			AnimationCommand invalidLayer = layer;
			invalidLayer.producerSequence = 3U;
			invalidLayer.correlationId = 2203U;
			invalidLayer.idempotencyKey = 22003U;
			invalidLayer.cancellationToken = 220003U;
			invalidLayer.layerWeight = 1.25F;
			context.Expect(queue.Enqueue(invalidLayer, store).status == AnimationStatus::InvalidArgument,
				"An out-of-range layer weight must fail before entering pending storage.");
		});

		registry.Add("Animation.Gameplay.NativeControllerCommandsReachTheTypedQueue", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(Descriptor(23U));
			NativeAnimationCommandRequest request =
				AbiRecord<NativeAnimationCommandRequest>(NativeAnimationRecordKind::CommandRequest);
			request.animator = {handle.slotIndex, handle.generation};
			request.command = NativeAnimationCommandKind::CrossFade;
			request.stateId = 42U;
			request.transitionSeconds = 0.25F;
			request.producerId = 23U;
			request.producerSequence = 1U;
			request.correlationId = 2301U;
			request.idempotencyKey = 23001U;
			request.cancellationToken = 230001U;
			NativeAnimationCommandReceipt receipt =
				AbiRecord<NativeAnimationCommandReceipt>(NativeAnimationRecordKind::CommandReceipt);
			context.Expect(Native_Animation_SubmitCommand(&request, &receipt)
				== NativeAnimationStatus::Queued,
				"The managed ABI cross-fade must no longer stop at an Unsupported shim.");

			NativeAnimationCancellationRequest cancellation =
				AbiRecord<NativeAnimationCancellationRequest>(NativeAnimationRecordKind::CancellationRequest);
			cancellation.cancellationToken = request.cancellationToken;
			cancellation.correlationId = 2399U;
			NativeAnimationCommandReceipt cancelled =
				AbiRecord<NativeAnimationCommandReceipt>(NativeAnimationRecordKind::CommandReceipt);
			context.Expect(Native_Animation_CancelCommand(&cancellation, &cancelled)
				== NativeAnimationStatus::Cancelled,
				"The queued controller command must retain its cancellation identity.");
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The native controller command fixture must retire its animator generation.");
		});

		registry.Add("Animation.Gameplay.IdempotencyReturnsOriginalReceiptAndRejectsConflicts", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(Descriptor(3U));
			AnimationCommandQueue queue;
			const AnimationCommand original = Command(target, 7U, 1U, 77U, 700U, 7000U);
			const AnimationCommandReceipt accepted = queue.Enqueue(original, store);
			const AnimationCommandReceipt duplicate = queue.Enqueue(original, store);
			context.Expect(accepted.status == AnimationStatus::Queued
				&& duplicate.status == AnimationStatus::Queued && duplicate.duplicate,
				"An exact idempotent retry must reconcile to the original receipt.");
			AnimationCommand conflict = original;
			conflict.value = 0.75F;
			const AnimationCommandReceipt rejected = queue.Enqueue(conflict, store);
			context.Expect(rejected.status == AnimationStatus::Rejected && rejected.duplicate,
				"Reusing an idempotency key with a different payload must fail atomically.");
		});

		registry.Add("Animation.Gameplay.CommandBatchRejectsWithoutPartialQueueMutation", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(Descriptor(31U));
			AnimationCommandQueue queue;
			std::vector<AnimationCommand> commands{
				Command(target, 12U, 1U, 1201U, 12001U, 120001U),
				Command(target, 12U, 2U, 1202U, 12002U, 120002U)};
			commands[1].value = std::monostate{};

			const AnimationCommandBatchReceipt rejected = queue.EnqueueBatch(commands, store);
			context.Expect(rejected.status == AnimationStatus::InvalidArgument
				&& rejected.commands.size() == commands.size()
				&& std::all_of(rejected.commands.begin(), rejected.commands.end(), [](const AnimationCommandReceipt& receipt) {
					return receipt.status == AnimationStatus::InvalidArgument;
				}),
				"One invalid command must reject every receipt in the submitted batch.");
			context.ExpectEqual(static_cast<std::size_t>(0U), queue.PendingCount(),
				"Batch validation must finish before any command enters pending storage.");
			context.Expect(queue.ReadReceipt(12U, 1U, 1201U).status == AnimationStatus::NotReady,
				"A rejected batch must not retain a receipt for an otherwise-valid prefix.");
		});

		registry.Add("Animation.Gameplay.CommandBatchCapacityAndRetriesAreAtomic", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(Descriptor(32U));
			AnimationCommandQueue queue(2U);
			std::vector<AnimationCommand> commands{
				Command(target, 13U, 1U, 1301U, 13001U, 130001U),
				Command(target, 13U, 2U, 1302U, 13002U, 130002U)};

			const AnimationCommandBatchReceipt accepted = queue.EnqueueBatch(commands, store);
			const AnimationCommandBatchReceipt replayed = queue.EnqueueBatch(commands, store);
			context.Expect(accepted.status == AnimationStatus::Queued
				&& replayed.status == AnimationStatus::Ok
				&& replayed.commands.size() == commands.size()
				&& std::all_of(replayed.commands.begin(), replayed.commands.end(), [](const AnimationCommandReceipt& receipt) {
					return receipt.duplicate && receipt.status == AnimationStatus::Queued;
				}),
				"An exact retry must return copied original receipts without enqueuing twice.");
			context.ExpectEqual(static_cast<std::size_t>(2U), queue.PendingCount(),
				"Idempotent batch replay must preserve the original bounded queue count.");

			AnimationCommand conflicting = commands.front();
			conflicting.value = 0.75F;
			const std::vector<AnimationCommand> conflictBatch{conflicting, commands.back()};
			context.Expect(queue.EnqueueBatch(conflictBatch, store).status == AnimationStatus::Rejected,
				"Changing one command while reusing a batch identity must reject the whole retry.");
			context.ExpectEqual(static_cast<std::size_t>(2U), queue.PendingCount(),
				"A conflicting retry must not disturb the already accepted command batch.");

			AnimationCommand overflowFirst = Command(target, 14U, 1U, 1401U, 14001U, 140001U);
			AnimationCommand overflowSecond = Command(target, 14U, 2U, 1402U, 14002U, 140002U);
			const std::vector<AnimationCommand> overflowBatch{overflowFirst, overflowSecond};
			context.Expect(queue.EnqueueBatch(overflowBatch, store).status == AnimationStatus::Rejected,
				"A batch that cannot wholly fit the pending budget must be rejected.");
			context.Expect(queue.ReadReceipt(14U, 1U, 1401U).status == AnimationStatus::NotReady,
				"Capacity rejection must not retain receipts for an unqueued prefix.");
		});

		registry.Add("Animation.Gameplay.CancellationPreservesSubmissionAndCancelCorrelations", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(Descriptor(4U));
			AnimationCommandQueue queue;
			const AnimationCommand command = Command(target, 3U, 4U, 44U, 444U, 4444U);
			context.Expect(queue.Enqueue(command, store).status == AnimationStatus::Queued,
				"The cancellable fixture must enter the pending queue.");
			const AnimationCommandReceipt cancellation = queue.Cancel(command.cancellationToken, 999U);
			const AnimationCommandReceipt original = queue.ReadReceipt(
				command.producerId, command.producerSequence, command.correlationId);
			context.Expect(cancellation.status == AnimationStatus::Cancelled && cancellation.correlationId == 999U,
				"The cancellation call must return its own correlation identity.");
			context.Expect(original.status == AnimationStatus::Cancelled
				&& original.correlationId == command.correlationId,
				"Polling the original request must retain its immutable submission correlation.");
			context.ExpectEqual(static_cast<std::size_t>(0U), queue.PendingCount(),
				"A successfully cancelled command must never reach protected capture.");
		});

		registry.Add("Animation.Gameplay.ReceiptsBecomeFinalAtExplicitPublication", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(Descriptor(5U));
			AnimationCommandQueue queue;
			const AnimationCommand command = Command(target, 8U, 9U, 89U, 890U, 8900U);
			queue.Enqueue(command, store);
			std::vector<AnimationCommand> drained;
			queue.DrainThrough(command.targetTick, drained, store);
			context.Expect(queue.ReadReceipt(8U, 9U, 89U).status == AnimationStatus::Queued,
				"Capture alone must not claim that a gameplay write is observable.");
			context.Expect(queue.Cancel(command.cancellationToken, 999U).status == AnimationStatus::NotReady,
				"Cancellation after protected capture must report that it is too late.");
			queue.PublishCompletion(command, AnimationStatus::Ok, command.targetTick, 12U);
			const AnimationCommandReceipt completed = queue.ReadReceipt(8U, 9U, 89U);
			context.Expect(completed.status == AnimationStatus::Ok
				&& completed.observableTick == command.targetTick
				&& completed.publicationGeneration == 12U,
				"Only protected publication may attach the final tick and authority generation.");
		});

		registry.Add("Animation.Gameplay.CapabilitiesAreExplicitAndGenerationChecked", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(Descriptor(6U));
			NativeAnimationCapabilityQuery query = AbiRecord<NativeAnimationCapabilityQuery>(
				NativeAnimationRecordKind::CapabilityQuery);
			query.animator = {handle.slotIndex, handle.generation};
			query.capability = NativeAnimationCapability::ParameterWrite;
			NativeAnimationCapabilityResult result = AbiRecord<NativeAnimationCapabilityResult>(
				NativeAnimationRecordKind::CapabilityResult);
			context.Expect(Native_Animation_QueryCapability(&query, &result) == NativeAnimationStatus::Ok
				&& result.supported == 1U,
				"Implemented parameter writes must advertise positive capability.");
			query.capability = NativeAnimationCapability::SocketRead;
			result = AbiRecord<NativeAnimationCapabilityResult>(NativeAnimationRecordKind::CapabilityResult);
			context.Expect(Native_Animation_QueryCapability(&query, &result) == NativeAnimationStatus::Ok
				&& result.supported == 1U,
				"Socket reads are supported even when the current published socket set is empty.");
			query.capability = NativeAnimationCapability::IkGoal;
			result = AbiRecord<NativeAnimationCapabilityResult>(NativeAnimationRecordKind::CapabilityResult);
			context.Expect(Native_Animation_QueryCapability(&query, &result) == NativeAnimationStatus::Ok
				&& result.supported == 1U,
				"The bounded copied procedural-goal owner must advertise the implemented IK capability.");
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The capability fixture should retire its runtime instance.");
			result = AbiRecord<NativeAnimationCapabilityResult>(NativeAnimationRecordKind::CapabilityResult);
			context.Expect(Native_Animation_QueryCapability(&query, &result) == NativeAnimationStatus::StaleHandle,
				"A reused or retired animator generation must never inherit capabilities.");
		});

		registry.Add("Animation.Gameplay.ConsumerClosureReportsAncestorsAndReasons", [](TestContext& context) {
			AnimationConsumerRegistry& consumers = AnimationConsumerRegistry::Instance();
			consumers.Clear();
			const AnimatorInstanceHandle animator{3U, 7U};
			AnimationConsumerDeclaration declaration;
			declaration.consumerId = 42U;
			declaration.kind = AnimationConsumerKind::Attachment;
			declaration.animator = animator;
			declaration.declarationGeneration = 1U;
			declaration.declarationTick = 5;
			declaration.maximumSnapshotAgeTicks = 1;
			declaration.joints = {3U};
			declaration.curves = {9U};
			declaration.events = {11U};
			declaration.rootMotion = true;
			context.Expect(consumers.PublishDeclarations({&declaration, 1U}, 1U, 5) == AnimationStatus::Ok,
				"A complete declaration batch must publish atomically before frame capture.");
			const std::vector<std::int32_t> parents{-1, 0, 1, 1};
			const AnimationResolvedConsumerRequirements resolved = consumers.Resolve(animator, parents, 6);
			context.Expect(resolved.status == AnimationStatus::Ok
				&& resolved.closure.joints == std::vector<std::uint32_t>({0U, 1U, 3U}),
				"Hybrid authority must include every requested joint ancestor.");
			context.Expect(resolved.closure.attachments && resolved.closure.rootMotion
				&& resolved.curves == std::vector<std::uint64_t>({9U})
				&& resolved.events == std::vector<std::uint64_t>({11U}),
				"Curve, event, root-motion, and attachment requirements must remain attributed.");
			context.Expect(resolved.attribution.size() == 1U
				&& resolved.attribution.front().consumerId == 42U
				&& resolved.attribution.front().closedJointCount == 3U,
				"Diagnostics must identify which consumer expanded the authoritative closure.");
			consumers.Clear();
		});

		registry.Add("Animation.Gameplay.AdvancedStoreDefersAndCopiesAtomically", [](TestContext& context) {
			AnimatorInstanceStore instances;
			const AnimatorInstanceHandle handle = instances.Create(Descriptor(40U));
			AdvancedAnimationCommandStore store(1U, 2U, 1U);
			NativeAnimationRetargetRequest request =
				AbiRecord<NativeAnimationRetargetRequest>(NativeAnimationRecordKind::RetargetRequest);
			request.animator = {handle.slotIndex, handle.generation};
			request.operation = NativeAnimationRetargetOperation::DeactivateProfile;
			request.correlationId = 4001U;
			request.targetTick = 2;
			context.Expect(store.Submit(request, instances).status == NativeAnimationStatus::Queued,
				"A valid copied advanced request must enter bounded pending storage.");
			NativeAnimationRetargetRequest overflow = request;
			overflow.correlationId = 4002U;
			context.Expect(store.Submit(overflow, instances).status
				== NativeAnimationStatus::HardLimitExceeded,
				"A full advanced queue must reject a later request without disturbing the first.");

			std::vector<NativeAnimationRetargetRequest> retarget;
			std::vector<NativeAnimationMotionMatchRequest> motion;
			// The V2 query stream is drained independently so its larger copied payload
			// cannot change the frozen V1 request stride or ordering contract.
			std::vector<NativeAnimationMotionMatchQueryRequestV2> motionV2;
			std::vector<NativeAnimationRagdollRequest> ragdoll;
			std::vector<NativeAnimationDeterminismRequest> determinism;
			store.DrainThrough(1, retarget, motion, motionV2, ragdoll, determinism);
			context.Expect(retarget.empty() && store.PendingCount() == 1U,
				"A future-tick request must stay invisible at an earlier capture boundary.");
			store.DrainThrough(2, retarget, motion, motionV2, ragdoll, determinism);
			context.Expect(retarget.size() == 1U && store.PendingCount() == 0U,
				"The due request must transfer as one deterministic copied batch.");

			NativeAnimationRetargetSnapshot published =
				AbiRecord<NativeAnimationRetargetSnapshot>(NativeAnimationRecordKind::RetargetSnapshot);
			published.status = NativeAnimationStatus::Ok;
			published.animator = request.animator;
			published.observationTick = 2;
			published.publicationGeneration = 9U;
			context.Expect(store.Publish(published) == AnimationStatus::Ok,
				"Protected publication must accept one complete copied observation.");
			std::uint32_t requiredCount = 0U;
			context.Expect(store.Copy(request.animator,
				std::span<NativeAnimationRetargetSnapshot>{}, requiredCount)
				== AnimationStatus::BufferTooSmall && requiredCount == 1U,
				"The first count query must report the exact required size without a prefix write.");
			std::array<NativeAnimationRetargetSnapshot, 1U> copied{};
			context.Expect(store.Copy(request.animator, copied, requiredCount) == AnimationStatus::Ok
				&& copied.front().publicationGeneration == 9U,
				"A sufficient destination must receive the complete retained publication.");
			store.RetireAnimator(request.animator);
			context.Expect(store.Copy(request.animator, copied, requiredCount) == AnimationStatus::NotReady,
				"Retiring an animator generation must remove every retained advanced observation.");
		});

		registry.Add("Animation.Gameplay.RagdollLifecycleBatchHasNoAcceptedPrefix", [](TestContext& context) {
			RagdollPhysicsBridge bridge;
			RagdollGenerationStamp generation{1U, 2U, 3U, 4U, 5};
			PhysicsPoseSnapshot pose;
			pose.generation = generation;
			pose.completeCurrentStep = true;
			pose.bodies.push_back({11U, 21U, {}, {}, {}, false, 1U});
			RagdollPhysicsRequest create;
			create.kind = RagdollPhysicsRequestKind::Create;
			create.ragdollId = 77U;
			create.generation = generation;
			create.activationPose = pose;
			create.correlationId = 7001U;
			create.producerOrder = 1U;
			RagdollPhysicsRequest activate = create;
			activate.kind = RagdollPhysicsRequestKind::Activate;
			activate.correlationId = 7002U;
			activate.producerOrder = 2U;
			activate.generation.fixedStep = 0;
			context.Expect(bridge.EnqueueBatch(std::array{create, activate})
				== AnimationStatus::InvalidArgument && bridge.PendingCount() == 0U,
				"One invalid lifecycle step must reject create and activate before either becomes visible.");
			activate.generation = generation;
			activate.activationPose = pose;
			context.Expect(bridge.EnqueueBatch(std::array{create, activate})
				== AnimationStatus::Queued && bridge.PendingCount() == 2U,
				"A valid create-plus-activate pair must enter the physics bridge atomically.");
			std::vector<RagdollPhysicsRequest> drained;
			context.Expect(bridge.Drain(drained) == AnimationStatus::Ok
				&& drained.size() == 2U && drained.front().producerOrder == 1U
				&& drained.back().producerOrder == 2U,
				"The protected physics boundary must receive the complete lifecycle pair in producer order.");
		});

		registry.Add("Animation.Gameplay.NativeDeterminismCapturePublishesCopiedChecksums", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(Descriptor(41U));
			NativeAnimationDeterminismRequest request =
				AbiRecord<NativeAnimationDeterminismRequest>(NativeAnimationRecordKind::DeterminismRequest);
			request.animator = {handle.slotIndex, handle.generation};
			request.operation = NativeAnimationDeterminismOperation::Capture;
			request.correlationId = 4101U;
			NativeAnimationDeterminismReceipt receipt =
				AbiRecord<NativeAnimationDeterminismReceipt>(NativeAnimationRecordKind::DeterminismReceipt);
			context.Expect(Native_Animation_SubmitDeterminismRequest(&request, &receipt)
				== NativeAnimationStatus::Queued,
				"The native deterministic API must queue real deferred work instead of returning Unsupported.");
			context.Expect(ExecuteGameplayAnimationFrame(system),
				"The protected animation frame must publish the queued deterministic capture.");
			NativeAnimationBufferInfo info =
				AbiRecord<NativeAnimationBufferInfo>(NativeAnimationRecordKind::BufferInfo);
			context.Expect(Native_Animation_CopyDeterminismSnapshots(
				&request.animator, nullptr, 0U, &info) == NativeAnimationStatus::BufferTooSmall
				&& info.requiredCount == 1U && info.writtenCount == 0U,
				"The copied deterministic API must implement the atomic two-call count protocol.");
			std::array<NativeAnimationDeterminismSnapshot, 1U> snapshots{};
			info = AbiRecord<NativeAnimationBufferInfo>(NativeAnimationRecordKind::BufferInfo);
			context.Expect(Native_Animation_CopyDeterminismSnapshots(
				&request.animator, snapshots.data(), snapshots.size(), &info)
					== NativeAnimationStatus::Ok
				&& info.writtenCount == 1U && snapshots.front().stateChecksum != 0U
				&& snapshots.front().poseChecksum != 0U,
				"A completed capture must expose finite copied state and pose checksums.");
			context.Expect(system.InvalidateAnimatorInstance(handle),
				"The deterministic bridge fixture must retire its generation-safe animator.");
		});

		registry.Add("Animation.Gameplay.ShutdownRetiresCopiedSocketPublications", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			AnimationAttachmentSnapshotPublisher& publisher = AnimationAttachmentSnapshotPublisher::Instance();
			publisher.Clear();
			AnimationBoneSocketSnapshot snapshot;
			snapshot.animator = {91U, 4U};
			// Publications are generation-qualified immutable data. Give this
			// lifecycle fixture the same minimum valid identity that the protected
			// runtime publisher supplies before testing Shutdown's cleanup.
			snapshot.poseGeneration = 1U;
			snapshot.skeleton.generation = 1U;
			snapshot.sockets.push_back({501U, {}});
			context.Expect(publisher.Publish(std::move(snapshot))
				&& publisher.PublishedSocketCount({91U, 4U}) == 1U,
				"The lifecycle fixture must begin with one copied socket publication.");
			system.Shutdown();
			context.Expect(publisher.PublishedSocketCount({91U, 4U}) == 0U,
				"Shutdown must retire copied socket publications from the previous runtime session.");
			// Restore the process-wide singleton so later tests do not inherit a
			// deliberately dormant animation runtime.
			system.Initialize();
		});
	}
}
