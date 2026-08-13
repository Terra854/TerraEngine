// AudioVoiceLimitTests.cpp - priority ranking and repeatable large voice plans.

#include "../TestHarness.h"

#include "Audio/AudioPreparationPlan.h"

#include <string>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::UUID MakeVoiceId(std::uint8_t value) { TerraEngine::UUID id{}; id.back() = value; return id; }
		Audio::AudioAuthoringSourceRecord Source(std::uint8_t id, std::uint32_t priority) {
			Audio::AudioAuthoringSourceRecord source; source.identity = {MakeVoiceId(id), 1U, 1U}; source.clipAssetId = MakeVoiceId(250U);
			source.clipGeneration = 1U; source.clipFrames = 1000000U; source.priority = priority; source.spatial = false; return source;
		}
		std::string AudibleSignature(const Audio::AudioPreparedFrame& prepared) { std::string result; for(const auto& voice : prepared.voices) if(voice.audibility == Audio::AudioAudibility::Audible) result += TerraEngine::ToHexString(voice.identity.objectId) + ','; return result; }
	}

	void RegisterAudioVoiceLimitTests(TestRegistry& registry) {
		registry.Add("Audio.VoiceLimit.HigherPriorityNeverLosesToLowerPriority", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData data; data.voiceLimit = 2U; data.sources = {Source(1U, 1U), Source(2U, 255U), Source(3U, 128U)};
			std::vector<Audio::AudioPlaybackSeed> seeds; for(const auto& source : data.sources) seeds.push_back({source.identity, Audio::AudioPlaybackState::Playing, 123U});
			const auto prepared = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), seeds);
			context.Expect(prepared.voices[1].audibility == Audio::AudioAudibility::Audible && prepared.voices[2].audibility == Audio::AudioAudibility::Audible,
				"The two highest priorities must own the bounded audible voices.");
			context.Expect(prepared.voices[0].audibility == Audio::AudioAudibility::VoiceLimited,
				"A lower-priority source must virtualize instead of evicting higher-priority work.");
		});

		registry.Add("Audio.VoiceLimit.Plans128SourcesIdenticallyAcross100Runs", [](TestContext& context) {
			std::string expected;
			for(std::uint32_t run = 0U; run < 100U; ++run) {
				Audio::AudioAuthoringSnapshotData data; data.voiceLimit = 16U;
				std::vector<Audio::AudioPlaybackSeed> seeds;
				for(std::uint32_t index = 0U; index < 128U; ++index) { data.sources.push_back(Source(static_cast<std::uint8_t>(index), index % 16U)); seeds.push_back({data.sources.back().identity, Audio::AudioPlaybackState::Playing, index}); }
				const auto prepared = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), seeds);
				const std::string signature = AudibleSignature(prepared); if(run == 0U) expected = signature; else context.Expect(signature == expected, "Equivalent 128-source inputs must select the exact same audible set every run.");
			}
		});

		registry.Add("Audio.VoiceLimit.ReacquisitionKeepsLogicalCursor", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData data; data.voiceLimit = 1U; data.sources = {Source(1U, 1U), Source(2U, 2U)};
			std::vector<Audio::AudioPlaybackSeed> seeds{{data.sources[0].identity, Audio::AudioPlaybackState::Playing, 777U}, {data.sources[1].identity, Audio::AudioPlaybackState::Playing, 888U}};
			const auto limited = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(data), seeds);
			context.Expect(limited.voices[0].audibility == Audio::AudioAudibility::VoiceLimited && limited.voices[0].logicalFrame == 777U,
				"Virtualization must not reset a lower-priority logical cursor.");
			data.sources[0].priority = 3U; const auto reacquired = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), seeds);
			context.Expect(reacquired.voices[0].audibility == Audio::AudioAudibility::Audible && reacquired.voices[0].logicalFrame == 777U,
				"A reacquired voice must seek from its preserved engine logical cursor.");
		});
	}
}
