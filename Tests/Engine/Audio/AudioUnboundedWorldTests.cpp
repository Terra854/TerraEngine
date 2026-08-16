// AudioUnboundedWorldTests.cpp - canonical-world localization regressions.

#include "../TestHarness.h"

#include "Audio/AudioPreparationPlan.h"
#include "Scene/WorldCoordinate.h"

#include <cmath>

namespace TerraEngine::Tests {
	namespace {
		Audio::AudioVector3 LocalizeForAudio(const Scene::WorldPosition& position, const Scene::WorldPosition& listener)
		{
			const Scene::WorldLocalizationResult localized = Scene::LocalizeWorldPosition(position, listener, 8'192.0);
			return {static_cast<float>(localized.offsetMetres.x), static_cast<float>(localized.offsetMetres.y), static_cast<float>(localized.offsetMetres.z)};
		}

		Audio::AudioPreparedVoice PrepareOneVoice(Audio::AudioVector3 sourcePosition, Audio::AudioVector3 listenerPosition)
		{
			Audio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::GenerateUUID(), 1U, 1U};
			source.clipAssetId = TerraEngine::GenerateUUID(); source.clipGeneration = 1U;
			source.clipFrames = 48'000U; source.clipSampleRate = 48'000U;
			source.position = sourcePosition; source.maximumDistance = 40.0F;
			Audio::AudioAuthoringSnapshotData data;
			data.sources = {source};
			data.listeners = {{TerraEngine::GenerateUUID(), 1U, listenerPosition, {0.0F, 0.0F, -1.0F}, Audio::AudioOutputMode::Stereo, true, true}};
			std::vector<Audio::AudioPlaybackSeed> playback{{source.identity, Audio::AudioPlaybackState::Playing, 0U, source.clipAssetId, source.clipGeneration}};
			return Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), playback).voices.front();
		}
	}

	void RegisterAudioUnboundedWorldTests(TestRegistry& registry)
	{
		registry.Add("Audio.UnboundedWorld.DistantListenerFramesLocalizeWithoutChangingMix", [](TestContext& context) {
			// The two islands are over 10^12 exact cells apart.  The audio worker sees
			// only the localized value, so no huge float subtraction enters its hot path.
			const Scene::WorldPosition firstListener{{1'000'000'000'000LL, -1'000'000'000'000LL, 41LL}, {20.0, -3.0, 10.0}};
			const Scene::WorldPosition firstSource{{1'000'000'000'000LL, -1'000'000'000'000LL, 41LL}, {24.0, -3.0, 2.0}};
			const Scene::WorldPosition secondListener{{-1'000'000'000'000LL, 1'000'000'000'000LL, -91LL}, {20.0, -3.0, 10.0}};
			const Scene::WorldPosition secondSource{{-1'000'000'000'000LL, 1'000'000'000'000LL, -91LL}, {24.0, -3.0, 2.0}};
			const Audio::AudioVector3 firstLocalSource = LocalizeForAudio(firstSource, firstListener);
			const Audio::AudioVector3 secondLocalSource = LocalizeForAudio(secondSource, secondListener);
			context.Expect(std::abs(firstLocalSource.x - secondLocalSource.x) < 0.00001F
				&& std::abs(firstLocalSource.z - secondLocalSource.z) < 0.00001F,
				"Equivalent source offsets in separate listener frames must localize identically.");

			const Audio::AudioPreparedVoice firstMix = PrepareOneVoice(firstLocalSource, {});
			const Audio::AudioPreparedVoice secondMix = PrepareOneVoice(secondLocalSource, {});
			context.Expect(firstMix.audibility == Audio::AudioAudibility::Audible && secondMix.audibility == Audio::AudioAudibility::Audible,
				"Canonical source/listener pairs inside their local frames should remain audible.");
			context.Expect(std::abs(firstMix.gain - secondMix.gain) < 0.00001F
				&& std::abs(firstMix.spatial.pan - secondMix.spatial.pan) < 0.00001F,
				"Moving the complete listener/source frame across unbounded cells must not alter mixing.");
		});
	}
}
