#include "Audio/AudioPreparationPlan.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Audio {
	namespace {
		constexpr float kPi = 3.14159265358979323846F;
		float Length(const AudioVector3& value) { return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z); }
		AudioVector3 Subtract(const AudioVector3& left, const AudioVector3& right) { return {left.x - right.x, left.y - right.y, left.z - right.z}; }
		AudioPlaybackSeed* FindSeed(std::vector<AudioPlaybackSeed>& seeds, const AudioSourceIdentity& authoredIdentity) {
			// Authoring identifies the durable component, while each Play command owns
			// a new playback generation. Matching the full identity would make every
			// replay after generation one disappear from the prepared frame.
			const auto found = std::find_if(seeds.begin(), seeds.end(), [&](const auto& seed) {
				return seed.identity.objectId == authoredIdentity.objectId
					&& seed.identity.componentGeneration == authoredIdentity.componentGeneration;
			});
			return found == seeds.end() ? nullptr : &*found;
		}
	}
	float ComputeAudioAttenuation(float distance, float maximumDistance) noexcept {
		// A source has no authored near-radius dead zone: attenuation begins as soon
		// as the listener leaves its position. Squaring the remaining normalized
		// range gives a clear, continuous falloff and reaches silence at maxDistance.
		if(!std::isfinite(distance) || !std::isfinite(maximumDistance) || maximumDistance <= 0.0F) return 0.0F;
		if(distance <= 0.0F) return 1.0F;
		if(distance >= maximumDistance) return 0.0F;
		const float remaining = 1.0F - distance / maximumDistance;
		return remaining * remaining;
	}
	float ComputeAudioPan(const AudioVector3& forward, const AudioVector3& offset) noexcept {
		const float length = Length(offset); if(length <= 0.0001F) return 0.0F;
		// TerraEngine uses a right-handed, Y-up world where authored forward is -Z.
		// Cross(forward, worldUp) therefore produces +X for the listener's right;
		// reversing this cross product would swap the stereo channels. Normalization
		// prevents transform scale from affecting the provider-neutral pan value.
		const AudioVector3 right{-forward.z, 0.0F, forward.x}; const float rightLength = Length(right);
		return rightLength <= 0.0001F ? 0.0F : std::clamp((right.x * offset.x + right.y * offset.y + right.z * offset.z) / (rightLength * length), -1.0F, 1.0F);
	}
	float ComputeAudioFrontness(const AudioVector3& forward, const AudioVector3& offset) noexcept {
		const float forwardLength = Length(forward);
		const float offsetLength = Length(offset);
		if(forwardLength <= 0.0001F || offsetLength <= 0.0001F) return 1.0F;
		return std::clamp((forward.x * offset.x + forward.y * offset.y + forward.z * offset.z)
			/ (forwardLength * offsetLength), -1.0F, 1.0F);
	}
	std::array<float, kAudioRenderChannelCount> ComputeAudioSpeakerGains(AudioOutputMode mode, float pan, float frontness) noexcept {
		std::array<float, kAudioRenderChannelCount> gains{};
		pan = std::clamp(std::isfinite(pan) ? pan : 0.0F, -1.0F, 1.0F);
		frontness = std::clamp(std::isfinite(frontness) ? frontness : 1.0F, -1.0F, 1.0F);
		if(mode == AudioOutputMode::Mono) {
			// Center is channel 2 in the standard 7.1 superset map. Device conversion
			// folds it equally into left/right when the physical endpoint is stereo.
			gains[2U] = 1.0F;
			return gains;
		}
		if(mode == AudioOutputMode::Stereo || mode == AudioOutputMode::Hrtf) {
			// Equal-power panning avoids the audible center dip of a linear crossfade.
			gains[0U] = std::sqrt(0.5F * (1.0F - pan));
			gains[1U] = std::sqrt(0.5F * (1.0F + pan));
			return gains;
		}

		struct Speaker { float angle; std::size_t channel; };
		// Angles are listener-relative: negative is left, positive is right, and
		// +/-180 degrees is directly behind. LFE stays zero because bass management
		// belongs to the output device rather than positional point-source panning.
		const std::array<Speaker, 5U> surround5{{
			{-110.0F * kPi / 180.0F, 4U}, {-30.0F * kPi / 180.0F, 0U},
			{0.0F, 2U}, {30.0F * kPi / 180.0F, 1U}, {110.0F * kPi / 180.0F, 5U}}};
		const std::array<Speaker, 7U> surround7{{
			{-150.0F * kPi / 180.0F, 4U}, {-90.0F * kPi / 180.0F, 6U},
			{-30.0F * kPi / 180.0F, 0U}, {0.0F, 2U}, {30.0F * kPi / 180.0F, 1U},
			{90.0F * kPi / 180.0F, 7U}, {150.0F * kPi / 180.0F, 5U}}};
		const float angle = std::atan2(pan, frontness);
		const auto distribute = [&](const auto& speakers) {
			for(std::size_t index = 0U; index < speakers.size(); ++index) {
				const Speaker& first = speakers[index];
				const Speaker& second = speakers[(index + 1U) % speakers.size()];
				float end = second.angle;
				if(index + 1U == speakers.size()) end += 2.0F * kPi;
				float wrapped = angle;
				if(wrapped < first.angle) wrapped += 2.0F * kPi;
				if(wrapped < first.angle || wrapped > end) continue;
				const float interpolation = (wrapped - first.angle) / (end - first.angle);
				gains[first.channel] = std::cos(interpolation * kPi * 0.5F);
				gains[second.channel] = std::sin(interpolation * kPi * 0.5F);
				return;
			}
		};
		if(mode == AudioOutputMode::Surround5Point1) distribute(surround5);
		else distribute(surround7);
		return gains;
	}
	AudioPreparedFrame PrepareAudioFrame(const AudioAuthoringSnapshot& snapshot, std::vector<AudioPlaybackSeed>& playback) {
		const auto& data = snapshot.Data(); AudioPreparedFrame prepared; prepared.frame = data.frame; prepared.configuredVoiceLimit = data.voiceLimit;
		for(const auto& listener : data.listeners) if(listener.enabled && listener.eligible) { prepared.listener = listener; break; }
		prepared.listenerCandidateCount = static_cast<std::uint32_t>(std::count_if(data.listeners.begin(), data.listeners.end(), [](const auto& item) { return item.enabled && item.eligible; }));
		for(const auto& source : data.sources) {
			AudioPlaybackSeed* seed = FindSeed(playback, source.identity); if(seed == nullptr) continue;
			if(seed->startedClipGeneration != 0U && (seed->startedClipAssetId != source.clipAssetId
				|| seed->startedClipGeneration != source.clipGeneration)) {
				seed->state = AudioPlaybackState::Stopped;
				seed->logicalFrame = 0U;
			}
			AudioPreparedVoice voice; voice.identity = seed->identity; voice.clipAssetId = source.clipAssetId; voice.clipGeneration = source.clipGeneration; voice.priority = source.priority; voice.loop = source.loop; voice.paused = seed->state == AudioPlaybackState::Paused;
			voice.logicalFrame = seed->logicalFrame; voice.pitch = NormalizeAudioPitch(source.pitch);
			voice.gain = NormalizeAudioUnit(source.volume) * NormalizeAudioUnit(source.mixVolume) * NormalizeAudioUnit(data.masterVolume);
			// A Play request may arrive before the clip publication. Preserve the
			// logical instance, but do not advance or offer a voice until the exact
			// provider-neutral asset generation is available.
			if(source.clipAssetId == TerraEngine::EMPTY_UUID || source.clipGeneration == 0U) {
				voice.audibility = AudioAudibility::PendingAsset;
				prepared.voices.push_back(std::move(voice));
				continue;
			}
			if(seed->state == AudioPlaybackState::Playing && !data.gamePaused && !voice.paused) {
				// Logical frames use the clip's own sample rate. Hand-authored tests and
				// pending legacy records may omit it, in which case the runtime default
				// preserves the established 48 kHz timeline rather than stalling.
				const std::uint32_t sampleRate = source.clipSampleRate == 0U ? 48000U : source.clipSampleRate;
				const auto advanced = static_cast<AudioFrame>(std::max(0.0F, data.deltaSeconds)
					* static_cast<float>(sampleRate) * NormalizeAudioPitch(source.pitch));
				seed->logicalFrame += advanced;
				if(source.clipFrames != 0U && seed->logicalFrame >= source.clipFrames) { if(source.loop) seed->logicalFrame %= source.clipFrames; else seed->state = AudioPlaybackState::Completed; }
			}
			voice.logicalFrame = seed->logicalFrame;
			if(seed->state != AudioPlaybackState::Playing) { voice.audibility = seed->state == AudioPlaybackState::Paused ? AudioAudibility::Audible : AudioAudibility::PendingAsset; prepared.voices.push_back(voice); continue; }
			if(data.masterMuted) { voice.audibility = AudioAudibility::MasterMuted; voice.gain = 0.0F; }
			else if(source.mixMuted) { voice.audibility = AudioAudibility::GroupMuted; voice.gain = 0.0F; }
			else if(source.spatial) {
				if(!prepared.listener) { voice.audibility = AudioAudibility::NoListener; voice.gain = 0.0F; }
				else {
					const auto offset = Subtract(source.position, prepared.listener->position);
					const float attenuation = ComputeAudioAttenuation(Length(offset), source.maximumDistance);
					const float spatialBlend = NormalizeAudioUnit(source.spatialBlend);
					const float pan = ComputeAudioPan(prepared.listener->forward, offset);
					const float frontness = ComputeAudioFrontness(prepared.listener->forward, offset);
					voice.spatial.position = source.position;
					voice.spatial.forward = prepared.listener->forward;
					voice.spatial.outputMode = prepared.listener->outputMode;
					voice.spatial.pan = pan;
					voice.spatial.frontness = frontness;
					voice.spatial.spatialBlend = spatialBlend;
					voice.spatial.speakerGains = ComputeAudioSpeakerGains(prepared.listener->outputMode, pan, frontness);
					voice.gain *= 1.0F + (attenuation - 1.0F) * spatialBlend;
					voice.audibility = attenuation == 0.0F ? AudioAudibility::OutOfRange : AudioAudibility::Audible;
				}
			} else voice.audibility = AudioAudibility::Audible;
			prepared.voices.push_back(voice);
		}
		std::vector<std::size_t> audible; for(std::size_t index = 0; index < prepared.voices.size(); ++index) if(prepared.voices[index].audibility == AudioAudibility::Audible) audible.push_back(index);
		std::sort(audible.begin(), audible.end(), [&](std::size_t left, std::size_t right) { const auto& a = prepared.voices[left]; const auto& b = prepared.voices[right]; if(a.priority != b.priority) return a.priority > b.priority; return a.identity.objectId == b.identity.objectId ? (a.identity.componentGeneration == b.identity.componentGeneration ? a.identity.playbackGeneration < b.identity.playbackGeneration : a.identity.componentGeneration < b.identity.componentGeneration) : a.identity.objectId < b.identity.objectId; });
		for(std::size_t ordinal = data.voiceLimit; ordinal < audible.size(); ++ordinal) { auto& voice = prepared.voices[audible[ordinal]]; voice.audibility = AudioAudibility::VoiceLimited; voice.gain = 0.0F; }
		return prepared;
	}
}
