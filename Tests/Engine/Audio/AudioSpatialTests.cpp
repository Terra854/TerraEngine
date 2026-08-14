// AudioSpatialTests.cpp - deterministic attenuation/listener/voice limits.

#include "../TestHarness.h"
#include "Audio/AudioPreparationPlan.h"
#include "Audio/AudioSpatialRenderer.h"

#include <cmath>

namespace TerraEngine::Tests {
	void RegisterAudioSpatialTests(TestRegistry& registry) {
		registry.Add("Audio.Spatial.AttenuationAndPanHaveStableEndpoints", [](TestContext& context) {
			context.Expect(Audio::ComputeAudioAttenuation(0.0F, 10.0F) == 1.0F,
				"The source position should retain full gain without an authored near-radius dead zone.");
			context.Expect(Audio::ComputeAudioAttenuation(10.0F, 10.0F) == 0.0F,
				"The far radius should virtualize exactly.");
			const float middleGain = Audio::ComputeAudioAttenuation(5.0F, 10.0F);
			context.Expect(std::abs(middleGain - 0.25F) < 0.000001F,
				"The midpoint must be substantially quieter under the normalized squared falloff.");
			const Audio::AudioVector3 engineForward{0.0F, 0.0F, -1.0F};
			context.Expect(Audio::ComputeAudioPan(engineForward, {1.0F, 0.0F, 0.0F}) == 1.0F,
				"A source on world +X must pan right when an unrotated listener faces engine-forward -Z.");
			context.Expect(Audio::ComputeAudioPan(engineForward, {-1.0F, 0.0F, 0.0F}) == -1.0F,
				"A source on world -X must pan left when an unrotated listener faces engine-forward -Z.");
			context.Expect(Audio::ComputeAudioPan(engineForward, {0.0F, 0.0F, -1.0F}) == 0.0F
				&& Audio::ComputeAudioPan(engineForward, {0.0F, 0.0F, 1.0F}) == 0.0F,
				"Sources directly in front of and behind the listener must remain centered rather than swapping sides.");
		});

		registry.Add("Audio.Spatial.BlendScalesPanAndImmediateDistanceFalloff", [](TestContext& context) {
			Audio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::GenerateUUID(), 1U, 1U};
			source.clipAssetId = TerraEngine::GenerateUUID();
			source.clipGeneration = 1U;
			source.clipFrames = 48000U;
			source.clipSampleRate = 48000U;
			source.position = {1.0F, 0.0F, 0.0F};
			source.maximumDistance = 10.0F;
			source.spatialBlend = 0.25F;

			Audio::AudioAuthoringSnapshotData data;
			data.sources.push_back(source);
			data.listeners.push_back({TerraEngine::GenerateUUID(), 1U, {}, {0.0F, 0.0F, -1.0F}, Audio::AudioOutputMode::Stereo, true, true});
			std::vector<Audio::AudioPlaybackSeed> seeds{
				{source.identity, Audio::AudioPlaybackState::Playing, 0U, source.clipAssetId, source.clipGeneration}
			};
			const Audio::AudioPreparedFrame prepared =
				Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), seeds);
			context.Expect(prepared.voices.size() == 1U,
				"The spatial blend fixture must prepare exactly one voice.");
			context.Expect(std::abs(prepared.voices.front().spatial.pan - 1.0F) < 0.000001F
				&& prepared.voices.front().spatial.spatialBlend == 0.25F,
				"Preparation must preserve direction while the renderer applies spatial blend exactly once.");
			context.Expect(std::abs(prepared.voices.front().gain - 0.9525F) < 0.000001F,
				"Distance falloff must begin immediately and spatial blend must scale the attenuated share exactly once.");
		});

		registry.Add("Audio.Spatial.VoiceLimitUsesPriorityThenIdentity", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData data; data.voiceLimit = 1U;
			data.listeners.push_back({TerraEngine::GenerateUUID(), 1U, {}, {0.0F,0.0F,-1.0F}, Audio::AudioOutputMode::Stereo, true, true});
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

		registry.Add("Audio.Spatial.OutputModesMapCardinalSpeakers", [](TestContext& context) {
			const auto mono = Audio::ComputeAudioSpeakerGains(Audio::AudioOutputMode::Mono, 0.0F, 1.0F);
			context.Expect(mono[2U] == 1.0F && std::count(mono.begin(), mono.end(), 0.0F) == 7,
				"Mono must route a point source exclusively through the center of the standard output map.");
			const auto stereoRight = Audio::ComputeAudioSpeakerGains(Audio::AudioOutputMode::Stereo, 1.0F, 0.0F);
			context.Expect(stereoRight[0U] == 0.0F && stereoRight[1U] == 1.0F,
				"Stereo must route a source on the listener's right to the right output channel.");
			const auto fiveRear = Audio::ComputeAudioSpeakerGains(Audio::AudioOutputMode::Surround5Point1, 0.0F, -1.0F);
			context.Expect(fiveRear[4U] > 0.7F && fiveRear[5U] > 0.7F && fiveRear[0U] == 0.0F && fiveRear[1U] == 0.0F,
				"5.1 must distribute a rear source between the back-left and back-right speakers.");
			const auto sevenSide = Audio::ComputeAudioSpeakerGains(Audio::AudioOutputMode::Surround7Point1, 1.0F, 0.0F);
			context.Expect(sevenSide[7U] == 1.0F,
				"7.1 must route a source at positive ninety degrees to the side-right speaker.");
		});

		registry.Add("Audio.Spatial.AnalyticHrtfAddsTimeLevelAndRearSpectralCues", [](TestContext& context) {
			constexpr std::uint32_t frameCount = 64U;
			std::array<float, frameCount * 2U> impulse{};
			impulse[0U] = 1.0F;
			impulse[1U] = 1.0F;
			std::array<float, frameCount * Audio::kAudioRenderChannelCount> rightOutput{};
			Audio::AudioSpatialParameters rightParameters;
			rightParameters.outputMode = Audio::AudioOutputMode::Hrtf;
			rightParameters.pan = 1.0F;
			rightParameters.frontness = 0.0F;
			rightParameters.spatialBlend = 1.0F;
			Audio::AudioSpatialRenderState rightState;
			Audio::RenderAudioSpatialFrames(impulse.data(), 2U, rightOutput.data(), Audio::kAudioRenderChannelCount,
				frameCount, 48000U, rightParameters, rightState);
			context.Expect(rightOutput[1U] > 0.0F && rightOutput[0U] == 0.0F,
				"A right-side HRTF impulse must reach the near ear before the delayed far ear.");

			std::array<float, frameCount * 2U> alternating{};
			for(std::uint32_t frame = 0U; frame < frameCount; ++frame) {
				alternating[frame * 2U] = alternating[frame * 2U + 1U] = (frame % 2U == 0U ? 1.0F : -1.0F);
			}
			const auto energyForFrontness = [&](float frontness) {
				std::array<float, frameCount * Audio::kAudioRenderChannelCount> output{};
				Audio::AudioSpatialParameters parameters;
				parameters.outputMode = Audio::AudioOutputMode::Hrtf;
				parameters.frontness = frontness;
				parameters.spatialBlend = 1.0F;
				Audio::AudioSpatialRenderState state;
				Audio::RenderAudioSpatialFrames(alternating.data(), 2U, output.data(), Audio::kAudioRenderChannelCount,
					frameCount, 48000U, parameters, state);
				float energy = 0.0F;
				for(std::uint32_t frame = 0U; frame < frameCount; ++frame)
					energy += output[frame * Audio::kAudioRenderChannelCount] * output[frame * Audio::kAudioRenderChannelCount]
						+ output[frame * Audio::kAudioRenderChannelCount + 1U] * output[frame * Audio::kAudioRenderChannelCount + 1U];
				return energy;
			};
			context.Expect(energyForFrontness(-1.0F) < energyForFrontness(1.0F),
				"Rear HRTF rendering must attenuate high-frequency energy to distinguish front from back.");
		});
	}
}
