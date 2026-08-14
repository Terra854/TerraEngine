// AudioSpatialRenderer.cpp - provider-neutral output-mode signal processing.

#include "Audio/AudioSpatialRenderer.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Audio {
	namespace {
		constexpr float kPi = 3.14159265358979323846F;

		float FilterCoefficient(float cutoff, std::uint32_t sampleRate) noexcept {
			const float safeRate = static_cast<float>(std::max(sampleRate, 1U));
			const float safeCutoff = std::clamp(cutoff, 20.0F, safeRate * 0.45F);
			return 1.0F - std::exp(-2.0F * kPi * safeCutoff / safeRate);
		}
	}

	void RenderAudioSpatialFrames(const float* input, std::uint32_t inputChannels,
		float* output, std::uint32_t outputChannels, std::uint32_t frameCount,
		std::uint32_t sampleRate, const AudioSpatialParameters& parameters,
		AudioSpatialRenderState& state) noexcept {
		if(input == nullptr || output == nullptr || inputChannels == 0U || outputChannels == 0U) return;
		const float blend = NormalizeAudioUnit(parameters.spatialBlend);
		const float pan = std::clamp(std::isfinite(parameters.pan) ? parameters.pan : 0.0F, -1.0F, 1.0F);
		const float frontness = std::clamp(std::isfinite(parameters.frontness) ? parameters.frontness : 1.0F, -1.0F, 1.0F);
		const float sideAmount = std::abs(pan);
		const float rearAmount = std::max(-frontness, 0.0F);
		const std::size_t maximumDelay = AudioSpatialRenderState::kDelayCapacity - 1U;
		const std::size_t delayFrames = std::min(maximumDelay, static_cast<std::size_t>(
			std::lround(sideAmount * 0.00063F * static_cast<float>(std::max(sampleRate, 1U)))));
		const float rearCutoff = 20000.0F + (7000.0F - 20000.0F) * rearAmount;
		const float shadowCutoff = 20000.0F + (4500.0F - 20000.0F) * sideAmount;
		const float leftCutoff = pan > 0.0F ? std::min(rearCutoff, shadowCutoff) : rearCutoff;
		const float rightCutoff = pan < 0.0F ? std::min(rearCutoff, shadowCutoff) : rearCutoff;
		const float leftCoefficient = FilterCoefficient(leftCutoff, sampleRate);
		const float rightCoefficient = FilterCoefficient(rightCutoff, sampleRate);

		for(std::uint32_t frame = 0U; frame < frameCount; ++frame) {
			const float* source = input + static_cast<std::size_t>(frame) * inputChannels;
			float mono = 0.0F;
			for(std::uint32_t channel = 0U; channel < inputChannels; ++channel) mono += source[channel];
			mono /= static_cast<float>(inputChannels);
			const float dryLeft = source[0U];
			const float dryRight = inputChannels > 1U ? source[1U] : source[0U];
			float spatial[kAudioRenderChannelCount]{};

			if(parameters.outputMode == AudioOutputMode::Hrtf) {
				// This analytic HRTF models the dominant binaural cues without an
				// external measured dataset: Woodworth-style ITD, head-shadow ILD and
				// low-pass filtering, plus rear spectral damping for front/back clarity.
				state.monoDelay[state.delayCursor] = mono;
				const std::size_t delayedIndex = (state.delayCursor + AudioSpatialRenderState::kDelayCapacity - delayFrames)
					% AudioSpatialRenderState::kDelayCapacity;
				const float delayed = state.monoDelay[delayedIndex];
				state.delayCursor = (state.delayCursor + 1U) % AudioSpatialRenderState::kDelayCapacity;
				const float nearGain = 0.70710678F + 0.29289322F * sideAmount;
				const float farGain = 0.70710678F - 0.35710678F * sideAmount;
				const float rearGain = 1.0F - 0.15F * rearAmount;
				const float leftTarget = rearGain * (pan > 0.0F ? delayed * farGain : mono * nearGain);
				const float rightTarget = rearGain * (pan < 0.0F ? delayed * farGain : mono * nearGain);
				state.leftFilter += leftCoefficient * (leftTarget - state.leftFilter);
				state.rightFilter += rightCoefficient * (rightTarget - state.rightFilter);
				spatial[0U] = state.leftFilter;
				spatial[1U] = state.rightFilter;
			} else {
				for(std::size_t channel = 0U; channel < parameters.speakerGains.size(); ++channel)
					spatial[channel] = mono * parameters.speakerGains[channel];
			}

			float* destination = output + static_cast<std::size_t>(frame) * outputChannels;
			for(std::uint32_t channel = 0U; channel < outputChannels; ++channel) {
				const float dry = channel == 0U ? dryLeft : channel == 1U ? dryRight : 0.0F;
				const float wet = channel < kAudioRenderChannelCount ? spatial[channel] : 0.0F;
				destination[channel] = dry + (wet - dry) * blend;
			}
		}
	}
}
