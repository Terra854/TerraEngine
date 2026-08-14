// AudioListenerTests.cpp - deterministic listener choice and no-listener behavior.

#include "../TestHarness.h"

#include "Audio/AudioPreparationPlan.h"
#include "Components/AudioListenerComponent.h"

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::UUID MakeId(std::uint8_t value) { TerraEngine::UUID id{}; id.back() = value; return id; }
		Audio::AudioAuthoringSourceRecord MakeSpatialSource() {
			Audio::AudioAuthoringSourceRecord source;
			source.identity = {MakeId(42U), 1U, 1U}; source.clipAssetId = MakeId(7U);
			source.clipGeneration = 1U; source.clipFrames = 480000U; source.position = {4.0F, 0.0F, 0.0F};
			source.spatial = true; source.maximumDistance = 20.0F;
			return source;
		}
	}

	void RegisterAudioListenerTests(TestRegistry& registry) {
		registry.Add("Audio.Listener.OutputModeDefaultsAndRejectsUnknownValues", [](TestContext& context) {
			AudioListenerComponent listener;
			context.Expect(listener.outputMode == Audio::AudioOutputMode::Stereo,
				"A newly authored listener must default to Stereo.");
			listener.SetOutputModeValue(999U);
			context.Expect(listener.outputMode == Audio::AudioOutputMode::Stereo,
				"Unknown serialized output modes must fail closed to Stereo.");
		});

		registry.Add("Audio.Listener.UsesUuidThenGenerationTieBreak", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData data; data.sources = {MakeSpatialSource()};
			// Input order is intentionally reversed. Snapshot construction must make
			// the same listener effective on every worker and every host platform.
			data.listeners = {{MakeId(9U), 1U, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, Audio::AudioOutputMode::Mono, true, true},
				{MakeId(3U), 2U, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, Audio::AudioOutputMode::Surround7Point1, true, true},
				{MakeId(3U), 1U, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, Audio::AudioOutputMode::Hrtf, true, true}};
			std::vector<Audio::AudioPlaybackSeed> playback{{data.sources.front().identity, Audio::AudioPlaybackState::Playing, 0U}};
			const auto prepared = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), playback);
			context.Expect(prepared.listener.has_value(), "At least one eligible listener should be selected.");
			context.Expect(prepared.listener->objectId == MakeId(3U) && prepared.listener->componentGeneration == 1U,
				"Listener selection must sort UUID ascending, then component generation ascending.");
			context.Expect(prepared.listener->outputMode == Audio::AudioOutputMode::Hrtf,
				"The selected listener must carry its authored renderer mode into preparation.");
			context.Expect(prepared.listenerCandidateCount == 3U,
				"Diagnostics input must retain the complete eligible-listener candidate count.");
		});

		registry.Add("Audio.Listener.VirtualizesOnlySpatialVoicesWhenMissing", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData data; data.sources = {MakeSpatialSource()};
			Audio::AudioAuthoringSourceRecord nonSpatial = data.sources.front(); nonSpatial.identity.objectId = MakeId(43U); nonSpatial.spatial = false;
			data.sources.push_back(nonSpatial);
			std::vector<Audio::AudioPlaybackSeed> playback{{data.sources[0].identity, Audio::AudioPlaybackState::Playing, 0U}, {data.sources[1].identity, Audio::AudioPlaybackState::Playing, 0U}};
			const auto prepared = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), playback);
			context.Expect(prepared.voices[0].audibility == Audio::AudioAudibility::NoListener,
				"A spatial source without an effective listener must remain logically playing but virtualized.");
			context.Expect(prepared.voices[1].audibility == Audio::AudioAudibility::Audible,
				"Non-spatial playback must not require a listener.");
		});
	}
}
