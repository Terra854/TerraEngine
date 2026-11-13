// Internal no-device observability for the real provider; not a product option.
#pragma once
#include "Audio/Backend/IAudioBackend.h"
#include <cstdint>
#include <memory>
#include <span>

namespace TerraEngine::Audio::Backend::detail {
	struct TERRAENGINE_API MiniaudioOfflineProbe final {
		// Create a real provider graph; SubmitFrame remains the voice admission path.
		static std::unique_ptr<IAudioBackend> Create(const AudioBackendCreateInfo& createInfo, AudioResult& result);
		// Manual mixing is allowed only on an exclusively owned offline provider.
		static AudioResult ReadFrames(IAudioBackend& backend, std::span<float> output, std::uint64_t frameCount);
		static std::uint32_t InputChannels(IAudioBackend& backend, AudioVoiceToken voice);
	};
}
