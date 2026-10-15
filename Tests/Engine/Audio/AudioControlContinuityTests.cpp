// AudioControlContinuityTests.cpp - retained requests and real task producers.

#include "../TestHarness.h"
#include "Audio/AudioCommands.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <fstream>
#include <future>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		Audio::AudioControlBatch Request(std::string key, std::uint64_t producer = 1U, Audio::AudioFrameIndex frame = 0U) {
			Audio::AudioControlBatch batch;
			batch.producerId = producer;
			batch.requestedFrame = frame;
			batch.idempotencyKey = std::move(key);
			batch.requestFingerprint = "same-operation";
			batch.commands.resize(1U);
			return batch;
		}
		void ApplyAll(Audio::AudioControlQueue& queue, Audio::AudioFrameIndex frame = 0U) {
			for(const auto& batch : queue.DrainEligible(frame))
				queue.Complete(batch.completionToken, Audio::AudioControlStatus::Applied);
		}
		Audio::AudioControlQueueLimits SmallLimits(std::size_t history = 2U) {
			Audio::AudioControlQueueLimits limits;
			limits.maximumRetainedIdempotencyKeys = history;
			return limits;
		}
		void RaisePeak(std::atomic<std::size_t>& peak, std::size_t value) {
			auto previous = peak.load();
			while(previous < value && !peak.compare_exchange_weak(previous, value)) {}
		}
		struct ProducerEvidence {
			std::size_t producers, requests, applied, pendingPeak, completedPeak, evictions, duplicateApplications, terminalOutcomes;
		};
		std::map<std::size_t, ProducerEvidence> producerEvidence;
		void WriteEvidence(TestContext& context, const ProducerEvidence& run) {
			producerEvidence[run.producers] = run;
			const char* destination = std::getenv("TERRAENGINE_AUDIO_CONTROL_EVIDENCE");
			if(destination == nullptr || *destination == '\0') return;
			std::ofstream output(destination, std::ios::trunc);
			output << "{\"passed\":" << (producerEvidence.size() == 3U ? "true" : "false") << ",\"runs\":[";
			bool comma = false;
			for(const auto& [count, value] : producerEvidence) {
				if(comma) output << ',';
				comma = true;
				output << "{\"producers\":" << count << ",\"requests\":" << value.requests
					<< ",\"applied\":" << value.applied << ",\"pendingPeak\":" << value.pendingPeak
					<< ",\"completedPeak\":" << value.completedPeak << ",\"evictions\":" << value.evictions
					<< ",\"duplicateApplications\":" << value.duplicateApplications
					<< ",\"terminalOutcomes\":" << value.terminalOutcomes << '}';
			}
			output << "]}";
			output.flush();
			context.Expect(output.good(), "Requested producer evidence must be written completely.");
		}
		void RunProducerMatrix(TestContext& context, std::size_t producerCount) {
			constexpr std::size_t requestCount = 20000U;
			Audio::AudioControlQueue queue;
			auto pool = ThreadPool::CreateOwned(producerCount);
			std::barrier start(static_cast<std::ptrdiff_t>(producerCount));
			std::atomic<std::size_t> active{0U}, peakActive{0U}, applied{0U}, duplicates{0U}, settled{0U};
			std::atomic<std::size_t> pendingPeak{0U}, completedPeak{0U};
			std::vector<std::atomic<unsigned>> applications(requestCount);
			for(auto& count : applications) count.store(0U);
			std::vector<std::future<void>> futures;
			// A barrier requires all producers to occupy distinct pool workers at
			// once; the claimed producer count is observed, not merely configured.
			for(std::size_t producer = 0U; producer < producerCount; ++producer) {
				auto task = pool->createTask([&, producer]() {
					RaisePeak(peakActive, ++active);
					start.arrive_and_wait();
					for(std::size_t index = producer; index < requestCount; index += producerCount) {
						auto request = Request("matrix-" + std::to_string(index), producer + 1U);
						request.commands.front().scalarValue = static_cast<float>(index);
						const auto receipt = queue.Submit(request);
						context.Expect(receipt.status == Audio::AudioControlStatus::Queued, "Every unique task-produced request should queue.");
						if(index == producer) {
							request.producerId += 100U;
							request.correlationId = "transport-only-retry";
							const auto retry = queue.Submit(request);
							context.Expect(retry.batchSequence == receipt.batchSequence && retry.status != Audio::AudioControlStatus::Queued,
								"Retained retries from another transport must not enqueue twice.");
							request.commands.front().scalarValue += 0.5F;
							context.Expect(queue.Submit(request).rejection == Audio::AudioControlRejection::IdempotencyConflict,
								"Concurrent producer retries must still reject changed typed payloads.");
							// Keep these first owners pending until every producer has
							// checked its retry, avoiding any assumption about retention
							// after an arbitrarily long operating-system deschedule.
							start.arrive_and_wait();
						}
						const auto sampled = queue.Statistics();
						RaisePeak(pendingPeak, sampled.pendingCommands);
						RaisePeak(completedPeak, sampled.completedRecords);
						context.Expect(sampled.pendingCommands <= queue.Capacity() && sampled.completedRecords <= 4096U,
							"Concurrent pending ownership and terminal history must remain bounded.");
						for(const auto& batch : queue.DrainEligible(0U)) {
							const auto commandId = static_cast<std::size_t>(batch.commands.front().scalarValue);
							if(applications.at(commandId).fetch_add(1U) != 0U) ++duplicates;
							++applied;
							context.Expect(queue.Complete(batch.completionToken, Audio::AudioControlStatus::Applied),
								"Every drained request must have exactly one terminal acknowledgement.");
							++settled;
						}
					}
					--active;
				});
				futures.push_back(task->getFuture());
				pool->enqueue(task, requestCount / producerCount);
			}
			for(auto& future : futures) future.get();
			pool->shutdown();
			const auto final = queue.Statistics();
			RaisePeak(completedPeak, final.completedRecords);
			context.ExpectEqual(producerCount, peakActive.load(), "All requested producers must actually run concurrently.");
			context.ExpectEqual(requestCount, applied.load(), "All 20000 unique requests must be applied once.");
			context.ExpectEqual(requestCount, settled.load(), "All applied requests must reach terminal ownership.");
			context.ExpectEqual(std::size_t{0U}, duplicates.load(), "No retained retry may cause duplicate application.");
			context.ExpectEqual(std::size_t{0U}, final.pendingCommands, "Every live charge must be released after settlement.");
			context.ExpectEqual(std::size_t{4096U}, final.completedRecords, "The terminal history should fill its bounded window.");
			context.ExpectEqual(requestCount - 4096U, static_cast<std::size_t>(final.evictedRecords), "Only oldest terminal records should expire.");
			WriteEvidence(context, {producerCount, requestCount, applied.load(), pendingPeak.load(), completedPeak.load(),
				static_cast<std::size_t>(final.evictedRecords), duplicates.load(), settled.load()});
		}
	}

	void RegisterAudioControlContinuityTests(TestRegistry& registry) {
		registry.Add("Audio.ControlContinuity.Request4097RemainsAdmissible", [](TestContext& context) {
			Audio::AudioControlQueue queue;
			for(std::size_t index = 0U; index < 4097U; ++index) {
				context.ExpectEqual(std::size_t{0U}, queue.QueuedCommandCount(), "Previous requests must not remain queued.");
				context.Expect(queue.Submit(Request("continuity-" + std::to_string(index))).status == Audio::AudioControlStatus::Queued,
					"A fresh keyed command must remain admissible after 4096 completed requests.");
				ApplyAll(queue);
			}
			context.ExpectEqual(std::size_t{4096U}, queue.Statistics().completedRecords, "Completed history must remain bounded.");
		});
		registry.Add("Audio.ControlContinuity.RetryWinsOverFullCapacity", [](TestContext& context) {
			auto limits = SmallLimits(); limits.maximumQueuedCommands = 1U; limits.maximumProducerCommands = 1U;
			Audio::AudioControlQueue queue(limits);
			const auto first = queue.Submit(Request("full"));
			const auto retry = queue.Submit(Request("full"));
			context.Expect(retry.status == Audio::AudioControlStatus::Accepted && retry.batchSequence == first.batchSequence,
				"Matching retries must bypass both full live and producer capacity.");
		});
		registry.Add("Audio.ControlContinuity.TypedPayloadCannotHideBehindFingerprint", [](TestContext& context) {
			Audio::AudioControlQueue queue;
			queue.Submit(Request("typed"));
			auto changed = Request("typed"); changed.commands.front().scalarValue = 0.25F;
			context.Expect(queue.Submit(changed).rejection == Audio::AudioControlRejection::IdempotencyConflict,
				"Identical textual fingerprints cannot conceal changed scalar payloads.");
			changed = Request("typed"); changed.commands.front().source.sceneGeneration = std::uint64_t{1U} << 40U;
			context.Expect(queue.Submit(changed).rejection == Audio::AudioControlRejection::IdempotencyConflict,
				"The complete 64-bit attachment observation belongs to the original signature.");
			changed = Request("typed"); changed.commands.front().targetMode = Audio::AudioControlTargetMode::BindCurrentAtAdmission;
			context.Expect(queue.Submit(changed).rejection == Audio::AudioControlRejection::IdempotencyConflict,
				"Admission target mode is a typed part of request equivalence.");
		});
		registry.Add("Audio.ControlContinuity.PendingKeySurvivesHistoryRollover", [](TestContext& context) {
			Audio::AudioControlQueue queue(SmallLimits());
			const auto pending = queue.Submit(Request("future", 1U, 100U));
			for(unsigned index = 0U; index < 12U; ++index) { queue.Submit(Request("done-" + std::to_string(index))); ApplyAll(queue); }
			context.ExpectEqual(pending.batchSequence, queue.Submit(Request("future", 2U, 100U)).batchSequence,
				"Completed FIFO pressure must never expire queued retry ownership.");
		});
		registry.Add("Audio.ControlContinuity.InFlightKeySurvivesHistoryRollover", [](TestContext& context) {
			Audio::AudioControlQueue queue(SmallLimits());
			const auto first = queue.Submit(Request("in-flight"));
			const auto owners = queue.DrainEligible(0U);
			for(unsigned index = 0U; index < 10U; ++index) { queue.Submit(Request("terminal-" + std::to_string(index))); ApplyAll(queue); }
			context.ExpectEqual(first.batchSequence, queue.Submit(Request("in-flight")).batchSequence, "Drained requests retain retry protection until acknowledged.");
			context.Expect(queue.Complete(owners.front().completionToken, Audio::AudioControlStatus::Applied), "In-flight owner must still settle.");
		});
		registry.Add("Audio.ControlContinuity.CompletedRetryDoesNotExtendRetention", [](TestContext& context) {
			Audio::AudioControlQueue queue(SmallLimits());
			const auto oldest = queue.Submit(Request("oldest")); ApplyAll(queue);
			queue.Submit(Request("second")); ApplyAll(queue);
			context.Expect(queue.Submit(Request("oldest")).status == Audio::AudioControlStatus::Applied, "A completed retry should report its terminal outcome.");
			queue.Submit(Request("third")); ApplyAll(queue);
			context.Expect(queue.Submit(Request("oldest")).batchSequence != oldest.batchSequence, "Reading an old retry must not refresh its FIFO position.");
		});
		registry.Add("Audio.ControlContinuity.RejectedTerminalRetainsOutcome", [](TestContext& context) {
			Audio::AudioControlQueue queue;
			const auto first = queue.Submit(Request("rejected"));
			const auto owners = queue.DrainEligible(0U);
			queue.Complete(owners.front().completionToken, Audio::AudioControlStatus::Rejected, Audio::AudioControlRejection::StaleTarget);
			const auto retry = queue.Submit(Request("rejected"));
			context.Expect(retry.status == Audio::AudioControlStatus::Rejected && retry.rejection == Audio::AudioControlRejection::StaleTarget
				&& retry.batchSequence == first.batchSequence, "Rejected logical requests must remain terminal rather than replaying.");
		});
		registry.Add("Audio.ControlContinuity.AdmissionFailureRollsBackAllIndexes", [](TestContext& context) {
			Audio::AudioControlQueue queue;
			const auto failed = queue.Submit(Request("rollback"), [](void*, Audio::AudioControlBatch&, Audio::AudioControlReceipt&) -> bool { throw std::bad_alloc(); });
			const auto state = queue.Statistics();
			context.Expect(failed.rejection == Audio::AudioControlRejection::InternalFailure && state.pendingCommands == 0U && state.retainedKeyCount == 0U,
				"A failure after index staging must leave no key, charge or request owner.");
			context.ExpectEqual(std::uint64_t{1U}, queue.Submit(Request("rollback")).batchSequence, "Rejected staging must not consume a sequence or poison retry admission.");
		});
		registry.Add("Audio.ControlContinuity.InFlightCapacityRemainsCharged", [](TestContext& context) {
			auto limits = SmallLimits(); limits.maximumQueuedCommands = 2U; limits.maximumProducerCommands = 1U;
			Audio::AudioControlQueue queue(limits);
			queue.Submit(Request("first", 1U)); auto owners = queue.DrainEligible(0U);
			context.ExpectEqual(std::size_t{0U}, queue.QueuedCommandCount(), "Drain changes the queued-only observation.");
			context.Expect(queue.Submit(Request("same-producer", 1U)).rejection == Audio::AudioControlRejection::ProducerCapacity, "Drain must not prematurely release producer capacity.");
			queue.Submit(Request("second", 2U));
			context.Expect(queue.Submit(Request("third", 3U)).rejection == Audio::AudioControlRejection::GlobalCapacity, "Queued plus in-flight commands share the live bound.");
			queue.Complete(owners.front().completionToken, Audio::AudioControlStatus::Applied);
			context.Expect(queue.Submit(Request("same-producer", 1U)).status == Audio::AudioControlStatus::Queued, "Acknowledgement releases capacity exactly once.");
		});
		registry.Add("Audio.ControlContinuity.ResetRejectsLateCompletion", [](TestContext& context) {
			Audio::AudioControlQueue queue;
			queue.Submit(Request("old")); const auto old = queue.DrainEligible(0U).front().completionToken;
			queue.Reset(); queue.Submit(Request("new")); const auto replacement = queue.DrainEligible(0U).front().completionToken;
			context.Expect(old.sequence == replacement.sequence && old.epoch != replacement.epoch, "Reset may reuse a local sequence only in a different epoch.");
			context.Expect(!queue.Complete(old, Audio::AudioControlStatus::Applied) && queue.Statistics().pendingCommands == 1U, "A late old completion must not settle its replacement.");
			context.Expect(queue.Complete(replacement, Audio::AudioControlStatus::Applied) && !queue.Complete(replacement, Audio::AudioControlStatus::Applied), "The current owner must settle exactly once.");
		});
		registry.Add("Audio.ControlContinuity.StopRetiresEveryOwnershipKind", [](TestContext& context) {
			Audio::AudioControlQueue queue;
			queue.Submit(Request("completed")); ApplyAll(queue);
			queue.Submit(Request("flight")); const auto owner = queue.DrainEligible(0U).front().completionToken;
			queue.Submit(Request("queued", 1U, 10U)); queue.Stop();
			const auto state = queue.Statistics();
			context.Expect(state.pendingCommands == 0U && state.completedRecords == 0U && state.retainedKeyCount == 0U, "Shutdown must retire queued, in-flight and terminal history.");
			context.Expect(!queue.Complete(owner, Audio::AudioControlStatus::Applied) && queue.Submit(Request("stopped")).rejection == Audio::AudioControlRejection::QueueStopped, "Stopped epochs reject admissions and old acknowledgements.");
		});
		registry.Add("Audio.ControlContinuity.RetentionFollowsCompletionOrder", [](TestContext& context) {
			Audio::AudioControlQueue queue(SmallLimits(1U));
			const auto first = queue.Submit(Request("first")); const auto second = queue.Submit(Request("second"));
			const auto owners = queue.DrainEligible(0U);
			queue.Complete(owners[1].completionToken, Audio::AudioControlStatus::Applied);
			queue.Complete(owners[0].completionToken, Audio::AudioControlStatus::Applied);
			context.ExpectEqual(first.batchSequence, queue.Submit(Request("first")).batchSequence, "The most recently completed owner should stay retained.");
			context.Expect(queue.Submit(Request("second")).batchSequence != second.batchSequence, "Completion order, rather than admission order, selects eviction.");
		});
		registry.Add("Audio.ControlContinuity.RetryPreservesOriginalUnresolvedRequest", [](TestContext& context) {
			Audio::AudioControlQueue queue; unsigned resolutions = 0U;
			auto resolver = [](void* opaque, Audio::AudioControlBatch& batch, Audio::AudioControlReceipt& receipt) {
				++*static_cast<unsigned*>(opaque); batch.commands.front().source.playbackGeneration = 918U;
				batch.requestedFrame = 47U; receipt.requestedFrame = 47U;
				receipt.resolvedPlaybackGeneration = 918U; return true;
			};
			const auto original = queue.Submit(Request("resolved"), resolver, &resolutions);
			const auto retry = queue.Submit(Request("resolved", 9U), resolver, &resolutions);
			context.Expect(original.resolvedPlaybackGeneration == 918U && retry.resolvedPlaybackGeneration == 918U && resolutions == 1U
				&& original.requestedFrame == 47U && retry.requestedFrame == 47U,
				"Retries compare original content and reuse the admitted resolution without invoking the allocator.");
		});
		registry.Add("Audio.ControlContinuity.UnkeyedTerminalOwnersRetireImmediately", [](TestContext& context) {
			Audio::AudioControlQueue queue; queue.Submit(Request("")); ApplyAll(queue);
			context.Expect(queue.Statistics().pendingCommands == 0U && queue.Statistics().completedRecords == 0U,
				"Unkeyed requests need no terminal retry retention.");
		});
		for(const std::size_t count : {1U, 2U, 8U})
			registry.Add("Audio.ControlContinuity.TaskProducers" + std::to_string(count) + "Complete20000Requests", [count](TestContext& context) { RunProducerMatrix(context, count); });
	}
}
