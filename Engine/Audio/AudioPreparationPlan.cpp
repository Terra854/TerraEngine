#include "Audio/AudioPreparationPlan.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Audio {
	namespace {
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
	float ComputeAudioAttenuation(float distance, float minimumDistance, float maximumDistance) noexcept {
		// This normalized inverse-distance curve is full at the near radius and
		// exactly silent at the far radius, which makes virtualizing deterministic.
		if(!std::isfinite(distance) || maximumDistance <= minimumDistance) return 0.0F;
		if(distance <= minimumDistance) return 1.0F; if(distance >= maximumDistance) return 0.0F;
		const float normalized = (distance - minimumDistance) / (maximumDistance - minimumDistance);
		return std::clamp(1.0F / (1.0F + 3.0F * normalized), 0.0F, 1.0F);
	}
	float ComputeAudioPan(const AudioVector3& forward, const AudioVector3& offset) noexcept {
		const float length = Length(offset); if(length <= 0.0001F) return 0.0F;
		// Horizontal right is perpendicular to the authored forward vector in an
		// Y-up world. Normalization prevents transform scale from affecting pan.
		const AudioVector3 right{forward.z, 0.0F, -forward.x}; const float rightLength = Length(right);
		return rightLength <= 0.0001F ? 0.0F : std::clamp((right.x * offset.x + right.y * offset.y + right.z * offset.z) / (rightLength * length), -1.0F, 1.0F);
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
				else { const auto offset = Subtract(source.position, prepared.listener->position); const float attenuation = ComputeAudioAttenuation(Length(offset), source.minimumDistance, source.maximumDistance); const float spatialBlend = NormalizeAudioUnit(source.spatialBlend); voice.spatial = {source.position, prepared.listener->forward, ComputeAudioPan(prepared.listener->forward, offset) * spatialBlend, spatialBlend}; voice.gain *= 1.0F + (attenuation - 1.0F) * spatialBlend; voice.audibility = attenuation == 0.0F ? AudioAudibility::OutOfRange : AudioAudibility::Audible; }
			} else voice.audibility = AudioAudibility::Audible;
			prepared.voices.push_back(voice);
		}
		std::vector<std::size_t> audible; for(std::size_t index = 0; index < prepared.voices.size(); ++index) if(prepared.voices[index].audibility == AudioAudibility::Audible) audible.push_back(index);
		std::sort(audible.begin(), audible.end(), [&](std::size_t left, std::size_t right) { const auto& a = prepared.voices[left]; const auto& b = prepared.voices[right]; if(a.priority != b.priority) return a.priority > b.priority; return a.identity.objectId == b.identity.objectId ? (a.identity.componentGeneration == b.identity.componentGeneration ? a.identity.playbackGeneration < b.identity.playbackGeneration : a.identity.componentGeneration < b.identity.componentGeneration) : a.identity.objectId < b.identity.objectId; });
		for(std::size_t ordinal = data.voiceLimit; ordinal < audible.size(); ++ordinal) { auto& voice = prepared.voices[audible[ordinal]]; voice.audibility = AudioAudibility::VoiceLimited; voice.gain = 0.0F; }
		return prepared;
	}
}
