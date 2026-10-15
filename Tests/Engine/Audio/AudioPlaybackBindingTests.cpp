// AudioPlaybackBindingTests.cpp - admission freezes full attachment/playback identity.
#include "../TestHarness.h"
#include "Audio/AudioSystem.h"
#include "Components/AudioSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "ThreadPool/ThreadPool.h"

#include <limits>

namespace TerraEngine::Tests {
	namespace {
		struct BindingFixture {
			Audio::AudioSystem& audio = Audio::AudioSystem::Instance();
			ObjectManager& objects = ObjectManager::instance();
			Object object;
			BindingFixture() {
				audio.Shutdown(); objects.clear(); ComponentRegistry::RegisterAllComponents();
				audio.SetConfiguredProvider("test.missing"); audio.SetRuntimePlaybackEnabled(true);
			}
			~BindingFixture() { audio.Shutdown(); audio.SetConfiguredProvider("terra.miniaudio"); objects.clear(); }
			void Start(bool autoplay = true) {
				object = objects.createObject("Audio binding fixture");
				auto* source = objects.AddComponent<AudioSourceComponent>(object);
				objects.AddComponent<Transform>(object);
				source->playOnStart = autoplay; source->spatialBlend = 0.0F;
				audio.Initialize(); audio.Update(0.01F);
			}
			Audio::AudioSourceBinding Binding(TestContext& context) {
				const auto binding = audio.ReadSourceBinding(object.entityValue);
				context.Expect(binding.has_value(), "Protected capture must expose a copied full source attachment.");
				return *binding;
			}
			Audio::AudioControlCommand Command(TestContext& context, Audio::AudioControlKind kind,
				Audio::AudioControlTargetMode mode = Audio::AudioControlTargetMode::BindCurrentAtAdmission,
				Audio::AudioGeneration exact = 0U) {
				const auto binding = Binding(context);
				Audio::AudioControlCommand command; command.kind = kind; command.targetMode = mode;
				command.source = {binding.attachment.objectId, binding.attachment.objectGeneration,
					binding.attachment.componentGeneration, exact, binding.attachment.sceneGeneration};
				return command;
			}
			Audio::AudioControlReceipt Submit(Audio::AudioControlCommand command, Audio::AudioFrame frame = 0U, std::string key = {}) {
				Audio::AudioControlBatch batch; batch.requestedFrame = frame; batch.idempotencyKey = std::move(key);
				batch.commands.push_back(command); return audio.SubmitControlBatch(std::move(batch));
			}
			Audio::AudioControlReceipt Applied(TestContext& context, const Audio::AudioControlReceipt& queued) {
				audio.Update(0.01F);
				const auto receipt = audio.ReadControlReceipt(queued.batchSequence);
				context.Expect(receipt && receipt->status == Audio::AudioControlStatus::Applied, "The complete control must apply at the protected snapshot boundary.");
				return *receipt;
			}
		};
	}
	void RegisterAudioPlaybackBindingTests(TestRegistry& registry) {
		registry.Add("Audio.PlaybackBinding.FreshFacadePausesAutomaticPlayback", [](TestContext& context) {
			BindingFixture fixture; fixture.Start();
			const auto before = fixture.audio.LatestPlaybackSnapshot();
			context.Expect(before && before->voices.size() == 1U, "Automatic playback must exist before a fresh facade control.");
			const auto queued = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Pause));
			const auto receipt = fixture.Applied(context, queued);
			context.Expect(receipt.resolvedPlaybackGeneration == before->voices.front().identity.playbackGeneration
				&& fixture.audio.LatestPlaybackSnapshot()->voices.front().paused, "A fresh facade must pause the exact automatic instance without calling Play.");
		});
		registry.Add("Audio.PlaybackBinding.AutomaticGenerationsSurviveAudioRestart", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(); const auto first = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			fixture.audio.Shutdown(); fixture.audio.Initialize(); fixture.audio.Update(0.01F);
			const auto second = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			context.Expect(first != 0U && second > first, "Audio restart must allocate a fresh central generation rather than recycling one.");
		});
		registry.Add("Audio.PlaybackBinding.RuntimeReentryWaitsForFreshProtectedCapture", [](TestContext& context) {
			BindingFixture fixture; fixture.Start();
			const auto first = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			const auto entity = fixture.object.entityValue;
			// Play Mode entry runs managed assignment callbacks before the next
			// audio capture. The retired edit-scene table must remain unavailable
			// throughout that gap rather than supplying a stale facade attachment.
			fixture.audio.SetRuntimePlaybackEnabled(false);
			fixture.audio.SetRuntimePlaybackEnabled(true);
			context.Expect(!fixture.audio.ReadSourceBinding(entity),
				"Runtime reentry must not expose an attachment captured before the lifecycle transition.");
			fixture.audio.Update(0.01F);
			const auto binding = fixture.audio.ReadSourceBinding(entity);
			const auto snapshot = fixture.audio.LatestPlaybackSnapshot();
			context.Expect(binding && snapshot && snapshot->voices.size() == 1U
				&& snapshot->voices.front().identity.playbackGeneration > first,
				"Fresh protected capture must publish the live attachment and allocate a new automatic playback instance.");
		});
		registry.Add("Audio.PlaybackBinding.PendingPlayThenPauseBindsAcceptedPredecessor", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(false);
			const auto play = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Play));
			const auto pause = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Pause));
			context.Expect(play.resolvedPlaybackGeneration != 0U && pause.resolvedPlaybackGeneration == play.resolvedPlaybackGeneration,
				"A pause admitted before the boundary must bind to the already accepted pending Play.");
			fixture.Applied(context, pause);
			context.Expect(fixture.audio.LatestPlaybackSnapshot()->voices.front().paused, "The pending Play/Pause pair must finish paused.");
		});
		registry.Add("Audio.PlaybackBinding.SameBatchPlayPauseUsesCommandOrder", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(false);
			Audio::AudioControlBatch batch;
			batch.commands.push_back(fixture.Command(context, Audio::AudioControlKind::Play));
			batch.commands.push_back(fixture.Command(context, Audio::AudioControlKind::Pause));
			const auto queued = fixture.audio.SubmitControlBatch(std::move(batch)); fixture.Applied(context, queued);
			context.Expect(fixture.audio.LatestPlaybackSnapshot()->voices.front().paused, "A later command in one batch must bind to that batch's staged Play.");
		});
		registry.Add("Audio.PlaybackBinding.FuturePlayCannotCaptureEarlierPause", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(); const auto current = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			const auto future = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Play), 8U);
			const auto pause = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Pause), 2U);
			context.Expect(pause.resolvedPlaybackGeneration == current && pause.resolvedPlaybackGeneration != future.resolvedPlaybackGeneration,
				"An earlier eligibility frame must ignore a future Play even when it was admitted first.");
			fixture.Applied(context, pause);
			context.Expect(fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration == current
				&& fixture.audio.LatestPlaybackSnapshot()->voices.front().paused, "The earlier boundary should pause only the current instance.");
		});
		registry.Add("Audio.PlaybackBinding.FailedPredecessorRejectsBoundFollower", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(); const auto current = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			Audio::AudioControlBatch rejected; rejected.commands.push_back(fixture.Command(context, Audio::AudioControlKind::Play));
			Audio::AudioControlCommand invalid; invalid.kind = Audio::AudioControlKind::SetMasterVolume; invalid.scalarValue = std::numeric_limits<float>::quiet_NaN();
			rejected.commands.push_back(invalid); const auto play = fixture.audio.SubmitControlBatch(std::move(rejected));
			const auto pause = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Pause)); fixture.audio.Update(0.01F);
			const auto outcome = fixture.audio.ReadControlReceipt(pause.batchSequence);
			context.Expect(pause.resolvedPlaybackGeneration == play.resolvedPlaybackGeneration && outcome && outcome->status == Audio::AudioControlStatus::Rejected,
				"A failed accepted predecessor must reject its frozen follower rather than rebinding to an older instance.");
			context.Expect(fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration == current
				&& !fixture.audio.LatestPlaybackSnapshot()->voices.front().paused, "The old instance must remain unchanged by both rejected batches.");
		});
		registry.Add("Audio.PlaybackBinding.ExactOldInstanceCannotControlReplacement", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(); const auto old = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			const auto play = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Play)); fixture.Applied(context, play);
			const auto pause = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Pause, Audio::AudioControlTargetMode::ExactInstance, old));
			fixture.audio.Update(0.01F); const auto outcome = fixture.audio.ReadControlReceipt(pause.batchSequence);
			context.Expect(outcome && outcome->status == Audio::AudioControlStatus::Rejected
				&& fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration == play.resolvedPlaybackGeneration,
				"A stale explicit instance must fail without affecting its replacement.");
		});
		registry.Add("Audio.PlaybackBinding.OldFacadeCannotFollowComponentReplacement", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(); auto old = fixture.Command(context, Audio::AudioControlKind::Pause);
			fixture.objects.RemoveComponent<AudioSourceComponent>(fixture.object);
			auto* replacement = fixture.objects.AddComponent<AudioSourceComponent>(fixture.object); replacement->playOnStart = true;
			fixture.audio.Update(0.01F);
			context.Expect(fixture.Submit(old).rejection == Audio::AudioControlRejection::StaleTarget, "An old facade must not refresh its attachment to a replacement component.");
		});
		registry.Add("Audio.PlaybackBinding.SourceUnavailableUntilProtectedCapture", [](TestContext& context) {
			BindingFixture fixture; fixture.object = fixture.objects.createObject("Not yet captured");
			fixture.objects.AddComponent<AudioSourceComponent>(fixture.object); fixture.objects.AddComponent<Transform>(fixture.object);
			context.Expect(!fixture.audio.ReadSourceBinding(fixture.object.entityValue), "Uncaptured source lookup must return missing rather than inspect live storage from a bridge.");
			fixture.audio.Initialize(); fixture.audio.Update(0.01F);
			context.Expect(fixture.audio.ReadSourceBinding(fixture.object.entityValue).has_value(), "The next protected capture should publish the source binding.");
		});
		registry.Add("Audio.PlaybackBinding.WorkerUsesOnlyCopiedBindingAndAdmission", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(); const auto entity = fixture.object.entityValue;
			auto pool = ThreadPool::CreateOwned(1U);
			auto task = pool->createTask([&fixture, entity]() {
				const auto binding = fixture.audio.ReadSourceBinding(entity);
				if(!binding) throw TestFailure("Worker could not read captured source binding.");
				Audio::AudioControlCommand pause; pause.kind = Audio::AudioControlKind::Pause; pause.targetMode = Audio::AudioControlTargetMode::BindCurrentAtAdmission;
				pause.source = {binding->attachment.objectId, binding->attachment.objectGeneration, binding->attachment.componentGeneration, 0U, binding->attachment.sceneGeneration};
				return fixture.Submit(pause);
			});
			auto future = task->getFuture(); pool->enqueue(task); const auto queued = future.get(); pool->shutdown(); fixture.Applied(context, queued);
			context.Expect(fixture.audio.LatestPlaybackSnapshot()->voices.front().paused, "A worker producer should control copied identity without a live component read or write.");
		});
		registry.Add("Audio.PlaybackBinding.PlayRetryReusesAllocatedGenerationAndFrame", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(false); auto command = fixture.Command(context, Audio::AudioControlKind::Play);
			const auto first = fixture.Submit(command, 0U, "one-play"); const auto retry = fixture.Submit(command, 0U, "one-play");
			context.Expect(first.batchSequence == retry.batchSequence && first.resolvedPlaybackGeneration == retry.resolvedPlaybackGeneration
				&& first.requestedFrame == retry.requestedFrame && first.requestedFrame != 0U,
				"An unresolved retry must reuse both the central generation and resolved eligibility frame.");
			fixture.Applied(context, first);
		});
		registry.Add("Audio.PlaybackBinding.ExactResumeCannotReviveStoppedInstance", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(); const auto generation = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			fixture.Applied(context, fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Stop)));
			const auto resume = fixture.Submit(fixture.Command(context, Audio::AudioControlKind::Resume, Audio::AudioControlTargetMode::ExactInstance, generation));
			fixture.audio.Update(0.01F); const auto outcome = fixture.audio.ReadControlReceipt(resume.batchSequence);
			context.Expect(outcome && outcome->status == Audio::AudioControlStatus::Rejected, "An explicit terminal instance must remain retired.");
		});
		registry.Add("Audio.PlaybackBinding.SceneReloadCannotReuseOldUuidPlayback", [](TestContext& context) {
			BindingFixture fixture; fixture.Start();
			const auto oldBinding = fixture.Binding(context); const auto oldCommand = fixture.Command(context, Audio::AudioControlKind::Pause);
			const auto oldGeneration = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			fixture.objects.clear(); fixture.object = fixture.objects.createObject("Reloaded source with same UUID");
			fixture.objects.UpdateObjectUUID(fixture.object, oldBinding.attachment.objectId);
			auto* source = fixture.objects.AddComponent<AudioSourceComponent>(fixture.object);
			fixture.objects.AddComponent<Transform>(fixture.object); source->playOnStart = true; source->spatialBlend = 0.0F;
			fixture.audio.Update(0.01F); const auto replacement = fixture.Binding(context);
			context.Expect(replacement.attachment.sceneGeneration != oldBinding.attachment.sceneGeneration
				&& !fixture.audio.ReadSourcePlayback(oldBinding.attachment), "Scene retirement must invalidate an old facade even when the durable UUID is restored.");
			context.Expect(fixture.Submit(oldCommand).rejection == Audio::AudioControlRejection::StaleTarget
				&& fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration > oldGeneration,
				"The restored source must own a fresh central instance and reject controls carrying the previous scene lifetime.");
		});
		registry.Add("Audio.PlaybackBinding.TerminalControlRejectsWholeMixedBatch", [](TestContext& context) {
			BindingFixture fixture; fixture.Start(); const auto generation = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity.playbackGeneration;
			Audio::AudioControlBatch batch;
			batch.commands.push_back(fixture.Command(context, Audio::AudioControlKind::Stop, Audio::AudioControlTargetMode::ExactInstance, generation));
			batch.commands.push_back(fixture.Command(context, Audio::AudioControlKind::Resume, Audio::AudioControlTargetMode::ExactInstance, generation));
			const auto queued = fixture.audio.SubmitControlBatch(std::move(batch)); fixture.audio.Update(0.01F);
			const auto receipt = fixture.audio.ReadControlReceipt(queued.batchSequence);
			const auto observation = fixture.audio.ReadSourcePlayback(fixture.Binding(context).attachment);
			context.Expect(receipt && receipt->status == Audio::AudioControlStatus::Rejected && observation
				&& observation->state == Audio::AudioPlaybackState::Playing,
				"A later illegal terminal resume must reject the batch without partially applying its preceding stop.");
		});
		registry.Add("Audio.PlaybackBinding.FullAttachmentSeparatesSnapshotFromOldPublication", [](TestContext& context) {
			BindingFixture fixture;
			Audio::AudioAuthoringSnapshotData data; Audio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::UUID{91U}, 7U, 0U}; source.attachment = {source.identity.objectId, 1U, 3U, 7U}; source.objectEntity = 33U;
			source.spatial = false; data.sources.push_back(source);
			fixture.audio.SetAuthoringSnapshot(Audio::AudioAuthoringSnapshot::Create(data)); fixture.audio.Initialize();
			auto command = Audio::AudioControlCommand{}; command.kind = Audio::AudioControlKind::Play; command.source = {source.identity.objectId, 3U, 7U, 0U, 1U};
			fixture.Applied(context, fixture.Submit(command)); const auto oldBinding = fixture.audio.ReadSourceBinding(33U);
			context.Expect(oldBinding && fixture.audio.ReadSourcePlayback(oldBinding->attachment), "The initial full binding must have a matching publication.");
			data.sources.front().attachment.sceneGeneration = 2U;
			fixture.audio.SetAuthoringSnapshot(Audio::AudioAuthoringSnapshot::Create(data));
			auto workloads = fixture.audio.DeclareFrameWorkloads(0.01F); Scheduling::JobGraphContext job;
			workloads[0].work(job); const auto replacement = fixture.audio.ReadSourceBinding(33U);
			context.Expect(replacement && !fixture.audio.ReadSourcePlayback(replacement->attachment) && !fixture.audio.ReadSourcePlayback(oldBinding->attachment),
				"Between snapshot and commit, reused UUID/component versions must not expose an old scene's playback.");
			workloads[1].work(job); workloads[2].work(job);
			context.Expect(!fixture.audio.ReadSourcePlayback(replacement->attachment), "The retired seed must not be relabeled as belonging to the new attachment.");
		});
	}
}
