// AudioResourceStabilityTests.cpp - provider retention stays bounded across churn.

#include "../TestHarness.h"
#include "Audio/AudioPreparationPlan.h"
#include "FakeAudioBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <future>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::UUID MakeClipId() { TerraEngine::UUID id{}; id.back() = 91U; return id; }
		struct ThreadPoolShutdownGuard final {
			~ThreadPoolShutdownGuard() { ThreadPool::instance().shutdown(); }
		};
	}
	void RegisterAudioResourceStabilityTests(TestRegistry& registry) {
		registry.Add("Audio.ResourceStability.ConcurrentPreparationUsesEngineTasks", [](TestContext& context) {
			ThreadPool& pool = ThreadPool::instance();
			pool.shutdown();
			pool.initialize(4U);
			ThreadPoolShutdownGuard shutdownGuard;
			std::vector<std::future<std::uint64_t>> futures;
			for(std::uint32_t partition = 0U; partition < 4U; ++partition) {
				auto task = pool.createTask([partition]() {
					TerraEngine::Audio::AudioAuthoringSnapshotData data;
					data.deltaSeconds = 1.0F / 60.0F;
					data.voiceLimit = 32U;
					std::vector<TerraEngine::Audio::AudioPlaybackSeed> playback;
					for(std::uint32_t index = 0U; index < 32U; ++index) {
						TerraEngine::Audio::AudioAuthoringSourceRecord source;
						source.identity = {TerraEngine::UUID{static_cast<std::uint8_t>(partition + 1U),
							static_cast<std::uint8_t>(index + 1U)}, 1U, index + 1U};
						source.clipAssetId = MakeClipId();
						source.clipGeneration = 1U;
						source.clipFrames = 100000U;
						source.spatial = false;
						data.sources.push_back(source);
						playback.push_back({source.identity, TerraEngine::Audio::AudioPlaybackState::Playing, 0U});
					}
					std::uint64_t checksum = 0U;
					for(std::uint32_t replay = 0U; replay < 100U; ++replay) {
						auto localPlayback = playback;
						const auto prepared = TerraEngine::Audio::PrepareAudioFrame(
							TerraEngine::Audio::AudioAuthoringSnapshot::Create(data), localPlayback);
						checksum += prepared.voices.size() + prepared.voices.front().logicalFrame;
					}
					return checksum;
				});
				futures.push_back(task->getFuture());
				pool.enqueue(task, 32U);
			}
			for(auto& future : futures) {
				context.Expect(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
					"Every audio preparation partition should complete on the engine task pool without deadlock.");
				context.Expect(future.get() == 83200U,
					"Concurrent preparation partitions must produce identical deterministic results.");
			}
		});

		registry.Add("Audio.ResourceStability.SharedClipAndVoiceChurnLeavesNoLeaks", [](TestContext& context) {
			Audio::FakeAudioBackend backend; (void)backend.Initialize({});
			const auto payload = std::make_shared<const std::vector<std::uint8_t>>(64U, 9U);
			for(std::uint64_t iteration = 1U; iteration <= 128U; ++iteration) {
				TerraEngine::Audio::AudioBackendFramePacket create; create.sequence = iteration * 2U - 1U;
				create.registrations.push_back({MakeClipId(), 1U, TerraEngine::Audio::AudioClipEncoding::Wav, 48000U, 2U, 48U, 1U, payload});
				create.voices.push_back({iteration, {}, MakeClipId(), 1U, 0U, 1.0F, 1.0F, true, false, false, false, {}}); (void)backend.SubmitFrame(create);
				TerraEngine::Audio::AudioBackendFramePacket destroy; destroy.sequence = iteration * 2U; destroy.releases.push_back({MakeClipId(), 1U}); destroy.voices.push_back({iteration, {}, {}, 0U, 0U, 1.0F, 1.0F, false, true, false, false, {}}); (void)backend.SubmitFrame(destroy);
			}
			context.Expect(backend.LiveVoiceCount() == 0U && backend.RegisteredClipCount() == 0U,
				"Repeated shared-clip create/release churn must not retain orphan provider records.");
			context.Expect(backend.Packets().size() == 256U, "Every churn packet should be captured deterministically for leak investigations.");
		});
	}
}
