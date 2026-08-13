// AudioSpatialTests.cpp - deterministic attenuation/listener/voice limits.

#include "../TestHarness.h"
#include "Audio/AudioPreparationPlan.h"

#include <cmath>

namespace TerraEngine::Tests {
	void RegisterAudioSpatialTests(TestRegistry& registry) {
		registry.Add("Audio.Spatial.AttenuationAndPanHaveStableEndpoints", [](TestContext& context) {
			context.Expect(Audio::ComputeAudioAttenuation(1.0F, 1.0F, 10.0F) == 1.0F,
				"The near radius should retain full gain.");
			context.Expect(Audio::ComputeAudioAttenuation(10.0F, 1.0F, 10.0F) == 0.0F,
				"The far radius should virtualize exactly.");
			context.Expect(Audio::ComputeAudioPan({0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F}) < 0.0F,
				"The horizontal side test must remain deterministic for the engine coordinate system.");
		});

		registry.Add("Audio.Spatial.BlendScalesFinalPanWithoutProviderAttenuation", [](TestContext& context) {
			Audio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::GenerateUUID(), 1U, 1U};
			source.clipAssetId = TerraEngine::GenerateUUID();
			source.clipGeneration = 1U;
			source.clipFrames = 48000U;
			source.clipSampleRate = 48000U;
			source.position = {1.0F, 0.0F, 0.0F};
			source.minimumDistance = 1.0F;
			source.maximumDistance = 10.0F;
			source.spatialBlend = 0.25F;

			Audio::AudioAuthoringSnapshotData data;
			data.sources.push_back(source);
			data.listeners.push_back({TerraEngine::GenerateUUID(), 1U, {}, {0.0F, 0.0F, -1.0F}, true, true});
			std::vector<Audio::AudioPlaybackSeed> seeds{
				{source.identity, Audio::AudioPlaybackState::Playing, 0U, source.clipAssetId, source.clipGeneration}
			};
			const Audio::AudioPreparedFrame prepared =
				Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), seeds);
			context.Expect(prepared.voices.size() == 1U,
				"The spatial blend fixture must prepare exactly one voice.");
			context.Expect(std::abs(prepared.voices.front().spatial.pan + 0.25F) < 0.000001F,
				"Spatial blend must scale the final provider-neutral pan exactly once.");
			context.Expect(std::abs(prepared.voices.front().gain - 1.0F) < 0.000001F,
				"A source at minimum distance must retain full gain before provider playback.");
		});

		registry.Add("Audio.Spatial.VoiceLimitUsesPriorityThenIdentity", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData data; data.voiceLimit = 1U;
			data.listeners.push_back({TerraEngine::GenerateUUID(), 1U, {}, {0.0F,0.0F,-1.0F}, true, true});
			std::vector<Audio::AudioPlaybackSeed> seeds;
			for(std::uint32_t priority : {10U, 200U}) {
				Audio::AudioAuthoringSourceRecord source; source.identity = {TerraEngine::GenerateUUID(), 1U, 1U};
				source.clipAssetId = TerraEngine::GenerateUUID(); source.clipGeneration = 1U; source.clipFrames = 100U;
				source.priority = priority; source.position = {0.0F, 0.0F, -2.0F}; data.sources.push_back(source);
				seeds.push_back({source.identity, Audio::AudioPlaybackState::Playing, 0U});
			}
			const auto result = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(data), seeds);
			const auto audible = std::count_if(result.voices.begin(), result.voices.end(), [](const auto& voice) { return voice.audibility == Audio::AudioAudibility::Audible; });
			context.ExpectEqual(1, static_cast<int>(audible), "The configured voice budget must never be exceeded.");
			const auto winner = std::find_if(result.voices.begin(), result.voices.end(), [](const auto& voice) { return voice.audibility == Audio::AudioAudibility::Audible; });
			context.Expect(winner != result.voices.end() && winner->priority == 200U,
				"Higher-priority voices must win deterministic admission.");
		});
	}
}
