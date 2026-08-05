// AnimationMutationContractTests.cpp - Typed animation runtime and mutation contracts.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationCommandQueue.h"
#include "Components/AnimatorComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"

#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimatorInstanceDescriptor MakeDescriptor(std::uint8_t seed) {
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0] = seed;
			descriptor.componentGeneration = 1U;
			return descriptor;
		}

		AnimationCommand MakeCommand(AnimatorInstanceHandle target, std::uint32_t producer, std::uint64_t sequence, std::uint64_t correlation) {
			AnimationCommand command;
			command.target = target;
			command.kind = AnimationCommandKind::SetParameter;
			command.parameterType = AnimationParameterType::Float;
			command.parameterId = 7U;
			command.value = 0.5F;
			command.targetTick = 12;
			command.producerId = producer;
			command.producerSequence = sequence;
			command.correlationId = correlation;
			return command;
		}
	}

	void RegisterAnimationMutationContractTests(TestRegistry& registry) {
		registry.Add("Animation.Mutation.GenerationSafeStoreSnapshotsAndDormancy", [](TestContext& context) {
			AnimatorInstanceStore store;
			const std::uint64_t initialMembershipRevision = store.MembershipRevision();
			std::vector<AnimationFrameInput> frames;
			store.CaptureFrameInputs(4, frames);
			context.Expect(frames.empty() && store.ActiveCount() == 0U,
				"A dormant store must have zero membership and no frame work to publish.");
			context.ExpectEqual(initialMembershipRevision, store.MembershipRevision(),
				"Read-only dormant snapshots must not advance the membership proof.");

			const AnimatorInstanceHandle first = store.Create(MakeDescriptor(1U));
			const std::uint64_t createdMembershipRevision = store.MembershipRevision();
			context.Expect(createdMembershipRevision > initialMembershipRevision,
				"Creating a live slot must advance the lock-free membership proof.");
			context.Expect(store.IsAlive(first), "A created instance should be alive for typed commands.");
			store.CaptureFrameInputs(5, frames);
			context.Expect(frames.size() == 1U && frames.front().handle == first && frames.front().tick == 5,
				"Workers must receive a copied immutable frame input, not live store state.");
			context.ExpectEqual(createdMembershipRevision, store.MembershipRevision(),
				"Liveness checks and frame capture must preserve an unchanged membership revision.");
			context.Expect(store.Invalidate(first) && !store.IsAlive(first), "Lifecycle invalidation must retire the exact generation.");
			context.Expect(store.MembershipRevision() > createdMembershipRevision,
				"Invalidating a live slot must invalidate every retained membership proof.");
			const AnimatorInstanceHandle replacement = store.Create(MakeDescriptor(2U));
			context.Expect(first.slotIndex == replacement.slotIndex && first.generation != replacement.generation,
				"Reused slots must advance generation so stale commands cannot target replacements.");
			context.Expect(!store.SetGenerationForTesting(replacement, first.generation).IsValid()
				&& store.IsAlive(replacement),
				"Even the test hook must reject a backward generation change that could revive a stale handle.");
			const AnimatorInstanceHandle exhausted = store.SetGenerationForTesting(replacement, UINT32_MAX);
			context.Expect(store.Invalidate(exhausted), "The test hook should retire the final usable generation.");
			const AnimatorInstanceHandle afterExhaustion = store.Create(MakeDescriptor(4U));
			context.Expect(afterExhaustion.slotIndex != exhausted.slotIndex && store.ActiveCount() == 1U,
				"An exhausted slot must retire permanently instead of wrapping and aliasing an ancient handle.");
		});

		registry.Add("Animation.Mutation.BulkInvalidationIsIdempotentAndRetiresExhaustedSlots", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle reusable = store.Create(MakeDescriptor(11U));
			const AnimatorInstanceHandle finalGenerationSource = store.Create(MakeDescriptor(12U));
			const AnimatorInstanceHandle exhausted =
				store.SetGenerationForTesting(finalGenerationSource, UINT32_MAX);
			context.Expect(reusable.IsValid() && exhausted.IsValid(),
				"The bulk-invalidation fixture requires one reusable and one exhausted live slot.");

			store.InvalidateAll();
			store.InvalidateAll();
			context.ExpectEqual(static_cast<std::size_t>(0U), store.ActiveCount(),
				"Repeated bulk invalidation must remain a no-op after every live slot is retired.");
			context.Expect(!store.IsAlive(reusable) && !store.IsAlive(exhausted),
				"Every pre-invalidation handle must remain dead after repeated lifecycle cleanup.");

			const AnimatorInstanceHandle replacement = store.Create(MakeDescriptor(13U));
			const AnimatorInstanceHandle firstFresh = store.Create(MakeDescriptor(14U));
			const AnimatorInstanceHandle secondFresh = store.Create(MakeDescriptor(15U));
			context.Expect(replacement.slotIndex == reusable.slotIndex
				&& replacement.generation != reusable.generation,
				"A normal invalidated slot must re-enter the usual generation-increment reuse path.");
			context.Expect(replacement.slotIndex != exhausted.slotIndex
				&& firstFresh.slotIndex != exhausted.slotIndex
				&& secondFresh.slotIndex != exhausted.slotIndex,
				"A slot invalidated at UINT32_MAX must retire permanently during bulk cleanup.");
			context.Expect(replacement.slotIndex != firstFresh.slotIndex
				&& replacement.slotIndex != secondFresh.slotIndex
				&& firstFresh.slotIndex != secondFresh.slotIndex
				&& store.ActiveCount() == 3U,
				"Repeated bulk cleanup must not duplicate a free-list entry or alias two live instances.");
		});

		registry.Add("Animation.Mutation.CommandsAreTypedBoundedAndDeterministicallyOrdered", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle target = store.Create(MakeDescriptor(3U));
			AnimationCommandQueue queue(2U);
			const AnimationCommand queuedCommand = MakeCommand(target, 9U, 2U, 2U);
			const AnimationCommandReceipt queuedReceipt = queue.Enqueue(queuedCommand, store);
			context.Expect(queuedReceipt.status == AnimationStatus::Queued
				&& queuedReceipt.target == target && queuedReceipt.producerSequence == 2U
				&& queuedReceipt.correlationId == 2U && queuedReceipt.targetTick == 12,
				"The bounded queue should accept a valid copied command.");
			context.Expect(queue.Enqueue(MakeCommand(target, 4U, 1U, 1U), store).status == AnimationStatus::Queued,
				"Commands from another producer should remain independently ordered.");
			std::vector<AnimationCommand> commands;
			queue.DrainThrough(12, commands, store);
			context.Expect(commands.size() == 2U && commands[0].producerId == 4U && commands[1].producerId == 9U,
				"Drain order must be stable by tick, producer identity, and producer sequence.");
			context.Expect(queue.Enqueue(MakeCommand(target, 1U, 1U, 3U), store).status == AnimationStatus::Queued,
				"A valid later command should fit after drain frees capacity.");
			context.Expect(queue.Enqueue(MakeCommand(target, 1U, 1U, 4U), store).status == AnimationStatus::Rejected,
				"Duplicate producer sequences must fail closed rather than permit ambiguous order.");
			AnimationEventQueue events(2U);
			context.Expect(events.Enqueue({target, 12, 1U, 42U}, store) == AnimationStatus::Queued,
				"Authoritative event packets should use a typed bounded queue.");
			context.Expect(events.Enqueue({target, 12, 1U, 99U}, store) == AnimationStatus::Rejected,
				"Duplicate source/tick/sequence identities must be rejected before concurrent enqueue order can affect delivery.");
			std::vector<AnimationEventPacket> publishedEvents;
			store.Invalidate(target);
			queue.DrainThrough(12, commands, store);
			events.DrainThrough(12, publishedEvents, store);
			context.Expect(commands.empty() && publishedEvents.empty(),
				"Drain-time generation checks must discard commands and outputs invalidated after enqueue.");
			const AnimatorInstanceHandle futureTarget = store.Create(MakeDescriptor(8U));
			AnimationCommand futureCommand = MakeCommand(futureTarget, 7U, 1U, 7U);
			futureCommand.targetTick = 100;
			context.Expect(queue.Enqueue(futureCommand, store).status == AnimationStatus::Queued,
				"A valid future command should enter the bounded queue before its target tick.");
			store.Invalidate(futureTarget);
			queue.DrainThrough(12, commands, store);
			context.Expect(queue.PendingCount() == 0U,
				"A stale future command must be discarded during early drain, not retained for slot reuse.");
			const AnimationCommand staleCommand = MakeCommand(target, 2U, 1U, 5U);
			const AnimationCommandReceipt staleReceipt = queue.Enqueue(staleCommand, store);
			context.Expect(staleReceipt.status == AnimationStatus::StaleHandle
				&& staleReceipt.target == target && staleReceipt.producerSequence == 1U
				&& staleReceipt.correlationId == 5U && staleReceipt.targetTick == 12,
				"Commands against invalidated generations must never enter the queue.");
		});

		registry.Add("Animation.Mutation.TypedPublicationsRemainInvisibleUntilProtectedFlush", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Animation Target");
			objects.AddComponent<AnimatorComponent>(object);
			const TerraEngine::UUID objectId = objects.getUUID(object);
			const std::uint64_t componentGeneration = objects.getObjectComponentGeneration(object);
			TerraEngine::Scene::TransactionalMutationBuilder builder("animation-visible", TerraEngine::Scene::TransactionOrigin::Test, "animation-test");
			builder.PublishAnimationMutations(objects, {{objectId, componentGeneration, 3U, 1U, 8, 1U, 1U, 99U}});
			TerraEngine::Scene::SceneMutationQueue queue;
			queue.EnqueueTransaction(builder.Build());
			context.Expect(queue.PendingTransactions().size() == 1U,
				"Typed animation publication must remain queued before its protected flush.");
			const auto result = queue.Apply(objects, "PublishAnimationAuthority");
			context.Expect(result.acceptedAnimationPublications.size() == 1U,
				"The declared protected boundary should expose exactly the committed typed receipt.");
			const auto diagnostics = queue.GetTransactionDiagnostics();
			context.Expect(!diagnostics.recentResults.empty() && diagnostics.recentResults.back().acceptedAnimationPublications.empty(),
				"Bounded diagnostic history must strip large immediate animation receipt payloads.");
			objects.clear();
		});

		registry.Add("Animation.Mutation.ConflictsRejectTheWholeBatchWithoutPartialPublication", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Atomic Animation Target");
			objects.AddComponent<AnimatorComponent>(object);
			const TerraEngine::UUID objectId = objects.getUUID(object);
			TerraEngine::Scene::TransactionalMutationBuilder builder("animation-conflict", TerraEngine::Scene::TransactionOrigin::Test, "animation-test");
			builder.SetName(objectId, "Must Not Publish")
				.PublishAnimationMutations(objects, {{objectId, objects.getObjectComponentGeneration(object) + 1U, 3U, 1U, 8, 1U, 1U, 99U}});
			TerraEngine::Scene::SceneMutationQueue queue;
			queue.EnqueueTransaction(builder.Build());
			const auto result = queue.Apply(objects, "PublishAnimationAuthority");
			context.Expect(result.acceptedAnimationPublications.empty(),
				"A generation conflict must not expose any portion of the animation batch.");
			context.Expect(objects.getObjectName(object) == "Atomic Animation Target",
				"A rejected later animation publication must roll back earlier scene writes.");
			objects.clear();
		});

		registry.Add("Animation.Mutation.BareObjectsRejectAnimationPublicationAtomically", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Bare Animation Target");
			const TerraEngine::UUID objectId = objects.getUUID(object);
			TerraEngine::Scene::TransactionalMutationBuilder builder("animation-bare", TerraEngine::Scene::TransactionOrigin::Test, "animation-test");
			builder.SetName(objectId, "Must Not Publish")
				.PublishAnimationMutations(objects, {{objectId, objects.getObjectComponentGeneration(object), 3U, 1U, 8, 1U, 1U, 99U}});
			TerraEngine::Scene::SceneMutationQueue queue;
			queue.EnqueueTransaction(builder.Build());
			const auto result = queue.Apply(objects, "PublishAnimationAuthority");
			const TerraEngine::Scene::TransactionResult* receipt = result.transactionResults.empty()
				? nullptr
				: &result.transactionResults.front();
			context.Expect(receipt != nullptr
				&& receipt->status == TerraEngine::Scene::TransactionStatus::Conflicted
				&& !receipt->conflicts.empty()
				&& receipt->conflicts.back().reason == TerraEngine::Scene::TransactionConflictReason::MissingTarget
				&& result.acceptedAnimationPublications.empty()
				&& objects.getObjectName(object) == "Bare Animation Target",
				"A bare object must report the missing AnimatorComponent and reject typed publication without exposing earlier writes.");
			objects.clear();
		});

		registry.Add("Animation.Mutation.PublicationRecordBudgetsBoundEveryTypedEntry", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Budgeted Animation Target");
			objects.AddComponent<AnimatorComponent>(object);
			const TerraEngine::UUID objectId = objects.getUUID(object);
			const std::uint64_t componentGeneration = objects.getObjectComponentGeneration(object);
			TerraEngine::Scene::TransactionalMutationBuilder builder("animation-budget", TerraEngine::Scene::TransactionOrigin::Test, "animation-test");
			builder.SetBudget({1U, 128U}).PublishAnimationMutations(objects, {
				{objectId, componentGeneration, 1U, 1U, 1, 1U, 1U, 1U},
				{objectId, componentGeneration, 2U, 1U, 1, 2U, 1U, 2U}
			});
			TerraEngine::Scene::SceneMutationQueue queue;
			queue.EnqueueTransaction(builder.Build());
			const auto result = queue.Apply(objects, "PublishAnimationAuthority");
			context.Expect(result.acceptedAnimationPublications.empty() && result.rejectedMutationCount == 2U,
				"Existing transaction budgets must reject every oversized typed publication record.");
			objects.clear();
		});
	}
}
