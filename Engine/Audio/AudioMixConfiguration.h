// AudioMixConfiguration.h - provider-neutral flat audio mixing policy.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <shared_mutex>
#include <mutex>
#include <optional>
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
		// Prepared publication holds exclusive ownership across the companion scene
		// commit. Destruction discards; Commit performs only no-fail swaps.
		class TERRAENGINE_API PreparedPublication final {
		public:
			PreparedPublication(PreparedPublication&& other) noexcept;
			PreparedPublication& operator=(PreparedPublication&&) = delete;
			PreparedPublication(const PreparedPublication&) = delete;
			PreparedPublication& operator=(const PreparedPublication&) = delete;
			const AudioMixSnapshot& Candidate() const noexcept { return m_Candidate; }
			void Commit() noexcept;
		private:
			friend class AudioMixConfiguration;
			PreparedPublication(AudioMixConfiguration& owner, std::unique_lock<std::shared_mutex> lock, AudioMixSnapshot candidate) noexcept;
			AudioMixConfiguration* m_Owner;
			std::unique_lock<std::shared_mutex> m_Lock;
			AudioMixSnapshot m_Candidate;
		};

		AudioMixConfiguration();

		[[nodiscard]] AudioMixSnapshot Snapshot() const;
		bool Replace(AudioMixSnapshot candidate, std::string& diagnostic);
		// expectedGeneration must match the caller's copied snapshot. All allocation,
		// validation and lock acquisition finish before the returned guard exists.
		std::optional<PreparedPublication> PrepareReplace(AudioMixSnapshot candidate, std::uint64_t expectedGeneration, std::string& diagnostic);
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
