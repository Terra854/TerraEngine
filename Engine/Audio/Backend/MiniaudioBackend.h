// MiniaudioBackend.h - private concrete provider; miniaudio remains hidden in .cpp.

#pragma once

#include "Audio/Backend/IAudioBackend.h"

#include <memory>

namespace TerraEngine::Audio::Backend {
	class MiniaudioBackend final : public IAudioBackend {
	public:
		MiniaudioBackend();
		~MiniaudioBackend() override;
		AudioResult Initialize(const AudioBackendCreateInfo& createInfo) override;
		AudioResult SubmitFrame(const AudioBackendFramePacket& packet) override;
		AudioResult PollObservations(std::span<AudioBackendObservation> destination, std::size_t& requiredCount) override;
		AudioResult Shutdown() override;
	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
	TERRAENGINE_API AudioProviderDescriptor MiniaudioProviderDescriptor();
	TERRAENGINE_API void RegisterMiniaudioBackend();
}
