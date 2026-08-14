// AudioSpatialRenderer.h - allocation-free speaker and analytic HRTF rendering.

#pragma once

#include "Audio/AudioTypes.h"

#include <array>
#include <cstdint>

namespace TerraEngine::Audio {
	// The delay line is intentionally fixed storage. Provider audio callbacks may
	// process it, so rendering cannot allocate, lock, or mutate scene state.
	struct AudioSpatialRenderState final {
		static constexpr std::size_t kDelayCapacity = 128U;
		std::array<float, kDelayCapacity> monoDelay{};
		std::size_t delayCursor = 0U;
		float leftFilter = 0.0F;
		float rightFilter = 0.0F;
	};

	// Converts interleaved source frames into TerraEngine's fixed 7.1 superset
	// channel map: FL, FR, C, LFE, BL, BR, SL, SR. HRTF and Stereo emit only FL/FR;
	// Mono emits C; surround modes use their authored speaker layout.
	TERRAENGINE_API void RenderAudioSpatialFrames(const float* input, std::uint32_t inputChannels,
		float* output, std::uint32_t outputChannels, std::uint32_t frameCount,
		std::uint32_t sampleRate, const AudioSpatialParameters& parameters,
		AudioSpatialRenderState& state) noexcept;
}
