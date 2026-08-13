// AudioBackendRegistry.h - immutable startup registration for audio providers.

#pragma once

#include "Audio/Backend/IAudioBackend.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace TerraEngine::Audio::Backend {
	using AudioBackendFactory = std::function<std::unique_ptr<IAudioBackend>()>;
	struct AudioProviderRegistrationResult { AudioResult result; AudioProviderDescriptor descriptor; };
	class TERRAENGINE_API AudioBackendRegistry final {
	public:
		AudioProviderRegistrationResult RegisterProvider(AudioProviderDescriptor descriptor, AudioBackendFactory factory);
		std::unique_ptr<IAudioBackend> Create(std::string_view providerId, AudioResult& result) const;
		std::optional<AudioProviderDescriptor> Find(std::string_view providerId) const;
		void Freeze();
		bool IsFrozen() const;
	private:
		struct Entry { AudioProviderDescriptor descriptor; AudioBackendFactory factory; };
		mutable std::mutex mutex;
		std::unordered_map<std::string, Entry> entries;
		bool frozen = false;
	};
	TERRAENGINE_API AudioBackendRegistry& GlobalAudioBackendRegistry();
}
