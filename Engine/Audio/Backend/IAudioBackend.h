// IAudioBackend.h - lifecycle boundary for a coarse engine-owned audio packet.

#pragma once

#include "Audio/AudioTypes.h"

#include <cstddef>
#include <span>

namespace TerraEngine::Audio::Backend {
	class TERRAENGINE_API IAudioBackend {
	public:
		virtual ~IAudioBackend() = default;
		virtual AudioResult Initialize(const AudioBackendCreateInfo& createInfo) = 0;
		virtual AudioResult SubmitFrame(const AudioBackendFramePacket& packet) = 0;
		// The caller supplies a fixed output span, so device callbacks never need
		// to allocate or wait for an engine-owned queue while publishing events.
		virtual AudioResult PollObservations(std::span<AudioBackendObservation> destination, std::size_t& requiredCount) = 0;
		virtual AudioResult Shutdown() = 0;
	};
}
