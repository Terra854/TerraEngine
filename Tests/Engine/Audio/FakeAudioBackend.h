// FakeAudioBackend.h - deterministic test-only provider for audio contracts.

#pragma once

#include "Audio/Backend/IAudioBackend.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Tests::Audio {
	class FakeAudioBackend final : public TerraEngine::Audio::Backend::IAudioBackend {
	public:
		TerraEngine::Audio::AudioResult Initialize(const TerraEngine::Audio::AudioBackendCreateInfo& createInfo) override;
		TerraEngine::Audio::AudioResult SubmitFrame(const TerraEngine::Audio::AudioBackendFramePacket& packet) override;
		TerraEngine::Audio::AudioResult PollObservations(std::span<TerraEngine::Audio::AudioBackendObservation> destination, std::size_t& requiredCount) override;
		TerraEngine::Audio::AudioResult Shutdown() override;
		void InjectObservation(TerraEngine::Audio::AudioBackendObservation observation);
		void FailNextSubmit(std::string message);
		const std::vector<TerraEngine::Audio::AudioBackendFramePacket>& Packets() const noexcept { return packets; }
		bool IsShutdown() const noexcept { return shutdown; }
		std::size_t LiveVoiceCount() const noexcept { return voices.size(); }
		std::size_t RegisteredClipCount() const noexcept { return clips.size(); }
		std::uint64_t DroppedObservationCount() const noexcept { return droppedObservations; }
	private:
		bool initialized = false, shutdown = false;
		TerraEngine::Audio::AudioPacketSequence lastSequence = 0U;
		std::string nextFailure;
		std::deque<TerraEngine::Audio::AudioBackendObservation> observations;
		std::vector<TerraEngine::Audio::AudioBackendFramePacket> packets;
		std::unordered_map<TerraEngine::Audio::AudioVoiceToken, TerraEngine::Audio::AudioVoiceCommand> voices;
		std::unordered_map<std::string, TerraEngine::Audio::AudioClipRegistration> clips;
		std::uint64_t droppedObservations = 0U;
	};
}
