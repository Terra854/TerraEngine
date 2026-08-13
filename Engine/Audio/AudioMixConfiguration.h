// AudioMixConfiguration.h - provider-neutral flat audio mixing policy.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

namespace TerraEngine::Audio {
	struct AudioMixGroupState {
		TerraEngine::UUID id{};
		std::string name;
		float volume = 1.0F;
		bool muted = false;
		std::uint64_t generation = 1U;
	};

	struct AudioMasterMixState {
		float volume = 1.0F;
		bool muted = false;
		std::uint64_t generation = 1U;
	};

	struct AudioMixSnapshot {
		AudioMasterMixState master;
		std::vector<AudioMixGroupState> groups;
		std::uint64_t generation = 1U;
	};

	class TERRAENGINE_API AudioMixConfiguration final {
	public:
		AudioMixConfiguration();

		[[nodiscard]] AudioMixSnapshot Snapshot() const;
		bool Replace(AudioMixSnapshot candidate, std::string& diagnostic);
		bool SetMasterVolume(float volume, std::string& diagnostic);
		bool SetMasterMuted(bool muted);
		bool SetGroupVolume(const TerraEngine::UUID& id, float volume, std::string& diagnostic);
		bool SetGroupMuted(const TerraEngine::UUID& id, bool muted, std::string& diagnostic);
		bool RenameGroup(const TerraEngine::UUID& id, std::string name, std::string& diagnostic);

		static float NormalizeVolume(float volume) noexcept;
		static bool Validate(const AudioMixSnapshot& candidate, std::string& diagnostic);

	private:
		mutable std::shared_mutex m_Mutex;
		AudioMixSnapshot m_State;
	};
}
