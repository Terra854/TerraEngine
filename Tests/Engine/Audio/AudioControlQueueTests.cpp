// AudioControlQueueTests.cpp - deterministic producer and mix-control contracts.

#include "../TestHarness.h"

#include "Audio/AudioCommands.h"
#include "Audio/AudioMixConfiguration.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		Audio::AudioControlBatch MakeBatch(
			std::uint64_t producer,
			Audio::AudioFrameIndex frame,
			std::size_t commandCount,
			std::string idempotencyKey = {}) {
			Audio::AudioControlBatch batch;
			batch.producerId = producer;
			batch.requestedFrame = frame;
			batch.correlationId = "audio-control-test";
			batch.idempotencyKey = std::move(idempotencyKey);
			batch.requestFingerprint = "play-default-source";
			batch.commands.resize(commandCount);
			for(Audio::AudioControlCommand& command : batch.commands)
				command.kind = Audio::AudioControlKind::Play;
			return batch;
		}
	}

	void RegisterAudioControlQueueTests(TestRegistry& registry) {
		registry.Add("Audio.ControlQueue.AdmissionIsAtomicAndBoundedPerProducer", [](TestContext& context) {
			Audio::AudioControlQueueLimits limits;
			limits.maximumQueuedCommands = 4U;
			limits.maximumProducerCommands = 3U;
			limits.maximumCommandsPerBatch = 3U;
			Audio::AudioControlQueue queue(limits);

			const auto accepted = queue.Submit(MakeBatch(11U, 2U, 3U));
			const auto rejected = queue.Submit(MakeBatch(11U, 2U, 1U));
			context.Expect(accepted.status == Audio::AudioControlStatus::Queued,
				"A complete batch within both bounds should queue.");
			context.Expect(rejected.rejection == Audio::AudioControlRejection::ProducerCapacity,
				"A producer must not consume more than its configured share.");
			context.ExpectEqual(static_cast<std::size_t>(3U), queue.QueuedCommandCount(),
				"Rejected batch admission must not partially consume capacity.");
		});

		registry.Add("Audio.ControlQueue.DrainsByFrameThenAssignedSequence", [](TestContext& context) {
			Audio::AudioControlQueue queue;
			const auto late = queue.Submit(MakeBatch(1U, 8U, 1U));
			const auto early = queue.Submit(MakeBatch(2U, 3U, 1U));
			const auto sameFrame = queue.Submit(MakeBatch(3U, 3U, 1U));
			const auto batches = queue.DrainEligible(3U);

			context.ExpectEqual(static_cast<std::size_t>(2U), batches.size(),
				"Only batches visible at the requested snapshot boundary should drain.");
			context.Expect(batches[0].batchSequence == early.batchSequence
				&& batches[1].batchSequence == sameFrame.batchSequence,
				"Equal-frame batches must preserve engine-assigned total order.");
			context.Expect(late.batchSequence < early.batchSequence,
				"Sequence assignment describes admission and must not replace frame ordering.");
		});

		registry.Add("Audio.ControlQueue.IdempotencyRejectsConflictingReuse", [](TestContext& context) {
			Audio::AudioControlQueue queue;
			const auto original = queue.Submit(MakeBatch(1U, 1U, 1U, "play-once"));
			auto duplicateBatch = MakeBatch(2U, 1U, 1U, "play-once");
			const auto duplicate = queue.Submit(std::move(duplicateBatch));
			auto conflictBatch = MakeBatch(2U, 1U, 1U, "play-once");
			conflictBatch.requestFingerprint = "stop-source";
			const auto conflict = queue.Submit(std::move(conflictBatch));

			context.Expect(duplicate.status == Audio::AudioControlStatus::Accepted
				&& duplicate.batchSequence == original.batchSequence,
				"A byte-equivalent retry should correlate to the accepted batch without replay.");
			context.Expect(conflict.rejection == Audio::AudioControlRejection::IdempotencyConflict,
				"Reusing an idempotency key for a different request must be rejected.");
			context.ExpectEqual(static_cast<std::size_t>(1U), queue.QueuedCommandCount(),
				"Neither retry path may enqueue a second command.");
		});

		registry.Add("Audio.MixConfiguration.ValidatesAndPublishesGenerations", [](TestContext& context) {
			Audio::AudioMixConfiguration mix;
			std::string diagnostic;
			const auto original = mix.Snapshot();
			context.ExpectEqual(static_cast<std::size_t>(1U), original.groups.size(),
				"Every project starts with one provider-neutral Default group.");
			context.Expect(mix.SetMasterVolume(2.0F, diagnostic),
				"Finite authored volumes should be accepted and normalized.");
			context.Expect(!mix.SetMasterVolume(std::nanf(""), diagnostic),
				"Non-finite mix values must be rejected before publication.");
			const auto published = mix.Snapshot();
			context.Expect(published.master.volume == 1.0F,
				"Master volume must stay inside the portable zero-to-one range.");
			context.Expect(published.generation == original.generation + 1U,
				"Only the successful mutation should advance the immutable snapshot generation.");
			const auto defaultGroupId = published.groups.front().id;
			context.Expect(mix.RenameGroup(defaultGroupId, "Gameplay", diagnostic)
				&& mix.Snapshot().groups.front().name == "Gameplay",
				"Mixer authoring must publish validated group renames through the immutable snapshot path.");
		});
	}
}
