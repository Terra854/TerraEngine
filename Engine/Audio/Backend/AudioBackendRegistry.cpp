#include "Audio/Backend/AudioBackendRegistry.h"

#include <cctype>

namespace TerraEngine::Audio::Backend {
	namespace { bool IsLowerDottedId(std::string_view value) { if(value.empty() || value.front() == '.' || value.back() == '.') return false; for(const char character : value) if(!(character == '.' || (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z'))) return false; return true; } }
	AudioProviderRegistrationResult AudioBackendRegistry::RegisterProvider(AudioProviderDescriptor descriptor, AudioBackendFactory factory) {
		std::scoped_lock lock(mutex);
		if(frozen) return {AudioResult::Rejected("Audio provider registration is closed after audio initialization."), {}};
		if(!IsLowerDottedId(descriptor.providerId) || !descriptor.IsCompatible() || !factory) return {AudioResult::Rejected("The audio provider descriptor or factory is incomplete or incompatible."), {}};
		if(entries.contains(descriptor.providerId)) return {AudioResult::Rejected("An audio provider with this ID is already registered."), {}};
		auto saved = descriptor;
		// Copy the key before moving the descriptor. Function-argument evaluation
		// order must never let a moved-from provider ID become the map key.
		const std::string providerId = descriptor.providerId;
		entries.emplace(providerId, Entry{std::move(descriptor), std::move(factory)});
		return {AudioResult::Accepted(), std::move(saved)};
	}
	std::unique_ptr<IAudioBackend> AudioBackendRegistry::Create(std::string_view providerId, AudioResult& result) const {
		std::scoped_lock lock(mutex); const auto found = entries.find(std::string(providerId));
		if(found == entries.end()) { result = AudioResult::Rejected("The configured audio provider is not registered."); return {}; }
		auto backend = found->second.factory(); if(!backend) result = {AudioResultCode::ProviderFailure, "The audio provider factory returned no backend."}; else result = AudioResult::Accepted(); return backend;
	}
	std::optional<AudioProviderDescriptor> AudioBackendRegistry::Find(std::string_view providerId) const { std::scoped_lock lock(mutex); const auto found = entries.find(std::string(providerId)); return found == entries.end() ? std::nullopt : std::optional<AudioProviderDescriptor>(found->second.descriptor); }
	void AudioBackendRegistry::Freeze() { std::scoped_lock lock(mutex); frozen = true; }
	bool AudioBackendRegistry::IsFrozen() const { std::scoped_lock lock(mutex); return frozen; }
	AudioBackendRegistry& GlobalAudioBackendRegistry() { static AudioBackendRegistry registry; return registry; }
}
